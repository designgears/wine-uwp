// Windows.Storage.AccessCache.StorageApplicationPermissions stand-in for Wine.
//
// After a user picks a file, an app adds it to the future-access list so it can reopen it on a
// later run without asking again. Media Player does this immediately after "Open file(s)", and
// with the class missing the activation throws straight out of the pick:
//
//   err:combase:RoGetActivationFactory Class L"Windows.Storage.AccessCache.StorageApplicationPermissions"
//       not found in L"C:\windows\system32\windows.storage.dll", hr 0x80040111
//   err:combase:combase_dump_unhandled_language_exception ... L"Exception from HRESULT: 0x80040111"
//
// The list is what a UWP app is given instead of raw paths, because the app is sandboxed and its
// access to a picked file has to outlive the pick. Wine's prefix has no such sandbox - the app can
// open any path it can name - so the list has nothing to grant. It is implemented as a real list
// that remembers tokens for the process lifetime: Add hands back a token, ContainsItem and
// CheckAccess answer for it, and the Get*Async calls return the item that token stands for. What
// it deliberately does not do is persist across runs; an app that relies on that will find an
// empty list, which is the same thing it sees when a user clears their history.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>

#define DEF_IID(name, a, b, c, d0,d1,d2,d3,d4,d5,d6,d7) \
    static const GUID name = {a, b, c, {d0,d1,d2,d3,d4,d5,d6,d7}}

DEF_IID(IID_IAgile_,          0x94ea2b94,0xe9cc,0x49e0, 0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90);
DEF_IID(IID_IAsyncInfo_,      0x00000036,0x0000,0x0000, 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEF_IID(IID_IStoragePermStatics_, 0x4391dfaa,0xd033,0x48f9, 0x80,0x60,0x3e,0xc8,0x47,0xd2,0xe3,0xf1);
DEF_IID(IID_IStorageItemAccessList_, 0x2caff6ad,0xde90,0x47f5, 0xb2,0xc3,0xdd,0x36,0xc9,0xfd,0xd4,0x53);
DEF_IID(IID_IStorageItem_,    0x4207a996,0xca2f,0x42f7, 0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30);
DEF_IID(IID_IStorageFile_,    0xfa3f6186,0x4214,0x428c, 0xa6,0x4c,0x14,0xc9,0xac,0x73,0x15,0xea);
/* IAsyncOperation<IStorageItem> and <StorageFile>, derived with ./piid.py. */
DEF_IID(IID_IAsyncOp_Item_,   0x5fc9c137,0xebb7,0x5e6c, 0x9c,0xba,0x68,0x6f,0x2e,0xc2,0xb0,0xbb);
DEF_IID(IID_IAsyncOp_File_,   0x5e52f8ce,0xaced,0x5a42, 0x95,0xb4,0xf6,0x74,0xdd,0x84,0x88,0x5e);
DEF_IID(IID_IMruList_,        0x016239d5,0x510d,0x411e, 0x8c,0xf1,0xc3,0xd1,0xef,0xfa,0x4c,0x33);
/* IStorageItemMostRecentlyUsedList2: Add and AddOrReplace taking an AccessCacheOptions. */
DEF_IID(IID_IMruList2_,       0xda481ea0,0xed8d,0x4731, 0xa1,0xdb,0xe4,0x4e,0xe2,0x20,0x40,0x93);
/* IVectorView<AccessListEntry>: a struct parameter signs as struct(Name;field;field). */
DEF_IID(IID_IVectorView_Entry_, 0x42b49b8a,0x3014,0x5d27, 0x8f,0x2c,0x1e,0xf5,0xee,0x89,0xec,0x00);

enum { ASYNC_COMPLETED = 1 };
#define MAX_ENTRIES 64

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_ACCESSCACHE_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[accesscache] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static HRESULT WINAPI insp_GetIids(void *, ULONG *count, IID **iids)
{ if (count) *count = 0; if (iids) *iids = nullptr; return S_OK; }
static HRESULT WINAPI insp_GetTrustLevel(void *, TrustLevel *level)
{ if (level) *level = BaseTrust; return S_OK; }

/* ---------------------------------------------------------------- a completed async operation */

struct async_item
{
    const void **vtbl;
    const void **vtbl_info;
    LONG ref;
    IUnknown *item;
    GUID iid;            /* which IAsyncOperation<T> this claims to be */
};
static const void *asyncitem_vtbl[9];
static const void *asynciteminfo_vtbl[11];
static async_item *aitem_from_info(void *i) { return CONTAINING_RECORD(i, async_item, vtbl_info); }

static HRESULT WINAPI aitem_QueryInterface(void *iface, REFIID iid, void **out)
{
    async_item *impl = (async_item *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, impl->iid))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IAsyncInfo_)) *out = &impl->vtbl_info;
    else { *out = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI aitem_AddRef(void *iface) { return InterlockedIncrement(&((async_item *)iface)->ref); }
static ULONG WINAPI aitem_Release(void *iface)
{
    async_item *impl = (async_item *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->item) impl->item->Release(); free(impl); }
    return ref;
}
static HRESULT WINAPI aitem_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.IStorageItem>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI aitem_put_Completed(void *iface, IUnknown *handler)
{
    if (!handler) return S_OK;
    handler->AddRef();
    typedef HRESULT (WINAPI *invoke)(IUnknown *, void *, INT32);
    ((invoke *)(*(void ***)handler))[3](handler, iface, ASYNC_COMPLETED);
    handler->Release();
    return S_OK;
}
static HRESULT WINAPI aitem_get_Completed(void *, IUnknown **h) { if (!h) return E_POINTER; *h = nullptr; return S_OK; }
static HRESULT WINAPI aitem_GetResults(void *iface, IUnknown **results)
{
    async_item *impl = (async_item *)iface;
    if (!results) return E_POINTER;
    if ((*results = impl->item)) (*results)->AddRef();
    return S_OK;
}
static HRESULT WINAPI ai_get_Id(void *, UINT32 *id) { if (id) *id = 1; return S_OK; }
static HRESULT WINAPI ai_get_Status(void *, INT32 *s) { if (s) *s = ASYNC_COMPLETED; return S_OK; }
static HRESULT WINAPI ai_get_ErrorCode(void *, HRESULT *hr) { if (hr) *hr = S_OK; return S_OK; }
static HRESULT WINAPI ai_Cancel(void *) { return S_OK; }
static HRESULT WINAPI ai_Close(void *) { return S_OK; }
static HRESULT WINAPI aii_QueryInterface(void *iface, REFIID iid, void **out)
{ return aitem_QueryInterface(aitem_from_info(iface), iid, out); }
static ULONG WINAPI aii_AddRef(void *iface) { return aitem_AddRef(aitem_from_info(iface)); }
static ULONG WINAPI aii_Release(void *iface) { return aitem_Release(aitem_from_info(iface)); }

static HRESULT make_async(IUnknown *item, REFIID iid, IUnknown **out)
{
    async_item *impl;
    if (!(impl = (async_item *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = asyncitem_vtbl;
    impl->vtbl_info = asynciteminfo_vtbl;
    impl->ref = 1;
    impl->iid = iid;
    if ((impl->item = item)) item->AddRef();
    *out = (IUnknown *)impl;
    return S_OK;
}

/* ---------------------------------------------------------------- the access list */

struct entry
{
    WCHAR token[40];
    IUnknown *item;
};

struct access_list
{
    const void **vtbl;
    const void **vtbl_mru;        /* IStorageItemMostRecentlyUsedList, on the MRU list only */
    const void **vtbl_mru2;       /* IStorageItemMostRecentlyUsedList2, likewise */
    bool is_mru;
    LONG ref;
    CRITICAL_SECTION cs;
    entry entries[MAX_ENTRIES];
    UINT32 count;
    LONG next_token;
    const WCHAR *name;            /* for traces: which of the two lists this is */
};
static const void *list_vtbl[22];

static HRESULT WINAPI list_QueryInterface(void *iface, REFIID iid, void **out)
{
    access_list *impl = (access_list *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IStorageItemAccessList_))
    { InterlockedIncrement(&impl->ref); *out = iface; return S_OK; }
    if (IsEqualGUID(iid, IID_IMruList_) && impl->is_mru)
    { InterlockedIncrement(&impl->ref); *out = &impl->vtbl_mru; return S_OK; }
    if (IsEqualGUID(iid, IID_IMruList2_) && impl->is_mru)
    { InterlockedIncrement(&impl->ref); *out = &impl->vtbl_mru2; return S_OK; }
    trace("%ls: QI {%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} -> E_NOINTERFACE", impl->name,
          iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
          iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI list_AddRef(void *iface) { return InterlockedIncrement(&((access_list *)iface)->ref); }
static ULONG WINAPI list_Release(void *iface) { return InterlockedDecrement(&((access_list *)iface)->ref); }
/* The two lists are different runtime classes, and .NET types the wrapper from this name: telling
 * it the MRU list is a plain StorageItemAccessList makes the managed cast to
 * StorageItemMostRecentlyUsedList fail. */
static HRESULT WINAPI list_GetRuntimeClassName(void *iface, HSTRING *name)
{
    access_list *impl = (access_list *)iface;
    const WCHAR *s = impl->is_mru ? L"Windows.Storage.AccessCache.StorageItemMostRecentlyUsedList"
                                  : L"Windows.Storage.AccessCache.StorageItemAccessList";
    return WindowsCreateString(s, lstrlenW(s), name);
}

static HRESULT list_add(access_list *impl, IUnknown *item, HSTRING *token)
{
    HRESULT hr = S_OK;

    if (!item) return E_INVALIDARG;
    EnterCriticalSection(&impl->cs);
    if (impl->count >= MAX_ENTRIES)
    {
        /* Windows evicts the oldest once the list is full; do the same rather than refuse. */
        impl->entries[0].item->Release();
        memmove(impl->entries, impl->entries + 1, (MAX_ENTRIES - 1) * sizeof(*impl->entries));
        impl->count--;
    }
    entry *e = &impl->entries[impl->count++];
    swprintf(e->token, ARRAYSIZE(e->token), L"wine-%ls-%ld", impl->name, InterlockedIncrement(&impl->next_token));
    item->AddRef();
    e->item = item;
    if (token) hr = WindowsCreateString(e->token, lstrlenW(e->token), token);
    trace("%ls: added %p as %ls (%u entries)", impl->name, item, e->token, impl->count);
    LeaveCriticalSection(&impl->cs);
    return hr;
}

static IUnknown *list_lookup(access_list *impl, HSTRING token)
{
    const WCHAR *str = WindowsGetStringRawBuffer(token, nullptr);
    IUnknown *found = nullptr;

    if (!str) return nullptr;
    EnterCriticalSection(&impl->cs);
    for (UINT32 i = 0; i < impl->count; i++)
        if (!lstrcmpW(impl->entries[i].token, str)) { found = impl->entries[i].item; found->AddRef(); break; }
    LeaveCriticalSection(&impl->cs);
    trace("%ls: lookup %ls -> %p", impl->name, str, found);
    return found;
}

static HRESULT WINAPI list_Add(void *iface, IUnknown *item, HSTRING *token)
{ return list_add((access_list *)iface, item, token); }
static HRESULT WINAPI list_AddWithMetadata(void *iface, IUnknown *item, HSTRING metadata, HSTRING *token)
{ return list_add((access_list *)iface, item, token); }
/* AddOrReplace against a token we issued is an update; treating it as an add keeps the newest
 * entry authoritative, which is what the caller wants. */
static HRESULT WINAPI list_AddOrReplace(void *iface, HSTRING token, IUnknown *item)
{ return list_add((access_list *)iface, item, nullptr); }
static HRESULT WINAPI list_AddOrReplaceWithMetadata(void *iface, HSTRING token, IUnknown *item, HSTRING metadata)
{ return list_add((access_list *)iface, item, nullptr); }

static HRESULT list_get_async(void *iface, HSTRING token, REFIID iid, IUnknown **operation)
{
    access_list *impl = (access_list *)iface;
    IUnknown *item = list_lookup(impl, token);
    HRESULT hr;

    if (!operation) return E_POINTER;
    *operation = nullptr;
    hr = make_async(item, iid, operation);
    if (item) item->Release();
    return hr;
}

static HRESULT WINAPI list_GetItemAsync(void *iface, HSTRING token, IUnknown **op)
{ return list_get_async(iface, token, IID_IAsyncOp_Item_, op); }
static HRESULT WINAPI list_GetFileAsync(void *iface, HSTRING token, IUnknown **op)
{ return list_get_async(iface, token, IID_IAsyncOp_File_, op); }
static HRESULT WINAPI list_GetFolderAsync(void *iface, HSTRING token, IUnknown **op)
{ return list_get_async(iface, token, IID_IAsyncOp_Item_, op); }
static HRESULT WINAPI list_GetItemAsyncWithOptions(void *iface, HSTRING token, INT32 options, IUnknown **op)
{ return list_get_async(iface, token, IID_IAsyncOp_Item_, op); }
static HRESULT WINAPI list_GetFileAsyncWithOptions(void *iface, HSTRING token, INT32 options, IUnknown **op)
{ return list_get_async(iface, token, IID_IAsyncOp_File_, op); }
static HRESULT WINAPI list_GetFolderAsyncWithOptions(void *iface, HSTRING token, INT32 options, IUnknown **op)
{ return list_get_async(iface, token, IID_IAsyncOp_Item_, op); }

static HRESULT WINAPI list_Remove(void *iface, HSTRING token)
{
    access_list *impl = (access_list *)iface;
    const WCHAR *str = WindowsGetStringRawBuffer(token, nullptr);

    if (!str) return E_INVALIDARG;
    EnterCriticalSection(&impl->cs);
    for (UINT32 i = 0; i < impl->count; i++)
    {
        if (lstrcmpW(impl->entries[i].token, str)) continue;
        impl->entries[i].item->Release();
        memmove(impl->entries + i, impl->entries + i + 1, (impl->count - i - 1) * sizeof(*impl->entries));
        impl->count--;
        break;
    }
    LeaveCriticalSection(&impl->cs);
    return S_OK;
}

static HRESULT WINAPI list_ContainsItem(void *iface, HSTRING token, boolean *value)
{
    access_list *impl = (access_list *)iface;
    IUnknown *item;
    if (!value) return E_POINTER;
    item = list_lookup(impl, token);
    *value = item ? 1 : 0;
    if (item) item->Release();
    return S_OK;
}

static HRESULT WINAPI list_Clear(void *iface)
{
    access_list *impl = (access_list *)iface;
    EnterCriticalSection(&impl->cs);
    while (impl->count) impl->entries[--impl->count].item->Release();
    LeaveCriticalSection(&impl->cs);
    return S_OK;
}

/* CheckAccess asks whether the app may still open an item. There is no sandbox here, so it can. */
static HRESULT WINAPI list_CheckAccess(void *iface, IUnknown *item, boolean *value)
{
    if (!value) return E_POINTER;
    *value = 1;
    return S_OK;
}

/* ---------------------------------------------------------------- the entry view
 *
 * Entries is an AccessListEntryView: a read-only IVectorView of {Token, Metadata} pairs. An app
 * reads it to show "recent files", and refusing it is fatal - returning E_NOTIMPL here threw
 * straight out of the pick.
 */
struct access_entry { HSTRING token; HSTRING metadata; };

struct entry_view
{
    const void **vtbl;
    LONG ref;
    access_entry items[MAX_ENTRIES];
    UINT32 count;
};
static const void *entryview_vtbl[10];

static HRESULT WINAPI ev_QueryInterface(void *iface, REFIID iid, void **out)
{
    entry_view *impl = (entry_view *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IVectorView_Entry_))
    { InterlockedIncrement(&impl->ref); *out = iface; return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI ev_AddRef(void *iface) { return InterlockedIncrement(&((entry_view *)iface)->ref); }
static ULONG WINAPI ev_Release(void *iface)
{
    entry_view *impl = (entry_view *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref)
    {
        for (UINT32 i = 0; i < impl->count; i++)
        { WindowsDeleteString(impl->items[i].token); WindowsDeleteString(impl->items[i].metadata); }
        free(impl);
    }
    return ref;
}
static HRESULT WINAPI ev_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Storage.AccessCache.AccessListEntryView";
    return WindowsCreateString(s, lstrlenW(s), name);
}
/* AccessListEntry is a struct of two strings, returned by value. */
static HRESULT WINAPI ev_GetAt(void *iface, UINT32 index, access_entry *value)
{
    entry_view *impl = (entry_view *)iface;
    if (!value) return E_POINTER;
    if (index >= impl->count) return E_BOUNDS;
    value->token = nullptr; value->metadata = nullptr;
    WindowsDuplicateString(impl->items[index].token, &value->token);
    WindowsDuplicateString(impl->items[index].metadata, &value->metadata);
    return S_OK;
}
static HRESULT WINAPI ev_get_Size(void *iface, UINT32 *value)
{
    entry_view *impl = (entry_view *)iface;
    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}
static HRESULT WINAPI ev_IndexOf(void *iface, access_entry item, UINT32 *index, boolean *found)
{
    if (!index || !found) return E_POINTER;
    *index = 0; *found = 0;
    return S_OK;
}
static HRESULT WINAPI ev_GetMany(void *iface, UINT32 start, UINT32 count, access_entry *items, UINT32 *written)
{
    entry_view *impl = (entry_view *)iface;
    UINT32 n = 0;
    if (!written) return E_POINTER;
    while (n < count && start + n < impl->count) { ev_GetAt(iface, start + n, items + n); n++; }
    *written = n;
    return S_OK;
}

static HRESULT WINAPI list_get_Entries(void *iface, IUnknown **value)
{
    access_list *impl = (access_list *)iface;
    entry_view *view;

    if (!value) return E_POINTER;
    *value = nullptr;
    if (!(view = (entry_view *)calloc(1, sizeof(*view)))) return E_OUTOFMEMORY;
    view->vtbl = entryview_vtbl;
    view->ref = 1;

    EnterCriticalSection(&impl->cs);
    for (UINT32 i = 0; i < impl->count; i++)
    {
        WindowsCreateString(impl->entries[i].token, lstrlenW(impl->entries[i].token), &view->items[i].token);
        WindowsCreateString(L"", 0, &view->items[i].metadata);
    }
    view->count = impl->count;
    LeaveCriticalSection(&impl->cs);

    trace("%ls: get_Entries -> %u entries", impl->name, view->count);
    *value = (IUnknown *)view;
    return S_OK;
}

/* IStorageItemMostRecentlyUsedList adds an item-removed event. Nothing evicts behind the app's
 * back except a full list, so it never fires - but the registration has to succeed. */
static HRESULT WINAPI mru_add_ItemRemoved(void *iface, void *handler, INT64 *token)
{
    trace("mru: add_ItemRemoved (never fires)");
    if (token) *token = 1;
    return S_OK;
}
static HRESULT WINAPI mru_remove_ItemRemoved(void *iface, INT64 token) { return S_OK; }
static access_list *mru_from_vtbl(void *i) { return CONTAINING_RECORD(i, access_list, vtbl_mru); }
static HRESULT WINAPI mru_QueryInterface(void *iface, REFIID iid, void **out)
{ return list_QueryInterface(mru_from_vtbl(iface), iid, out); }
static ULONG WINAPI mru_AddRef(void *iface) { return list_AddRef(mru_from_vtbl(iface)); }
static ULONG WINAPI mru_Release(void *iface) { return list_Release(mru_from_vtbl(iface)); }
static const void *mrulist_vtbl[8];
static const void *mrulist2_vtbl[8];
static access_list *mru_from_vtbl(void *i);
static access_list *mru2_from_vtbl(void *i);
static HRESULT WINAPI mru_GetRuntimeClassName(void *iface, HSTRING *name)
{ return list_GetRuntimeClassName(mru_from_vtbl(iface), name); }
static HRESULT WINAPI mru2_GetRuntimeClassName(void *iface, HSTRING *name)
{ return list_GetRuntimeClassName(mru2_from_vtbl(iface), name); }



/* The "2" interface is the same add, plus an AccessCacheOptions telling the list how to treat the
 * entry (whether to allow it in the future list too, and so on). Nothing here acts on the option,
 * because there is no persistence to differentiate. */
static access_list *mru2_from_vtbl(void *i) { return CONTAINING_RECORD(i, access_list, vtbl_mru2); }
static HRESULT WINAPI mru2_QueryInterface(void *iface, REFIID iid, void **out)
{ return list_QueryInterface(mru2_from_vtbl(iface), iid, out); }
static ULONG WINAPI mru2_AddRef(void *iface) { return list_AddRef(mru2_from_vtbl(iface)); }
static ULONG WINAPI mru2_Release(void *iface) { return list_Release(mru2_from_vtbl(iface)); }
static HRESULT WINAPI mru2_Add(void *iface, IUnknown *item, HSTRING metadata, INT32 options,
                               HSTRING *token)
{
    trace("mru2: Add with options %d", options);
    return list_add(mru2_from_vtbl(iface), item, token);
}
static HRESULT WINAPI mru2_AddOrReplace(void *iface, HSTRING token, IUnknown *item, HSTRING metadata,
                                        INT32 options)
{
    trace("mru2: AddOrReplace with options %d", options);
    return list_add(mru2_from_vtbl(iface), item, nullptr);
}

static HRESULT WINAPI list_get_MaximumItemsAllowed(void *iface, UINT32 *value)
{
    if (!value) return E_POINTER;
    *value = MAX_ENTRIES;
    return S_OK;
}

/* ---------------------------------------------------------------- statics */

struct statics_object { const void **vtbl; };
static const void *statics_vtbl[8];
static const void *factory_vtbl[7];
static statics_object statics, factory;
static access_list future_list, mru_list;

static HRESULT WINAPI statics_QueryInterface(void *, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IStoragePermStatics_))
    { *out = &statics.vtbl; return S_OK; }
    if (IsEqualGUID(iid, IID_IActivationFactory)) { *out = &factory.vtbl; return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI statics_AddRef(void *) { return 2; }
static ULONG WINAPI statics_Release(void *) { return 1; }
static HRESULT WINAPI statics_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Storage.AccessCache.StorageApplicationPermissions";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI statics_get_FutureAccessList(void *, IUnknown **value)
{
    if (!value) return E_POINTER;
    list_AddRef(&future_list);
    *value = (IUnknown *)&future_list;
    return S_OK;
}
static HRESULT WINAPI statics_get_MostRecentlyUsedList(void *, IUnknown **value)
{
    if (!value) return E_POINTER;
    list_AddRef(&mru_list);
    *value = (IUnknown *)&mru_list;
    return S_OK;
}
static HRESULT WINAPI factory_ActivateInstance(void *, IInspectable **instance)
{
    /* A static class; there is nothing to construct. */
    if (instance) *instance = nullptr;
    return E_NOTIMPL;
}

static void init_vtbls(void)
{
    asyncitem_vtbl[0] = (void *)aitem_QueryInterface;
    asyncitem_vtbl[1] = (void *)aitem_AddRef;
    asyncitem_vtbl[2] = (void *)aitem_Release;
    asyncitem_vtbl[3] = (void *)insp_GetIids;
    asyncitem_vtbl[4] = (void *)aitem_GetRuntimeClassName;
    asyncitem_vtbl[5] = (void *)insp_GetTrustLevel;
    asyncitem_vtbl[6] = (void *)aitem_put_Completed;
    asyncitem_vtbl[7] = (void *)aitem_get_Completed;
    asyncitem_vtbl[8] = (void *)aitem_GetResults;

    asynciteminfo_vtbl[0] = (void *)aii_QueryInterface;
    asynciteminfo_vtbl[1] = (void *)aii_AddRef;
    asynciteminfo_vtbl[2] = (void *)aii_Release;
    asynciteminfo_vtbl[3] = (void *)insp_GetIids;
    asynciteminfo_vtbl[4] = (void *)aitem_GetRuntimeClassName;
    asynciteminfo_vtbl[5] = (void *)insp_GetTrustLevel;
    asynciteminfo_vtbl[6] = (void *)ai_get_Id;
    asynciteminfo_vtbl[7] = (void *)ai_get_Status;
    asynciteminfo_vtbl[8] = (void *)ai_get_ErrorCode;
    asynciteminfo_vtbl[9] = (void *)ai_Cancel;
    asynciteminfo_vtbl[10] = (void *)ai_Close;

    list_vtbl[0]  = (void *)list_QueryInterface;
    list_vtbl[1]  = (void *)list_AddRef;
    list_vtbl[2]  = (void *)list_Release;
    list_vtbl[3]  = (void *)insp_GetIids;
    list_vtbl[4]  = (void *)list_GetRuntimeClassName;
    list_vtbl[5]  = (void *)insp_GetTrustLevel;
    list_vtbl[6]  = (void *)list_Add;
    list_vtbl[7]  = (void *)list_AddWithMetadata;
    list_vtbl[8]  = (void *)list_AddOrReplace;
    list_vtbl[9]  = (void *)list_AddOrReplaceWithMetadata;
    list_vtbl[10] = (void *)list_GetItemAsync;
    list_vtbl[11] = (void *)list_GetFileAsync;
    list_vtbl[12] = (void *)list_GetFolderAsync;
    list_vtbl[13] = (void *)list_GetItemAsyncWithOptions;
    list_vtbl[14] = (void *)list_GetFileAsyncWithOptions;
    list_vtbl[15] = (void *)list_GetFolderAsyncWithOptions;
    list_vtbl[16] = (void *)list_Remove;
    list_vtbl[17] = (void *)list_ContainsItem;
    list_vtbl[18] = (void *)list_Clear;
    list_vtbl[19] = (void *)list_CheckAccess;
    list_vtbl[20] = (void *)list_get_Entries;
    list_vtbl[21] = (void *)list_get_MaximumItemsAllowed;

    statics_vtbl[0] = (void *)statics_QueryInterface;
    statics_vtbl[1] = (void *)statics_AddRef;
    statics_vtbl[2] = (void *)statics_Release;
    statics_vtbl[3] = (void *)insp_GetIids;
    statics_vtbl[4] = (void *)statics_GetRuntimeClassName;
    statics_vtbl[5] = (void *)insp_GetTrustLevel;
    statics_vtbl[6] = (void *)statics_get_FutureAccessList;
    statics_vtbl[7] = (void *)statics_get_MostRecentlyUsedList;

    factory_vtbl[0] = (void *)statics_QueryInterface;
    factory_vtbl[1] = (void *)statics_AddRef;
    factory_vtbl[2] = (void *)statics_Release;
    factory_vtbl[3] = (void *)insp_GetIids;
    factory_vtbl[4] = (void *)statics_GetRuntimeClassName;
    factory_vtbl[5] = (void *)insp_GetTrustLevel;
    factory_vtbl[6] = (void *)factory_ActivateInstance;

    entryview_vtbl[0] = (void *)ev_QueryInterface;
    entryview_vtbl[1] = (void *)ev_AddRef;
    entryview_vtbl[2] = (void *)ev_Release;
    entryview_vtbl[3] = (void *)insp_GetIids;
    entryview_vtbl[4] = (void *)ev_GetRuntimeClassName;
    entryview_vtbl[5] = (void *)insp_GetTrustLevel;
    entryview_vtbl[6] = (void *)ev_GetAt;
    entryview_vtbl[7] = (void *)ev_get_Size;
    entryview_vtbl[8] = (void *)ev_IndexOf;
    entryview_vtbl[9] = (void *)ev_GetMany;

    mrulist_vtbl[0] = (void *)mru_QueryInterface;
    mrulist_vtbl[1] = (void *)mru_AddRef;
    mrulist_vtbl[2] = (void *)mru_Release;
    mrulist_vtbl[3] = (void *)insp_GetIids;
    mrulist_vtbl[4] = (void *)mru_GetRuntimeClassName;
    mrulist_vtbl[5] = (void *)insp_GetTrustLevel;
    mrulist_vtbl[6] = (void *)mru_add_ItemRemoved;
    mrulist_vtbl[7] = (void *)mru_remove_ItemRemoved;

    mrulist2_vtbl[0] = (void *)mru2_QueryInterface;
    mrulist2_vtbl[1] = (void *)mru2_AddRef;
    mrulist2_vtbl[2] = (void *)mru2_Release;
    mrulist2_vtbl[3] = (void *)insp_GetIids;
    mrulist2_vtbl[4] = (void *)mru2_GetRuntimeClassName;
    mrulist2_vtbl[5] = (void *)insp_GetTrustLevel;
    mrulist2_vtbl[6] = (void *)mru2_Add;
    mrulist2_vtbl[7] = (void *)mru2_AddOrReplace;

    statics.vtbl = statics_vtbl;
    factory.vtbl = factory_vtbl;
    future_list.vtbl = list_vtbl; future_list.ref = 1; future_list.name = L"future";
    mru_list.vtbl = list_vtbl;    mru_list.ref = 1;    mru_list.name = L"mru";
    mru_list.vtbl_mru = mrulist_vtbl; mru_list.vtbl_mru2 = mrulist2_vtbl; mru_list.is_mru = true;
    InitializeCriticalSectionEx(&future_list.cs, 0, 0);
    InitializeCriticalSectionEx(&mru_list.cs, 0, 0);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *str = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!str || lstrcmpW(str, L"Windows.Storage.AccessCache.StorageApplicationPermissions"))
    {
        trace("not ours: %ls", str ? str : L"(null)");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    init_vtbls();
    trace("serving %ls", str);
    *out = (IActivationFactory *)&statics;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

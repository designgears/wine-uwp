// Windows.Storage.StorageLibrary stand-in for Wine.
//
// Once AppCapability reports musicLibrary as Allowed (shims/appcapability), Media Player's library
// pages go on to ask for this class and Wine's windows.storage does not have it:
//
//   err:combase:RoGetActivationFactory Class L"Windows.Storage.StorageLibrary" not found in
//       L"C:\windows\system32\windows.storage.dll", hr 0x80040111
//
// It belongs in dlls/windows.storage - the async and folder machinery is all there - but Wine's
// include/windows.storage.idl declares neither StorageLibrary nor KnownLibraryId, and include/ is
// not ours to change. So it lives here, with hand-written vtables.
//
// A library is a list of folders plus a save folder. A Wine prefix has no library database, so the
// library for a KnownLibraryId is exactly the one known folder Wine already maps to the user's
// profile directory (dlls/windows.storage/storagefolder.c) - which this gets by calling
// KnownFolders itself rather than duplicating the path logic.
//
// The parameterised IIDs below come from ./piid.py, whose --self-test checks the
// derivation against IIDs widl generates. They are in no .winmd and in no registry.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>
#include <eventtoken.h>

/* ---------------------------------------------------------------- IIDs */

#define DEF_IID(name, a, b, c, d0,d1,d2,d3,d4,d5,d6,d7) \
    static const GUID name = {a, b, c, {d0,d1,d2,d3,d4,d5,d6,d7}}

DEF_IID(IID_IAgile,                  0x94ea2b94,0xe9cc,0x49e0, 0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90);
DEF_IID(IID_IAsyncInfo,              0x00000036,0x0000,0x0000, 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
/* Windows.Storage */
DEF_IID(IID_IStorageLibraryStatics,  0x4208a6db,0x684a,0x49c6, 0x9e,0x59,0x90,0x12,0x1e,0xe0,0x50,0xd6);
DEF_IID(IID_IStorageLibraryStatics2, 0xffb08ddc,0xfa75,0x4695, 0xb9,0xd1,0x7f,0x81,0xf9,0x78,0x32,0xe3);
DEF_IID(IID_IStorageLibrary,         0x1edd7103,0x0e5e,0x4d6c, 0xb5,0xe8,0x93,0x18,0x98,0x3d,0x6a,0x03);
DEF_IID(IID_IStorageLibrary2,        0x5b0ce348,0xfcb3,0x4031, 0xaf,0xb0,0xa6,0x8d,0x7b,0xd4,0x45,0x34);
DEF_IID(IID_IStorageLibrary3,        0x8a281291,0x2154,0x4201, 0x81,0x13,0xd2,0xc0,0x5c,0xe1,0xad,0x23);
DEF_IID(IID_IKnownFoldersStatics,    0x5a2a7520,0x4802,0x452d, 0x9a,0xd9,0x43,0x51,0xad,0xa7,0xec,0x35);
/* Parameterised - see piid.py */
DEF_IID(IID_IAsyncOperation_Library, 0x2f160a19,0x99c1,0x52b9, 0x8d,0xca,0x14,0xe4,0xab,0x79,0xf2,0x87);
DEF_IID(IID_IObservableVector_Folder,0x358f44df,0x2a45,0x5cb8, 0x93,0x85,0x1f,0xf6,0x68,0x08,0xcd,0xe0);
DEF_IID(IID_IVector_Folder,          0x6c26b7be,0x5f01,0x5a60, 0x9d,0xd7,0xfd,0x17,0xbe,0x3a,0x9d,0xd6);
DEF_IID(IID_IVectorView_Folder,      0xe20debc6,0xdc4e,0x542e, 0xa2,0xe7,0xa2,0x4d,0x19,0xc8,0xdd,0x62);
DEF_IID(IID_IIterable_Folder,        0x4669befc,0xae5c,0x52b1, 0x8a,0x97,0x54,0x66,0xce,0x61,0xe9,0x4e);
DEF_IID(IID_IIterator_Folder,        0x5aac96fb,0xb3b9,0x5a7f, 0xa9,0x20,0x4b,0x5a,0x8d,0xf8,0x11,0x68);

/* Windows.Storage.KnownLibraryId */
enum { LIBRARY_MUSIC = 0, LIBRARY_PICTURES = 1, LIBRARY_VIDEOS = 2, LIBRARY_DOCUMENTS = 3 };
/* Windows.Foundation.AsyncStatus */
enum { ASYNC_STARTED = 0, ASYNC_COMPLETED = 1, ASYNC_CANCELED = 2, ASYNC_ERROR = 3 };

static const char *iid_str(REFIID iid)
{
    static char buf[4][48];
    static LONG n;
    char *out = buf[InterlockedIncrement(&n) & 3];
    sprintf(out, "{%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x}", iid.Data1, iid.Data2, iid.Data3,
            iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
            iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return out;
}

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_STORAGELIBRARY_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[storagelibrary] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* Every object here is agile and process-lifetime-ish; a small shared IInspectable tail keeps the
 * vtables readable. Each object's first field is its primary vtable pointer. */
static HRESULT WINAPI insp_GetIids(void *, ULONG *count, IID **iids)
{
    if (count) *count = 0;
    if (iids) *iids = nullptr;
    return S_OK;
}
static HRESULT WINAPI insp_GetTrustLevel(void *, TrustLevel *level)
{
    if (level) *level = BaseTrust;
    return S_OK;
}

/* ---------------------------------------------------------------- the one known folder */

/* KnownFolders is implemented by Wine, so ask it rather than re-deriving paths. The returned
 * IStorageFolder is the library's only folder and its save folder. */
static HRESULT known_folder_for_library(INT32 id, IUnknown **out)
{
    static const WCHAR *cls = L"Windows.Storage.KnownFolders";
    IUnknown *statics = nullptr, *folder = nullptr;
    HSTRING name = nullptr;
    HRESULT hr;
    unsigned slot;

    *out = nullptr;
    switch (id)
    {
    case LIBRARY_MUSIC:     slot = 6; break;
    case LIBRARY_PICTURES:  slot = 7; break;
    case LIBRARY_VIDEOS:    slot = 8; break;
    case LIBRARY_DOCUMENTS: slot = 9; break;
    default:
        trace("unknown KnownLibraryId %d", id);
        return E_INVALIDARG;
    }

    if (FAILED(hr = WindowsCreateString(cls, lstrlenW(cls), &name))) return hr;
    hr = RoGetActivationFactory(name, IID_IKnownFoldersStatics, (void **)&statics);
    WindowsDeleteString(name);
    if (FAILED(hr)) { trace("KnownFolders statics: %#lx", hr); return hr; }

    /* IKnownFoldersStatics slot 6..9 are get_MusicLibrary/Pictures/Videos/Documents, each
     * HRESULT(IStorageFolder **). */
    typedef HRESULT (WINAPI *getter)(void *, IUnknown **);
    getter fn = ((getter *)(*(void ***)statics))[slot];
    hr = fn(statics, &folder);
    ((IUnknown *)statics)->Release();
    if (FAILED(hr)) { trace("known folder slot %u: %#lx", slot, hr); return hr; }

    trace("library %d -> folder %p", id, folder);
    *out = folder;
    return S_OK;
}

/* ---------------------------------------------------------------- IIterator<StorageFolder> */

struct folder_iterator
{
    const void **vtbl;
    LONG ref;
    IUnknown *folder;       /* the single element, or null once consumed */
    bool consumed;
};
static const void *iterator_vtbl[10];

static HRESULT WINAPI it_QueryInterface(void *iface, REFIID iid, void **out)
{
    folder_iterator *impl = (folder_iterator *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile) || IsEqualGUID(iid, IID_IIterator_Folder))
    {
        InterlockedIncrement(&impl->ref);
        *out = iface;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI it_AddRef(void *iface) { return InterlockedIncrement(&((folder_iterator *)iface)->ref); }
static ULONG WINAPI it_Release(void *iface)
{
    folder_iterator *impl = (folder_iterator *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->folder) impl->folder->Release(); free(impl); }
    return ref;
}
static HRESULT WINAPI it_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.StorageFolder>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI it_get_Current(void *iface, IUnknown **value)
{
    folder_iterator *impl = (folder_iterator *)iface;
    if (!value) return E_POINTER;
    *value = impl->consumed ? nullptr : impl->folder;
    if (*value) (*value)->AddRef();
    return S_OK;
}
static HRESULT WINAPI it_get_HasCurrent(void *iface, boolean *value)
{
    folder_iterator *impl = (folder_iterator *)iface;
    if (!value) return E_POINTER;
    *value = (!impl->consumed && impl->folder) ? 1 : 0;
    return S_OK;
}
static HRESULT WINAPI it_MoveNext(void *iface, boolean *value)
{
    folder_iterator *impl = (folder_iterator *)iface;
    impl->consumed = true;          /* one element, so the first move ends the iteration */
    if (value) *value = 0;
    return S_OK;
}
static HRESULT WINAPI it_GetMany(void *iface, UINT32 count, IUnknown **items, UINT32 *written)
{
    folder_iterator *impl = (folder_iterator *)iface;
    UINT32 n = 0;
    if (!written) return E_POINTER;
    if (count && !impl->consumed && impl->folder)
    {
        items[0] = impl->folder;
        impl->folder->AddRef();
        impl->consumed = true;
        n = 1;
    }
    *written = n;
    return S_OK;
}

/* ---------------------------------------------------------------- the folder collection
 *
 * One object answers IObservableVector<StorageFolder>, IVector<StorageFolder>,
 * IVectorView<StorageFolder> and IIterable<StorageFolder>: the app may QI for any of them, and a
 * single immutable element makes the three shapes indistinguishable in practice. Mutation
 * (Append/RemoveAt/Clear) is refused - the prefix has no library database to write to - and
 * VectorChanged registrations succeed but never fire, because the contents cannot change.
 */
struct folder_vector
{
    const void **vtbl;          /* IObservableVector<StorageFolder> */
    const void **vtbl_vector;   /* IVector<StorageFolder> */
    const void **vtbl_view;     /* IVectorView<StorageFolder> */
    const void **vtbl_iterable; /* IIterable<StorageFolder> */
    LONG ref;
    IUnknown *folder;
};
static const void *obsvec_vtbl[8];
static const void *vector_vtbl[18];
static const void *view_vtbl[10];
static const void *iterable_vtbl[7];

static folder_vector *fv_from_vector(void *i)   { return CONTAINING_RECORD(i, folder_vector, vtbl_vector); }
static folder_vector *fv_from_view(void *i)     { return CONTAINING_RECORD(i, folder_vector, vtbl_view); }
static folder_vector *fv_from_iterable(void *i) { return CONTAINING_RECORD(i, folder_vector, vtbl_iterable); }

static HRESULT WINAPI fv_QueryInterface(void *iface, REFIID iid, void **out)
{
    folder_vector *impl = (folder_vector *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile) || IsEqualGUID(iid, IID_IObservableVector_Folder))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IVector_Folder))        *out = &impl->vtbl_vector;
    else if (IsEqualGUID(iid, IID_IVectorView_Folder))    *out = &impl->vtbl_view;
    else if (IsEqualGUID(iid, IID_IIterable_Folder))      *out = &impl->vtbl_iterable;
    else
    {
        trace("folders QI %s -> E_NOINTERFACE", iid_str(iid));
        *out = nullptr;
        return E_NOINTERFACE;
    }
    trace("folders QI %s -> ok", iid_str(iid));
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI fv_AddRef(void *iface) { return InterlockedIncrement(&((folder_vector *)iface)->ref); }
static ULONG WINAPI fv_Release(void *iface)
{
    folder_vector *impl = (folder_vector *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->folder) impl->folder->Release(); free(impl); }
    return ref;
}
static HRESULT WINAPI fv_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.Collections.IObservableVector`1<Windows.Storage.StorageFolder>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
/* The alternate views share the object, so their IUnknown walks back to the primary vtable. */
#define FV_FORWARD_UNKNOWN(tag, back)                                                              \
    static HRESULT WINAPI tag##_QueryInterface(void *i, REFIID iid, void **out)                    \
    { return fv_QueryInterface(back(i), iid, out); }                                               \
    static ULONG WINAPI tag##_AddRef(void *i) { return fv_AddRef(back(i)); }                        \
    static ULONG WINAPI tag##_Release(void *i) { return fv_Release(back(i)); }
FV_FORWARD_UNKNOWN(vec,  fv_from_vector)
FV_FORWARD_UNKNOWN(view, fv_from_view)
FV_FORWARD_UNKNOWN(iter, fv_from_iterable)

/* IObservableVector<T>: add_VectorChanged / remove_VectorChanged. The contents are fixed, so the
 * registration is a token that never fires - but it has to succeed, because a refused event
 * registration is the thing that kills these apps. */
static HRESULT WINAPI fv_add_VectorChanged(void *, void *, EventRegistrationToken *token)
{
    if (token) token->value = 1;
    return S_OK;
}
static HRESULT WINAPI fv_remove_VectorChanged(void *, EventRegistrationToken) { return S_OK; }

static HRESULT fv_get_at(folder_vector *impl, UINT32 index, IUnknown **value)
{
    if (!value) return E_POINTER;
    *value = nullptr;
    trace("folders GetAt(%u)", index);
    if (index || !impl->folder) return E_BOUNDS;
    *value = impl->folder;
    impl->folder->AddRef();
    return S_OK;
}
static HRESULT fv_size(folder_vector *impl, UINT32 *value)
{
    if (!value) return E_POINTER;
    *value = impl->folder ? 1 : 0;
    trace("folders get_Size -> %u", *value);
    return S_OK;
}
static HRESULT fv_index_of(folder_vector *impl, IUnknown *item, UINT32 *index, boolean *found)
{
    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = (impl->folder && item == impl->folder) ? 1 : 0;
    return S_OK;
}
static HRESULT fv_get_many(folder_vector *impl, UINT32 start, UINT32 count, IUnknown **items, UINT32 *written)
{
    UINT32 n = 0;
    if (!written) return E_POINTER;
    if (!start && count && impl->folder) { items[0] = impl->folder; impl->folder->AddRef(); n = 1; }
    *written = n;
    return S_OK;
}

/* IVector<StorageFolder> */
static HRESULT WINAPI vec_GetAt(void *i, UINT32 index, IUnknown **v) { return fv_get_at(fv_from_vector(i), index, v); }
static HRESULT WINAPI vec_get_Size(void *i, UINT32 *v) { return fv_size(fv_from_vector(i), v); }
static HRESULT WINAPI vec_GetView(void *i, void **v)
{
    folder_vector *impl = fv_from_vector(i);
    if (!v) return E_POINTER;
    fv_AddRef(impl);
    *v = &impl->vtbl_view;
    return S_OK;
}
static HRESULT WINAPI vec_IndexOf(void *i, IUnknown *item, UINT32 *index, boolean *found)
{ return fv_index_of(fv_from_vector(i), item, index, found); }
static HRESULT WINAPI vec_GetMany(void *i, UINT32 start, UINT32 count, IUnknown **items, UINT32 *written)
{ return fv_get_many(fv_from_vector(i), start, count, items, written); }
/* The library's folder list is not ours to change; say so rather than pretend. */
static HRESULT WINAPI vec_readonly(void *) { trace("vector mutation refused"); return E_NOTIMPL; }

/* IVectorView<StorageFolder> */
static HRESULT WINAPI view_GetAt(void *i, UINT32 index, IUnknown **v) { return fv_get_at(fv_from_view(i), index, v); }
static HRESULT WINAPI view_get_Size(void *i, UINT32 *v) { return fv_size(fv_from_view(i), v); }
static HRESULT WINAPI view_IndexOf(void *i, IUnknown *item, UINT32 *index, boolean *found)
{ return fv_index_of(fv_from_view(i), item, index, found); }
static HRESULT WINAPI view_GetMany(void *i, UINT32 start, UINT32 count, IUnknown **items, UINT32 *written)
{ return fv_get_many(fv_from_view(i), start, count, items, written); }

/* IIterable<StorageFolder> */
static HRESULT WINAPI iter_First(void *i, void **out)
{
    folder_vector *impl = fv_from_iterable(i);
    folder_iterator *it;
    if (!out) return E_POINTER;
    *out = nullptr;
    trace("folders First()");
    if (!(it = (folder_iterator *)calloc(1, sizeof(*it)))) return E_OUTOFMEMORY;
    it->vtbl = iterator_vtbl;
    it->ref = 1;
    if ((it->folder = impl->folder)) it->folder->AddRef();
    *out = it;
    return S_OK;
}

static HRESULT folder_vector_create(IUnknown *folder, folder_vector **out)
{
    folder_vector *impl;
    if (!(impl = (folder_vector *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = obsvec_vtbl;
    impl->vtbl_vector = vector_vtbl;
    impl->vtbl_view = view_vtbl;
    impl->vtbl_iterable = iterable_vtbl;
    impl->ref = 1;
    if ((impl->folder = folder)) folder->AddRef();
    *out = impl;
    return S_OK;
}

/* ---------------------------------------------------------------- StorageLibrary */

struct storage_library
{
    const void **vtbl;      /* IStorageLibrary */
    const void **vtbl2;     /* IStorageLibrary2 */
    const void **vtbl3;     /* IStorageLibrary3 */
    LONG ref;
    INT32 id;
    IUnknown *folder;
    folder_vector *folders;
};
static const void *library_vtbl[12];
static const void *library2_vtbl[7];
static const void *library3_vtbl[7];

static storage_library *lib_from_2(void *i) { return CONTAINING_RECORD(i, storage_library, vtbl2); }
static storage_library *lib_from_3(void *i) { return CONTAINING_RECORD(i, storage_library, vtbl3); }

static HRESULT WINAPI lib_QueryInterface(void *iface, REFIID iid, void **out)
{
    storage_library *impl = (storage_library *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile) || IsEqualGUID(iid, IID_IStorageLibrary))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IStorageLibrary2)) *out = &impl->vtbl2;
    else if (IsEqualGUID(iid, IID_IStorageLibrary3)) *out = &impl->vtbl3;
    else
    {
        trace("library QI %s -> E_NOINTERFACE", iid_str(iid));
        *out = nullptr;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI lib_AddRef(void *iface) { return InterlockedIncrement(&((storage_library *)iface)->ref); }
static ULONG WINAPI lib_Release(void *iface)
{
    storage_library *impl = (storage_library *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref)
    {
        if (impl->folders) fv_Release(impl->folders);
        if (impl->folder) impl->folder->Release();
        free(impl);
    }
    return ref;
}
static HRESULT WINAPI lib_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Storage.StorageLibrary";
    return WindowsCreateString(s, lstrlenW(s), name);
}
#define LIB_FORWARD_UNKNOWN(tag, back)                                                             \
    static HRESULT WINAPI tag##_QueryInterface(void *i, REFIID iid, void **out)                    \
    { return lib_QueryInterface(back(i), iid, out); }                                              \
    static ULONG WINAPI tag##_AddRef(void *i) { return lib_AddRef(back(i)); }                       \
    static ULONG WINAPI tag##_Release(void *i) { return lib_Release(back(i)); }
LIB_FORWARD_UNKNOWN(lib2, lib_from_2)
LIB_FORWARD_UNKNOWN(lib3, lib_from_3)

static HRESULT WINAPI lib_get_Folders(void *iface, void **value)
{
    storage_library *impl = (storage_library *)iface;
    if (!value) return E_POINTER;
    fv_AddRef(impl->folders);
    *value = &impl->folders->vtbl;
    trace("get_Folders -> %p (1 folder)", *value);
    return S_OK;
}
static HRESULT WINAPI lib_get_SaveFolder(void *iface, IUnknown **value)
{
    storage_library *impl = (storage_library *)iface;
    if (!value) return E_POINTER;
    *value = impl->folder;
    if (*value) (*value)->AddRef();
    trace("get_SaveFolder -> %p", *value);
    return S_OK;
}
/* Adding or removing a library folder is a shell UI flow on Windows; there is nothing behind it
 * here. Report it as unimplemented rather than returning a null async operation, which the caller
 * would dereference. */
static HRESULT WINAPI lib_RequestAddFolderAsync(void *, void **operation)
{
    trace("RequestAddFolderAsync: not implemented");
    if (operation) *operation = nullptr;
    return E_NOTIMPL;
}
static HRESULT WINAPI lib_RequestRemoveFolderAsync(void *, IUnknown *, void **operation)
{
    trace("RequestRemoveFolderAsync: not implemented");
    if (operation) *operation = nullptr;
    return E_NOTIMPL;
}
/* The definition cannot change, so this registration never fires - but it must succeed. */
static HRESULT WINAPI lib_add_DefinitionChanged(void *, void *, EventRegistrationToken *token)
{
    if (token) token->value = 1;
    return S_OK;
}
static HRESULT WINAPI lib_remove_DefinitionChanged(void *, EventRegistrationToken) { return S_OK; }
/* IStorageLibrary2::get_ChangeTracker hands out a StorageLibraryChangeTracker. Media Player's
 * library page does not use it, and inventing one would mean a second class whose methods we
 * cannot honour; E_NOTIMPL is an answer callers handle, a null pointer is not. */
static HRESULT WINAPI lib2_get_ChangeTracker(void *, IUnknown **value)
{
    trace("get_ChangeTracker: not implemented");
    if (value) *value = nullptr;
    return E_NOTIMPL;
}
static HRESULT WINAPI lib3_AreFolderSuggestionsAvailableAsync(void *, void **operation)
{
    trace("AreFolderSuggestionsAvailableAsync: not implemented");
    if (operation) *operation = nullptr;
    return E_NOTIMPL;
}

static HRESULT storage_library_create(INT32 id, storage_library **out)
{
    storage_library *impl;
    IUnknown *folder = nullptr;
    HRESULT hr;

    *out = nullptr;
    if (FAILED(hr = known_folder_for_library(id, &folder))) return hr;
    if (!(impl = (storage_library *)calloc(1, sizeof(*impl)))) { folder->Release(); return E_OUTOFMEMORY; }
    impl->vtbl = library_vtbl;
    impl->vtbl2 = library2_vtbl;
    impl->vtbl3 = library3_vtbl;
    impl->ref = 1;
    impl->id = id;
    impl->folder = folder;
    if (FAILED(hr = folder_vector_create(folder, &impl->folders)))
    {
        folder->Release();
        free(impl);
        return hr;
    }
    *out = impl;
    return S_OK;
}

/* ---------------------------------------------------------------- IAsyncOperation<StorageLibrary>
 *
 * The result is ready before the operation exists, so this is a completed operation: Status is
 * Completed from the start and put_Completed invokes the handler immediately, on the caller's
 * thread. That is what Wine's own async_info does for work that has already finished, and it keeps
 * the library off a worker thread that would need its own apartment.
 */
struct async_library
{
    const void **vtbl;          /* IAsyncOperation<StorageLibrary> */
    const void **vtbl_info;     /* IAsyncInfo */
    LONG ref;
    storage_library *result;
    HRESULT hr;
};
static const void *async_vtbl[9];
static const void *asyncinfo_vtbl[11];

static async_library *al_from_info(void *i) { return CONTAINING_RECORD(i, async_library, vtbl_info); }

static HRESULT WINAPI al_QueryInterface(void *iface, REFIID iid, void **out)
{
    async_library *impl = (async_library *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile) || IsEqualGUID(iid, IID_IAsyncOperation_Library))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IAsyncInfo)) *out = &impl->vtbl_info;
    else { *out = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI al_AddRef(void *iface) { return InterlockedIncrement(&((async_library *)iface)->ref); }
static ULONG WINAPI al_Release(void *iface)
{
    async_library *impl = (async_library *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->result) lib_Release(impl->result); free(impl); }
    return ref;
}
static HRESULT WINAPI al_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageLibrary>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
#define AL_FORWARD_UNKNOWN(tag, back)                                                              \
    static HRESULT WINAPI tag##_QueryInterface(void *i, REFIID iid, void **out)                    \
    { return al_QueryInterface(back(i), iid, out); }                                               \
    static ULONG WINAPI tag##_AddRef(void *i) { return al_AddRef(back(i)); }                        \
    static ULONG WINAPI tag##_Release(void *i) { return al_Release(back(i)); }
AL_FORWARD_UNKNOWN(ai, al_from_info)

/* AsyncOperationCompletedHandler<StorageLibrary>::Invoke(IAsyncOperation<T> *, AsyncStatus) is
 * slot 3, straight after IUnknown - a delegate has no IInspectable. */
static HRESULT WINAPI al_put_Completed(void *iface, IUnknown *handler)
{
    async_library *impl = (async_library *)iface;
    typedef HRESULT (WINAPI *invoke)(IUnknown *, void *, INT32);

    if (!handler) return S_OK;
    trace("put_Completed %p: invoking now (already complete)", handler);
    handler->AddRef();
    invoke fn = ((invoke *)(*(void ***)handler))[3];
    fn(handler, iface, SUCCEEDED(impl->hr) ? ASYNC_COMPLETED : ASYNC_ERROR);
    handler->Release();
    return S_OK;
}
static HRESULT WINAPI al_get_Completed(void *, IUnknown **handler)
{
    if (!handler) return E_POINTER;
    *handler = nullptr;
    return S_OK;
}
static HRESULT WINAPI al_GetResults(void *iface, void **results)
{
    async_library *impl = (async_library *)iface;
    if (!results) return E_POINTER;
    *results = nullptr;
    if (FAILED(impl->hr)) return impl->hr;
    lib_AddRef(impl->result);
    *results = &impl->result->vtbl;
    trace("GetResults -> %p", *results);
    return S_OK;
}

/* IAsyncInfo */
static HRESULT WINAPI ai_get_Id(void *, UINT32 *id) { if (id) *id = 1; return S_OK; }
static HRESULT WINAPI ai_get_Status(void *iface, INT32 *status)
{
    async_library *impl = al_from_info(iface);
    if (!status) return E_POINTER;
    *status = SUCCEEDED(impl->hr) ? ASYNC_COMPLETED : ASYNC_ERROR;
    return S_OK;
}
static HRESULT WINAPI ai_get_ErrorCode(void *iface, HRESULT *code)
{
    async_library *impl = al_from_info(iface);
    if (!code) return E_POINTER;
    *code = impl->hr;
    return S_OK;
}
static HRESULT WINAPI ai_Cancel(void *) { return S_OK; }   /* already finished */
static HRESULT WINAPI ai_Close(void *) { return S_OK; }

static HRESULT async_library_create(INT32 id, void **out)
{
    async_library *impl;
    storage_library *library = nullptr;
    HRESULT hr = storage_library_create(id, &library);

    if (!out) return E_POINTER;
    *out = nullptr;
    /* A failure has to be reported through the operation, not returned: the caller is awaiting it
     * and would dereference a null operation. */
    if (!(impl = (async_library *)calloc(1, sizeof(*impl))))
    {
        if (library) lib_Release(library);
        return E_OUTOFMEMORY;
    }
    impl->vtbl = async_vtbl;
    impl->vtbl_info = asyncinfo_vtbl;
    impl->ref = 1;
    impl->result = library;
    impl->hr = hr;
    *out = impl;
    return S_OK;
}

/* ---------------------------------------------------------------- statics and factory */

struct statics_object { const void **vtbl; };
static const void *statics_vtbl[10];
static const void *statics2_vtbl[7];
static const void *factory_vtbl[7];
static statics_object statics, statics2, factory;

static HRESULT WINAPI statics_QueryInterface(void *, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile) || IsEqualGUID(iid, IID_IStorageLibraryStatics))
        *out = &statics.vtbl;
    else if (IsEqualGUID(iid, IID_IStorageLibraryStatics2)) *out = &statics2.vtbl;
    else if (IsEqualGUID(iid, IID_IActivationFactory))      *out = &factory.vtbl;
    else { *out = nullptr; return E_NOINTERFACE; }
    return S_OK;
}
static ULONG WINAPI statics_AddRef(void *) { return 2; }
static ULONG WINAPI statics_Release(void *) { return 1; }
static HRESULT WINAPI statics_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Storage.StorageLibrary";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI statics_GetLibraryAsync(void *, INT32 id, void **operation)
{
    trace("GetLibraryAsync(%d)", id);
    return async_library_create(id, operation);
}
static HRESULT WINAPI statics2_GetLibraryForUserAsync(void *, IUnknown *, INT32 id, void **operation)
{
    /* One user per prefix, so the user argument cannot select anything else. */
    trace("GetLibraryForUserAsync(%d)", id);
    return async_library_create(id, operation);
}
static HRESULT WINAPI factory_ActivateInstance(void *, IUnknown **instance)
{
    /* No default constructor on Windows either - a library comes from GetLibraryAsync. */
    if (instance) *instance = nullptr;
    return E_NOTIMPL;
}

static void init_vtbls(void)
{
    iterator_vtbl[0] = (void *)it_QueryInterface;
    iterator_vtbl[1] = (void *)it_AddRef;
    iterator_vtbl[2] = (void *)it_Release;
    iterator_vtbl[3] = (void *)insp_GetIids;
    iterator_vtbl[4] = (void *)it_GetRuntimeClassName;
    iterator_vtbl[5] = (void *)insp_GetTrustLevel;
    iterator_vtbl[6] = (void *)it_get_Current;
    iterator_vtbl[7] = (void *)it_get_HasCurrent;
    iterator_vtbl[8] = (void *)it_MoveNext;
    iterator_vtbl[9] = (void *)it_GetMany;

    obsvec_vtbl[0] = (void *)fv_QueryInterface;
    obsvec_vtbl[1] = (void *)fv_AddRef;
    obsvec_vtbl[2] = (void *)fv_Release;
    obsvec_vtbl[3] = (void *)insp_GetIids;
    obsvec_vtbl[4] = (void *)fv_GetRuntimeClassName;
    obsvec_vtbl[5] = (void *)insp_GetTrustLevel;
    obsvec_vtbl[6] = (void *)fv_add_VectorChanged;
    obsvec_vtbl[7] = (void *)fv_remove_VectorChanged;

    vector_vtbl[0]  = (void *)vec_QueryInterface;
    vector_vtbl[1]  = (void *)vec_AddRef;
    vector_vtbl[2]  = (void *)vec_Release;
    vector_vtbl[3]  = (void *)insp_GetIids;
    vector_vtbl[4]  = (void *)fv_GetRuntimeClassName;
    vector_vtbl[5]  = (void *)insp_GetTrustLevel;
    vector_vtbl[6]  = (void *)vec_GetAt;
    vector_vtbl[7]  = (void *)vec_get_Size;
    vector_vtbl[8]  = (void *)vec_GetView;
    vector_vtbl[9]  = (void *)vec_IndexOf;
    vector_vtbl[10] = (void *)vec_readonly;   /* SetAt */
    vector_vtbl[11] = (void *)vec_readonly;   /* InsertAt */
    vector_vtbl[12] = (void *)vec_readonly;   /* RemoveAt */
    vector_vtbl[13] = (void *)vec_readonly;   /* Append */
    vector_vtbl[14] = (void *)vec_readonly;   /* RemoveAtEnd */
    vector_vtbl[15] = (void *)vec_readonly;   /* Clear */
    vector_vtbl[16] = (void *)vec_GetMany;
    vector_vtbl[17] = (void *)vec_readonly;   /* ReplaceAll */

    view_vtbl[0] = (void *)view_QueryInterface;
    view_vtbl[1] = (void *)view_AddRef;
    view_vtbl[2] = (void *)view_Release;
    view_vtbl[3] = (void *)insp_GetIids;
    view_vtbl[4] = (void *)fv_GetRuntimeClassName;
    view_vtbl[5] = (void *)insp_GetTrustLevel;
    view_vtbl[6] = (void *)view_GetAt;
    view_vtbl[7] = (void *)view_get_Size;
    view_vtbl[8] = (void *)view_IndexOf;
    view_vtbl[9] = (void *)view_GetMany;

    iterable_vtbl[0] = (void *)iter_QueryInterface;
    iterable_vtbl[1] = (void *)iter_AddRef;
    iterable_vtbl[2] = (void *)iter_Release;
    iterable_vtbl[3] = (void *)insp_GetIids;
    iterable_vtbl[4] = (void *)fv_GetRuntimeClassName;
    iterable_vtbl[5] = (void *)insp_GetTrustLevel;
    iterable_vtbl[6] = (void *)iter_First;

    library_vtbl[0]  = (void *)lib_QueryInterface;
    library_vtbl[1]  = (void *)lib_AddRef;
    library_vtbl[2]  = (void *)lib_Release;
    library_vtbl[3]  = (void *)insp_GetIids;
    library_vtbl[4]  = (void *)lib_GetRuntimeClassName;
    library_vtbl[5]  = (void *)insp_GetTrustLevel;
    library_vtbl[6]  = (void *)lib_RequestAddFolderAsync;
    library_vtbl[7]  = (void *)lib_RequestRemoveFolderAsync;
    library_vtbl[8]  = (void *)lib_get_Folders;
    library_vtbl[9]  = (void *)lib_get_SaveFolder;
    library_vtbl[10] = (void *)lib_add_DefinitionChanged;
    library_vtbl[11] = (void *)lib_remove_DefinitionChanged;

    library2_vtbl[0] = (void *)lib2_QueryInterface;
    library2_vtbl[1] = (void *)lib2_AddRef;
    library2_vtbl[2] = (void *)lib2_Release;
    library2_vtbl[3] = (void *)insp_GetIids;
    library2_vtbl[4] = (void *)lib_GetRuntimeClassName;
    library2_vtbl[5] = (void *)insp_GetTrustLevel;
    library2_vtbl[6] = (void *)lib2_get_ChangeTracker;

    library3_vtbl[0] = (void *)lib3_QueryInterface;
    library3_vtbl[1] = (void *)lib3_AddRef;
    library3_vtbl[2] = (void *)lib3_Release;
    library3_vtbl[3] = (void *)insp_GetIids;
    library3_vtbl[4] = (void *)lib_GetRuntimeClassName;
    library3_vtbl[5] = (void *)insp_GetTrustLevel;
    library3_vtbl[6] = (void *)lib3_AreFolderSuggestionsAvailableAsync;

    async_vtbl[0] = (void *)al_QueryInterface;
    async_vtbl[1] = (void *)al_AddRef;
    async_vtbl[2] = (void *)al_Release;
    async_vtbl[3] = (void *)insp_GetIids;
    async_vtbl[4] = (void *)al_GetRuntimeClassName;
    async_vtbl[5] = (void *)insp_GetTrustLevel;
    async_vtbl[6] = (void *)al_put_Completed;
    async_vtbl[7] = (void *)al_get_Completed;
    async_vtbl[8] = (void *)al_GetResults;

    asyncinfo_vtbl[0] = (void *)ai_QueryInterface;
    asyncinfo_vtbl[1] = (void *)ai_AddRef;
    asyncinfo_vtbl[2] = (void *)ai_Release;
    asyncinfo_vtbl[3] = (void *)insp_GetIids;
    asyncinfo_vtbl[4] = (void *)al_GetRuntimeClassName;
    asyncinfo_vtbl[5] = (void *)insp_GetTrustLevel;
    asyncinfo_vtbl[6] = (void *)ai_get_Id;
    asyncinfo_vtbl[7] = (void *)ai_get_Status;
    asyncinfo_vtbl[8] = (void *)ai_get_ErrorCode;
    asyncinfo_vtbl[9] = (void *)ai_Cancel;
    asyncinfo_vtbl[10] = (void *)ai_Close;

    statics_vtbl[0] = (void *)statics_QueryInterface;
    statics_vtbl[1] = (void *)statics_AddRef;
    statics_vtbl[2] = (void *)statics_Release;
    statics_vtbl[3] = (void *)insp_GetIids;
    statics_vtbl[4] = (void *)statics_GetRuntimeClassName;
    statics_vtbl[5] = (void *)insp_GetTrustLevel;
    statics_vtbl[6] = (void *)statics_GetLibraryAsync;

    statics2_vtbl[0] = (void *)statics_QueryInterface;
    statics2_vtbl[1] = (void *)statics_AddRef;
    statics2_vtbl[2] = (void *)statics_Release;
    statics2_vtbl[3] = (void *)insp_GetIids;
    statics2_vtbl[4] = (void *)statics_GetRuntimeClassName;
    statics2_vtbl[5] = (void *)insp_GetTrustLevel;
    statics2_vtbl[6] = (void *)statics2_GetLibraryForUserAsync;

    factory_vtbl[0] = (void *)statics_QueryInterface;
    factory_vtbl[1] = (void *)statics_AddRef;
    factory_vtbl[2] = (void *)statics_Release;
    factory_vtbl[3] = (void *)insp_GetIids;
    factory_vtbl[4] = (void *)statics_GetRuntimeClassName;
    factory_vtbl[5] = (void *)insp_GetTrustLevel;
    factory_vtbl[6] = (void *)factory_ActivateInstance;

    statics.vtbl = statics_vtbl;
    statics2.vtbl = statics2_vtbl;
    factory.vtbl = factory_vtbl;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *str = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!str || lstrcmpW(str, L"Windows.Storage.StorageLibrary"))
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

// Microsoft.GamePlatform.Services.*: a stand-in for the Gaming Services client classes.
//
// The Xbox app ships Microsoft.GamePlatform.Services.dll, a client of the Gaming Services NT
// services (GamingServices.exe, LocalSystem, with the xvdd.sys and gameflt.sys kernel drivers).
// Those cannot run under Wine, so the client reports GamePlatformApiStatus "Missing" and the app
// tries to install Gaming Services and never leaves its splash.
//
// This stand-in answers for the client classes instead. It is registered with OverridesPackage=1,
// which makes combase prefer it to the package's own DLL. What it says is honest for a machine
// with a game platform that has no games and does nothing:
//   PlatformStatus.ApiStatus = Ready, its change event registers and never fires;
//   every event registration succeeds; every other call is traced by name and answered as the package's own
//   client answers without the service (0x87E00196), unless it is in `specials` below.
//
// Every class with an I<Class>Statics interface is covered: the class and slot table (slots.h) is generated from
// Microsoft.GamePlatform.Services.winmd by gen-gameplatform.py, and install-overrides.sh registers them all.
#define NOMINMAX
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>
#include <algorithm>
#include <winhttp.h>
#include "../common/json.h"

static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};

/* what the package's own client answers when the Gaming Services service is not there */
#define GP_E_NO_SERVICE ((HRESULT)0x87E00196)

static void tracef(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(stderr, "[gameplatform] %s\n", buf);
    fflush(stderr);
}

#include "../common/storeservices.h"

struct gp_slot { int slot; const char *name; int kind; /* 0 a method, 1 add_ event, 2 remove_ event */ };
struct gp_class { const wchar_t *name; GUID statics; unsigned count; const gp_slot *slots; };
#include "slots.h"
enum { K_COUNT = ARRAYSIZE(gp_classes), SLOTS = 40 };

struct Factory
{
    const void **vtbl;
    LONG ref;
    int kind;
    LONGLONG next_token;
    bool heap;          /* an object handed out (a "...Instance" entry), freed on its last Release */
    wchar_t source[64]; /* for a Package: the source it was created from */
};

static const void *factory_vtbl[K_COUNT][SLOTS];
static Factory factories[K_COUNT];

static const gp_slot *slot_info(int kind, int n)
{
    for (unsigned i = 0; i < gp_classes[kind].count; i++)
        if (gp_classes[kind].slots[i].slot == n) return &gp_classes[kind].slots[i];
    return nullptr;
}
static const char *class_name(int kind)
{
    static char names[K_COUNT][96];
    if (!names[kind][0])
        WideCharToMultiByte(CP_UTF8, 0, gp_classes[kind].name + 32 /* after "Microsoft.GamePlatform.Services." */, -1,
                            names[kind], sizeof(names[kind]), nullptr, nullptr);
    return names[kind];
}
static int kind_of(const wchar_t *name)
{
    for (int k = 0; k < K_COUNT; k++) if (!wcscmp(gp_classes[k].name, name)) return k;
    return -1;
}

static void log_guid(const char *what, int kind, REFIID g)
{
    tracef("%s: %s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", class_name(kind), what,
           (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
           g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/* IWeakReferenceSource {00000038} / IWeakReference {00000037}: C++/WinRT keeps weak references to objects it gets
 * (the app's GameCatalog holds a weak_ref to every IPackage, and dereferences the QI without checking). The reference
 * here holds the object: these objects are small and few. */
static const GUID IID_IWeakReferenceSource_ = {0x00000038,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IWeakReference_ = {0x00000037,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
struct WeakRef : IUnknown
{
    LONG ref = 1;
    IUnknown *target;
    WeakRef(IUnknown *t) : target(t) { target->AddRef(); }
    ~WeakRef() { target->Release(); }   /* not virtual: a virtual destructor would take vtable slot 3 */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IWeakReference_)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    virtual HRESULT STDMETHODCALLTYPE Resolve(REFIID iid, IInspectable **out) { return target->QueryInterface(iid, (void **)out); }
};
struct WeakSource : IUnknown
{
    LONG ref = 1;
    IUnknown *target;
    WeakSource(IUnknown *t) : target(t) { target->AddRef(); }
    ~WeakSource() { target->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) { *out = this; AddRef(); return S_OK; }
        return target->QueryInterface(iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    virtual HRESULT STDMETHODCALLTYPE GetWeakReference(IUnknown **out) { if (!out) return E_POINTER; *out = new WeakRef(target); return S_OK; }
};

static HRESULT WINAPI f_QueryInterface(Factory *f, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IWeakReferenceSource_))
    {
        *out = new WeakSource((IUnknown *)f);
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IActivationFactory) || IsEqualGUID(iid, gp_classes[f->kind].statics))
    {
        InterlockedIncrement(&f->ref);
        *out = f;
        return S_OK;
    }
    log_guid("QueryInterface refused", f->kind, iid);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI f_AddRef(Factory *f) { return InterlockedIncrement(&f->ref); }
static ULONG WINAPI f_Release(Factory *f)
{
    ULONG r = InterlockedDecrement(&f->ref);
    if (!r && f->heap) delete f;
    return r;
}
static HRESULT WINAPI f_GetIids(Factory *, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI f_GetRuntimeClassName(Factory *f, HSTRING *n)
{
    UINT32 len = (UINT32)wcslen(gp_classes[f->kind].name);
    if (f->heap) len -= 8; /* "Instance" */
    return WindowsCreateString(gp_classes[f->kind].name, len, n);
}
static HRESULT WINAPI f_GetTrustLevel(Factory *, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* The generic slot: an event registration succeeds (and never fires), anything else is traced and answered as
 * the package's own client answers without the service. */
/* "module+offset" of a caller, for the trace */
static void caller_name(void *addr, char *buf, size_t size)
{
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?", *base;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const char *)addr, &mod))
    { snprintf(buf, size, "%p", addr); return; }
    GetModuleFileNameA(mod, path, sizeof(path));
    base = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    snprintf(buf, size, "%s+%#llx", base, (unsigned long long)((char *)addr - (char *)mod));
}

static HRESULT generic_slot(Factory *f, int n, void *a1, void *a2, void *ret)
{
    const gp_slot *s = slot_info(f->kind, n);
    if (s && s->kind == 1)
    {
        LONGLONG *token = (LONGLONG *)a2;
        if (!token) return E_POINTER;
        *token = InterlockedIncrement64(&f->next_token);
        tracef("%s.%s: registered (token %lld)", class_name(f->kind), s->name, *token);
        return S_OK;
    }
    if (s && s->kind == 2) return S_OK;
    char caller[160];
    caller_name(ret, caller, sizeof(caller));
    tracef("%s.%s (slot %d, from %s) -> not implemented here (0x87E00196)", class_name(f->kind), s ? s->name : "?", n, caller);
    return GP_E_NO_SERVICE;
}
#define THUNK(n) \
    static HRESULT WINAPI t_##n(Factory *f, void *a1, void *a2, void *, void *, void *) { return generic_slot(f, n, a1, a2, __builtin_return_address(0)); }
THUNK(6) THUNK(7) THUNK(8) THUNK(9) THUNK(10) THUNK(11) THUNK(12) THUNK(13) THUNK(14) THUNK(15) THUNK(16) THUNK(17)
THUNK(18) THUNK(19) THUNK(20) THUNK(21) THUNK(22) THUNK(23) THUNK(24) THUNK(25) THUNK(26) THUNK(27) THUNK(28) THUNK(29)
THUNK(30) THUNK(31) THUNK(32) THUNK(33) THUNK(34) THUNK(35) THUNK(36) THUNK(37) THUNK(38) THUNK(39)
static const void *thunks[SLOTS] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    (void *)t_6, (void *)t_7, (void *)t_8, (void *)t_9, (void *)t_10, (void *)t_11, (void *)t_12, (void *)t_13,
    (void *)t_14, (void *)t_15, (void *)t_16, (void *)t_17, (void *)t_18, (void *)t_19, (void *)t_20, (void *)t_21,
    (void *)t_22, (void *)t_23, (void *)t_24, (void *)t_25, (void *)t_26, (void *)t_27, (void *)t_28, (void *)t_29,
    (void *)t_30, (void *)t_31, (void *)t_32, (void *)t_33, (void *)t_34, (void *)t_35, (void *)t_36, (void *)t_37,
    (void *)t_38, (void *)t_39 };

/* ------------------------------------------------------------------------------------------- */
/* The slots answered for real                                                                   */

/* PlatformStatus.ApiStatus: the platform is ready */
static HRESULT WINAPI ps_get_ApiStatus(Factory *f, INT32 *value)
{
    tracef("PlatformStatus.ApiStatus -> Ready");
    if (!value) return E_POINTER;
    *value = 0; /* GamePlatformApiStatus.Ready */
    return S_OK;
}

/* Settings, kept per prefix in HKCU\Software\Wine\WineUWP\GamePlatform */
static const wchar_t SETTINGS_KEY[] = L"Software\\Wine\\WineUWP\\GamePlatform";
static DWORD setting_dword(const wchar_t *name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &size)) v = def;
    return v;
}
static void set_setting_dword(const wchar_t *name, DWORD v)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_DWORD, &v, sizeof(v));
}
/* The game folder of the one storage device: C:\XboxGames unless the app moved it */
static std::wstring game_folder(void)
{
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"GameFolder", RRF_RT_REG_SZ, nullptr, buf, &size)) return buf;
    return L"C:\\XboxGames";
}

static HRESULT hstr(const std::wstring &s, HSTRING *out)
{
    if (!out) return E_POINTER;
    return WindowsCreateString(s.c_str(), (UINT32)s.size(), out);
}

#define IINSPECTABLE_BOILERPLATE(cls_name)                                                                         \
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }                                \
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; } \
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; } \
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(cls_name, n); }               \
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static bool base_iid(REFIID iid)
{
    return IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_);
}

/* Stable stand-in volume IDs for the prefix and the host game-data drive. */
static const GUID DEVICE_ID = {0x7e1d3a52,0x5c1b,0x4f3e,{0x9a,0x52,0x77,0x69,0x6e,0x65,0x75,0x77}};
/* Installed payloads live in the host data directory, addressed through Wine's Z: drive. */
static const GUID DATA_DEVICE_ID = {0x7e1d3a53,0x5c1b,0x4f3e,{0x9a,0x52,0x77,0x69,0x6e,0x65,0x75,0x77}};
static std::wstring games_data_unix(void);
static std::wstring unix_to_windows(const std::wstring &);
static std::wstring data_game_folder(void) { return unix_to_windows(games_data_unix()) + L"\\stage"; }

/* IVectorView<Guid> {9520e64b} with IIterable<Guid> {f4ca3045}; the iterator is IIterator<Guid> {d3d64048} */
static const GUID IID_IVectorView_Guid = {0x9520e64b,0x15b2,0x52a6,{0x98,0xed,0x31,0x91,0xfa,0x6c,0xf6,0x8a}};
static const GUID IID_IIterable_Guid = {0xf4ca3045,0x5dd7,0x54be,{0x98,0x2e,0xd8,0x8d,0x8c,0xa0,0x87,0x6e}};
static const GUID IID_IIterator_Guid = {0xd3d64048,0x82b3,0x53c7,{0x92,0x85,0xb0,0xbe,0x18,0x36,0x84,0x82}};
struct IGuidViewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(const GUID *, UINT32 *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, GUID *, UINT32 *) = 0;
};
struct IGuidIterableAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE First(IInspectable **) = 0; };
struct IGuidIteratorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Current(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, GUID *, UINT32 *) = 0;
};
struct GuidView : IGuidViewAbi, IGuidIterableAbi
{
    LONG ref = 1;
    std::vector<GUID> items;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IVectorView_Guid)) *out = static_cast<IGuidViewAbi *>(this);
        else if (IsEqualGUID(iid, IID_IIterable_Guid)) *out = static_cast<IGuidIterableAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.Collections.IVectorView`1<Guid>")
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, GUID *v) override
    {
        if (!v) return E_POINTER;
        if (i >= items.size()) return E_BOUNDS;
        *v = items[i];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *v) override { if (!v) return E_POINTER; *v = (UINT32)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(const GUID *g, UINT32 *idx, boolean *found) override
    {
        if (!g || !idx || !found) return E_POINTER;
        *found = FALSE; *idx = 0;
        for (UINT32 i = 0; i < items.size(); i++) if (IsEqualGUID(items[i], *g)) { *idx = i; *found = TRUE; break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 n, GUID *out, UINT32 *got) override
    {
        if (!got || (n && !out)) return E_POINTER;
        *got = 0;
        for (UINT32 i = start; i < items.size() && *got < n; i++) out[(*got)++] = items[i];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE First(IInspectable **v) override;
};
struct GuidIterator : IGuidIteratorAbi
{
    LONG ref = 1;
    GuidView *view;
    UINT32 pos = 0;
    GuidIterator(GuidView *v) : view(v) { view->AddRef(); }
    ~GuidIterator() { view->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IIterator_Guid)) *out = static_cast<IGuidIteratorAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.Collections.IIterator`1<Guid>")
    HRESULT STDMETHODCALLTYPE get_Current(GUID *v) override { return view->GetAt(pos, v); }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override { if (!v) return E_POINTER; *v = pos < view->items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override { if (pos < view->items.size()) pos++; return get_HasCurrent(v); }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 n, GUID *out, UINT32 *got) override
    {
        HRESULT hr = view->GetMany(pos, n, out, got);
        if (SUCCEEDED(hr)) pos += *got;
        return hr;
    }
};
HRESULT STDMETHODCALLTYPE GuidView::First(IInspectable **v)
{
    if (!v) return E_POINTER;
    *v = new GuidIterator(this);
    return S_OK;
}

/* An empty IVectorView<T> of interfaces (RunningGame), with its IIterable<T> */
struct IEmptyViewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *, UINT32 *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IInspectable **, UINT32 *) = 0;
};
struct IEmptyIteratorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Current(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, IInspectable **, UINT32 *) = 0;
};
struct EmptyView : IEmptyViewAbi, IGuidIterableAbi, IEmptyIteratorAbi
{
    LONG ref = 1;
    GUID view_iid, iterable_iid;
    const wchar_t *name;
    EmptyView(REFIID v, REFIID it, const wchar_t *n) : view_iid(v), iterable_iid(it), name(n) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, view_iid)) *out = static_cast<IEmptyViewAbi *>(this);
        else if (IsEqualGUID(iid, iterable_iid)) *out = static_cast<IGuidIterableAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(name)
    HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable **v) override { if (v) *v = nullptr; return E_BOUNDS; }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *, UINT32 *idx, boolean *found) override
    { if (!idx || !found) return E_POINTER; *idx = 0; *found = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IInspectable **, UINT32 *got) override { if (!got) return E_POINTER; *got = 0; return S_OK; }
    /* the iterator over nothing is this object again (it has no state) */
    HRESULT STDMETHODCALLTYPE First(IInspectable **v) override { if (!v) return E_POINTER; AddRef(); *v = static_cast<IEmptyIteratorAbi *>(this); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Current(IInspectable **v) override { if (v) *v = nullptr; return E_BOUNDS; }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override { return get_HasCurrent(v); }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32, IInspectable **, UINT32 *got) override { if (!got) return E_POINTER; *got = 0; return S_OK; }
};

/* StorageDeviceDetails {fd57cdc7} */
static const GUID IID_IStorageDeviceDetails = {0xfd57cdc7,0xe911,0x5a3a,{0x9b,0x3f,0xb3,0xb5,0xd3,0x64,0xcd,0x3b}};
struct IStorageDeviceDetailsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_CrdId(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FreeSpace(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DriveLetter(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_GameFolder(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BusType(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BusDetails(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AssociationType(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Flags(INT32 *) = 0;
};
struct StorageDeviceDetails : IStorageDeviceDetailsAbi
{
    LONG ref = 1;
    bool data_device;
    ULARGE_INTEGER total = {}, free_ = {};
    StorageDeviceDetails(bool data = false) : data_device(data)
    {
        /* a fresh data directory has no stage/ until the first install: measure the nearest folder that exists */
        std::wstring path = data ? data_game_folder() : L"C:\\";
        while (!GetDiskFreeSpaceExW(path.c_str(), &free_, &total, nullptr))
        {
            size_t slash = path.find_last_of(L'\\');
            if (slash == std::wstring::npos || slash < 3) break;
            path.resize(slash);
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IStorageDeviceDetails)) *out = static_cast<IStorageDeviceDetailsAbi *>(this);
        else { log_guid("StorageDeviceDetails: QueryInterface refused", 0, iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.StorageDeviceDetails")
    HRESULT STDMETHODCALLTYPE get_CrdId(UINT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *v) override { if (!v) return E_POINTER; *v = total.QuadPart; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FreeSpace(UINT64 *v) override { if (!v) return E_POINTER; *v = free_.QuadPart; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(INT32 *v) override { if (!v) return E_POINTER; *v = 1; /* Ready */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DriveLetter(HSTRING *v) override { tracef("StorageDeviceDetails.DriveLetter -> %s", data_device ? "Z:" : "C:"); return hstr(data_device ? L"Z:" : L"C:", v); }
    HRESULT STDMETHODCALLTYPE get_GameFolder(HSTRING *v) override { return hstr(data_device ? data_game_folder() : game_folder(), v); }
    HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *v) override { return hstr(data_device ? L"Game data" : L"Local Disk", v); }
    HRESULT STDMETHODCALLTYPE get_BusType(INT32 *v) override { if (!v) return E_POINTER; *v = 0; /* Unknown */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BusDetails(INT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AssociationType(INT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Flags(INT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
};

static bool is_package_instance(const GUID *id);
static HRESULT WINAPI st_QueryDeviceDetails(Factory *, const GUID *id, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    /* Older queue records use the package instance as their device ID. Those payloads are in the
     * host data directory too; report the same drive that QueryInstallPath and enumeration expose. */
    if (id && !IsEqualGUID(*id, DEVICE_ID) && is_package_instance(id))
    {
        tracef("Storage.QueryDeviceDetails(a package instance) -> Z:");
        *out = new StorageDeviceDetails(true);
        return S_OK;
    }
    if (id && IsEqualGUID(*id, DATA_DEVICE_ID))
    {
        tracef("Storage.QueryDeviceDetails -> Z:");
        *out = new StorageDeviceDetails(true);
        return S_OK;
    }
    if (!id || !IsEqualGUID(*id, DEVICE_ID))
    {
        if (id)
            tracef("Storage.QueryDeviceDetails(unknown device {%08lx-%04x-%04x-...}, from %p)", id->Data1, id->Data2, id->Data3,
                   __builtin_return_address(0));
        else tracef("Storage.QueryDeviceDetails(NULL)");
        return E_INVALIDARG;
    }
    tracef("Storage.QueryDeviceDetails -> C:");
    *out = new StorageDeviceDetails();
    return S_OK;
}
static HRESULT WINAPI st_EnumerateDevices(Factory *, IInspectable **out)
{
    if (!out) return E_POINTER;
    GuidView *v = new GuidView();
    v->items.push_back(DEVICE_ID);
    v->items.push_back(DATA_DEVICE_ID);
    tracef("Storage.EnumerateDevices -> C:, Z:");
    *out = static_cast<IGuidViewAbi *>(v);
    return S_OK;
}
static HRESULT WINAPI st_EnumerateContent(Factory *, const GUID *, IInspectable **out);
static HRESULT WINAPI st_QueryDefaultDevice(Factory *, INT32 category, boolean validate, GUID *out)
{
    if (!out) return E_POINTER;
    tracef("Storage.QueryDefaultDevice(category %d) -> C:", category);
    *out = DEVICE_ID;
    return S_OK;
}
static HRESULT WINAPI st_SetDefaultDevice(Factory *, INT32 category, const GUID *)
{
    tracef("Storage.SetDefaultDevice(category %d): there is one device", category);
    return S_OK;
}
static HRESULT WINAPI gf_QueryForDevice(Factory *, const GUID *id, HSTRING *out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!id || (!IsEqualGUID(*id, DEVICE_ID) && !IsEqualGUID(*id, DATA_DEVICE_ID))) return E_INVALIDARG;
    return hstr(IsEqualGUID(*id, DATA_DEVICE_ID) ? data_game_folder() : game_folder(), out);
}
static HRESULT WINAPI gf_SetForDevice(Factory *, const GUID *id, HSTRING path)
{
    UINT32 len = 0;
    const wchar_t *p = WindowsGetStringRawBuffer(path, &len);
    if (!id || !len) return E_INVALIDARG;
    if (IsEqualGUID(*id, DATA_DEVICE_ID))
    {
        /* WINE_UWP_GAMES_DATA owns this location. Do not accept a move we cannot perform. */
        return !_wcsicmp(p, data_game_folder().c_str()) ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    if (!IsEqualGUID(*id, DEVICE_ID)) return E_INVALIDARG;
    tracef("GameFolder.SetForDevice(%ls)", p);
    return HRESULT_FROM_WIN32(RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"GameFolder", REG_SZ, p, (len + 1) * sizeof(wchar_t)));
}

/* CustomizationGlobalProps {c2f4f8e3}: the update settings, a property bag read and written as a whole */
static const GUID IID_ICustomizationGlobalProps = {0xc2f4f8e3,0xa535,0x53ef,{0x81,0x71,0xec,0xab,0x8c,0x86,0x2c,0x56}};
struct ICustomizationGlobalPropsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_AutomaticUpdates(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutomaticUpdates(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UpdatesDuringGameplay(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UpdatesDuringGameplay(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UninstallStaleGPGames(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UninstallStaleGPGames(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PowerOffAfterUpdates(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PowerOffAfterUpdates(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NetworkThrottling(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_NetworkThrottling(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxNetworkBandwidthMbps(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MaxNetworkBandwidthMbps(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UpdateTimeWindow(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UpdateTimeWindow(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UpdateWindowStartTimeMins(UINT16 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UpdateWindowStartTimeMins(UINT16) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UpdateWindowEndTimeMins(UINT16 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UpdateWindowEndTimeMins(UINT16) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ResolutionCap(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ResolutionCap(INT32) = 0;
};
struct GlobalProps : ICustomizationGlobalPropsAbi
{
    LONG ref = 1;
    /* Windows' defaults: automatic updates on, updates during gameplay off */
    DWORD automatic = setting_dword(L"AutomaticUpdates", 1), during = setting_dword(L"UpdatesDuringGameplay", 0),
          stale = setting_dword(L"UninstallStaleGPGames", 0), poweroff = setting_dword(L"PowerOffAfterUpdates", 0),
          throttling = setting_dword(L"NetworkThrottling", 0), mbps = setting_dword(L"MaxNetworkBandwidthMbps", 0),
          window = setting_dword(L"UpdateTimeWindow", 0), start = setting_dword(L"UpdateWindowStartTimeMins", 0),
          end = setting_dword(L"UpdateWindowEndTimeMins", 0), cap = setting_dword(L"ResolutionCap", 0);
    void save()
    {
        set_setting_dword(L"AutomaticUpdates", automatic); set_setting_dword(L"UpdatesDuringGameplay", during);
        set_setting_dword(L"UninstallStaleGPGames", stale); set_setting_dword(L"PowerOffAfterUpdates", poweroff);
        set_setting_dword(L"NetworkThrottling", throttling); set_setting_dword(L"MaxNetworkBandwidthMbps", mbps);
        set_setting_dword(L"UpdateTimeWindow", window); set_setting_dword(L"UpdateWindowStartTimeMins", start);
        set_setting_dword(L"UpdateWindowEndTimeMins", end); set_setting_dword(L"ResolutionCap", cap);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_ICustomizationGlobalProps)) *out = static_cast<ICustomizationGlobalPropsAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.CustomizationGlobalProps")
#define BOOLPROP(name, field) \
    HRESULT STDMETHODCALLTYPE get_##name(boolean *v) override { if (!v) return E_POINTER; *v = !!field; return S_OK; } \
    HRESULT STDMETHODCALLTYPE put_##name(boolean v) override { field = !!v; return S_OK; }
#define NUMPROP(name, field, type) \
    HRESULT STDMETHODCALLTYPE get_##name(type *v) override { if (!v) return E_POINTER; *v = (type)field; return S_OK; } \
    HRESULT STDMETHODCALLTYPE put_##name(type v) override { field = (DWORD)v; return S_OK; }
    BOOLPROP(AutomaticUpdates, automatic) BOOLPROP(UpdatesDuringGameplay, during) BOOLPROP(UninstallStaleGPGames, stale)
    BOOLPROP(PowerOffAfterUpdates, poweroff) BOOLPROP(NetworkThrottling, throttling) NUMPROP(MaxNetworkBandwidthMbps, mbps, UINT32)
    BOOLPROP(UpdateTimeWindow, window) NUMPROP(UpdateWindowStartTimeMins, start, UINT16) NUMPROP(UpdateWindowEndTimeMins, end, UINT16)
    NUMPROP(ResolutionCap, cap, INT32)
#undef BOOLPROP
#undef NUMPROP
};
static HRESULT WINAPI cu_QueryGlobalProps(Factory *, IInspectable **out)
{
    if (!out) return E_POINTER;
    GlobalProps *p = new GlobalProps();
    tracef("Customization.QueryGlobalProps (automatic updates %lu, during gameplay %lu)", p->automatic, p->during);
    *out = static_cast<ICustomizationGlobalPropsAbi *>(p);
    return S_OK;
}
static HRESULT WINAPI cu_SetGlobalProps(Factory *, IInspectable *props)
{
    ICustomizationGlobalPropsAbi *in = nullptr;
    if (!props || FAILED(props->QueryInterface(IID_ICustomizationGlobalProps, (void **)&in))) return E_INVALIDARG;
    /* the app may pass its own CustomizationGlobalProps (the package's class): copy it field by field */
    GlobalProps p;
    boolean b; UINT32 u; UINT16 w; INT32 i;
    if (SUCCEEDED(in->get_AutomaticUpdates(&b))) p.automatic = b;
    if (SUCCEEDED(in->get_UpdatesDuringGameplay(&b))) p.during = b;
    if (SUCCEEDED(in->get_UninstallStaleGPGames(&b))) p.stale = b;
    if (SUCCEEDED(in->get_PowerOffAfterUpdates(&b))) p.poweroff = b;
    if (SUCCEEDED(in->get_NetworkThrottling(&b))) p.throttling = b;
    if (SUCCEEDED(in->get_MaxNetworkBandwidthMbps(&u))) p.mbps = u;
    if (SUCCEEDED(in->get_UpdateTimeWindow(&b))) p.window = b;
    if (SUCCEEDED(in->get_UpdateWindowStartTimeMins(&w))) p.start = w;
    if (SUCCEEDED(in->get_UpdateWindowEndTimeMins(&w))) p.end = w;
    if (SUCCEEDED(in->get_ResolutionCap(&i))) p.cap = i;
    in->Release();
    p.save();
    tracef("Customization.SetGlobalProps (automatic updates %lu, during gameplay %lu)", p.automatic, p.during);
    return S_OK;
}

/* Manage reads the per-game settings independently of the global update policy. */
static const GUID IID_ICustomizationGameProps = {0xfee91193,0x2e09,0x5fb9,{0x8f,0x59,0x03,0x5d,0x0c,0x45,0xf3,0xe1}};
struct ICustomizationGamePropsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_UpdatePriority(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UpdatePriority(INT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CommandLineArgs(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_CommandLineArgs(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ResolutionCap(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ResolutionCap(INT32) = 0;
};
struct GameProps : ICustomizationGamePropsAbi
{
    LONG ref = 1;
    INT32 priority = 0, cap = 0; /* Standard priority, Default resolution cap */
    std::wstring args;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_ICustomizationGameProps)) *out = static_cast<ICustomizationGamePropsAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.CustomizationGameProps")
    HRESULT STDMETHODCALLTYPE get_UpdatePriority(INT32 *v) override { if (!v) return E_POINTER; *v = priority; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_UpdatePriority(INT32 v) override { if (v < 0 || v > 2) return E_INVALIDARG; priority = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_CommandLineArgs(HSTRING *v) override { return hstr(args, v); }
    HRESULT STDMETHODCALLTYPE put_CommandLineArgs(HSTRING v) override { UINT32 n; const wchar_t *p = WindowsGetStringRawBuffer(v, &n); args.assign(p, n); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ResolutionCap(INT32 *v) override { if (!v) return E_POINTER; *v = cap; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_ResolutionCap(INT32 v) override { if (v < 0 || v > 2) return E_INVALIDARG; cap = v; return S_OK; }
};
static HRESULT game_props_key(HSTRING family, std::wstring &key)
{
    UINT32 n;
    const wchar_t *p = WindowsGetStringRawBuffer(family, &n);
    if (!n || std::find(p, p + n, L'\\') != p + n || std::find(p, p + n, L'\0') != p + n) return E_INVALIDARG;
    key = std::wstring(SETTINGS_KEY) + L"\\Games\\" + std::wstring(p, n);
    return S_OK;
}
static HRESULT WINAPI cu_QueryGameProps(Factory *, HSTRING family, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    std::wstring key;
    HRESULT hr = game_props_key(family, key);
    if (FAILED(hr)) return hr;
    GameProps *p = new GameProps;
    DWORD size = sizeof(p->priority);
    RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"UpdatePriority", RRF_RT_REG_DWORD, nullptr, &p->priority, &size);
    size = sizeof(p->cap);
    RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"ResolutionCap", RRF_RT_REG_DWORD, nullptr, &p->cap, &size);
    if (p->priority < 0 || p->priority > 2) p->priority = 0;
    if (p->cap < 0 || p->cap > 2) p->cap = 0;
    size = 0;
    if (!RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"CommandLineArgs", RRF_RT_REG_SZ, nullptr, nullptr, &size) && size)
    {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
        if (!RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"CommandLineArgs", RRF_RT_REG_SZ, nullptr, buf.data(), &size)) p->args = buf.data();
    }
    tracef("Customization.QueryGameProps(%ls) -> priority %d", WindowsGetStringRawBuffer(family, nullptr), p->priority);
    *out = static_cast<ICustomizationGamePropsAbi *>(p);
    return S_OK;
}
static HRESULT WINAPI cu_SetGameProps(Factory *, HSTRING family, IInspectable *props)
{
    std::wstring key;
    HRESULT hr = game_props_key(family, key);
    if (FAILED(hr)) return hr;
    ICustomizationGamePropsAbi *in = nullptr;
    if (!props || FAILED(props->QueryInterface(IID_ICustomizationGameProps, (void **)&in))) return E_INVALIDARG;
    INT32 priority = 0, cap = 0;
    HSTRING args = nullptr;
    hr = in->get_UpdatePriority(&priority);
    if (SUCCEEDED(hr)) hr = in->get_ResolutionCap(&cap);
    if (SUCCEEDED(hr)) hr = in->get_CommandLineArgs(&args);
    in->Release();
    if (FAILED(hr)) { WindowsDeleteString(args); return hr; }
    if (priority < 0 || priority > 2 || cap < 0 || cap > 2) { WindowsDeleteString(args); return E_INVALIDARG; }
    LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"UpdatePriority", REG_DWORD, &priority, sizeof(priority));
    if (!status) status = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"ResolutionCap", REG_DWORD, &cap, sizeof(cap));
    UINT32 n;
    const wchar_t *p = WindowsGetStringRawBuffer(args, &n);
    if (!status) status = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"CommandLineArgs", REG_SZ, p, (n + 1) * sizeof(wchar_t));
    WindowsDeleteString(args);
    tracef("Customization.SetGameProps(%ls) -> priority %d", WindowsGetStringRawBuffer(family, nullptr), priority);
    return HRESULT_FROM_WIN32(status);
}

/* SupportedFeatures: none of the optional Gaming Services features (streaming, remote install, ...) */
static HRESULT WINAPI sf_QueryFeatures(Factory *, UINT32 *out)
{
    if (!out) return E_POINTER;
    *out = 0;
    tracef("SupportedFeatures.Query*Features -> None");
    return S_OK;
}
/* ProcessMonitor: no game is running */
static HRESULT WINAPI pm_QueryGamingServicesUsage(Factory *, UINT32 *out)
{
    if (!out) return E_POINTER;
    *out = 0;
    return S_OK;
}
static HRESULT WINAPI pm_EnumerateRunningGames(Factory *, IInspectable **out)
{
    static const GUID IID_IVectorView_RunningGame = {0xfe83e010,0x2ed8,0x5f52,{0x84,0x7a,0x4d,0x18,0xc5,0x33,0x70,0xac}};
    static const GUID IID_IIterable_RunningGame = {0x1be6bc9c,0x7b82,0x5ad7,{0x82,0xe3,0x2e,0x91,0x68,0xd7,0x32,0x69}};
    if (!out) return E_POINTER;
    *out = static_cast<IEmptyViewAbi *>(new EmptyView(IID_IVectorView_RunningGame, IID_IIterable_RunningGame,
        L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.RunningGame>"));
    return S_OK;
}

/* Package.Create(source, sourceType): no game is installed, so no source names a package here.
 * (experiment: WINE_UWP_GP_CREATE_HR picks the HRESULT) */
static HRESULT WINAPI pk_Create(Factory *, HSTRING source, INT32 type, IInspectable **out)
{
    char env[32] = "";
    int k = kind_of(L"Microsoft.GamePlatform.Services.PackageInstance");
    if (!out) return E_POINTER;
    *out = nullptr;
    if (GetEnvironmentVariableA("WINE_UWP_GP_CREATE_HR", env, sizeof(env)))
    {
        HRESULT hr = (HRESULT)strtoul(env, nullptr, 16);
        tracef("Package.Create(%ls, type %d) -> %#lx", WindowsGetStringRawBuffer(source, nullptr), type, (unsigned long)hr);
        return hr;
    }
    Factory *f = new Factory();
    f->vtbl = factory_vtbl[k];
    f->ref = 1;
    f->kind = k;
    f->heap = true;
    lstrcpynW(f->source, WindowsGetStringRawBuffer(source, nullptr), ARRAYSIZE(f->source));
    tracef("Package.Create(%ls, type %d) -> a package object (traced)", f->source, type);
    *out = (IInspectable *)f;
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
/* Store.QueryProductDetails: what Gaming Services knows of a Store product before it is installed.     */
/* Gaming Services reads it from the Store catalogue; so does this, from the public display catalogue       */
/* (displaycatalog.mp.microsoft.com, no account needed).                                                     */

struct ProductDetails { GUID content_id = {}; std::wstring family, store_id, wu_category; UINT64 size = 0; INT32 type = 2; bool uwp = true, encrypted = false; bool found = false; };

static std::wstring wide(const std::string &s)
{
    std::wstring w(s.size(), 0);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? n : 0);
    return w;
}

static ProductDetails query_product(const std::wstring &id)
{
    static std::map<std::wstring, ProductDetails> cache;
    static SRWLOCK lock = SRWLOCK_INIT;
    ProductDetails d;
    AcquireSRWLockShared(&lock);
    auto it = cache.find(id);
    if (it != cache.end()) d = it->second;
    ReleaseSRWLockShared(&lock);
    if (d.found) return d;

    char market[8] = "US";
    GEOID geo = GetUserGeoID(GEOCLASS_NATION);
    wchar_t iso[8];
    if (geo != GEOID_NOT_AVAILABLE && GetGeoInfoW(geo, GEO_ISO2, iso, ARRAYSIZE(iso), 0))
        WideCharToMultiByte(CP_UTF8, 0, iso, -1, market, sizeof(market), nullptr, nullptr);
    DWORD status = 0;
    std::string body = http_get(L"displaycatalog.mp.microsoft.com",
                                L"/v7.0/products?bigIds=" + id + L"&market=" + wide(market) + L"&languages=en-US", &status);
    Json j = parse_json(body);
    const Json &prod = j["Products"].at(0);
    if (status != 200 || prod.type != Json::OBJ)
    {
        tracef("Store.QueryProductDetails(%ls): catalogue HTTP %lu, no product", id.c_str(), status);
        return d;
    }
    d.store_id = id;
    d.family = wide(prod["Properties"]["PackageFamilyName"].str());
    std::string kind = prod["ProductKind"].str();
    d.type = kind == "Game" ? 0 : kind == "Durable" ? 1 : 2;
    if (prod["Properties"]["IsBundle"].b || kind == "Bundle") d.type = 3;
    /* the first package for the desktop, of the first SKU that has one */
    const Json &skus = prod["DisplaySkuAvailabilities"];
    for (size_t i = 0; i < skus.a.size() && !d.found; i++)
    {
        const Json &pkgs = skus.at(i)["Sku"]["Properties"]["Packages"];
        if (d.wu_category.empty()) d.wu_category = wide(skus.at(i)["Sku"]["Properties"]["FulfillmentData"]["WuCategoryId"].str());
        for (size_t k = 0; k < pkgs.a.size(); k++)
        {
            const Json &pk = pkgs.at(k);
            bool desktop = false;
            for (auto &dep : pk["PlatformDependencies"].a) desktop |= dep["PlatformName"].str() == "Windows.Desktop";
            if (!desktop && pkgs.a.size() > 1) continue;
            std::wstring cid = L"{" + wide(pk["ContentId"].str()) + L"}";
            CLSIDFromString(cid.c_str(), &d.content_id);
            if (d.family.empty()) d.family = wide(pk["PackageFamilyName"].str());
            d.size = (UINT64)pk["MaxInstallSizeInBytes"].n;
            if (!d.size) d.size = (UINT64)pk["MaxDownloadSizeInBytes"].n;
            std::string fmt = pk["PackageFormat"].str();
            d.uwp = fmt.find("Xvc") == std::string::npos && fmt.find("XVC") == std::string::npos;
            /* EAppx, EAppxBundle, EMsix, EMsixBundle: DRM-encrypted, the keys come with the licence */
            d.encrypted = d.uwp && (fmt.compare(0, 5, "EAppx") == 0 || fmt.compare(0, 5, "EMsix") == 0);
            d.found = true;
            break;
        }
    }
    if (!d.found) { d.found = true; tracef("Store.QueryProductDetails(%ls): no package for the desktop", id.c_str()); }
    tracef("Store.QueryProductDetails(%ls) -> %s, family %ls, %llu bytes, %s%s", id.c_str(), kind.c_str(), d.family.c_str(),
           (unsigned long long)d.size, d.uwp ? "UWP" : "not UWP", d.encrypted ? ", encrypted" : "");
    AcquireSRWLockExclusive(&lock);
    cache[id] = d;
    ReleaseSRWLockExclusive(&lock);
    return d;
}

static const GUID IID_IStoreProductDetails = {0x92cac4ca,0x4cef,0x5585,{0xac,0xbb,0x59,0x7c,0x52,0xc4,0xf3,0xc9}};
struct IStoreProductDetailsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_ContentId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FamilyName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StoreId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Type(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RequiresSystemDrive(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsUwp(boolean *) = 0;
};
struct StoreProductDetails : IStoreProductDetailsAbi
{
    LONG ref = 1;
    ProductDetails d;
    StoreProductDetails(const ProductDetails &p) : d(p) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IStoreProductDetails)) *out = static_cast<IStoreProductDetailsAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.StoreProductDetails")
    HRESULT STDMETHODCALLTYPE get_ContentId(GUID *v) override { if (!v) return E_POINTER; *v = d.content_id; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FamilyName(HSTRING *v) override { return hstr(d.family, v); }
    HRESULT STDMETHODCALLTYPE get_StoreId(HSTRING *v) override { return hstr(d.store_id, v); }
    HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *v) override { if (!v) return E_POINTER; *v = d.size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Type(INT32 *v) override { if (!v) return E_POINTER; *v = d.type; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_RequiresSystemDrive(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsUwp(boolean *v) override { if (!v) return E_POINTER; *v = d.uwp; return S_OK; }
};
static HRESULT WINAPI store_QueryProductDetails(Factory *, HSTRING id, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    ProductDetails d = query_product(WindowsGetStringRawBuffer(id, nullptr));
    if (d.store_id.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    *out = new StoreProductDetails(d);
    return S_OK;
}
static HRESULT WINAPI store_QueryProductType(Factory *, HSTRING id, INT32 *out)
{
    if (!out) return E_POINTER;
    ProductDetails d = query_product(WindowsGetStringRawBuffer(id, nullptr));
    if (d.store_id.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    *out = d.type;
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
/* An IAsyncOperation<interface> run on its own thread                                           */

static const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
struct IAsyncOpAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IUnknown **) = 0;
};
struct IAsyncInfoAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct AsyncOp : IAsyncOpAbi, IAsyncInfoAbi
{
    LONG ref = 1;
    GUID iid;
    SRWLOCK lock = SRWLOCK_INIT;
    INT32 status = 0;
    HRESULT error = S_OK;
    IUnknown *handler = nullptr, *result = nullptr;
    AsyncOp(REFIID i) : iid(i) {}
    ~AsyncOp() { if (handler) handler->Release(); if (result) result->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(riid) || IsEqualGUID(riid, iid)) *out = static_cast<IAsyncOpAbi *>(this);
        else if (IsEqualGUID(riid, IID_IAsyncInfo_)) *out = static_cast<IAsyncInfoAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.IAsyncOperation`1")
    void invoke(IUnknown *h, INT32 st)
    {
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IAsyncOpAbi *, INT32);
        ((invoke_fn)(*(void ***)h)[3])(h, static_cast<IAsyncOpAbi *>(this), st);
    }
    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *h) override
    {
        AcquireSRWLockExclusive(&lock);
        if (handler) { ReleaseSRWLockExclusive(&lock); return E_ILLEGAL_DELEGATE_ASSIGNMENT; }
        if (h) h->AddRef();
        handler = h;
        INT32 st = status;
        ReleaseSRWLockExclusive(&lock);
        if (h && st) invoke(h, st);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **h) override
    {
        if (!h) return E_POINTER;
        AcquireSRWLockShared(&lock);
        if ((*h = handler)) handler->AddRef();
        ReleaseSRWLockShared(&lock);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResults(IUnknown **out) override
    {
        if (!out) return E_POINTER;
        AcquireSRWLockShared(&lock);
        INT32 st = status;
        HRESULT err = error;
        if (st == 1 && (*out = result)) result->AddRef();
        ReleaseSRWLockShared(&lock);
        return st == 1 ? S_OK : st == 3 ? err : E_ILLEGAL_METHOD_CALL;
    }
    HRESULT STDMETHODCALLTYPE get_Id(UINT32 *v) override { if (!v) return E_POINTER; *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(INT32 *v) override
    { if (!v) return E_POINTER; AcquireSRWLockShared(&lock); *v = status; ReleaseSRWLockShared(&lock); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *v) override
    { if (!v) return E_POINTER; AcquireSRWLockShared(&lock); *v = status == 3 ? error : S_OK; ReleaseSRWLockShared(&lock); return S_OK; }
    HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
    void finish(HRESULT hr, IUnknown *res)
    {
        AcquireSRWLockExclusive(&lock);
        status = FAILED(hr) ? 3 : 1;
        error = hr;
        result = res;
        IUnknown *h = handler;
        if (h) h->AddRef();
        INT32 st = status;
        ReleaseSRWLockExclusive(&lock);
        if (h) { invoke(h, st); h->Release(); }
    }
};
template <typename F> static AsyncOp *start_async(REFIID iid, F work)
{
    struct Ctx { AsyncOp *op; F work; };
    AsyncOp *op = new AsyncOp(iid);
    op->AddRef();
    Ctx *ctx = new Ctx{ op, work };
    HANDLE t = CreateThread(nullptr, 0, [](void *param) -> DWORD {
        Ctx *c = (Ctx *)param;
        IUnknown *res = nullptr;
        HRESULT hr = c->work(&res);
        c->op->finish(hr, res);
        c->op->Release();
        delete c;
        return 0;
    }, ctx, 0, nullptr);
    if (t) CloseHandle(t);
    else { op->finish(E_OUTOFMEMORY, nullptr); op->Release(); delete ctx; }
    return op;
}

/* ------------------------------------------------------------------------------------------- */
/* ContentAccess.GetRawContentAccessDataAsync: which games the user may play (owned, Game Pass).          */
/* Gaming Services fetches it from the Content Access Service; so does this, with the user's Xbox Live      */
/* token from shims/xboxauth: contentaccess.exp.xboxservices.com/all/v1, the endpoint the app itself uses    */
/* when it does not use Gaming Services as its source. The data is handed over as it arrives.               */

static const GUID IID_IRawContentAccessDataResult = {0xd84407df,0xb7e6,0x58a2,{0xa1,0xe8,0x2d,0xb3,0x66,0xc4,0xff,0x11}};
static const GUID IID_AsyncOp_RawContentAccessDataResult = {0x25ece3f5,0xf64b,0x5328,{0x93,0xf4,0xfa,0xc2,0xfd,0xf3,0x27,0x3b}};
struct IRawContentAccessDataResultAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Cv(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT64 *) = 0;
    /* "out u1[] buffer" is a FillArray (the parameter is not by reference): the caller passes its buffer */
    virtual HRESULT STDMETHODCALLTYPE GetData(UINT32, BYTE *, UINT64) = 0;
};
struct RawContentAccessData : IRawContentAccessDataResultAbi
{
    LONG ref = 1;
    std::wstring cv;
    std::string data;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IRawContentAccessDataResult)) *out = static_cast<IRawContentAccessDataResultAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.RawContentAccessDataResult")
    HRESULT STDMETHODCALLTYPE get_Cv(HSTRING *v) override { return hstr(cv, v); }
    HRESULT STDMETHODCALLTYPE get_Size(UINT64 *v) override { if (!v) return E_POINTER; *v = data.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetData(UINT32 len, BYTE *buf, UINT64 size) override
    {
        if (len && !buf) return E_POINTER;
        UINT32 n = (UINT32)std::min((UINT64)data.size(), std::min((UINT64)len, size));
        memcpy(buf, data.data(), n);
        tracef("RawContentAccessDataResult.GetData(%u, %llu) -> %u bytes", len, (unsigned long long)size, n);
        return S_OK;
    }
};

static SRWLOCK content_access_lock = SRWLOCK_INIT;
static std::string content_access_blob;   /* the last content access data the app fetched (protobuf) */

static HRESULT WINAPI ca_GetRawContentAccessDataAsync(Factory *, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = static_cast<IAsyncOpAbi *>(start_async(IID_AsyncOp_RawContentAccessDataResult, [](IUnknown **res) -> HRESULT {
        typedef HRESULT (WINAPI *get_fn)(const char *, const char *, const char *, unsigned char **, UINT32 *, DWORD *);
        HMODULE xa = LoadLibraryW(L"wineuwp_xboxauth.dll");
        get_fn get = xa ? (get_fn)GetProcAddress(xa, "XboxAuthHttpGet") : nullptr;
        if (!get) { tracef("ContentAccess: no wineuwp_xboxauth.dll"); return GP_E_NO_SERVICE; }
        char market[8] = "US", url[256], cv[64], extra[96];
        GEOID geo = GetUserGeoID(GEOCLASS_NATION);
        wchar_t iso[8];
        if (geo != GEOID_NOT_AVAILABLE && GetGeoInfoW(geo, GEO_ISO2, iso, ARRAYSIZE(iso), 0))
            WideCharToMultiByte(CP_UTF8, 0, iso, -1, market, sizeof(market), nullptr, nullptr);
        snprintf(url, sizeof(url), "https://contentaccess.exp.xboxservices.com/all/v1?market=%s&offering=XGPUPCAPP", market);
        /* a correlation vector: 16 base64 characters and a counter */
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 16; i++) cv[i] = b64[(GetTickCount() * 2654435761u + i * 40503u + (unsigned)GetCurrentThreadId() * 97u) % 64];
        memcpy(cv + 16, ".0", 3);
        snprintf(extra, sizeof(extra), "MS-CV: %s", cv);
        unsigned char *body = nullptr;
        UINT32 len = 0;
        DWORD status = 0;
        HRESULT hr = get(url, extra, "1", &body, &len, &status);
        if (FAILED(hr) || status != 200)
        {
            tracef("ContentAccess.GetRawContentAccessDataAsync: hr %#lx, HTTP %lu", (unsigned long)hr, status);
            if (body) CoTaskMemFree(body);
            return FAILED(hr) ? hr : E_FAIL;
        }
        RawContentAccessData *r = new RawContentAccessData();
        r->data.assign((const char *)body, len);
        /* kept: which products the subscription includes, for install features that need one (gdkpackage.inc) */
        AcquireSRWLockExclusive(&content_access_lock);
        content_access_blob = r->data;
        ReleaseSRWLockExclusive(&content_access_lock);
        r->cv = std::wstring(cv, cv + strlen(cv));
        CoTaskMemFree(body);
        tracef("ContentAccess.GetRawContentAccessDataAsync -> %u bytes", len);
        *res = static_cast<IRawContentAccessDataResultAbi *>(r);
        return S_OK;
    }));
    return S_OK;
}

#include "install.inc"
#include "gdkpackage.inc"

/* Update-availability queries must succeed even when no update is available. Feature changes use
 * an explicit PackageRequest; a temporary PackagePreview must not advertise a package update. */
static const GUID IID_IPackageUpdate_ = {0x6afcfc17,0xa4a4,0x5293,{0xae,0x0d,0x32,0x38,0x0d,0x47,0x8e,0x60}};
static const GUID IID_AsyncOp_PackageUpdate_ = {0x297569b9,0x7e29,0x5534,{0xa9,0x93,0xf2,0x57,0x0b,0x81,0x32,0xbe}};
static const GUID IID_IVectorView_PackageUpdate_ = {0x63d04add,0xa47c,0x5978,{0x93,0x3a,0x58,0xc2,0x52,0x4d,0x9f,0x30}};
static const GUID IID_IIterable_PackageUpdate_ = {0x3b76b432,0xcc28,0x59d5,{0x8b,0xdf,0x06,0xc1,0x6f,0x2d,0x9e,0xb4}};
static const GUID IID_IIterator_PackageUpdate_ = {0x1e40d83e,0x7caa,0x5f07,{0xa6,0x66,0x66,0xc6,0x01,0xda,0x2f,0xa1}};
static const GUID IID_AsyncOp_PackageUpdateView_ = {0x939df71b,0xd0cc,0x5681,{0x8a,0x21,0xdc,0x19,0x1c,0x3b,0xe5,0xcd}};
struct IPackageUpdateAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InstanceId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableDate(INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartUpdateAsync(INT32, HSTRING, IInspectable **) = 0;
};

struct PackageUpdateObj : IPackageUpdateAbi
{
    LONG ref = 1;
    std::wstring store_id, instance;
    UINT64 size = 0;
    INT32 status = 2; /* NoUpdateAvailable */
    PackageUpdateObj(const InstalledGame &game) : store_id(game.store_id), instance(game.instance) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageUpdate_)) *out = static_cast<IPackageUpdateAbi *>(this);
        else if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageUpdate")
    HRESULT STDMETHODCALLTYPE get_Status(INT32 *v) override
    {
        if (!v) return E_POINTER;
        *v = status;
        tracef("PackageUpdate.Status(%ls) -> %d", store_id.c_str(), *v);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_InstanceId(HSTRING *v) override
    {
        tracef("PackageUpdate.InstanceId(%ls) -> %ls", store_id.c_str(), instance.c_str());
        return hstr(instance, v);
    }
    HRESULT STDMETHODCALLTYPE get_AvailableDate(INT64 *v) override
    {
        if (!v) return E_POINTER;
        *v = 0;
        tracef("PackageUpdate.AvailableDate(%ls) -> 0", store_id.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        *v = static_cast<IPackageRequestSizeAbi *>(new PackageRequestSize(size, true));
        tracef("PackageUpdate.QuerySize(%ls) -> %llu", store_id.c_str(), (unsigned long long)size);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartUpdateAsync(INT32, HSTRING, IInspectable **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
};

static PackageUpdateObj *package_update_for(const std::wstring &source)
{
    InstalledGame game;
    if (!find_installed(source, game)) return nullptr;
    tracef("PackageUpdate.QueryUpdate(%ls) -> no update", game.store_id.c_str());
    return new PackageUpdateObj(game);
}

static HRESULT WINAPI pu_QueryUpdate(Factory *, HSTRING source, INT32 source_type, boolean include_related, IInspectable **out)
{
    if (!out) return E_POINTER;
    std::wstring key = WindowsGetStringRawBuffer(source, nullptr);
    tracef("PackageUpdate.QueryUpdate source=%ls type=%d related=%d", key.c_str(), source_type, include_related);
    PackageUpdateObj *update = package_update_for(key);
    if (!update) { *out = nullptr; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    *out = static_cast<IPackageUpdateAbi *>(update);
    return S_OK;
}
static HRESULT WINAPI pu_QueryUpdateAsync(Factory *, HSTRING source, INT32 source_type, boolean include_related, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    std::wstring key = WindowsGetStringRawBuffer(source, nullptr);
    tracef("PackageUpdate.QueryUpdateAsync source=%ls type=%d related=%d", key.c_str(), source_type, include_related);
    PackageUpdateObj *update = package_update_for(key);
    if (!update) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    AsyncOp *op = new AsyncOp(IID_AsyncOp_PackageUpdate_);
    op->finish(S_OK, static_cast<IPackageUpdateAbi *>(update));
    *out = static_cast<IAsyncOpAbi *>(op);
    return S_OK;
}
static HRESULT pu_query_updates_async(INT32 filter, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    ObjView *updates = new ObjView(IID_IVectorView_PackageUpdate_, IID_IIterable_PackageUpdate_, IID_IIterator_PackageUpdate_,
                                    L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageUpdate>");
    AsyncOp *op = new AsyncOp(IID_AsyncOp_PackageUpdateView_);
    op->finish(S_OK, static_cast<IEmptyViewAbi *>(updates));
    *out = static_cast<IAsyncOpAbi *>(op);
    tracef("PackageUpdate.QueryUpdatesAsync(filter %d) -> 0", filter);
    return S_OK;
}
static HRESULT WINAPI pu_QueryUpdatesAsync(Factory *, IInspectable **out)
{ return pu_query_updates_async(0, out); }
static HRESULT WINAPI pu_QueryUpdatesAsyncFiltered(Factory *, INT32 filter, IInspectable **out)
{ return pu_query_updates_async(filter, out); }

/* Storage.EnumerateContent: the games installed through the queue (stage directories with a marker) */
static HRESULT WINAPI st_EnumerateContent(Factory *, const GUID *device, IInspectable **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!device || (!IsEqualGUID(*device, DEVICE_ID) && !IsEqualGUID(*device, DATA_DEVICE_ID))) return E_INVALIDARG;
    HStringView *v = new HStringView;
    if (IsEqualGUID(*device, DATA_DEVICE_ID))
        for (auto &g : installed_games()) v->items.push_back(g.instance);
    tracef("Storage.EnumerateContent -> %zu", v->items.size());
    *out = static_cast<IHStringViewAbi *>(v);
    return S_OK;
}

/* No products are blocked for migration on this local Gaming Services stand-in. Returning the empty
 * list lets the Xbox product page continue; the generic no-service HRESULT turns its content panel
 * into "Content failed to load." */
static HRESULT WINAPI store_EnumerateMigrationBlockedProducts(Factory *, IInspectable **out)
{
    if (!out) return E_POINTER;
    HStringView *v = new HStringView;
    tracef("Store.EnumerateMigrationBlockedProducts -> 0");
    *out = static_cast<IHStringViewAbi *>(v);
    return S_OK;
}

struct special { const wchar_t *cls; int slot; const void *fn; };
static const special specials[] =
{
    { L"Microsoft.GamePlatform.Services.PackageUpdate", 6, (void *)pu_QueryUpdate },
    { L"Microsoft.GamePlatform.Services.PackageUpdate", 7, (void *)pu_QueryUpdateAsync },
    { L"Microsoft.GamePlatform.Services.PackageUpdate", 10, (void *)pu_QueryUpdatesAsync },
    { L"Microsoft.GamePlatform.Services.PackageUpdate", 11, (void *)pu_QueryUpdatesAsyncFiltered },
    { L"Microsoft.GamePlatform.Services.PlatformStatus", 6, (void *)ps_get_ApiStatus },
    { L"Microsoft.GamePlatform.Services.Storage", 6, (void *)st_QueryDeviceDetails },
    { L"Microsoft.GamePlatform.Services.Storage", 7, (void *)st_EnumerateDevices },
    { L"Microsoft.GamePlatform.Services.Storage", 8, (void *)st_EnumerateContent },
    { L"Microsoft.GamePlatform.Services.Storage", 14, (void *)st_QueryDefaultDevice },
    { L"Microsoft.GamePlatform.Services.Storage", 15, (void *)st_SetDefaultDevice },
    { L"Microsoft.GamePlatform.Services.GameFolder", 6, (void *)gf_QueryForDevice },
    { L"Microsoft.GamePlatform.Services.GameFolder", 7, (void *)gf_SetForDevice },
    { L"Microsoft.GamePlatform.Services.Customization", 6, (void *)cu_QueryGlobalProps },
    { L"Microsoft.GamePlatform.Services.Customization", 7, (void *)cu_SetGlobalProps },
    { L"Microsoft.GamePlatform.Services.Customization", 8, (void *)cu_QueryGameProps },
    { L"Microsoft.GamePlatform.Services.Customization", 9, (void *)cu_SetGameProps },
    { L"Microsoft.GamePlatform.Services.SupportedFeatures", 6, (void *)sf_QueryFeatures },
    { L"Microsoft.GamePlatform.Services.SupportedFeatures", 7, (void *)sf_QueryFeatures },
    { L"Microsoft.GamePlatform.Services.ProcessMonitor", 6, (void *)pm_QueryGamingServicesUsage },
    { L"Microsoft.GamePlatform.Services.ProcessMonitor", 7, (void *)pm_EnumerateRunningGames },
    { L"Microsoft.GamePlatform.Services.Package", 6, (void *)pk_Create },
    { L"Microsoft.GamePlatform.Services.Store", 8, (void *)store_QueryProductDetails },
    { L"Microsoft.GamePlatform.Services.Store", 7, (void *)store_EnumerateMigrationBlockedProducts },
    { L"Microsoft.GamePlatform.Services.ContentAccess", 6, (void *)ca_GetRawContentAccessDataAsync },
    { L"Microsoft.GamePlatform.Services.Store", 9, (void *)store_QueryProductType },
    { L"Microsoft.GamePlatform.Services.PackageRequest", 6, (void *)rq_CreateForSource },
    { L"Microsoft.GamePlatform.Services.PackageRequest", 7, (void *)rq_CreateForSourceDevice },
    { L"Microsoft.GamePlatform.Services.PackageRequest", 8, (void *)rq_CreateForStoreId },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 6, (void *)pq_SubmitRequestAsync },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 8, (void *)pq_PauseInstance },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 9, (void *)pq_ResumeInstance },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 10, (void *)pq_CancelInstance },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 11, (void *)pq_PauseId },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 12, (void *)pq_ResumeId },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 13, (void *)pq_CancelId },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 14, (void *)pq_PauseAll },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 15, (void *)pq_ResumeAll },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 16, (void *)pq_CancelAll },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 17, (void *)pq_Enumerate },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 18, (void *)pq_EnumerateId },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 19, (void *)pq_QueryInstallationInfo },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 20, (void *)pq_QueryInstallationSummary },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 23, (void *)pq_CreateViewInstance },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 24, (void *)pq_CreateViewId },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 25, (void *)pq_add_InstallationChanged },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 26, (void *)pq_remove_InstallationChanged },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 27, (void *)pq_add_QueueChanged },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 28, (void *)pq_remove_QueueChanged },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 29, (void *)pq_add_ItemAdded },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 30, (void *)pq_remove_ItemAdded },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 31, (void *)pq_add_ItemRemoved },
    { L"Microsoft.GamePlatform.Services.PackageQueue", 32, (void *)pq_remove_ItemRemoved },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 6, (void *)pi_QueryInstallPath },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 7, (void *)pi_QueryInstanceId },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 8, (void *)pi_QueryIdentity },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 9, (void *)pi_QuerySize },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 10, (void *)pi_QuerySize },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 11, (void *)pi_QueryStorageCategory },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 12, (void *)pi_QueryHeader },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 13, (void *)pi_QueryDetails },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 15, (void *)pi_QueryFeatures },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 16, (void *)pi_QueryRecipes },
    { L"Microsoft.GamePlatform.Services.PackagePreview", 6, (void *)pv_Create },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 24, (void *)pi_IsFlatStreamed },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 25, (void *)pi_IsLaunchable },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 27, (void *)pi_GetRunningProcessId },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 28, (void *)pi_Activate },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 29, (void *)pi_ActivateAumid },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 37, (void *)pi_add_InstallationChanged },
    { L"Microsoft.GamePlatform.Services.PackageInstance", 38, (void *)pi_remove_InstallationChanged },
};

static void init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int k = 0; k < K_COUNT; k++)
    {
        memcpy(factory_vtbl[k], thunks, sizeof(thunks));
        factory_vtbl[k][0] = (void *)f_QueryInterface;
        factory_vtbl[k][1] = (void *)f_AddRef;
        factory_vtbl[k][2] = (void *)f_Release;
        factory_vtbl[k][3] = (void *)f_GetIids;
        factory_vtbl[k][4] = (void *)f_GetRuntimeClassName;
        factory_vtbl[k][5] = (void *)f_GetTrustLevel;
        factories[k].vtbl = factory_vtbl[k];
        factories[k].ref = 1;
        factories[k].kind = k;
    }
    for (auto &sp : specials)
    {
        int k = kind_of(sp.cls);
        if (k >= 0) factory_vtbl[k][sp.slot] = sp.fn;
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const wchar_t *name = WindowsGetStringRawBuffer(classid, nullptr);
    int k;

    init();
    if ((k = kind_of(name)) >= 0)
    {
        tracef("%s: factory", class_name(k));
        InterlockedIncrement(&factories[k].ref);
        *factory = (IActivationFactory *)&factories[k];
        return S_OK;
    }
    tracef("%ls: no stand-in", name);
    *factory = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

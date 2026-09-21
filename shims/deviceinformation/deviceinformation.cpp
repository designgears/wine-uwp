// Windows.Devices.Enumeration.DeviceInformation: CreateWatcher(DeviceClass) only.
//
// Solitaire's AppHost::EngineInitialize creates a device watcher for DeviceClass.AudioRender.
// Wine's windows.devices.enumeration implements every other CreateWatcher overload but leaves
// this one `return E_NOTIMPL`, and the app is C++/CX: vccorlib140_app turns that HRESULT into a
// thrown Platform::Exception, which abandons the rest of EngineInitialize - including
// AppHost::SubscribeToRenderWindowEvents. Without those handlers the engine never learns the
// swap-chain panel's real size and keeps rendering into the 1x1 surface it made at startup.
//
// DeviceClass is just shorthand for an AQS filter on the interface class, which is how Windows
// implements the overload itself, so this maps the class and calls the real CreateWatcherAqsFilter.
// Everything else forwards to Wine's implementation untouched - this is a one-method patch, not a
// reimplementation.
//
// The fix belongs in dlls/windows.devices.enumeration/main.c, but that DLL does not currently
// link in this tree (multiple definition of RuntimeClass_* across its translation units, a
// pre-existing breakage in generated headers under include/). A shim
// gets the behaviour without touching either.
//
// IDeviceInformationStatics {c17f100e-3a46-4a78-8013-769dc9b97390}, from Windows.Devices.winmd:
//    6 CreateFromIdAsync                 7 CreateFromIdAsyncAdditionalProperties
//    8 FindAllAsync                      9 FindAllAsyncDeviceClass
//   10 FindAllAsyncAqsFilter            11 FindAllAsyncAqsFilterAndAdditionalProperties
//   12 CreateWatcher                    13 CreateWatcherDeviceClass   <-- the stub
//   14 CreateWatcherAqsFilter           15 CreateWatcherAqsFilterAndAdditionalProperties
//
// The watcher Wine hands back is then itself a problem. Solitaire subscribes to all five of its
// events, and Wine implements only three:
//
//     add_Updated    -> S_OK but drops the handler     remove_Updated -> E_NOTIMPL
//     add_Removed    -> E_NOTIMPL                      remove_Removed -> E_NOTIMPL
//
// `add_Removed` returning E_NOTIMPL is fatal for the same reason the CreateWatcher stub was: this
// app is C++/CX, so vccorlib140_app turns the failed HRESULT into a thrown Platform::Exception.
// The log says exactly this, with no gap:
//
//     fixme:enumeration:device_watcher_add_Updated ... stub!
//     fixme:enumeration:device_watcher_add_Removed ... stub!
//     [   1.211] *** CRITICAL ERROR ***  Microsoft C++ Exception
//
// Registering an event has to succeed even when the event can never fire. So CreateWatcher* here
// returns a proxy that forwards all 19 slots to Wine's watcher except those four, which it answers
// itself - retaining each handler against a unique token so the add/remove pairing is honest.
//
// IDeviceWatcher {c9eab97d-8f6b-4f96-a9f4-abc814e22271}:
//    6 add_Added                 7 remove_Added         8 add_Updated     9 remove_Updated
//   10 add_Removed              11 remove_Removed      12 add_EnumerationCompleted
//   13 remove_EnumerationCompleted  14 add_Stopped     15 remove_Stopped
//   16 get_Status               17 Start              18 Stop
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>

static const GUID IID_IDeviceInformationStatics_ =
    {0xc17f100e,0x3a46,0x4a78,{0x80,0x13,0x76,0x9d,0xc9,0xb9,0x73,0x90}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IDeviceWatcher_ =
    {0xc9eab97d,0x8f6b,0x4f96,{0xa9,0xf4,0xab,0xc8,0x14,0xe2,0x22,0x71}};
static const GUID IID_IReferenceTracker_ =
    {0x11d3b13a,0x180e,0x4789,{0xa8,0xbe,0x77,0x12,0x88,0x28,0x93,0xe6}};

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_DEVENUM_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[deviceinformation] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* The real Wine implementation, loaded by module name so the builtin wins. */
static IActivationFactory *real_factory;
/* The real IDeviceInformationStatics. Its first word is the vtable, so calling slot n means
 * real_statics->vtbl[n] with real_statics as `this`. */
struct RealStatics { void **vtbl; };
static RealStatics *real_statics;

static bool load_real(void)
{
    static bool tried;
    if (tried) return real_statics != nullptr;
    tried = true;

    HMODULE mod = LoadLibraryW(L"windows.devices.enumeration.dll");
    if (!mod) { trace("cannot load windows.devices.enumeration.dll"); return false; }
    auto get_factory = reinterpret_cast<HRESULT (WINAPI *)(HSTRING, IActivationFactory **)>(
        GetProcAddress(mod, "DllGetActivationFactory"));
    if (!get_factory) { trace("no DllGetActivationFactory"); return false; }

    static const WCHAR cls[] = L"Windows.Devices.Enumeration.DeviceInformation";
    HSTRING name = nullptr;
    if (FAILED(WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name))) return false;
    HRESULT hr = get_factory(name, &real_factory);
    WindowsDeleteString(name);
    if (FAILED(hr) || !real_factory) { trace("real factory failed %#lx", hr); return false; }

    hr = real_factory->QueryInterface(IID_IDeviceInformationStatics_, (void **)&real_statics);
    if (FAILED(hr)) { trace("real statics QI failed %#lx", hr); real_statics = nullptr; return false; }
    trace("forwarding to Wine's implementation at %p", real_statics);
    return true;
}

/* Call slot n on the real statics. */
template <typename Fn> static Fn real_slot(unsigned n)
{
    return reinterpret_cast<Fn>(real_statics->vtbl[n]);
}

struct Statics
{
    const void **vtbl;
    LONG ref;
};
static Statics statics_obj;
static const void *statics_vtbl[16];

static HRESULT WINAPI statics_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_IDeviceInformationStatics_))
    {
        InterlockedIncrement(&statics_obj.ref);
        *out = &statics_obj.vtbl;
        return S_OK;
    }
    /* Any other revision (IDeviceInformationStatics2/3, ...) is Wine's to answer, unchanged. */
    if (!IsEqualGUID(iid, IID_IReferenceTracker_) && load_real() && real_factory)
        return real_factory->QueryInterface(iid, out);
    *out = nullptr;
    return E_NOINTERFACE;
}

static ULONG WINAPI statics_AddRef(void *iface) { return InterlockedIncrement(&statics_obj.ref); }
static ULONG WINAPI statics_Release(void *iface) { return InterlockedDecrement(&statics_obj.ref); }

static HRESULT WINAPI statics_GetIids(void *iface, ULONG *count, IID **iids)
{
    if (count) *count = 0;
    if (iids) *iids = nullptr;
    return S_OK;
}
static HRESULT WINAPI statics_GetRuntimeClassName(void *iface, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.Devices.Enumeration.DeviceInformation";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
}
static HRESULT WINAPI statics_GetTrustLevel(void *iface, TrustLevel *level)
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

/* Forwarders: same arguments, real object as `this`. The argument lists are opaque here because
 * every one is (pointer-sized args..., out pointer) and we neither read nor write them. */
#define FORWARD4(name, slot)                                                                   \
    static HRESULT WINAPI name(void *iface, void *a, void *b, void *c, void *d)                \
    {                                                                                          \
        if (!load_real()) return E_NOINTERFACE;                                                \
        auto fn = real_slot<HRESULT (WINAPI *)(void *, void *, void *, void *, void *)>(slot); \
        return fn(real_statics, a, b, c, d);                                                   \
    }
FORWARD4(statics_CreateFromIdAsync,        6)
FORWARD4(statics_CreateFromIdAsyncProps,   7)
FORWARD4(statics_FindAllAsync,             8)
FORWARD4(statics_FindAllAsyncDeviceClass,  9)
FORWARD4(statics_FindAllAsyncAqs,         10)
FORWARD4(statics_FindAllAsyncAqsProps,    11)
static void wrap_watcher(void **watcher);   /* defined below, with the proxy it builds */

#define FORWARD_WATCHER(name, slot, nargs)                                                     \
    static HRESULT WINAPI name(void *iface, void *a, void *b, void *c)                         \
    {                                                                                          \
        void **watcher = (void **)(nargs == 1 ? a : nargs == 2 ? b : c);                       \
        HRESULT hr;                                                                            \
        if (!load_real()) return E_NOINTERFACE;                                                \
        auto fn = real_slot<HRESULT (WINAPI *)(void *, void *, void *, void *)>(slot);         \
        hr = fn(real_statics, a, b, c);                                                        \
        if (SUCCEEDED(hr) && watcher) wrap_watcher(watcher);                                   \
        return hr;                                                                             \
    }
/* The three watcher-returning overloads. The out-parameter is always the last argument; its
 * position differs per overload, so each names it explicitly rather than guessing. */
FORWARD_WATCHER(statics_CreateWatcher,         12, 1)  /* (watcher) */
FORWARD_WATCHER(statics_CreateWatcherAqs,      14, 2)  /* (filter, watcher) */
FORWARD_WATCHER(statics_CreateWatcherAqsProps, 15, 3)  /* (filter, properties, watcher) */


/* ---------------------------------------------------------------------------------------------
 * The watcher proxy.
 *
 * Every slot forwards to Wine's watcher except the four event methods it refuses. Those are held
 * here: the handler is AddRef'd and parked against a token, so a later remove_* can release the
 * right one. The events never fire - there is no device arrival/removal plumbing behind this - but
 * "registered and silent" is a state callers handle, and "E_NOTIMPL" is not.
 * ------------------------------------------------------------------------------------------- */
#define WATCHER_SLOTS 19
#define WATCHER_MAX_HANDLERS 16

struct Watcher
{
    const void **vtbl;
    LONG ref;
    IUnknown *real;                 /* Wine's IDeviceWatcher */
    void **real_vtbl;
    CRITICAL_SECTION cs;
    LONGLONG next_token;
    struct { LONGLONG token; IUnknown *handler; } handlers[WATCHER_MAX_HANDLERS];
};
static const void *watcher_vtbl[WATCHER_SLOTS];

/* An EventRegistrationToken is a struct wrapping one INT64; by value it is passed as that INT64. */
static HRESULT watcher_add( Watcher *w, IUnknown *handler, LONGLONG *token )
{
    HRESULT hr = E_OUTOFMEMORY;
    if (!token) return E_POINTER;
    *token = 0;
    EnterCriticalSection(&w->cs);
    for (int i = 0; i < WATCHER_MAX_HANDLERS; i++)
    {
        if (w->handlers[i].token) continue;
        w->handlers[i].token = ++w->next_token;
        w->handlers[i].handler = handler;
        if (handler) handler->AddRef();
        *token = w->handlers[i].token;
        hr = S_OK;
        break;
    }
    LeaveCriticalSection(&w->cs);
    trace("watcher add handler %p -> token %lld hr %#lx", handler, (long long)*token, hr);
    return hr;
}

static HRESULT watcher_remove( Watcher *w, LONGLONG token )
{
    IUnknown *drop = nullptr;
    EnterCriticalSection(&w->cs);
    for (int i = 0; i < WATCHER_MAX_HANDLERS; i++)
    {
        if (w->handlers[i].token != token || !token) continue;
        drop = w->handlers[i].handler;
        w->handlers[i].token = 0;
        w->handlers[i].handler = nullptr;
        break;
    }
    LeaveCriticalSection(&w->cs);
    if (drop) drop->Release();
    /* Removing an unknown token is not an error worth throwing over. */
    return S_OK;
}

static HRESULT WINAPI watcher_QueryInterface(void *iface, REFIID iid, void **out)
{
    Watcher *w = (Watcher *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IDeviceWatcher_))
    {
        InterlockedIncrement(&w->ref);
        *out = &w->vtbl;
        return S_OK;
    }
    /* Anything else (IDeviceWatcher2, ...) is Wine's to answer. Those interfaces have their own
     * methods and do not route through this proxy's overrides, which is fine - the app only asked
     * for the base interface's events. */
    if (IsEqualGUID(iid, IID_IReferenceTracker_)) { *out = nullptr; return E_NOINTERFACE; }
    return w->real->QueryInterface(iid, out);
}

static ULONG WINAPI watcher_AddRef(void *iface)
{
    return InterlockedIncrement(&((Watcher *)iface)->ref);
}

static ULONG WINAPI watcher_Release(void *iface)
{
    Watcher *w = (Watcher *)iface;
    ULONG ref = InterlockedDecrement(&w->ref);
    if (!ref)
    {
        for (int i = 0; i < WATCHER_MAX_HANDLERS; i++)
            if (w->handlers[i].handler) w->handlers[i].handler->Release();
        w->real->Release();
        DeleteCriticalSection(&w->cs);
        free(w);
    }
    return ref;
}

/* Forward slot n to the real watcher, unchanged. */
#define WATCHER_FORWARD(name, slot, sig, args)                                       \
    static HRESULT WINAPI name sig                                                   \
    {                                                                                \
        Watcher *w = (Watcher *)iface;                                               \
        auto fn = reinterpret_cast<HRESULT (WINAPI *)sig>(w->real_vtbl[slot]);       \
        return fn args;                                                              \
    }
WATCHER_FORWARD(watcher_GetIids,      3, (void *iface, ULONG *a, IID **b), ((void *)w->real, a, b))
WATCHER_FORWARD(watcher_GetClassName, 4, (void *iface, HSTRING *a),        ((void *)w->real, a))
WATCHER_FORWARD(watcher_GetTrust,     5, (void *iface, TrustLevel *a),     ((void *)w->real, a))
WATCHER_FORWARD(watcher_add_Added,    6, (void *iface, void *a, void *b),  ((void *)w->real, a, b))
WATCHER_FORWARD(watcher_rem_Added,    7, (void *iface, LONGLONG a),        ((void *)w->real, a))
WATCHER_FORWARD(watcher_add_EnumDone,12, (void *iface, void *a, void *b),  ((void *)w->real, a, b))
WATCHER_FORWARD(watcher_rem_EnumDone,13, (void *iface, LONGLONG a),        ((void *)w->real, a))
WATCHER_FORWARD(watcher_add_Stopped, 14, (void *iface, void *a, void *b),  ((void *)w->real, a, b))
WATCHER_FORWARD(watcher_rem_Stopped, 15, (void *iface, LONGLONG a),        ((void *)w->real, a))
WATCHER_FORWARD(watcher_get_Status,  16, (void *iface, int *a),            ((void *)w->real, a))
WATCHER_FORWARD(watcher_Start,       17, (void *iface),                    ((void *)w->real))
WATCHER_FORWARD(watcher_Stop,        18, (void *iface),                    ((void *)w->real))

/* The four Wine refuses. */
static HRESULT WINAPI watcher_add_Updated(void *iface, IUnknown *h, LONGLONG *t)
{ return watcher_add((Watcher *)iface, h, t); }
static HRESULT WINAPI watcher_rem_Updated(void *iface, LONGLONG t)
{ return watcher_remove((Watcher *)iface, t); }
static HRESULT WINAPI watcher_add_Removed(void *iface, IUnknown *h, LONGLONG *t)
{ return watcher_add((Watcher *)iface, h, t); }
static HRESULT WINAPI watcher_rem_Removed(void *iface, LONGLONG t)
{ return watcher_remove((Watcher *)iface, t); }

static void init_watcher_vtbl(void)
{
    if (watcher_vtbl[0]) return;
    watcher_vtbl[0]  = (void *)watcher_QueryInterface;
    watcher_vtbl[1]  = (void *)watcher_AddRef;
    watcher_vtbl[2]  = (void *)watcher_Release;
    watcher_vtbl[3]  = (void *)watcher_GetIids;
    watcher_vtbl[4]  = (void *)watcher_GetClassName;
    watcher_vtbl[5]  = (void *)watcher_GetTrust;
    watcher_vtbl[6]  = (void *)watcher_add_Added;
    watcher_vtbl[7]  = (void *)watcher_rem_Added;
    watcher_vtbl[8]  = (void *)watcher_add_Updated;
    watcher_vtbl[9]  = (void *)watcher_rem_Updated;
    watcher_vtbl[10] = (void *)watcher_add_Removed;
    watcher_vtbl[11] = (void *)watcher_rem_Removed;
    watcher_vtbl[12] = (void *)watcher_add_EnumDone;
    watcher_vtbl[13] = (void *)watcher_rem_EnumDone;
    watcher_vtbl[14] = (void *)watcher_add_Stopped;
    watcher_vtbl[15] = (void *)watcher_rem_Stopped;
    watcher_vtbl[16] = (void *)watcher_get_Status;
    watcher_vtbl[17] = (void *)watcher_Start;
    watcher_vtbl[18] = (void *)watcher_Stop;
}

/* Wrap a watcher Wine just created. On any failure the real one is handed back untouched: a
 * working watcher that refuses two events beats no watcher at all. */
static void wrap_watcher(void **watcher)
{
    IUnknown *real = (IUnknown *)*watcher;
    if (!real) return;

    IUnknown *base = nullptr;
    if (FAILED(real->QueryInterface(IID_IDeviceWatcher_, (void **)&base)) || !base)
    {
        trace("watcher %p does not answer IDeviceWatcher - not wrapping", real);
        return;
    }

    Watcher *w = (Watcher *)calloc(1, sizeof(*w));
    if (!w) { base->Release(); return; }

    init_watcher_vtbl();
    w->vtbl = watcher_vtbl;
    w->ref = 1;
    w->real = base;                       /* owns the reference from the QI above */
    w->real_vtbl = *(void ***)base;
    InitializeCriticalSection(&w->cs);

    real->Release();                      /* drop the caller's original reference */
    *watcher = &w->vtbl;
    trace("wrapped watcher %p as %p", base, *watcher);
}

/* The one method this shim exists for. */
static HRESULT WINAPI statics_CreateWatcherDeviceClass(void *iface, int device_class, void **watcher)
{
    /* Interface class GUIDs, in DeviceClass declaration order. All(0) means "no filter". */
    static const WCHAR *class_guids[] =
    {
        nullptr,
        L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}",  /* AudioCapture */
        L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}",  /* AudioRender */
        L"{6AC27878-A6FA-4155-BA85-F98F491D4F33}",  /* PortableStorageDevice */
        L"{E5323777-F976-4F5B-9B55-B94699C46E44}",  /* VideoCapture */
        L"{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}",  /* ImageScanner */
        L"{BFA794E4-F964-4FDB-90F6-51056BFE4B44}",  /* Location */
    };
    WCHAR buffer[160];
    HSTRING filter = nullptr;
    HRESULT hr;

    if (!watcher) return E_POINTER;
    *watcher = nullptr;
    if (device_class < 0 || device_class >= (int)ARRAYSIZE(class_guids)) return E_INVALIDARG;
    if (!load_real()) return E_NOINTERFACE;

    if (class_guids[device_class])
    {
        swprintf(buffer, ARRAYSIZE(buffer), L"System.Devices.InterfaceClassGuid:=\"%s\"",
                 class_guids[device_class]);
        if (FAILED(hr = WindowsCreateString(buffer, (UINT32)wcslen(buffer), &filter))) return hr;
    }

    auto fn = real_slot<HRESULT (WINAPI *)(void *, HSTRING, void **)>(14);  /* CreateWatcherAqsFilter */
    hr = fn(real_statics, filter, watcher);
    trace("CreateWatcher(DeviceClass=%d) -> %#lx watcher %p", device_class, hr, *watcher);
    WindowsDeleteString(filter);
    if (SUCCEEDED(hr)) wrap_watcher(watcher);
    return hr;
}

static void init_vtbl(void)
{
    statics_vtbl[0]  = (void *)statics_QueryInterface;
    statics_vtbl[1]  = (void *)statics_AddRef;
    statics_vtbl[2]  = (void *)statics_Release;
    statics_vtbl[3]  = (void *)statics_GetIids;
    statics_vtbl[4]  = (void *)statics_GetRuntimeClassName;
    statics_vtbl[5]  = (void *)statics_GetTrustLevel;
    statics_vtbl[6]  = (void *)statics_CreateFromIdAsync;
    statics_vtbl[7]  = (void *)statics_CreateFromIdAsyncProps;
    statics_vtbl[8]  = (void *)statics_FindAllAsync;
    statics_vtbl[9]  = (void *)statics_FindAllAsyncDeviceClass;
    statics_vtbl[10] = (void *)statics_FindAllAsyncAqs;
    statics_vtbl[11] = (void *)statics_FindAllAsyncAqsProps;
    statics_vtbl[12] = (void *)statics_CreateWatcher;
    statics_vtbl[13] = (void *)statics_CreateWatcherDeviceClass;
    statics_vtbl[14] = (void *)statics_CreateWatcherAqs;
    statics_vtbl[15] = (void *)statics_CreateWatcherAqsProps;
    statics_obj.vtbl = statics_vtbl;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.Devices.Enumeration.DeviceInformation"))
    {
        trace("not ours: %ls", cls ? cls : L"(null)");
        return REGDB_E_CLASSNOTREG;
    }
    init_vtbl();
    InterlockedIncrement(&statics_obj.ref);
    *out = (IActivationFactory *)&statics_obj.vtbl;
    trace("serving DeviceInformation");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

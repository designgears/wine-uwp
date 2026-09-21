// Windows.Networking.Connectivity.NetworkInformation: make NetworkStatusChanged actually fire once.
//
// This is what holds Solitaire on its splash screen after everything else is fixed. The app logs
// `PlayFab login successful`, then nothing at all for 150+ seconds, with no error and no dialog.
//
// Its own RTTI names the mechanism. `Sgi::Network::ReachableWinRT::Startup` registers a
// `NetworkStatusChangedEventHandler`, and every component that gates on connectivity takes a
// `(bool, Sgi::Network::Reachable::Status)` callback from it:
//
//     UserDataFlowAppComponent::Initialize      BootflowItemThemes::BeginInit
//     DlcDownloaderAppComponent::Initialize     PermanentPurchaseAppComponent::Initialize
//     NoInternetAppComponent::Initialize        SolitaireApp::Startup
//
// Wine's `network_information_statics_add_NetworkStatusChanged` is a stub: it returns S_OK with a
// dummy cookie and the event is never raised. So the registration *succeeds* - which is why this
// never showed up as an error, and why the app never falls back to its own `ShowNoInternetDialog`
// path. It simply waits for a status callback that cannot arrive.
//
// Note this is the **opposite** of the usual trap. AGENTS.md says a refusal from an event
// registration is usually the fatal thing, and that registering must succeed even when the event
// never fires. That is right for events an app only needs for *updates*. It is wrong when the app
// takes its **initial** state from the first callback: then "registered and permanently silent" is
// just as fatal as E_NOTIMPL, and quieter. If an app registers for an event and then stops, ask
// whether it is waiting for the first one.
//
// So: everything forwards to Wine's real implementation, and slot 12 additionally fires the
// handler once, shortly after registration, from a worker thread. Wine's connectivity data is
// real - its NetworkListManager reports 0x660 (IPV4_INTERNET | IPV6_INTERNET | ...) here - so the
// app re-reads a true status when it is poked; this shim invents no connectivity, it only delivers
// the notification Wine never sends.
//
//   INetworkInformationStatics {5074f851-950d-4165-9c15-365619481eea}:
//      6 GetConnectionProfiles   7 GetInternetConnectionProfile  8 GetLanIdentifiers
//      9 GetHostNames           10 GetProxyConfigurationAsync   11 GetSortedEndpointPairs
//     12 add_NetworkStatusChanged  <-- overridden      13 remove_NetworkStatusChanged
//
// The handler is a WinRT delegate: three IUnknown slots then Invoke at slot 3, not 6.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

static const GUID IID_INetworkInformationStatics_ =
    {0x5074f851,0x950d,0x4165,{0x9c,0x15,0x36,0x56,0x19,0x48,0x1e,0xea}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IReferenceTracker_ =
    {0x11d3b13a,0x180e,0x4789,{0xa8,0xbe,0x77,0x12,0x88,0x28,0x93,0xe6}};

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_NETSTATUS_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[networkstatus] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static IActivationFactory *real_factory;
struct RealStatics { void **vtbl; };
static RealStatics *real_statics;

static bool load_real(void)
{
    static bool tried;
    if (tried) return real_statics != nullptr;
    tried = true;

    HMODULE mod = LoadLibraryW(L"windows.networking.connectivity.dll");
    if (!mod) { trace("cannot load windows.networking.connectivity.dll"); return false; }
    auto get_factory = reinterpret_cast<HRESULT (WINAPI *)(HSTRING, IActivationFactory **)>(
        GetProcAddress(mod, "DllGetActivationFactory"));
    if (!get_factory) { trace("no DllGetActivationFactory"); return false; }

    static const WCHAR cls[] = L"Windows.Networking.Connectivity.NetworkInformation";
    HSTRING name = nullptr;
    if (FAILED(WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name))) return false;
    HRESULT hr = get_factory(name, &real_factory);
    WindowsDeleteString(name);
    if (FAILED(hr) || !real_factory) { trace("real factory failed %#lx", hr); return false; }
    if (FAILED(real_factory->QueryInterface(IID_INetworkInformationStatics_, (void **)&real_statics)))
    { real_statics = nullptr; return false; }
    trace("forwarding to Wine's implementation at %p", real_statics);
    return true;
}

template <typename Fn> static Fn real_slot(unsigned n)
{
    return reinterpret_cast<Fn>(real_statics->vtbl[n]);
}

struct Statics { const void **vtbl; LONG ref; };
static Statics statics_obj;
static const void *statics_vtbl[14];

static HRESULT WINAPI s_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_INetworkInformationStatics_))
    {
        InterlockedIncrement(&statics_obj.ref);
        *out = &statics_obj.vtbl;
        return S_OK;
    }
    /* Later revisions are Wine's to answer, unchanged. */
    if (!IsEqualGUID(iid, IID_IReferenceTracker_) && load_real() && real_factory)
        return real_factory->QueryInterface(iid, out);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(void *i)  { return InterlockedIncrement(&statics_obj.ref); }
static ULONG WINAPI s_Release(void *i) { return InterlockedDecrement(&statics_obj.ref); }
static HRESULT WINAPI s_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI s_GetRuntimeClassName(void *i, HSTRING *n)
{
    static const WCHAR cls[] = L"Windows.Networking.Connectivity.NetworkInformation";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, n);
}
static HRESULT WINAPI s_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

#define FORWARD(name, slot)                                                                    \
    static HRESULT WINAPI name(void *iface, void *a, void *b, void *c, void *d)                \
    {                                                                                          \
        if (!load_real()) return E_NOINTERFACE;                                                \
        auto fn = real_slot<HRESULT (WINAPI *)(void *, void *, void *, void *, void *)>(slot); \
        return fn(real_statics, a, b, c, d);                                                   \
    }
FORWARD(s_GetConnectionProfiles,       6)
FORWARD(s_GetInternetConnectionProfile,7)
FORWARD(s_GetLanIdentifiers,           8)
FORWARD(s_GetHostNames,                9)
FORWARD(s_GetProxyConfigurationAsync, 10)
FORWARD(s_GetSortedEndpointPairs,     11)
FORWARD(s_remove_NetworkStatusChanged,13)

/* A WinRT delegate: Invoke is slot 3. The sender for this event is null on Windows too - the
 * handler is expected to go and read the current status for itself, which is exactly what we want,
 * because then the answer comes from Wine's real connectivity data and not from this shim. */
typedef HRESULT (WINAPI *status_invoke)(void *handler, void *sender);

static DWORD WINAPI poke_thread(void *param)
{
    IUnknown *handler = (IUnknown *)param;
    /* Let the caller finish registering and get back to its own loop first. */
    Sleep(250);
    trace("firing NetworkStatusChanged once for handler %p", handler);
    HRESULT hr = ((status_invoke)(*(void ***)handler)[3])(handler, nullptr);
    trace("handler returned %#lx", hr);
    handler->Release();
    return 0;
}

static HRESULT WINAPI s_add_NetworkStatusChanged(void *iface, IUnknown *handler, LONGLONG *token)
{
    HRESULT hr = E_NOINTERFACE;

    if (load_real())
    {
        auto fn = real_slot<HRESULT (WINAPI *)(void *, void *, LONGLONG *)>(12);
        hr = fn(real_statics, handler, token);
    }
    if (FAILED(hr))
    {
        /* Wine's is a stub that cannot fail, but do not depend on that. */
        if (!token) return E_POINTER;
        *token = 1;
        hr = S_OK;
    }
    trace("add_NetworkStatusChanged -> %#lx token %lld", hr, token ? (long long)*token : 0);

    /* The part Wine never does: deliver one notification, so an app that takes its initial
     * reachability from the first callback is not left waiting for ever. */
    if (SUCCEEDED(hr) && handler)
    {
        handler->AddRef();
        HANDLE t = CreateThread(nullptr, 0, poke_thread, handler, 0, nullptr);
        if (t) CloseHandle(t);
        else handler->Release();
    }
    return hr;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.Networking.Connectivity.NetworkInformation"))
        return REGDB_E_CLASSNOTREG;

    statics_vtbl[0]  = (void *)s_QueryInterface;
    statics_vtbl[1]  = (void *)s_AddRef;
    statics_vtbl[2]  = (void *)s_Release;
    statics_vtbl[3]  = (void *)s_GetIids;
    statics_vtbl[4]  = (void *)s_GetRuntimeClassName;
    statics_vtbl[5]  = (void *)s_GetTrustLevel;
    statics_vtbl[6]  = (void *)s_GetConnectionProfiles;
    statics_vtbl[7]  = (void *)s_GetInternetConnectionProfile;
    statics_vtbl[8]  = (void *)s_GetLanIdentifiers;
    statics_vtbl[9]  = (void *)s_GetHostNames;
    statics_vtbl[10] = (void *)s_GetProxyConfigurationAsync;
    statics_vtbl[11] = (void *)s_GetSortedEndpointPairs;
    statics_vtbl[12] = (void *)s_add_NetworkStatusChanged;
    statics_vtbl[13] = (void *)s_remove_NetworkStatusChanged;
    statics_obj.vtbl = statics_vtbl;
    InterlockedIncrement(&statics_obj.ref);
    *out = (IActivationFactory *)&statics_obj.vtbl;
    trace("serving NetworkInformation");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

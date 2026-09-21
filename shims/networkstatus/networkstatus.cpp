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
static const GUID IID_IConnectionProfile_ =
    {0x71ba143c,0x598e,0x49d0,{0x84,0xeb,0x8f,0xeb,0xae,0xdc,0xc1,0x95}};
static const GUID IID_INetworkAdapter_ =
    {0x3b542e03,0x5388,0x496c,{0xa8,0xa3,0xaf,0xfd,0x39,0xae,0xc2,0xe6}};
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


/* ---------------------------------------------------------------------------------------------
 * INetworkAdapter, and a wrapper for the ConnectionProfile that hands one out.
 *
 * This is the actual reason Solitaire never leaves its splash screen.
 * `Sgi::Network::ReachableWinRT`'s status handler does, in order:
 *
 *     NetworkInformation::GetInternetConnectionProfile()      (statics slot 7)
 *     profile->GetNetworkConnectivityLevel()                  (slot 7)   -> needs > 1
 *     profile.as<IConnectionProfile2>()->get_IsWlan/Wwan...   (slots 6,7)
 *     profile->get_NetworkAdapter()                           (slot 11)  <-- E_NOTIMPL in Wine
 *     adapter->get_OutboundMaxBitsPerSecond()                 (slot 6)
 *     adapter->get_IanaInterfaceType()                        (slot 8)
 *
 * The slot offsets were read straight off the disassembly (`0x58(%rax)` = slot 11, `0x30` = 6,
 * `0x40` = 8), which is what pinned this down: everything else on that list Wine implements, and
 * `get_NetworkAdapter` is the single stub. It returns E_NOTIMPL, the handler bails, the status is
 * never computed as reachable, and every component that registered a
 * `(bool, Reachable::Status)` callback waits for ever - `UserDataFlowAppComponent::Initialize`,
 * `BootflowItemThemes::BeginInit`, `DlcDownloaderAppComponent::Initialize`, `SolitaireApp::Startup`.
 * A `ud2` on that callback's `_Func_impl_no_alloc::_Do_call` confirms it never fires.
 *
 * I had dismissed this stub earlier on the grounds that the app "kept logging 200 lines past it".
 * That was bad reasoning and worth remembering: a process continuing to run is not evidence that
 * the computation which hit the stub succeeded.
 *
 * The values reported are the honest ones for this host: a wired adapter
 * (IANA ifType 6, ethernetCsmacd) at 1 Gbit/s, which is what "not metered, not mobile" looks like.
 * Nothing here invents connectivity - the *level* still comes from Wine's real
 * NetworkListManager, which reports IPV4_INTERNET|IPV6_INTERNET on this machine.
 * ------------------------------------------------------------------------------------------- */
#define ADAPTER_SLOTS 12
static const void *adapter_vtbl[ADAPTER_SLOTS];
struct Adapter { const void **vtbl; LONG ref; };

static HRESULT WINAPI ad_QueryInterface(void *iface, REFIID iid, void **out)
{
    Adapter *a = (Adapter *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_INetworkAdapter_))
    {
        InterlockedIncrement(&a->ref);
        *out = &a->vtbl;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI ad_AddRef(void *i) { return InterlockedIncrement(&((Adapter *)i)->ref); }
static ULONG WINAPI ad_Release(void *i)
{
    Adapter *a = (Adapter *)i;
    ULONG r = InterlockedDecrement(&a->ref);
    if (!r) free(a);
    return r;
}
static HRESULT WINAPI ad_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI ad_GetRuntimeClassName(void *i, HSTRING *n)
{
    static const WCHAR cls[] = L"Windows.Networking.Connectivity.NetworkAdapter";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, n);
}
static HRESULT WINAPI ad_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static HRESULT WINAPI ad_get_OutboundMaxBitsPerSecond(void *i, UINT64 *v)
{
    if (!v) return E_POINTER;
    *v = 1000000000ull;                 /* 1 Gbit/s: a wired link, not a metered one */
    trace("get_OutboundMaxBitsPerSecond -> 1 Gbit/s");
    return S_OK;
}
static HRESULT WINAPI ad_get_InboundMaxBitsPerSecond(void *i, UINT64 *v)
{
    if (!v) return E_POINTER;
    *v = 1000000000ull;
    return S_OK;
}
static HRESULT WINAPI ad_get_IanaInterfaceType(void *i, UINT32 *v)
{
    if (!v) return E_POINTER;
    *v = 6;                             /* IANA ifType 6 = ethernetCsmacd */
    trace("get_IanaInterfaceType -> 6 (ethernet)");
    return S_OK;
}
static HRESULT WINAPI ad_get_NetworkItem(void *i, void **v)
{
    /* Not reached by the reachability path; traced so a later run says if that changes. */
    trace("get_NetworkItem -> E_NOTIMPL (not implemented; nothing has needed it)");
    if (v) *v = nullptr;
    return E_NOTIMPL;
}
static HRESULT WINAPI ad_get_NetworkAdapterId(void *i, GUID *v)
{
    /* A stable, obviously-synthetic id. Callers use it only as a key. */
    static const GUID id = {0x57494e45,0x0001,0x0001,{0x77,0x69,0x6e,0x65,0x75,0x77,0x70,0x00}};
    if (!v) return E_POINTER;
    *v = id;
    return S_OK;
}
static HRESULT WINAPI ad_GetConnectedProfileAsync(void *i, void **op)
{
    trace("GetConnectedProfileAsync -> E_NOTIMPL (not implemented; nothing has needed it)");
    if (op) *op = nullptr;
    return E_NOTIMPL;
}

static void init_adapter_vtbl(void)
{
    if (adapter_vtbl[0]) return;
    adapter_vtbl[0]  = (void *)ad_QueryInterface;
    adapter_vtbl[1]  = (void *)ad_AddRef;
    adapter_vtbl[2]  = (void *)ad_Release;
    adapter_vtbl[3]  = (void *)ad_GetIids;
    adapter_vtbl[4]  = (void *)ad_GetRuntimeClassName;
    adapter_vtbl[5]  = (void *)ad_GetTrustLevel;
    adapter_vtbl[6]  = (void *)ad_get_OutboundMaxBitsPerSecond;
    adapter_vtbl[7]  = (void *)ad_get_InboundMaxBitsPerSecond;
    adapter_vtbl[8]  = (void *)ad_get_IanaInterfaceType;
    adapter_vtbl[9]  = (void *)ad_get_NetworkItem;
    adapter_vtbl[10] = (void *)ad_get_NetworkAdapterId;
    adapter_vtbl[11] = (void *)ad_GetConnectedProfileAsync;
}

/* The profile wrapper: 15 real slots, padded - a vtable shorter than the interface is a wild jump,
 * not an error. Everything forwards to Wine's profile except slot 11. */
#define PROFILE_SLOTS 20
static const void *profile_vtbl[PROFILE_SLOTS];
struct Profile { const void **vtbl; LONG ref; IUnknown *real; void **real_vtbl; };

static HRESULT WINAPI pr_QueryInterface(void *iface, REFIID iid, void **out)
{
    Profile *p = (Profile *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IConnectionProfile_))
    {
        InterlockedIncrement(&p->ref);
        *out = &p->vtbl;
        return S_OK;
    }
    /* IConnectionProfile2/3/4 are Wine's and work - hand the real object over for those. */
    if (IsEqualGUID(iid, IID_IReferenceTracker_)) { *out = nullptr; return E_NOINTERFACE; }
    return p->real->QueryInterface(iid, out);
}
static ULONG WINAPI pr_AddRef(void *i) { return InterlockedIncrement(&((Profile *)i)->ref); }
static ULONG WINAPI pr_Release(void *i)
{
    Profile *p = (Profile *)i;
    ULONG r = InterlockedDecrement(&p->ref);
    if (!r) { p->real->Release(); free(p); }
    return r;
}

#define PROFILE_FORWARD(name, slot)                                                            \
    static HRESULT WINAPI name(void *iface, void *a, void *b, void *c, void *d)                \
    {                                                                                          \
        Profile *p = (Profile *)iface;                                                         \
        auto fn = reinterpret_cast<HRESULT (WINAPI *)(void *, void *, void *, void *, void *)>(\
            p->real_vtbl[slot]);                                                               \
        return fn((void *)p->real, a, b, c, d);                                                \
    }
PROFILE_FORWARD(pr_GetIids,                        3)
PROFILE_FORWARD(pr_GetRuntimeClassName,            4)
PROFILE_FORWARD(pr_GetTrustLevel,                  5)
PROFILE_FORWARD(pr_get_ProfileName,                6)
PROFILE_FORWARD(pr_GetNetworkConnectivityLevel,    7)
PROFILE_FORWARD(pr_GetNetworkNames,                8)
PROFILE_FORWARD(pr_GetConnectionCost,              9)
PROFILE_FORWARD(pr_GetDataPlanStatus,             10)
PROFILE_FORWARD(pr_GetLocalUsage,                 12)
PROFILE_FORWARD(pr_GetLocalUsagePerRoamingStates, 13)
PROFILE_FORWARD(pr_get_NetworkSecuritySettings,   14)

/* The one slot this wrapper exists for. */
static HRESULT WINAPI pr_get_NetworkAdapter(void *iface, void **adapter)
{
    Adapter *a;
    if (!adapter) return E_POINTER;
    *adapter = nullptr;
    if (!(a = (Adapter *)calloc(1, sizeof(*a)))) return E_OUTOFMEMORY;
    init_adapter_vtbl();
    a->vtbl = adapter_vtbl;
    a->ref = 1;
    *adapter = &a->vtbl;
    trace("get_NetworkAdapter -> %p (Wine returns E_NOTIMPL here)", *adapter);
    return S_OK;
}

/* Anything past the known end of the interface: answer rather than jump into whatever follows. */
static HRESULT WINAPI pr_unknown_slot(void *iface, void *a, void *b, void *c, void *d)
{
    trace("an unexpected ConnectionProfile slot was called -> E_NOTIMPL");
    return E_NOTIMPL;
}

static void init_profile_vtbl(void)
{
    if (profile_vtbl[0]) return;
    for (unsigned i = 0; i < PROFILE_SLOTS; i++) profile_vtbl[i] = (void *)pr_unknown_slot;
    profile_vtbl[0]  = (void *)pr_QueryInterface;
    profile_vtbl[1]  = (void *)pr_AddRef;
    profile_vtbl[2]  = (void *)pr_Release;
    profile_vtbl[3]  = (void *)pr_GetIids;
    profile_vtbl[4]  = (void *)pr_GetRuntimeClassName;
    profile_vtbl[5]  = (void *)pr_GetTrustLevel;
    profile_vtbl[6]  = (void *)pr_get_ProfileName;
    profile_vtbl[7]  = (void *)pr_GetNetworkConnectivityLevel;
    profile_vtbl[8]  = (void *)pr_GetNetworkNames;
    profile_vtbl[9]  = (void *)pr_GetConnectionCost;
    profile_vtbl[10] = (void *)pr_GetDataPlanStatus;
    profile_vtbl[11] = (void *)pr_get_NetworkAdapter;
    profile_vtbl[12] = (void *)pr_GetLocalUsage;
    profile_vtbl[13] = (void *)pr_GetLocalUsagePerRoamingStates;
    profile_vtbl[14] = (void *)pr_get_NetworkSecuritySettings;
}

static void wrap_profile(void **profile)
{
    IUnknown *real = (IUnknown *)*profile;
    IUnknown *base = nullptr;
    Profile *p;

    if (!real) return;
    if (FAILED(real->QueryInterface(IID_IConnectionProfile_, (void **)&base)) || !base)
    { trace("profile %p does not answer IConnectionProfile - not wrapping", real); return; }
    if (!(p = (Profile *)calloc(1, sizeof(*p)))) { base->Release(); return; }
    init_profile_vtbl();
    p->vtbl = profile_vtbl;
    p->ref = 1;
    p->real = base;                  /* owns the QI reference */
    p->real_vtbl = *(void ***)base;
    real->Release();                 /* drop the caller's original */
    *profile = &p->vtbl;
    trace("wrapped connection profile %p as %p", base, *profile);
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
FORWARD(s_GetLanIdentifiers,           8)
FORWARD(s_GetHostNames,                9)
FORWARD(s_GetProxyConfigurationAsync, 10)
FORWARD(s_GetSortedEndpointPairs,     11)
FORWARD(s_remove_NetworkStatusChanged,13)

static HRESULT WINAPI s_GetInternetConnectionProfile(void *iface, void **profile)
{
    HRESULT hr;
    if (!load_real()) return E_NOINTERFACE;
    auto fn = real_slot<HRESULT (WINAPI *)(void *, void **)>(7);
    hr = fn(real_statics, profile);
    if (SUCCEEDED(hr) && profile && *profile) wrap_profile(profile);
    return hr;
}

/* A WinRT delegate: Invoke is slot 3. The sender for this event is null on Windows too - the
 * handler is expected to go and read the current status for itself, which is exactly what we want,
 * because then the answer comes from Wine's real connectivity data and not from this shim. */
typedef HRESULT (WINAPI *status_invoke)(void *handler, void *sender);

/* Fired more than once, on purpose. `ReachableWinRT::Startup` registers this handler very early -
 * about 0.25 s in - but the components that wait on reachability register their own callbacks
 * later still (`UserDataFlowAppComponent::Initialize` runs at ~1.5 s). A single notification is
 * therefore delivered before anyone is listening, and an implementation that only fans out on a
 * *change* never tells the late registrants anything. Windows raises this event whenever the
 * network moves, so repeating it is in keeping with the contract rather than a trick; the payload
 * is not invented either, because the handler goes and re-reads the real state each time. */
#define POKE_COUNT   20
#define POKE_PERIOD  1500

static DWORD WINAPI poke_thread(void *param)
{
    IUnknown *handler = (IUnknown *)param;
    Sleep(250);
    for (int i = 0; i < POKE_COUNT; i++)
    {
        HRESULT hr = ((status_invoke)(*(void ***)handler)[3])(handler, nullptr);
        if (i == 0 || getenv("WINE_UWP_NETSTATUS_TRACE"))
            trace("fired NetworkStatusChanged #%d for handler %p -> %#lx", i + 1, handler, hr);
        Sleep(POKE_PERIOD);
    }
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

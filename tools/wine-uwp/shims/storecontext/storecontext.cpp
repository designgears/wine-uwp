// Windows.Services.Store.StoreContext: a traced stand-in, to find out what the app actually calls.
//
// This is deliberately a *diagnostic* first, not an implementation. StoreContext is the last thing
// Solitaire touches before its startup goes quiet forever:
//
//     RoGetActivationFactory (L"Windows.Services.Store.StoreContext", {9c06ee5f-...})
//     RoGetActivationFactory (L"Windows.Services.Store.Internal.StoreContextServer", ...)
//     err:ole:create_server class {22f5b1df-7d7a-4d21-97f8-c21aefba859c} not registered
//     ... then nothing but the idle pump
//
// {22f5b1df-...} is "ContainerHostActivation" in the Windows hive - an out-of-process server that
// cannot exist here. That is the same shape as the TaskbarManager/ImmersiveShell deadlock, so the
// Store is the obvious suspect for the subscriber that never returns from the event raise inside
// AppHost::EngineAppStartup. But "obvious" is not evidence, and IStoreContext has 26+ slots:
// guessing which ones matter, and what a plausible non-null answer looks like for each, is exactly
// the kind of forward-reasoning-from-a-mechanism that AGENTS.md warns about.
//
// So every slot gets its own traced thunk. Set WINE_UWP_STORE_TRACE=1 and the app tells us which
// methods it calls, in order. Only then is it worth implementing them properly.
//
// Two slots are answered rather than refused, because a refusal there is known to be fatal:
// add_/remove_OfflineLicensesChanged. Registering an event has to succeed even when the event can
// never fire.
//
// Everything else returns E_NOTIMPL. That will throw in C++/WinRT - which is the point: a throw
// names the method and unblocks the thread, where a hang tells us nothing. E_NOTIMPL is also an
// answer callers are written to handle, unlike an invented value.
//
//   IStoreContextStatics {9c06ee5f-15c0-4e72-9330-d6191cebd19c}:  6 GetDefault  7 GetForUser
//   IStoreContext        {ac98b6be-f4fd-4912-babd-5035e5e8bcab}:
//      6 get_User                       7 add_OfflineLicensesChanged
//      8 remove_OfflineLicensesChanged  9 GetCustomerPurchaseIdAsync
//     10 GetCustomerCollectionsIdAsync 11 GetAppLicenseAsync
//     12 GetStoreProductForCurrentAppAsync          13 GetStoreProductsAsync
//     14 GetAssociatedStoreProductsAsync            15 ...WithPagingAsync
//     16 GetUserCollectionAsync                     17 ...WithPagingAsync
//     18 ReportConsumableFulfillmentAsync           19 GetConsumableBalanceRemainingAsync
//     20 AcquireStoreLicenseForOptionalPackageAsync 21 RequestPurchaseAsync
//     22 RequestPurchaseAsync                       23 GetAppAndOptionalStorePackageUpdatesAsync
//     24 RequestDownloadStorePackageUpdatesAsync
//     25 RequestDownloadAndInstallStorePackageUpdatesAsync
//     26 RequestDownloadAndInstallStorePackagesAsync
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>

static const GUID IID_IStoreContextStatics_ =
    {0x9c06ee5f,0x15c0,0x4e72,{0x93,0x30,0xd6,0x19,0x1c,0xeb,0xd1,0x9c}};
static const GUID IID_IStoreContext_ =
    {0xac98b6be,0xf4fd,0x4912,{0xba,0xbd,0x50,0x35,0xe5,0xe8,0xbc,0xab}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};

/* Always on: the whole point of this shim is the trace, and it is only installed deliberately. */
static void slotlog(const char *what)
{
    fprintf(stderr, "[storecontext] %s\n", what);
    fflush(stderr);
}

struct Ctx { const void **vtbl; LONG ref; LONGLONG next_token; };
static Ctx ctx_obj;
/* Generously sized: IStoreContext is 27 slots today, and a vtable shorter than the interface is a
 * wild jump rather than an error (AGENTS.md), so pad well past the known end. */
#define CTX_SLOTS 48
static const void *ctx_vtbl[CTX_SLOTS];

static HRESULT WINAPI ctx_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IStoreContext_))
    {
        InterlockedIncrement(&ctx_obj.ref);
        *out = &ctx_obj.vtbl;
        return S_OK;
    }
    {
        /* Log the IID so the later revisions the app wants show up by name in the trace rather
         * than as a silent E_NOINTERFACE. */
        char buf[128];
        const GUID *g = &iid;
        snprintf(buf, sizeof(buf),
                 "QueryInterface refused {%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x}",
                 (unsigned long)g->Data1, g->Data2, g->Data3,
                 g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
                 g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
        slotlog(buf);
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI ctx_AddRef(void *iface)  { return InterlockedIncrement(&ctx_obj.ref); }
static ULONG WINAPI ctx_Release(void *iface) { return InterlockedDecrement(&ctx_obj.ref); }
static HRESULT WINAPI ctx_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI ctx_GetRuntimeClassName(void *i, HSTRING *n)
{
    static const WCHAR cls[] = L"Windows.Services.Store.StoreContext";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, n);
}
static HRESULT WINAPI ctx_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* One thunk per slot. Six pointer arguments covers every signature in this interface; none of them
 * is read, and nothing is written through any of them - the out-parameter position differs per
 * method and guessing it writes eight bytes into the wrong place. */
#define CTX_THUNK(n)                                                                   \
    static HRESULT WINAPI ctx_slot_##n(void *iface, void *a, void *b, void *c,         \
                                       void *d, void *e)                              \
    {                                                                                  \
        slotlog("slot " #n " called -> E_NOTIMPL");                                    \
        return E_NOTIMPL;                                                              \
    }
CTX_THUNK(6)  CTX_THUNK(9)  CTX_THUNK(10) CTX_THUNK(11) CTX_THUNK(12) CTX_THUNK(13)
CTX_THUNK(14) CTX_THUNK(15) CTX_THUNK(16) CTX_THUNK(17) CTX_THUNK(18) CTX_THUNK(19)
CTX_THUNK(20) CTX_THUNK(21) CTX_THUNK(22) CTX_THUNK(23) CTX_THUNK(24) CTX_THUNK(25)
CTX_THUNK(26) CTX_THUNK(27) CTX_THUNK(28) CTX_THUNK(29) CTX_THUNK(30) CTX_THUNK(31)
CTX_THUNK(32) CTX_THUNK(33) CTX_THUNK(34) CTX_THUNK(35) CTX_THUNK(36) CTX_THUNK(37)
CTX_THUNK(38) CTX_THUNK(39) CTX_THUNK(40) CTX_THUNK(41) CTX_THUNK(42) CTX_THUNK(43)
CTX_THUNK(44) CTX_THUNK(45) CTX_THUNK(46) CTX_THUNK(47)

/* Event registration must succeed even though the event can never fire. */
static HRESULT WINAPI ctx_add_OfflineLicensesChanged(void *iface, void *handler, LONGLONG *token)
{
    slotlog("slot 7 add_OfflineLicensesChanged -> S_OK");
    if (!token) return E_POINTER;
    *token = ++ctx_obj.next_token;
    return S_OK;
}
static HRESULT WINAPI ctx_remove_OfflineLicensesChanged(void *iface, LONGLONG token)
{
    slotlog("slot 8 remove_OfflineLicensesChanged -> S_OK");
    return S_OK;
}

static void init_ctx(void)
{
    if (ctx_obj.vtbl) return;
    ctx_vtbl[0] = (void *)ctx_QueryInterface;
    ctx_vtbl[1] = (void *)ctx_AddRef;
    ctx_vtbl[2] = (void *)ctx_Release;
    ctx_vtbl[3] = (void *)ctx_GetIids;
    ctx_vtbl[4] = (void *)ctx_GetRuntimeClassName;
    ctx_vtbl[5] = (void *)ctx_GetTrustLevel;
#define SET(n) ctx_vtbl[n] = (void *)ctx_slot_##n;
    SET(6)
    ctx_vtbl[7] = (void *)ctx_add_OfflineLicensesChanged;
    ctx_vtbl[8] = (void *)ctx_remove_OfflineLicensesChanged;
    SET(9)  SET(10) SET(11) SET(12) SET(13) SET(14) SET(15) SET(16) SET(17) SET(18)
    SET(19) SET(20) SET(21) SET(22) SET(23) SET(24) SET(25) SET(26) SET(27) SET(28)
    SET(29) SET(30) SET(31) SET(32) SET(33) SET(34) SET(35) SET(36) SET(37) SET(38)
    SET(39) SET(40) SET(41) SET(42) SET(43) SET(44) SET(45) SET(46) SET(47)
#undef SET
    ctx_obj.vtbl = ctx_vtbl;
}

struct Statics { const void **vtbl; LONG ref; };
static Statics statics_obj;
static const void *statics_vtbl[8];

static HRESULT WINAPI s_QueryInterface(void *i, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_IStoreContextStatics_))
    {
        InterlockedIncrement(&statics_obj.ref);
        *out = &statics_obj.vtbl;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(void *i)  { return InterlockedIncrement(&statics_obj.ref); }
static ULONG WINAPI s_Release(void *i) { return InterlockedDecrement(&statics_obj.ref); }

static HRESULT WINAPI s_GetDefault(void *i, void **out)
{
    slotlog("GetDefault");
    if (!out) return E_POINTER;
    init_ctx();
    InterlockedIncrement(&ctx_obj.ref);
    *out = &ctx_obj.vtbl;
    return S_OK;
}
static HRESULT WINAPI s_GetForUser(void *i, void *user, void **out)
{
    slotlog("GetForUser");
    return s_GetDefault(i, out);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.Services.Store.StoreContext")) return REGDB_E_CLASSNOTREG;
    statics_vtbl[0] = (void *)s_QueryInterface;
    statics_vtbl[1] = (void *)s_AddRef;
    statics_vtbl[2] = (void *)s_Release;
    statics_vtbl[3] = (void *)ctx_GetIids;
    statics_vtbl[4] = (void *)ctx_GetRuntimeClassName;
    statics_vtbl[5] = (void *)ctx_GetTrustLevel;
    statics_vtbl[6] = (void *)s_GetDefault;
    statics_vtbl[7] = (void *)s_GetForUser;
    statics_obj.vtbl = statics_vtbl;
    InterlockedIncrement(&statics_obj.ref);
    *out = (IActivationFactory *)&statics_obj.vtbl;
    slotlog("serving StoreContext");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

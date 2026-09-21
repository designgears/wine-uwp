// Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager: complete the asyncs.
//
// This is the subscriber that hangs Solitaire's startup once the earlier blockers are out of the
// way. The app's own log names it exactly:
//
//   [1.389] PremiumGameAppComponent::Initialize -- StoreAppComponent::Connect - InitializingStoreSilent
//   [1.390] XblAccountStatusHelperWinRT::PopulateXblAccountStatusAsync - Start.
//   [1.390] UserIdentityUtils::GetRpsTokenAsync - Calling with service: user.auth.xboxlive.com.
//   ... and no completion line, ever
//
// `PopulateXblAccountStatusAsync` never logs a finish, and a `ud2` on the success path of
// AppHost::EngineAppStartup still never fires, so this is the subscriber that never returns from
// the event raise inside it. The Web Account Manager needs the out-of-process token broker, which
// does not exist under Wine, so its IAsyncOperation is never completed - the same shape as the
// TaskbarManager/ImmersiveShell deadlock, and the third instance of it in this app.
//
// What this answers, and why:
//
//   FindAccountProviderAsync -> a *completed* async whose result is **null**. Null is the
//   documented answer for "no such provider", which is the truth here: there is no Xbox Live
//   account provider on this machine. This is the one place in this shim where null is correct
//   rather than dangerous - AGENTS.md's "null is not a neutral answer" is about getters whose
//   result the caller immediately uses, and a provider lookup that can legitimately find nothing
//   is not one of those. The app should conclude it cannot sign in and carry on as Guest, which is
//   what it already logged ("starting as ID[Guest]").
//
//   GetTokenSilentlyAsync / RequestTokenAsync / FindAccountAsync -> traced E_NOTIMPL. These return
//   a WebTokenRequestResult, and handing back a null one would be exactly the mistake above: the
//   caller reads ResponseStatus off it straight away. Rather than invent a result object, these
//   report that they are not implemented and log that they were reached, so the next run says
//   whether the app needs them at all. If it does, the right answer is a real
//   WebTokenRequestResult with ResponseStatus = UserInteractionRequired, which is what Windows
//   returns when silent auth cannot succeed.
//
//   IWebAuthenticationCoreManagerStatics {6aca7c92-a581-4479-9c10-752eff44fd34}:
//      6 GetTokenSilentlyAsync   7 GetTokenSilentlyAsync   8 RequestTokenAsync
//      9 RequestTokenAsync      10 FindAccountAsync       11 FindAccountProviderAsync
//     12 FindAccountProviderAsync
//
// IAsyncOperation<WebAccountProvider> {88c66009-12f7-58e2-8dbe-6efc620c85ba} is parameterised and
// in no .winmd; derived with piid.py from WebAccountProvider's default interface
// IWebAccountProvider {29dcc8c3-7ab9-4a7c-a336-b942f9dbf7c7}, read out of the shipping metadata
// rather than from an .idl - the two disagree often enough that AGENTS.md calls it out.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>

static const GUID IID_IWebAuthenticationCoreManagerStatics_ =
    {0x6aca7c92,0xa581,0x4479,{0x9c,0x10,0x75,0x2e,0xff,0x44,0xfd,0x34}};
static const GUID IID_IAsyncOperation_WebAccountProvider_ =
    {0x88c66009,0x12f7,0x58e2,{0x8d,0xbe,0x6e,0xfc,0x62,0x0c,0x85,0xba}};
static const GUID IID_IAsyncInfo_ =
    {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};

static void trace(const char *msg)
{
    fprintf(stderr, "[webauthcore] %s\n", msg);
    fflush(stderr);
}

/* A completed IAsyncOperation<WebAccountProvider> whose result is null. */
struct AsyncNull
{
    const void **op_vtbl;
    const void **info_vtbl;
    LONG ref;
    IUnknown *handler;
};
static const void *anull_op_vtbl[9];
static const void *anull_info_vtbl[11];
static AsyncNull *from_op(void *i)   { return (AsyncNull *)i; }
static AsyncNull *from_info(void *i) { return (AsyncNull *)((char *)i - sizeof(void *)); }

static HRESULT anull_qi(AsyncNull *a, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IAsyncOperation_WebAccountProvider_))
    {
        InterlockedIncrement(&a->ref);
        *out = &a->op_vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IAsyncInfo_))
    {
        InterlockedIncrement(&a->ref);
        *out = &a->info_vtbl;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG anull_release(AsyncNull *a)
{
    ULONG ref = InterlockedDecrement(&a->ref);
    if (!ref) { if (a->handler) a->handler->Release(); free(a); }
    return ref;
}
static HRESULT WINAPI op_QI(void *i, REFIID iid, void **o) { return anull_qi(from_op(i), iid, o); }
static ULONG   WINAPI op_AddRef(void *i)  { return InterlockedIncrement(&from_op(i)->ref); }
static ULONG   WINAPI op_Release(void *i) { return anull_release(from_op(i)); }
static HRESULT WINAPI info_QI(void *i, REFIID iid, void **o) { return anull_qi(from_info(i), iid, o); }
static ULONG   WINAPI info_AddRef(void *i)  { return InterlockedIncrement(&from_info(i)->ref); }
static ULONG   WINAPI info_Release(void *i) { return anull_release(from_info(i)); }

static HRESULT WINAPI a_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI a_GetRuntimeClassName(void *i, HSTRING *n)
{
    static const WCHAR cls[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Security.Credentials.WebAccountProvider>";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, n);
}
static HRESULT WINAPI a_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* A delegate is three IUnknown slots then Invoke at slot 3. */
typedef HRESULT (WINAPI *completed_invoke)(void *handler, void *async, int status);

static HRESULT WINAPI op_put_Completed(void *iface, IUnknown *handler)
{
    AsyncNull *a = from_op(iface);
    if (a->handler) { a->handler->Release(); a->handler = nullptr; }
    if (handler)
    {
        handler->AddRef();
        a->handler = handler;
        ((completed_invoke)(*(void ***)handler)[3])(handler, &a->op_vtbl, 1 /* Completed */);
    }
    return S_OK;
}
static HRESULT WINAPI op_get_Completed(void *iface, IUnknown **h)
{
    AsyncNull *a = from_op(iface);
    if (!h) return E_POINTER;
    if ((*h = a->handler)) a->handler->AddRef();
    return S_OK;
}
static HRESULT WINAPI op_GetResults(void *iface, void **result)
{
    if (!result) return E_POINTER;
    *result = nullptr;          /* "no such account provider" - the documented answer */
    return S_OK;
}
static HRESULT WINAPI info_get_Id(void *i, UINT32 *id)      { if (!id) return E_POINTER; *id = 1; return S_OK; }
static HRESULT WINAPI info_get_Status(void *i, int *s)      { if (!s) return E_POINTER; *s = 1; return S_OK; }
static HRESULT WINAPI info_get_ErrorCode(void *i, HRESULT *e){ if (!e) return E_POINTER; *e = S_OK; return S_OK; }
static HRESULT WINAPI info_Cancel(void *i)                  { return S_OK; }
static HRESULT WINAPI info_Close(void *i)                   { return S_OK; }

static HRESULT make_completed_null_async(void **out)
{
    AsyncNull *a;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!(a = (AsyncNull *)calloc(1, sizeof(*a)))) return E_OUTOFMEMORY;
    if (!anull_op_vtbl[0])
    {
        anull_op_vtbl[0] = (void *)op_QI;      anull_op_vtbl[1] = (void *)op_AddRef;
        anull_op_vtbl[2] = (void *)op_Release; anull_op_vtbl[3] = (void *)a_GetIids;
        anull_op_vtbl[4] = (void *)a_GetRuntimeClassName;
        anull_op_vtbl[5] = (void *)a_GetTrustLevel;
        anull_op_vtbl[6] = (void *)op_put_Completed;
        anull_op_vtbl[7] = (void *)op_get_Completed;
        anull_op_vtbl[8] = (void *)op_GetResults;

        anull_info_vtbl[0] = (void *)info_QI;      anull_info_vtbl[1] = (void *)info_AddRef;
        anull_info_vtbl[2] = (void *)info_Release; anull_info_vtbl[3] = (void *)a_GetIids;
        anull_info_vtbl[4] = (void *)a_GetRuntimeClassName;
        anull_info_vtbl[5] = (void *)a_GetTrustLevel;
        anull_info_vtbl[6] = (void *)info_get_Id;
        anull_info_vtbl[7] = (void *)info_get_Status;
        anull_info_vtbl[8] = (void *)info_get_ErrorCode;
        anull_info_vtbl[9] = (void *)info_Cancel;
        anull_info_vtbl[10] = (void *)info_Close;
    }
    a->op_vtbl = anull_op_vtbl;
    a->info_vtbl = anull_info_vtbl;
    a->ref = 1;
    *out = &a->op_vtbl;
    return S_OK;
}

struct Statics { const void **vtbl; LONG ref; };
static Statics statics_obj;
static const void *statics_vtbl[16];

static HRESULT WINAPI s_QueryInterface(void *i, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_IWebAuthenticationCoreManagerStatics_))
    {
        InterlockedIncrement(&statics_obj.ref);
        *out = &statics_obj.vtbl;
        return S_OK;
    }
    /* Statics2..5 are refused deliberately and logged, so a run says whether the app wants them. */
    {
        char buf[128]; const GUID *g = &iid;
        snprintf(buf, sizeof(buf), "refused interface {%08lx-%04x-%04x-...}",
                 (unsigned long)g->Data1, g->Data2, g->Data3);
        trace(buf);
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(void *i)  { return InterlockedIncrement(&statics_obj.ref); }
static ULONG WINAPI s_Release(void *i) { return InterlockedDecrement(&statics_obj.ref); }
static HRESULT WINAPI s_GetRuntimeClassName(void *i, HSTRING *n)
{
    static const WCHAR cls[] = L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, n);
}

#define NOTIMPL(name, label)                                                            \
    static HRESULT WINAPI name(void *i, void *a, void *b, void *c, void *d)             \
    {                                                                                   \
        trace(label " -> E_NOTIMPL (needs a real WebTokenRequestResult; see the header)"); \
        return E_NOTIMPL;                                                               \
    }
NOTIMPL(s_GetTokenSilentlyAsync,   "slot 6 GetTokenSilentlyAsync")
NOTIMPL(s_GetTokenSilentlyAsync2,  "slot 7 GetTokenSilentlyAsync")
NOTIMPL(s_RequestTokenAsync,       "slot 8 RequestTokenAsync")
NOTIMPL(s_RequestTokenAsync2,      "slot 9 RequestTokenAsync")
NOTIMPL(s_FindAccountAsync,        "slot 10 FindAccountAsync")

/* The out-parameter is the last argument in both overloads: (providerId, op) and
 * (providerId, authority, op). */
static HRESULT WINAPI s_FindAccountProviderAsync(void *i, void *provider_id, void **op)
{
    trace("slot 11 FindAccountProviderAsync -> completed, no provider");
    return make_completed_null_async(op);
}
static HRESULT WINAPI s_FindAccountProviderAsync2(void *i, void *provider_id, void *authority,
                                                  void **op)
{
    trace("slot 12 FindAccountProviderAsync(authority) -> completed, no provider");
    return make_completed_null_async(op);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager"))
        return REGDB_E_CLASSNOTREG;
    statics_vtbl[0]  = (void *)s_QueryInterface;
    statics_vtbl[1]  = (void *)s_AddRef;
    statics_vtbl[2]  = (void *)s_Release;
    statics_vtbl[3]  = (void *)a_GetIids;
    statics_vtbl[4]  = (void *)s_GetRuntimeClassName;
    statics_vtbl[5]  = (void *)a_GetTrustLevel;
    statics_vtbl[6]  = (void *)s_GetTokenSilentlyAsync;
    statics_vtbl[7]  = (void *)s_GetTokenSilentlyAsync2;
    statics_vtbl[8]  = (void *)s_RequestTokenAsync;
    statics_vtbl[9]  = (void *)s_RequestTokenAsync2;
    statics_vtbl[10] = (void *)s_FindAccountAsync;
    statics_vtbl[11] = (void *)s_FindAccountProviderAsync;
    statics_vtbl[12] = (void *)s_FindAccountProviderAsync2;
    statics_obj.vtbl = statics_vtbl;
    InterlockedIncrement(&statics_obj.ref);
    *out = (IActivationFactory *)&statics_obj.vtbl;
    trace("serving WebAuthenticationCoreManager");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

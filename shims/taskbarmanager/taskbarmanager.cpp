// Windows.UI.Shell.TaskbarManager: a stand-in whose async methods actually complete.
//
// This is what stops Microsoft Solitaire Collection dead. Near the end of its startup the app
// asks whether it is pinned to the taskbar. The real implementation reaches for the shell:
//
//     fixme:combase:RoGetActivationFactory (L"Windows.UI.Shell.TaskbarManager", {db32ab74-...})
//     fixme:combase:RoGetActivationFactory (L"Windows.Internal.ApplicationModel.TaskbarPinnableSurface", ...)
//     err:ole:create_server class {c2f03a33-21f5-47fa-b4bb-156362a2f239} not registered
//
// {c2f03a33-...} is ImmersiveShell, which does not exist under Wine. The IAsyncOperation the app
// is holding is therefore never completed - not failed, never completed - and the app's blocking
// wait on it never returns. The thread parks in windows.ui.dll and the whole startup stops there:
//
//     AppHost::EngineAppStartup -- START      (never reaches END)
//     WinUwp::BootStrap -- Startup Began      (never reaches "Startup Finished")
//
// and because the bootstrap never finishes, the engine's render loop never starts. Nothing is ever
// presented (Dx11RenderWindow::SwapBuffers never executes), which is why the window is a flat
// 1280x800 of the XAML page's background. The 1x1 swap chain that looked like the bug is just the
// size the engine created before its first resize - a symptom two layers downstream.
//
// A never-completing async is the worst possible answer, worse than a failure: there is no error
// to handle and no timeout. The rule in AGENTS.md is that a refusal is usually the fatal thing;
// this is the same rule one level up - an *async* refusal has to still complete.
//
// So: IsSupported/IsPinningAllowed are false (this is not a shell that can pin anything), and
// every async method hands back an IAsyncOperation<bool> that is already in the Completed state
// and whose put_Completed invokes the handler immediately. Both consumption styles then work:
// C++/WinRT's .get() sees Status == Completed and calls GetResults without ever waiting, and a
// co_await gets its callback synchronously.
//
//   ITaskbarManagerStatics {db32ab74-de52-4fe6-b7b6-95ff9f8395df}:  6 GetDefault
//   ITaskbarManager        {87490a19-1ad9-49f4-b2e8-86738dc5ac40}:
//      6 get_IsSupported   7 get_IsPinningAllowed    8 IsCurrentAppPinnedAsync
//      9 IsAppListEntryPinnedAsync  10 RequestPinCurrentAppAsync  11 RequestPinAppListEntryAsync
//   ITaskbarManager2       {79f0a06e-7b02-4911-918c-dee0bbd20ba4}:
//      6 IsSecondaryTilePinnedAsync  7 RequestPinSecondaryTileAsync  8 TryUnpinSecondaryTileAsync
//
// IAsyncOperation<bool> {cdb5efb3-5788-509d-9be1-71ccb8a3362a} and
// IAsyncOperationCompletedHandler<bool> {c1d3d1a2-ae17-5a5f-b5a2-bdcc8844889a} are parameterised
// IIDs, in no .winmd; both were derived with tools/wine-uwp/piid.py (whose --self-test passes).
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

static const GUID IID_ITaskbarManagerStatics_ =
    {0xdb32ab74,0xde52,0x4fe6,{0xb7,0xb6,0x95,0xff,0x9f,0x83,0x95,0xdf}};
static const GUID IID_ITaskbarManager_ =
    {0x87490a19,0x1ad9,0x49f4,{0xb2,0xe8,0x86,0x73,0x8d,0xc5,0xac,0x40}};
static const GUID IID_ITaskbarManager2_ =
    {0x79f0a06e,0x7b02,0x4911,{0x91,0x8c,0xde,0xe0,0xbb,0xd2,0x0b,0xa4}};
static const GUID IID_IAsyncOperation_bool_ =
    {0xcdb5efb3,0x5788,0x509d,{0x9b,0xe1,0x71,0xcc,0xb8,0xa3,0x36,0x2a}};
static const GUID IID_IAsyncInfo_ =
    {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IReferenceTracker_ =
    {0x11d3b13a,0x180e,0x4789,{0xa8,0xbe,0x77,0x12,0x88,0x28,0x93,0xe6}};

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_TASKBAR_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[taskbarmanager] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------------------------------------
 * An IAsyncOperation<bool> that is born completed.
 * Two interfaces on one allocation: the IAsyncOperation vtable first, IAsyncInfo second, so an
 * IAsyncInfo pointer is the object address plus one pointer.
 * ------------------------------------------------------------------------------------------- */
struct AsyncBool
{
    const void **op_vtbl;
    const void **info_vtbl;
    LONG ref;
    boolean result;
    IUnknown *handler;
};
static const void *async_op_vtbl[9];
static const void *async_info_vtbl[11];

static AsyncBool *from_op(void *iface)   { return (AsyncBool *)iface; }
static AsyncBool *from_info(void *iface) { return (AsyncBool *)((char *)iface - sizeof(void *)); }

static HRESULT async_qi(AsyncBool *a, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IAsyncOperation_bool_))
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
    /* A plain WinRT object must refuse IReferenceTracker - claiming it and then not honouring the
     * tracking calls is what fail-fasts XAML (AGENTS.md). */
    *out = nullptr;
    return E_NOINTERFACE;
}

static ULONG async_release(AsyncBool *a)
{
    ULONG ref = InterlockedDecrement(&a->ref);
    if (!ref)
    {
        if (a->handler) a->handler->Release();
        free(a);
    }
    return ref;
}

static HRESULT WINAPI aop_QueryInterface(void *i, REFIID iid, void **o) { return async_qi(from_op(i), iid, o); }
static ULONG   WINAPI aop_AddRef(void *i)  { return InterlockedIncrement(&from_op(i)->ref); }
static ULONG   WINAPI aop_Release(void *i) { return async_release(from_op(i)); }
static HRESULT WINAPI ainfo_QueryInterface(void *i, REFIID iid, void **o) { return async_qi(from_info(i), iid, o); }
static ULONG   WINAPI ainfo_AddRef(void *i)  { return InterlockedIncrement(&from_info(i)->ref); }
static ULONG   WINAPI ainfo_Release(void *i) { return async_release(from_info(i)); }

static HRESULT WINAPI async_GetIids(void *i, ULONG *c, IID **ids)
{
    if (c) *c = 0;
    if (ids) *ids = nullptr;
    return S_OK;
}
static HRESULT WINAPI async_GetRuntimeClassName(void *i, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.Foundation.IAsyncOperation`1<Boolean>";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
}
static HRESULT WINAPI async_GetTrustLevel(void *i, TrustLevel *l)
{
    if (!l) return E_POINTER;
    *l = BaseTrust;
    return S_OK;
}

/* An IAsyncOperationCompletedHandler is a delegate: three IUnknown slots, then Invoke at slot 3 -
 * not slot 6. Calling slot 6 on one lands past the end of its vtable (AGENTS.md). */
typedef HRESULT (WINAPI *completed_invoke)(void *handler, void *async, int status);

static HRESULT WINAPI aop_put_Completed(void *iface, IUnknown *handler)
{
    AsyncBool *a = from_op(iface);
    if (a->handler) { a->handler->Release(); a->handler = nullptr; }
    if (handler)
    {
        handler->AddRef();
        a->handler = handler;
        /* Completed already, so invoke straight away. AsyncStatus::Completed == 1. */
        void **vtbl = *(void ***)handler;
        trace("put_Completed: invoking handler %p immediately", handler);
        ((completed_invoke)vtbl[3])(handler, &a->op_vtbl, 1);
    }
    return S_OK;
}
static HRESULT WINAPI aop_get_Completed(void *iface, IUnknown **handler)
{
    AsyncBool *a = from_op(iface);
    if (!handler) return E_POINTER;
    if ((*handler = a->handler)) a->handler->AddRef();
    return S_OK;
}
static HRESULT WINAPI aop_GetResults(void *iface, boolean *result)
{
    if (!result) return E_POINTER;
    *result = from_op(iface)->result;
    return S_OK;
}

static HRESULT WINAPI ainfo_get_Id(void *iface, UINT32 *id)        { if (!id) return E_POINTER; *id = 1; return S_OK; }
static HRESULT WINAPI ainfo_get_Status(void *iface, int *status)   { if (!status) return E_POINTER; *status = 1 /* Completed */; return S_OK; }
static HRESULT WINAPI ainfo_get_ErrorCode(void *iface, HRESULT *e) { if (!e) return E_POINTER; *e = S_OK; return S_OK; }
static HRESULT WINAPI ainfo_Cancel(void *iface)                    { return S_OK; }
static HRESULT WINAPI ainfo_Close(void *iface)                     { return S_OK; }

static void init_async_vtbls(void)
{
    if (async_op_vtbl[0]) return;
    async_op_vtbl[0] = (void *)aop_QueryInterface;
    async_op_vtbl[1] = (void *)aop_AddRef;
    async_op_vtbl[2] = (void *)aop_Release;
    async_op_vtbl[3] = (void *)async_GetIids;
    async_op_vtbl[4] = (void *)async_GetRuntimeClassName;
    async_op_vtbl[5] = (void *)async_GetTrustLevel;
    async_op_vtbl[6] = (void *)aop_put_Completed;
    async_op_vtbl[7] = (void *)aop_get_Completed;
    async_op_vtbl[8] = (void *)aop_GetResults;

    async_info_vtbl[0] = (void *)ainfo_QueryInterface;
    async_info_vtbl[1] = (void *)ainfo_AddRef;
    async_info_vtbl[2] = (void *)ainfo_Release;
    async_info_vtbl[3] = (void *)async_GetIids;
    async_info_vtbl[4] = (void *)async_GetRuntimeClassName;
    async_info_vtbl[5] = (void *)async_GetTrustLevel;
    async_info_vtbl[6] = (void *)ainfo_get_Id;
    async_info_vtbl[7] = (void *)ainfo_get_Status;
    async_info_vtbl[8] = (void *)ainfo_get_ErrorCode;
    async_info_vtbl[9] = (void *)ainfo_Cancel;
    async_info_vtbl[10] = (void *)ainfo_Close;
}

static HRESULT make_completed_async(boolean value, void **out)
{
    AsyncBool *a;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!(a = (AsyncBool *)calloc(1, sizeof(*a)))) return E_OUTOFMEMORY;
    init_async_vtbls();
    a->op_vtbl = async_op_vtbl;
    a->info_vtbl = async_info_vtbl;
    a->ref = 1;
    a->result = value;
    *out = &a->op_vtbl;
    trace("handing back a completed IAsyncOperation<bool> = %d", value);
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
struct Manager { const void **vtbl; const void **vtbl2; LONG ref; };
static const void *manager_vtbl[12];
static const void *manager2_vtbl[9];
static Manager manager_obj;

static Manager *from_m(void *i)  { return (Manager *)i; }
static Manager *from_m2(void *i) { return (Manager *)((char *)i - sizeof(void *)); }

static HRESULT manager_qi(Manager *m, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_ITaskbarManager_))
    {
        InterlockedIncrement(&m->ref);
        *out = &m->vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_ITaskbarManager2_))
    {
        InterlockedIncrement(&m->ref);
        *out = &m->vtbl2;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static HRESULT WINAPI m_QueryInterface(void *i, REFIID iid, void **o) { return manager_qi(from_m(i), iid, o); }
static ULONG   WINAPI m_AddRef(void *i)  { return InterlockedIncrement(&from_m(i)->ref); }
static ULONG   WINAPI m_Release(void *i) { return InterlockedDecrement(&from_m(i)->ref); }
static HRESULT WINAPI m2_QueryInterface(void *i, REFIID iid, void **o) { return manager_qi(from_m2(i), iid, o); }
static ULONG   WINAPI m2_AddRef(void *i)  { return InterlockedIncrement(&from_m2(i)->ref); }
static ULONG   WINAPI m2_Release(void *i) { return InterlockedDecrement(&from_m2(i)->ref); }

static HRESULT WINAPI m_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI m_GetRuntimeClassName(void *i, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.UI.Shell.TaskbarManager";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
}
static HRESULT WINAPI m_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static HRESULT WINAPI m_get_IsSupported(void *i, boolean *v)
{
    if (!v) return E_POINTER;
    *v = 0;                      /* there is no shell here that can pin anything */
    trace("get_IsSupported -> false");
    return S_OK;
}
static HRESULT WINAPI m_get_IsPinningAllowed(void *i, boolean *v)
{
    if (!v) return E_POINTER;
    *v = 0;
    return S_OK;
}
/* Every async answer is "no", but it is a *completed* no. */
static HRESULT WINAPI m_IsCurrentAppPinnedAsync(void *i, void **op)      { return make_completed_async(0, op); }
static HRESULT WINAPI m_IsAppListEntryPinnedAsync(void *i, void *e, void **op)  { return make_completed_async(0, op); }
static HRESULT WINAPI m_RequestPinCurrentAppAsync(void *i, void **op)    { return make_completed_async(0, op); }
static HRESULT WINAPI m_RequestPinAppListEntryAsync(void *i, void *e, void **op){ return make_completed_async(0, op); }
static HRESULT WINAPI m2_IsSecondaryTilePinnedAsync(void *i, void *t, void **op)   { return make_completed_async(0, op); }
static HRESULT WINAPI m2_RequestPinSecondaryTileAsync(void *i, void *t, void **op) { return make_completed_async(0, op); }
static HRESULT WINAPI m2_TryUnpinSecondaryTileAsync(void *i, void *t, void **op)   { return make_completed_async(0, op); }

/* ------------------------------------------------------------------------------------------- */
struct Statics { const void **vtbl; LONG ref; };
static Statics statics_obj;
static const void *statics_vtbl[7];

static HRESULT WINAPI s_QueryInterface(void *i, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_ITaskbarManagerStatics_))
    {
        InterlockedIncrement(&statics_obj.ref);
        *out = &statics_obj.vtbl;
        return S_OK;
    }
    if (!IsEqualGUID(iid, IID_IReferenceTracker_))
        trace("statics QI for an unhandled interface");
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG   WINAPI s_AddRef(void *i)  { return InterlockedIncrement(&statics_obj.ref); }
static ULONG   WINAPI s_Release(void *i) { return InterlockedDecrement(&statics_obj.ref); }
static HRESULT WINAPI s_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI s_GetRuntimeClassName(void *i, HSTRING *n) { return m_GetRuntimeClassName(i, n); }
static HRESULT WINAPI s_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static void init_manager(void);
static HRESULT WINAPI s_GetDefault(void *i, void **manager)
{
    if (!manager) return E_POINTER;
    init_manager();
    InterlockedIncrement(&manager_obj.ref);
    *manager = &manager_obj.vtbl;
    trace("GetDefault -> %p", *manager);
    return S_OK;
}

static void init_manager(void)
{
    if (manager_obj.vtbl) return;
    manager_vtbl[0]  = (void *)m_QueryInterface;
    manager_vtbl[1]  = (void *)m_AddRef;
    manager_vtbl[2]  = (void *)m_Release;
    manager_vtbl[3]  = (void *)m_GetIids;
    manager_vtbl[4]  = (void *)m_GetRuntimeClassName;
    manager_vtbl[5]  = (void *)m_GetTrustLevel;
    manager_vtbl[6]  = (void *)m_get_IsSupported;
    manager_vtbl[7]  = (void *)m_get_IsPinningAllowed;
    manager_vtbl[8]  = (void *)m_IsCurrentAppPinnedAsync;
    manager_vtbl[9]  = (void *)m_IsAppListEntryPinnedAsync;
    manager_vtbl[10] = (void *)m_RequestPinCurrentAppAsync;
    manager_vtbl[11] = (void *)m_RequestPinAppListEntryAsync;

    manager2_vtbl[0] = (void *)m2_QueryInterface;
    manager2_vtbl[1] = (void *)m2_AddRef;
    manager2_vtbl[2] = (void *)m2_Release;
    manager2_vtbl[3] = (void *)m_GetIids;
    manager2_vtbl[4] = (void *)m_GetRuntimeClassName;
    manager2_vtbl[5] = (void *)m_GetTrustLevel;
    manager2_vtbl[6] = (void *)m2_IsSecondaryTilePinnedAsync;
    manager2_vtbl[7] = (void *)m2_RequestPinSecondaryTileAsync;
    manager2_vtbl[8] = (void *)m2_TryUnpinSecondaryTileAsync;

    manager_obj.vtbl2 = manager2_vtbl;
    manager_obj.vtbl = manager_vtbl;   /* last: doubles as the initialised flag */
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.UI.Shell.TaskbarManager"))
    {
        trace("not ours: %ls", cls ? cls : L"(null)");
        return REGDB_E_CLASSNOTREG;
    }
    statics_vtbl[0] = (void *)s_QueryInterface;
    statics_vtbl[1] = (void *)s_AddRef;
    statics_vtbl[2] = (void *)s_Release;
    statics_vtbl[3] = (void *)s_GetIids;
    statics_vtbl[4] = (void *)s_GetRuntimeClassName;
    statics_vtbl[5] = (void *)s_GetTrustLevel;
    statics_vtbl[6] = (void *)s_GetDefault;
    statics_obj.vtbl = statics_vtbl;
    InterlockedIncrement(&statics_obj.ref);
    *out = (IActivationFactory *)&statics_obj.vtbl;
    trace("serving TaskbarManager");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

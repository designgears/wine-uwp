// Windows.System.Launcher: open URIs, files and folders with the host's handlers.
//
// Shapr3D's "Log In" computes a PKCE challenge (HashAlgorithmNames) and hands its sign-in URL to
// Launcher.LaunchUriAsync. The genuine Windows.System.Launcher.dll routes that through the shell:
//
//     fixme:combase:RoGetActivationFactory (L"Windows.Internal.Shell.Holographic.ContextIdentifier", ...)
//     fixme:combase:RoGetActivationFactory (L"Windows.Internal.Shell.Popups.PopupClient", ...)
//     fixme:dcomp:comp_object_nx_veh NX at 0000000000000000 ...
//     err:seh:RaiseFailFastException fail-fast: code 0x1007
//
// - there is no shell broker here, and the click took the whole app down. On Wine the equivalent
// of "open this with whatever handles it" is ShellExecute, which winebrowser forwards to the
// host's default browser (xdg-open). So the launch methods are real, and each hands back an
// IAsyncOperation<bool> that is already Completed with the outcome - the TaskbarManager shape,
// see shims/taskbarmanager for why a completed operation is the only safe kind.
//
// WINE_UWP_LAUNCHER=log: log what would be launched and report success without launching. Test
// runs on the Xvfb display must use it: xdg-open talks to the user's running browser over D-Bus,
// so a real launch opens a tab on the *real* desktop. The target is also written to
// C:\users\Public\wineuwp-launched.uri for browser-callback.sh.
//
//   ILauncherStatics  {277151c3-9e3e-42f6-91a4-5dfdeb232451}:
//      6 LaunchFileAsync(file)  7 LaunchFileAsync(file, options)
//      8 LaunchUriAsync(uri)    9 LaunchUriAsync(uri, options)
//   ILauncherStatics2 {59ba2fbb-24cb-4c02-a4c4-8294569d54f1}:
//      8 LaunchUriAsync(uri, options, inputData); the ForResults / Query / Find methods are traced
//   ILauncherStatics3 {234261a8-9db3-4683-aa42-dc6f51d33847}:
//      6 LaunchFolderAsync(folder)  7 LaunchFolderAsync(folder, options)
//   ILauncherStatics5 {5b24ef84-d895-5fea-9153-1ac49aed9ba9}:
//      6 LaunchFolderPathAsync(path)  7 (path, options)  8 ForUser(user, path)  9 (user, path, options)
//   ILauncherStatics4 {b9ec819f-b5a5-41c6-b3b3-dd1b3178bcf2}:
//      9 LaunchUriForUserAsync(user, uri)  10 (user, uri, options)  11 (user, uri, options, data)
//
// IAsyncOperation<bool> {cdb5efb3-5788-509d-9be1-71ccb8a3362a} is a parameterised IID (piid.py).
#include <windows.h>
#include <shellapi.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

static const GUID IID_ILauncherStatics_  = {0x277151c3,0x9e3e,0x42f6,{0x91,0xa4,0x5d,0xfd,0xeb,0x23,0x24,0x51}};
static const GUID IID_ILauncherStatics2_ = {0x59ba2fbb,0x24cb,0x4c02,{0xa4,0xc4,0x82,0x94,0x56,0x9d,0x54,0xf1}};
static const GUID IID_ILauncherStatics3_ = {0x234261a8,0x9db3,0x4683,{0xaa,0x42,0xdc,0x6f,0x51,0xd3,0x38,0x47}};
static const GUID IID_ILauncherStatics4_ = {0xb9ec819f,0xb5a5,0x41c6,{0xb3,0xb3,0xdd,0x1b,0x31,0x78,0xbc,0xf2}};
static const GUID IID_ILauncherStatics5_ = {0x5b24ef84,0xd895,0x5fea,{0x91,0x53,0x1a,0xc4,0x9a,0xed,0x9b,0xa9}};
static const GUID IID_IAsyncOperation_bool_ = {0xcdb5efb3,0x5788,0x509d,{0x9b,0xe1,0x71,0xcc,0xb8,0xa3,0x36,0x2a}};
static const GUID IID_IAsyncInfo_   = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IUriRuntimeClass_ = {0x9e365e57,0x48b2,0x4160,{0x95,0x6f,0xc7,0x38,0x51,0x20,0xbb,0xfc}};
static const GUID IID_IStorageItem_ = {0x4207a996,0xca2f,0x42f7,{0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30}};

static void trace(const char *fmt, ...)
{
    /* launches are rare and worth seeing: always on */
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[launcher] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------------------------------------
 * An IAsyncOperation<bool> that is born completed (see shims/taskbarmanager).
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

/* a completion handler is a delegate: Invoke is slot 3 */
typedef HRESULT (WINAPI *completed_invoke)(void *handler, void *async, int status);

static HRESULT WINAPI aop_put_Completed(void *iface, IUnknown *handler)
{
    AsyncBool *a = from_op(iface);
    if (a->handler) { a->handler->Release(); a->handler = nullptr; }
    if (handler)
    {
        handler->AddRef();
        a->handler = handler;
        ((completed_invoke)(*(void ***)handler)[3])(handler, &a->op_vtbl, 1 /* Completed */);
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
static HRESULT WINAPI ainfo_get_Status(void *iface, int *status)   { if (!status) return E_POINTER; *status = 1; return S_OK; }
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
    return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * Launching
 * ------------------------------------------------------------------------------------------- */

/* Hand a URI or path to the host. The operation's result is whether a handler took it. */
static HRESULT launch(const WCHAR *what, const WCHAR *target, void **op)
{
    char mode[8];
    boolean ok;

    if (!target || !*target)
    {
        trace("%ls: nothing to launch", what);
        return make_completed_async(0, op);
    }
    if (GetEnvironmentVariableA("WINE_UWP_LAUNCHER", mode, sizeof(mode)) && !strcmp(mode, "log"))
    {
        /* Also leave it where a host-side helper can pick it up: browser-callback.sh opens it in
         * a browser the user can see and hands the app's scheme callback back to activate-uri.sh. */
        HANDLE file = CreateFileW(L"C:\\users\\Public\\wineuwp-launched.uri", GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            char utf8[8192];
            DWORD written;
            int len = WideCharToMultiByte(CP_UTF8, 0, target, -1, utf8, sizeof(utf8), nullptr, nullptr);
            if (len > 1) WriteFile(file, utf8, len - 1, &written, nullptr);
            CloseHandle(file);
        }
        trace("%ls %ls (dry run, WINE_UWP_LAUNCHER=log)", what, target);
        return make_completed_async(1, op);
    }
    ok = (INT_PTR)ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL) > 32;
    trace("%ls %ls -> %s", what, target, ok ? "launched" : "no handler");
    return make_completed_async(ok, op);
}

/* IUriRuntimeClass::get_AbsoluteUri is slot 6 */
static HRESULT launch_uri(IUnknown *uri, void **op)
{
    typedef HRESULT (WINAPI *get_string)(void *iface, HSTRING *value);
    void *iface = nullptr;
    HSTRING str = nullptr;
    HRESULT hr;

    if (!op) return E_POINTER;
    if (!uri) return E_INVALIDARG;
    if (FAILED(hr = uri->QueryInterface(IID_IUriRuntimeClass_, &iface))) return hr;
    hr = ((get_string)(*(void ***)iface)[6])(iface, &str);
    ((IUnknown *)iface)->Release();
    if (FAILED(hr)) return hr;
    hr = launch(L"uri", WindowsGetStringRawBuffer(str, nullptr), op);
    WindowsDeleteString(str);
    return hr;
}

/* IStorageItem::get_Path is slot 12, for StorageFile and StorageFolder alike */
static HRESULT launch_item(IUnknown *item, void **op)
{
    typedef HRESULT (WINAPI *get_string)(void *iface, HSTRING *value);
    void *iface = nullptr;
    HSTRING str = nullptr;
    HRESULT hr;

    if (!op) return E_POINTER;
    if (!item) return E_INVALIDARG;
    if (FAILED(hr = item->QueryInterface(IID_IStorageItem_, &iface))) return hr;
    hr = ((get_string)(*(void ***)iface)[12])(iface, &str);
    ((IUnknown *)iface)->Release();
    if (FAILED(hr)) return hr;
    hr = launch(L"item", WindowsGetStringRawBuffer(str, nullptr), op);
    WindowsDeleteString(str);
    return hr;
}

/* ------------------------------------------------------------------------------------------- */
struct Statics
{
    const void **vtbl, **vtbl2, **vtbl3, **vtbl4, **vtbl5;
};
static Statics statics_obj;
static const void *statics_vtbl[10], *statics2_vtbl[16], *statics3_vtbl[8], *statics4_vtbl[14], *statics5_vtbl[10];

static HRESULT WINAPI s_QueryInterface(void *i, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_ILauncherStatics_))
        *out = &statics_obj.vtbl;
    else if (IsEqualGUID(iid, IID_ILauncherStatics2_)) *out = &statics_obj.vtbl2;
    else if (IsEqualGUID(iid, IID_ILauncherStatics3_)) *out = &statics_obj.vtbl3;
    else if (IsEqualGUID(iid, IID_ILauncherStatics4_)) *out = &statics_obj.vtbl4;
    else if (IsEqualGUID(iid, IID_ILauncherStatics5_)) *out = &statics_obj.vtbl5;
    else
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    return S_OK;
}
static ULONG   WINAPI s_AddRef(void *i)  { return 2; }
static ULONG   WINAPI s_Release(void *i) { return 1; }
static HRESULT WINAPI s_GetIids(void *i, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI s_GetRuntimeClassName(void *i, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.System.Launcher";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
}
static HRESULT WINAPI s_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* ILauncherStatics */
static HRESULT WINAPI s_LaunchFileAsync(void *i, IUnknown *file, void **op)                  { return launch_item(file, op); }
static HRESULT WINAPI s_LaunchFileWithOptionsAsync(void *i, IUnknown *file, IUnknown *o, void **op) { return launch_item(file, op); }
static HRESULT WINAPI s_LaunchUriAsync(void *i, IUnknown *uri, void **op)                    { return launch_uri(uri, op); }
static HRESULT WINAPI s_LaunchUriWithOptionsAsync(void *i, IUnknown *uri, IUnknown *o, void **op)   { return launch_uri(uri, op); }
/* ILauncherStatics2 slot 8 */
static HRESULT WINAPI s2_LaunchUriWithDataAsync(void *i, IUnknown *uri, IUnknown *o, IUnknown *data, void **op) { return launch_uri(uri, op); }
/* ILauncherStatics3 */
static HRESULT WINAPI s3_LaunchFolderAsync(void *i, IUnknown *folder, void **op)             { return launch_item(folder, op); }
static HRESULT WINAPI s3_LaunchFolderWithOptionsAsync(void *i, IUnknown *folder, IUnknown *o, void **op) { return launch_item(folder, op); }
/* ILauncherStatics4 slots 9-11: the user argument is ignored, there is one user */
static HRESULT WINAPI s4_LaunchUriForUserAsync(void *i, IUnknown *user, IUnknown *uri, void **op) { return launch_uri(uri, op); }
static HRESULT WINAPI s4_LaunchUriForUserWithOptionsAsync(void *i, IUnknown *user, IUnknown *uri, IUnknown *o, void **op) { return launch_uri(uri, op); }
static HRESULT WINAPI s4_LaunchUriForUserWithDataAsync(void *i, IUnknown *user, IUnknown *uri, IUnknown *o, IUnknown *d, void **op) { return launch_uri(uri, op); }
/* ILauncherStatics5 */
static HRESULT WINAPI s5_LaunchFolderPathAsync(void *i, HSTRING path, void **op)
{
    return launch(L"folder", WindowsGetStringRawBuffer(path, nullptr), op);
}
static HRESULT WINAPI s5_LaunchFolderPathWithOptionsAsync(void *i, HSTRING path, IUnknown *o, void **op)
{
    return launch(L"folder", WindowsGetStringRawBuffer(path, nullptr), op);
}
static HRESULT WINAPI s5_LaunchFolderPathForUserAsync(void *i, IUnknown *user, HSTRING path, void **op)
{
    return launch(L"folder", WindowsGetStringRawBuffer(path, nullptr), op);
}
static HRESULT WINAPI s5_LaunchFolderPathForUserWithOptionsAsync(void *i, IUnknown *user, HSTRING path, IUnknown *o, void **op)
{
    return launch(L"folder", WindowsGetStringRawBuffer(path, nullptr), op);
}

/* The query/find/for-results methods have other result types and nothing here to answer them
 * with: traced thunks, so a caller that needs one shows up by name in the log. */
#define TRACED_SLOT(iface, n) \
    static HRESULT WINAPI iface##_slot##n(void *i) { trace(#iface " slot %d not implemented", n); return E_NOTIMPL; }
TRACED_SLOT(s2, 6) TRACED_SLOT(s2, 7) TRACED_SLOT(s2, 9) TRACED_SLOT(s2, 10) TRACED_SLOT(s2, 11)
TRACED_SLOT(s2, 12) TRACED_SLOT(s2, 13) TRACED_SLOT(s2, 14) TRACED_SLOT(s2, 15)
TRACED_SLOT(s4, 6) TRACED_SLOT(s4, 7) TRACED_SLOT(s4, 8) TRACED_SLOT(s4, 12) TRACED_SLOT(s4, 13)

static void fill_inspectable(const void **v)
{
    v[0] = (void *)s_QueryInterface;
    v[1] = (void *)s_AddRef;
    v[2] = (void *)s_Release;
    v[3] = (void *)s_GetIids;
    v[4] = (void *)s_GetRuntimeClassName;
    v[5] = (void *)s_GetTrustLevel;
}

static void init_statics(void)
{
    if (statics_obj.vtbl) return;
    fill_inspectable(statics_vtbl);
    statics_vtbl[6] = (void *)s_LaunchFileAsync;
    statics_vtbl[7] = (void *)s_LaunchFileWithOptionsAsync;
    statics_vtbl[8] = (void *)s_LaunchUriAsync;
    statics_vtbl[9] = (void *)s_LaunchUriWithOptionsAsync;

    fill_inspectable(statics2_vtbl);
    statics2_vtbl[6] = (void *)s2_slot6;
    statics2_vtbl[7] = (void *)s2_slot7;
    statics2_vtbl[8] = (void *)s2_LaunchUriWithDataAsync;
    statics2_vtbl[9] = (void *)s2_slot9;
    statics2_vtbl[10] = (void *)s2_slot10;
    statics2_vtbl[11] = (void *)s2_slot11;
    statics2_vtbl[12] = (void *)s2_slot12;
    statics2_vtbl[13] = (void *)s2_slot13;
    statics2_vtbl[14] = (void *)s2_slot14;
    statics2_vtbl[15] = (void *)s2_slot15;

    fill_inspectable(statics3_vtbl);
    statics3_vtbl[6] = (void *)s3_LaunchFolderAsync;
    statics3_vtbl[7] = (void *)s3_LaunchFolderWithOptionsAsync;

    fill_inspectable(statics4_vtbl);
    statics4_vtbl[6] = (void *)s4_slot6;
    statics4_vtbl[7] = (void *)s4_slot7;
    statics4_vtbl[8] = (void *)s4_slot8;
    statics4_vtbl[9] = (void *)s4_LaunchUriForUserAsync;
    statics4_vtbl[10] = (void *)s4_LaunchUriForUserWithOptionsAsync;
    statics4_vtbl[11] = (void *)s4_LaunchUriForUserWithDataAsync;
    statics4_vtbl[12] = (void *)s4_slot12;
    statics4_vtbl[13] = (void *)s4_slot13;

    fill_inspectable(statics5_vtbl);
    statics5_vtbl[6] = (void *)s5_LaunchFolderPathAsync;
    statics5_vtbl[7] = (void *)s5_LaunchFolderPathWithOptionsAsync;
    statics5_vtbl[8] = (void *)s5_LaunchFolderPathForUserAsync;
    statics5_vtbl[9] = (void *)s5_LaunchFolderPathForUserWithOptionsAsync;

    statics_obj.vtbl2 = statics2_vtbl;
    statics_obj.vtbl3 = statics3_vtbl;
    statics_obj.vtbl4 = statics4_vtbl;
    statics_obj.vtbl5 = statics5_vtbl;
    statics_obj.vtbl = statics_vtbl;   /* last: doubles as the initialised flag */
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.System.Launcher")) return REGDB_E_CLASSNOTREG;
    init_statics();
    *out = (IActivationFactory *)&statics_obj.vtbl;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

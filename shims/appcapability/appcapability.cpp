// Windows.Security.Authorization.AppCapabilityAccess.AppCapability stand-in for Wine.
//
// Media Player's library pages ask this class whether the process may use the musicLibrary and
// videosLibrary capabilities it declares in its manifest. On Windows the class lives in
// CapabilityAccessManagerClient.dll, which RPCs to the CapabilityAccessManager service; that
// service does not exist in the prefix, so every CheckAccess comes back denied and the app paints
// "Media Player can't access your music library" instead of the library.
//
// A Wine prefix has exactly one user and no per-app capability database, so the only sensible
// answer is: a capability the manifest declares is allowed, anything else is NotDeclaredByApp.
// Wine already backs KnownFolders.MusicLibrary et al with the real user profile directories
// (dlls/windows.storage/storagefolder.c), so "allowed" is also true.
//
// Interface layouts come from Windows.Security.winmd:
//   {7c353e2a-46ee-44e5-af3d-6ad3fc49bd22} IAppCapabilityStatics
//       6 RequestAccessForCapabilitiesAsync   7 RequestAccessForCapabilitiesForUserAsync
//       8 Create                              9 CreateWithProcessIdForUser
//   {4c49d915-8a2a-4295-9437-2df7c396aff4} IAppCapability
//       6 get_CapabilityName  7 get_User  8 RequestAccessAsync  9 CheckAccess
//      10 add_AccessChanged  11 remove_AccessChanged
//   {11c7ccb6-c74f-50a3-b960-88008767d939} IAppCapability2
//       6 get_DisplayMessage  7 put_DisplayMessage
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>

static const GUID IID_IAppCapabilityStatics_ =
    {0x7c353e2a,0x46ee,0x44e5,{0xaf,0x3d,0x6a,0xd3,0xfc,0x49,0xbd,0x22}};
static const GUID IID_IAppCapability_ =
    {0x4c49d915,0x8a2a,0x4295,{0x94,0x37,0x2d,0xf7,0xc3,0x96,0xaf,0xf4}};
static const GUID IID_IAppCapability2_ =
    {0x11c7ccb6,0xc74f,0x50a3,{0xb9,0x60,0x88,0x00,0x87,0x67,0xd9,0x39}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};

/* Windows.Security.Authorization.AppCapabilityAccess.AppCapabilityAccessStatus, in declaration
 * order: DeniedBySystem, NotDeclaredByApp, DeniedByUser, UserPromptRequired, Allowed. */
enum { STATUS_DENIED_BY_SYSTEM = 0, STATUS_NOT_DECLARED = 1, STATUS_DENIED_BY_USER = 2,
       STATUS_PROMPT_REQUIRED = 3, STATUS_ALLOWED = 4 };

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_APPCAPABILITY_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[appcapability] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------- manifest capabilities */

/* The capabilities the running package declares. Read once from the AppxManifest.xml that
 * WINE_UWP_PACKAGE_PATH points at: the answer has to depend on the manifest, or an app that does
 * not declare a capability would be told it has one and fail later, deeper, with a worse message. */
static WCHAR declared[64][64];
static int declared_count = -1;

static void add_declared(const WCHAR *name, size_t len)
{
    if (declared_count >= (int)ARRAYSIZE(declared) || len >= ARRAYSIZE(declared[0])) return;
    memcpy(declared[declared_count], name, len * sizeof(WCHAR));
    declared[declared_count][len] = 0;
    declared_count++;
}

static void load_declared(void)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD size, read = 0;
    char *text;

    if (declared_count >= 0) return;
    declared_count = 0;

    if (!GetEnvironmentVariableW(L"WINE_UWP_PACKAGE_PATH", path, ARRAYSIZE(path) - 20)) return;
    lstrcatW(path, L"\\AppxManifest.xml");
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) { trace("no manifest at %ls", path); return; }
    size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) { CloseHandle(file); return; }
    if (!(text = (char *)calloc(1, size + 1))) { CloseHandle(file); return; }
    ReadFile(file, text, size, &read, nullptr);
    CloseHandle(file);

    /* Capability elements are <Capability Name="x"/>, <uap:Capability .../>,
     * <DeviceCapability .../> and friends - all of them carry Name="x" and end in "Capability". */
    for (const char *p = text; (p = strstr(p, "Capability")); p += 10)
    {
        const char *attr = strstr(p, "Name=\"");
        const char *close = strpbrk(p, ">");
        if (!attr || (close && attr > close)) continue;
        attr += 6;
        const char *end = strchr(attr, '"');
        if (!end || end - attr >= 63) continue;
        WCHAR wide[64];
        int n = MultiByteToWideChar(CP_UTF8, 0, attr, (int)(end - attr), wide, ARRAYSIZE(wide) - 1);
        if (n <= 0) continue;
        wide[n] = 0;
        add_declared(wide, n);
        trace("manifest declares %ls", wide);
    }
    free(text);
}

static int status_for(const WCHAR *capability)
{
    load_declared();
    if (!capability || !*capability) return STATUS_NOT_DECLARED;
    for (int i = 0; i < declared_count; i++)
        if (!lstrcmpiW(declared[i], capability)) return STATUS_ALLOWED;
    /* No manifest at all (a probe, or a launch without WINE_UWP_PACKAGE_PATH): allow, because a
     * caller that got this far asked for a capability on purpose and there is nothing to check
     * it against. With a manifest, an undeclared capability is genuinely not declared. */
    return declared_count ? STATUS_NOT_DECLARED : STATUS_ALLOWED;
}

/* ---------------------------------------------------------------- IAppCapability */

struct capability
{
    const void **vtbl;          /* IAppCapability */
    const void **vtbl2;         /* IAppCapability2 */
    LONG ref;
    WCHAR name[64];
    HSTRING message;
};

static const void *capability_vtbl[12];
static const void *capability2_vtbl[8];

static HRESULT WINAPI cap_QueryInterface(void *iface, REFIID iid, void **out)
{
    capability *impl = (capability *)iface;

    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAppCapability_) || IsEqualGUID(iid, IID_IAgileObject_))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IAppCapability2_))
        *out = &impl->vtbl2;
    else { *out = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}

static ULONG WINAPI cap_AddRef(void *iface)
{
    return InterlockedIncrement(&((capability *)iface)->ref);
}

static ULONG WINAPI cap_Release(void *iface)
{
    capability *impl = (capability *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->message) WindowsDeleteString(impl->message); free(impl); }
    return ref;
}

static HRESULT WINAPI cap_GetIids(void *, ULONG *count, IID **iids)
{
    if (count) *count = 0;
    if (iids) *iids = nullptr;
    return S_OK;
}

static HRESULT WINAPI cap_GetRuntimeClassName(void *, HSTRING *name)
{
    return WindowsCreateString(L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability",
                               64, name);
}

static HRESULT WINAPI cap_GetTrustLevel(void *, TrustLevel *level)
{
    if (level) *level = BaseTrust;
    return S_OK;
}

/* IAppCapability2 shares the object, so its IUnknown has to walk back to vtbl. */
static inline capability *from_vtbl2(void *iface)
{
    return CONTAINING_RECORD(iface, capability, vtbl2);
}

static HRESULT WINAPI cap2_QueryInterface(void *iface, REFIID iid, void **out)
{
    return cap_QueryInterface(from_vtbl2(iface), iid, out);
}
static ULONG WINAPI cap2_AddRef(void *iface) { return cap_AddRef(from_vtbl2(iface)); }
static ULONG WINAPI cap2_Release(void *iface) { return cap_Release(from_vtbl2(iface)); }

static HRESULT WINAPI cap2_get_DisplayMessage(void *iface, HSTRING *value)
{
    capability *impl = from_vtbl2(iface);
    if (!value) return E_POINTER;
    return WindowsDuplicateString(impl->message, value);
}

static HRESULT WINAPI cap2_put_DisplayMessage(void *iface, HSTRING value)
{
    capability *impl = from_vtbl2(iface);
    HSTRING copy = nullptr;
    HRESULT hr = WindowsDuplicateString(value, &copy);
    if (FAILED(hr)) return hr;
    if (impl->message) WindowsDeleteString(impl->message);
    impl->message = copy;
    return S_OK;
}

static HRESULT WINAPI cap_get_CapabilityName(void *iface, HSTRING *value)
{
    capability *impl = (capability *)iface;
    if (!value) return E_POINTER;
    return WindowsCreateString(impl->name, lstrlenW(impl->name), value);
}

/* get_User hands out a Windows.System.User. Nothing in Media Player's library path reads it, and
 * inventing one would mean standing up another class; report that there is no user object rather
 * than writing a pointer the caller would then call through. */
static HRESULT WINAPI cap_get_User(void *iface, void **value)
{
    trace("get_User: no user object");
    if (!value) return E_POINTER;
    *value = nullptr;
    return S_OK;
}

static HRESULT WINAPI cap_CheckAccess(void *iface, INT32 *status)
{
    capability *impl = (capability *)iface;
    if (!status) return E_POINTER;
    *status = status_for(impl->name);
    trace("CheckAccess(%ls) -> %d", impl->name, *status);
    return S_OK;
}

/* RequestAccessAsync returns IAsyncOperation<AppCapabilityAccessStatus>. Building one means a
 * completed async op with the right parameterised IID, which the caller gets by QI. Rather than
 * guess at that layout, report that there is nothing to request: on Windows this only ever
 * prompts, and CheckAccess above already says Allowed, so an app that respects CheckAccess never
 * reaches here. If one does, E_NOTIMPL is visible in the log instead of a silent wrong answer. */
static HRESULT WINAPI cap_RequestAccessAsync(void *iface, void **operation)
{
    capability *impl = (capability *)iface;
    trace("RequestAccessAsync(%ls): not implemented", impl->name);
    if (operation) *operation = nullptr;
    return E_NOTIMPL;
}

/* Access never changes here, so a registration is a token that is never fired. Returning S_OK with
 * a non-zero token keeps callers that check the HRESULT happy; remove is a no-op. */
static HRESULT WINAPI cap_add_AccessChanged(void *iface, void *handler, INT64 *token)
{
    if (token) *token = 1;
    return S_OK;
}

static HRESULT WINAPI cap_remove_AccessChanged(void *iface, INT64 token) { return S_OK; }

/* ---------------------------------------------------------------- IAppCapabilityStatics */

struct statics_object { const void **vtbl; };
static const void *statics_vtbl[10];
static const void *factory_vtbl[7];
static statics_object statics;
static statics_object factory;

static HRESULT create_capability(HSTRING name, void **out)
{
    capability *impl;
    UINT32 len = 0;
    const WCHAR *str = name ? WindowsGetStringRawBuffer(name, &len) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!(impl = (capability *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = capability_vtbl;
    impl->vtbl2 = capability2_vtbl;
    impl->ref = 1;
    if (str) lstrcpynW(impl->name, str, ARRAYSIZE(impl->name));
    trace("Create(%ls)", impl->name);
    *out = impl;
    return S_OK;
}

static HRESULT WINAPI statics_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAppCapabilityStatics_) || IsEqualGUID(iid, IID_IAgileObject_))
        { *out = &statics.vtbl; return S_OK; }
    if (IsEqualGUID(iid, IID_IActivationFactory)) { *out = &factory.vtbl; return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}

static ULONG WINAPI statics_AddRef(void *) { return 2; }
static ULONG WINAPI statics_Release(void *) { return 1; }

static HRESULT WINAPI statics_RequestAccessForCapabilitiesAsync(void *iface, void *names, void **op)
{
    trace("RequestAccessForCapabilitiesAsync: not implemented");
    if (op) *op = nullptr;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_RequestAccessForCapabilitiesForUserAsync(void *iface, void *user,
                                                                       void *names, void **op)
{
    trace("RequestAccessForCapabilitiesForUserAsync: not implemented");
    if (op) *op = nullptr;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_Create(void *iface, HSTRING name, void **value)
{
    return create_capability(name, value);
}

static HRESULT WINAPI statics_CreateWithProcessIdForUser(void *iface, void *user, HSTRING name,
                                                         UINT32 pid, void **value)
{
    return create_capability(name, value);
}

static HRESULT WINAPI factory_ActivateInstance(void *iface, void **instance)
{
    /* The class has no default constructor on Windows either. */
    if (instance) *instance = nullptr;
    return E_NOTIMPL;
}

static void init_vtbls(void)
{
    capability_vtbl[0]  = (void *)cap_QueryInterface;
    capability_vtbl[1]  = (void *)cap_AddRef;
    capability_vtbl[2]  = (void *)cap_Release;
    capability_vtbl[3]  = (void *)cap_GetIids;
    capability_vtbl[4]  = (void *)cap_GetRuntimeClassName;
    capability_vtbl[5]  = (void *)cap_GetTrustLevel;
    capability_vtbl[6]  = (void *)cap_get_CapabilityName;
    capability_vtbl[7]  = (void *)cap_get_User;
    capability_vtbl[8]  = (void *)cap_RequestAccessAsync;
    capability_vtbl[9]  = (void *)cap_CheckAccess;
    capability_vtbl[10] = (void *)cap_add_AccessChanged;
    capability_vtbl[11] = (void *)cap_remove_AccessChanged;

    capability2_vtbl[0] = (void *)cap2_QueryInterface;
    capability2_vtbl[1] = (void *)cap2_AddRef;
    capability2_vtbl[2] = (void *)cap2_Release;
    capability2_vtbl[3] = (void *)cap_GetIids;
    capability2_vtbl[4] = (void *)cap_GetRuntimeClassName;
    capability2_vtbl[5] = (void *)cap_GetTrustLevel;
    capability2_vtbl[6] = (void *)cap2_get_DisplayMessage;
    capability2_vtbl[7] = (void *)cap2_put_DisplayMessage;

    statics_vtbl[0] = (void *)statics_QueryInterface;
    statics_vtbl[1] = (void *)statics_AddRef;
    statics_vtbl[2] = (void *)statics_Release;
    statics_vtbl[3] = (void *)cap_GetIids;
    statics_vtbl[4] = (void *)cap_GetRuntimeClassName;
    statics_vtbl[5] = (void *)cap_GetTrustLevel;
    statics_vtbl[6] = (void *)statics_RequestAccessForCapabilitiesAsync;
    statics_vtbl[7] = (void *)statics_RequestAccessForCapabilitiesForUserAsync;
    statics_vtbl[8] = (void *)statics_Create;
    statics_vtbl[9] = (void *)statics_CreateWithProcessIdForUser;

    factory_vtbl[0] = (void *)statics_QueryInterface;
    factory_vtbl[1] = (void *)statics_AddRef;
    factory_vtbl[2] = (void *)statics_Release;
    factory_vtbl[3] = (void *)cap_GetIids;
    factory_vtbl[4] = (void *)cap_GetRuntimeClassName;
    factory_vtbl[5] = (void *)cap_GetTrustLevel;
    factory_vtbl[6] = (void *)factory_ActivateInstance;

    statics.vtbl = statics_vtbl;
    factory.vtbl = factory_vtbl;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *str = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!str || lstrcmpW(str, L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability"))
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

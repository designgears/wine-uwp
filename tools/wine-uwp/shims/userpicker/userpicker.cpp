// Windows.System.UserPicker stand-in for Wine.
//
// Solitaire's first two log lines are:
//
//     [0.001] LiveManagerXsapi2 - Failed to initialize XAL with HRESULT: 0X80004005
//     [0.004] LiveManagerXsapi2 - Failed to initialize XSAPI with HRESULT: -2147467259
//
// preceded by "Failed to find library for L\"Windows.System.UserPicker\"". XAL is the Xbox
// Authentication Library; it asks UserPicker whether an interactive user picker is available
// before it decides how to sign in. On Windows the class is registered ActivationType=1 with
// Server=UserManager - an out-of-process class served by a system service - so there is no
// library to load and RoGetActivationFactory fails outright, which XAL reports as E_FAIL.
//
// A Wine prefix has exactly one local user and no picker UI, so the honest answer is
// IsSupported = false: that is a value the caller is designed to handle (it means "do not offer
// to switch users"), whereas a missing class is not. PickSingleUserAsync is therefore never the
// right call to make, and says so rather than inventing a user.
//
// Interface layouts from Windows.System.winmd:
//   {de3290dc-7e73-4df6-a1ae-4d7eca82b40d} IUserPickerStatics
//       6 IsSupported
//   {7d548008-f1e3-4a6c-8ddc-a9bb0f488aed} IUserPicker
//       6 get_AllowGuestAccounts   7 put_AllowGuestAccounts
//       8 get_SuggestedSelectedUser 9 put_SuggestedSelectedUser
//      10 PickSingleUserAsync
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

static const GUID IID_IUserPickerStatics_ =
    {0xde3290dc,0x7e73,0x4df6,{0xa1,0xae,0x4d,0x7e,0xca,0x82,0xb4,0x0d}};
static const GUID IID_IUserPicker_ =
    {0x7d548008,0xf1e3,0x4a6c,{0x8d,0xdc,0xa9,0xbb,0x0f,0x48,0x8a,0xed}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
/* .NET Native asks this of every WinRT object and a plain object must refuse it. */
static const GUID IID_IReferenceTracker_ =
    {0x11d3b13a,0x180e,0x4789,{0xa8,0xbe,0x77,0x12,0x88,0x28,0x93,0xe6}};

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_USERPICKER_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[userpicker] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static char *guidstr(REFIID iid, char *buf)
{
    sprintf(buf, "{%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x}",
            (unsigned long)iid.Data1, iid.Data2, iid.Data3,
            iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
            iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return buf;
}

/* ---------------------------------------------------------------- UserPicker instance */

struct UserPicker
{
    const void **vtbl;
    LONG ref;
    boolean allow_guests;
    IInspectable *suggested;
};

static HRESULT WINAPI picker_QueryInterface(void *iface, REFIID iid, void **out)
{
    UserPicker *self = (UserPicker *)iface;
    char b[64];

    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IUserPicker_))
    {
        InterlockedIncrement(&self->ref);
        *out = iface;
        return S_OK;
    }
    if (!IsEqualGUID(iid, IID_IReferenceTracker_))
        trace("picker: QI %s -> E_NOINTERFACE", guidstr(iid, b));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI picker_AddRef(void *iface)
{
    return InterlockedIncrement(&((UserPicker *)iface)->ref);
}

static ULONG WINAPI picker_Release(void *iface)
{
    UserPicker *self = (UserPicker *)iface;
    ULONG ref = InterlockedDecrement(&self->ref);
    if (!ref)
    {
        if (self->suggested) self->suggested->Release();
        free(self);
    }
    return ref;
}

static HRESULT WINAPI picker_GetIids(void *iface, ULONG *count, IID **iids)
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI picker_GetRuntimeClassName(void *iface, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.System.UserPicker";
    return WindowsCreateString(cls, (UINT32)(sizeof(cls)/sizeof(cls[0]) - 1), name);
}

static HRESULT WINAPI picker_GetTrustLevel(void *iface, TrustLevel *level)
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI picker_get_AllowGuestAccounts(void *iface, boolean *value)
{
    if (!value) return E_POINTER;
    *value = ((UserPicker *)iface)->allow_guests;
    return S_OK;
}

static HRESULT WINAPI picker_put_AllowGuestAccounts(void *iface, boolean value)
{
    trace("picker: AllowGuestAccounts = %d", (int)value);
    ((UserPicker *)iface)->allow_guests = value;
    return S_OK;
}

/* Nullable: "no suggestion" is a real value here, not a refusal. */
static HRESULT WINAPI picker_get_SuggestedSelectedUser(void *iface, IInspectable **value)
{
    UserPicker *self = (UserPicker *)iface;
    if (!value) return E_POINTER;
    if ((*value = self->suggested)) (*value)->AddRef();
    return S_OK;
}

static HRESULT WINAPI picker_put_SuggestedSelectedUser(void *iface, IInspectable *value)
{
    UserPicker *self = (UserPicker *)iface;
    if (value) value->AddRef();
    if (self->suggested) self->suggested->Release();
    self->suggested = value;
    return S_OK;
}

/* There is no picker UI to show. A caller that got IsSupported = false should never reach here;
 * one that does is better served by a clear failure than by a fabricated user, which it would
 * then try to sign in as. */
static HRESULT WINAPI picker_PickSingleUserAsync(void *iface, IInspectable **operation)
{
    trace("picker: PickSingleUserAsync - there is no picker UI in a Wine prefix");
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static const void *picker_vtbl[] =
{
    (void *)picker_QueryInterface,
    (void *)picker_AddRef,
    (void *)picker_Release,
    (void *)picker_GetIids,
    (void *)picker_GetRuntimeClassName,
    (void *)picker_GetTrustLevel,
    (void *)picker_get_AllowGuestAccounts,
    (void *)picker_put_AllowGuestAccounts,
    (void *)picker_get_SuggestedSelectedUser,
    (void *)picker_put_SuggestedSelectedUser,
    (void *)picker_PickSingleUserAsync,
};

/* ---------------------------------------------------------------- activation factory */

struct PickerFactory
{
    const void **vtbl;      /* IActivationFactory */
    const void **statics;   /* IUserPickerStatics */
    LONG ref;
};

static PickerFactory factory;

static HRESULT WINAPI factory_QueryInterface(void *iface, REFIID iid, void **out)
{
    char b[64];

    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory))
    {
        InterlockedIncrement(&factory.ref);
        *out = &factory.vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IUserPickerStatics_))
    {
        InterlockedIncrement(&factory.ref);
        *out = &factory.statics;
        return S_OK;
    }
    if (!IsEqualGUID(iid, IID_IReferenceTracker_))
        trace("factory: QI %s -> E_NOINTERFACE", guidstr(iid, b));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(void *iface) { return InterlockedIncrement(&factory.ref); }
static ULONG WINAPI factory_Release(void *iface) { return InterlockedDecrement(&factory.ref); }

static HRESULT WINAPI factory_GetIids(void *iface, ULONG *count, IID **iids)
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName(void *iface, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.System.UserPicker";
    return WindowsCreateString(cls, (UINT32)(sizeof(cls)/sizeof(cls[0]) - 1), name);
}

static HRESULT WINAPI factory_GetTrustLevel(void *iface, TrustLevel *level)
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance(void *iface, IInspectable **instance)
{
    UserPicker *self;

    trace("factory: ActivateInstance");
    if (!instance) return E_POINTER;
    if (!(self = (UserPicker *)calloc(1, sizeof(*self)))) return E_OUTOFMEMORY;
    self->vtbl = picker_vtbl;
    self->ref = 1;
    *instance = (IInspectable *)self;
    return S_OK;
}

static const void *factory_vtbl[] =
{
    (void *)factory_QueryInterface,
    (void *)factory_AddRef,
    (void *)factory_Release,
    (void *)factory_GetIids,
    (void *)factory_GetRuntimeClassName,
    (void *)factory_GetTrustLevel,
    (void *)factory_ActivateInstance,
};

/* IUserPickerStatics is a second interface on the same factory object; its slots 0-5 forward to
 * the factory's, so CONTAINING_RECORD-style adjustment is not needed - both point at `factory`. */
static HRESULT WINAPI statics_QueryInterface(void *iface, REFIID iid, void **out)
{
    return factory_QueryInterface(iface, iid, out);
}
static ULONG WINAPI statics_AddRef(void *iface) { return InterlockedIncrement(&factory.ref); }
static ULONG WINAPI statics_Release(void *iface) { return InterlockedDecrement(&factory.ref); }

static HRESULT WINAPI statics_IsSupported(void *iface, boolean *value)
{
    /* One local user, no picker UI. False is what the caller is built to handle. */
    trace("statics: IsSupported -> false");
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static const void *statics_vtbl[] =
{
    (void *)statics_QueryInterface,
    (void *)statics_AddRef,
    (void *)statics_Release,
    (void *)factory_GetIids,
    (void *)factory_GetRuntimeClassName,
    (void *)factory_GetTrustLevel,
    (void *)statics_IsSupported,
};

/* ---------------------------------------------------------------- entry point */

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    UINT32 len = 0;
    const WCHAR *cls = WindowsGetStringRawBuffer(name, &len);

    if (!out) return E_POINTER;
    *out = NULL;
    if (!cls || wcscmp(cls, L"Windows.System.UserPicker"))
    {
        trace("not ours: %ls", cls ? cls : L"(null)");
        return REGDB_E_CLASSNOTREG;
    }

    trace("serving Windows.System.UserPicker");
    factory.vtbl = factory_vtbl;
    factory.statics = statics_vtbl;
    InterlockedIncrement(&factory.ref);
    *out = (IActivationFactory *)&factory.vtbl;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

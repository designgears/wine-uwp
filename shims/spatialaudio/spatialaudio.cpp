// Windows.Media.Audio.SpatialAudioDeviceConfiguration: answer the interface instead of refusing it.
//
// This is what kills Solitaire once it reaches its splash screen. Three adjacent lines:
//
//   fixme:combase:RoGetActivationFactory (L"Windows.Media.Audio.SpatialAudioDeviceConfiguration", ...)
//   fixme:mmdevapi:windows_media_devices_QueryInterface {3ec37f7b-936d-4e04-9728-2827d9f758c4}
//       not implemented, returning E_NOINTERFACE.
//   wine: Unhandled exception 0xe06d7363 in thread 1fc ...
//   [   1.649] *** CRITICAL ERROR ***
//
// Wine routes that class to windows.media.devices.dll, whose single factory serves
// IMediaDeviceStatics and refuses everything else. Wwise asks for the interface with C++/WinRT's
// `.as<>()`, which throws on E_NOINTERFACE - on its own audio thread, with nothing to catch it, so
// the process dies. The app's own crash dump names the path exactly:
//
//   CAkAudioMgr::BeginPerform -> AK JobMgr -> AkSinkCoreAudio (AK_MSCoreAudioThread)
//     -> winrt::throw_hresult, and the return address lands in that function's 0x80004002 branch
//        - E_NOINTERFACE, confirming which HRESULT was thrown rather than inferring it.
//
// This is a genuine exception to AGENTS.md's "do not claim an interface you cannot implement".
// That rule is about interfaces with behavioural contracts we cannot honour - answering XAML's
// IReferenceTrackerInternal made it call tracking methods on an object that could not honour them.
// This one has no such contract: it is a small property bag, and the honest answer to all of it is
// "this machine has no spatial audio". Refusing the interface is not a safer answer here, it is a
// fatal one - the caller is written on the assumption that the platform always has this class,
// which on Windows it does.
//
//   ISpatialAudioDeviceConfigurationStatics {3ec37f7b-936d-4e04-9728-2827d9f758c4}: 6 GetForDeviceId
//   ISpatialAudioDeviceConfiguration        {ee830034-61cf-5749-9da4-10f0fe028199}:
//      6 get_DeviceId        7 get_IsSpatialAudioSupported   8 IsSpatialAudioFormatSupported
//      9 get_ActiveSpatialAudioFormat      10 get_DefaultSpatialAudioFormat
//     11 SetDefaultSpatialAudioFormatAsync 12 add_ConfigurationChanged
//     13 remove_ConfigurationChanged
//
// The two format getters hand back an **empty** HSTRING. A WinRT empty string is NULL and is
// perfectly valid, which is why that is not the "null is not a neutral answer" trap - there is no
// object being handed over for the caller to dereference. No format-subtype string is invented:
// the documented constants ("Windows.Spatial.Sound...") appear nowhere in the shipping
// Windows.Media.dll here, so there is nothing to copy and guessing one would be exactly the
// mistake that rule warns about. Both getters trace, so a later run says if the app needs a real
// one.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdlib>

static const GUID IID_ISpatialAudioDeviceConfigurationStatics_ =
    {0x3ec37f7b,0x936d,0x4e04,{0x97,0x28,0x28,0x27,0xd9,0xf7,0x58,0xc4}};
static const GUID IID_ISpatialAudioDeviceConfiguration_ =
    {0xee830034,0x61cf,0x5749,{0x9d,0xa4,0x10,0xf0,0xfe,0x02,0x81,0x99}};
static const GUID IID_ISpatialAudioFormatSubtypeStatics_ =
    {0xb3de8a47,0x83ee,0x4266,{0xa9,0x45,0xbe,0xdf,0x50,0x7a,0xfe,0xed}};
static const GUID IID_ISpatialAudioFormatSubtypeStatics2_ =
    {0x4565e6cb,0xd95b,0x5621,{0xb6,0xaf,0x0e,0x88,0x49,0xc5,0x7c,0x80}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IReferenceTracker_ =
    {0x11d3b13a,0x180e,0x4789,{0xa8,0xbe,0x77,0x12,0x88,0x28,0x93,0xe6}};

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_SPATIALAUDIO_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[spatialaudio] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

#define MAX_HANDLERS 8

struct Config
{
    const void **vtbl;
    LONG ref;
    HSTRING device_id;
    LONGLONG next_token;
    struct { LONGLONG token; IUnknown *handler; } handlers[MAX_HANDLERS];
};
static const void *config_vtbl[14];

static HRESULT WINAPI cfg_QueryInterface(void *iface, REFIID iid, void **out)
{
    Config *c = (Config *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_ISpatialAudioDeviceConfiguration_))
    {
        InterlockedIncrement(&c->ref);
        *out = &c->vtbl;
        return S_OK;
    }
    /* A plain WinRT object must still refuse IReferenceTracker. */
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI cfg_AddRef(void *iface) { return InterlockedIncrement(&((Config *)iface)->ref); }
static ULONG WINAPI cfg_Release(void *iface)
{
    Config *c = (Config *)iface;
    ULONG ref = InterlockedDecrement(&c->ref);
    if (!ref)
    {
        for (int i = 0; i < MAX_HANDLERS; i++)
            if (c->handlers[i].handler) c->handlers[i].handler->Release();
        WindowsDeleteString(c->device_id);
        free(c);
    }
    return ref;
}
static HRESULT WINAPI cfg_GetIids(void *i, ULONG *n, IID **ids) { if (n) *n = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI cfg_GetRuntimeClassName(void *i, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.Media.Audio.SpatialAudioDeviceConfiguration";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
}
static HRESULT WINAPI cfg_GetTrustLevel(void *i, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static HRESULT WINAPI cfg_get_DeviceId(void *iface, HSTRING *value)
{
    Config *c = (Config *)iface;
    if (!value) return E_POINTER;
    return WindowsDuplicateString(c->device_id, value);   /* handles a NULL (empty) source */
}
static HRESULT WINAPI cfg_get_IsSpatialAudioSupported(void *iface, boolean *value)
{
    if (!value) return E_POINTER;
    *value = 0;
    trace("get_IsSpatialAudioSupported -> false");
    return S_OK;
}
static HRESULT WINAPI cfg_IsSpatialAudioFormatSupported(void *iface, HSTRING subtype, boolean *value)
{
    if (!value) return E_POINTER;
    *value = 0;
    trace("IsSpatialAudioFormatSupported(%ls) -> false",
          subtype ? WindowsGetStringRawBuffer(subtype, nullptr) : L"(null)");
    return S_OK;
}
/* An empty HSTRING is NULL in WinRT and is a valid string, not a null object. */
static HRESULT WINAPI cfg_get_ActiveSpatialAudioFormat(void *iface, HSTRING *value)
{
    if (!value) return E_POINTER;
    *value = nullptr;
    trace("get_ActiveSpatialAudioFormat -> \"\" (no format subtype invented)");
    return S_OK;
}
static HRESULT WINAPI cfg_get_DefaultSpatialAudioFormat(void *iface, HSTRING *value)
{
    if (!value) return E_POINTER;
    *value = nullptr;
    trace("get_DefaultSpatialAudioFormat -> \"\" (no format subtype invented)");
    return S_OK;
}
static HRESULT WINAPI cfg_SetDefaultSpatialAudioFormatAsync(void *iface, HSTRING subtype, void **op)
{
    trace("SetDefaultSpatialAudioFormatAsync -> E_NOTIMPL (nothing to set)");
    if (op) *op = nullptr;
    return E_NOTIMPL;
}
/* Registering an event must succeed even though it can never fire. */
static HRESULT WINAPI cfg_add_ConfigurationChanged(void *iface, IUnknown *handler, LONGLONG *token)
{
    Config *c = (Config *)iface;
    if (!token) return E_POINTER;
    *token = 0;
    for (int i = 0; i < MAX_HANDLERS; i++)
    {
        if (c->handlers[i].token) continue;
        c->handlers[i].token = ++c->next_token;
        c->handlers[i].handler = handler;
        if (handler) handler->AddRef();
        *token = c->handlers[i].token;
        trace("add_ConfigurationChanged -> token %lld", (long long)*token);
        return S_OK;
    }
    return E_OUTOFMEMORY;
}
static HRESULT WINAPI cfg_remove_ConfigurationChanged(void *iface, LONGLONG token)
{
    Config *c = (Config *)iface;
    for (int i = 0; i < MAX_HANDLERS; i++)
    {
        if (c->handlers[i].token != token || !token) continue;
        if (c->handlers[i].handler) c->handlers[i].handler->Release();
        c->handlers[i].token = 0;
        c->handlers[i].handler = nullptr;
        break;
    }
    return S_OK;
}

static void init_config_vtbl(void)
{
    if (config_vtbl[0]) return;
    config_vtbl[0]  = (void *)cfg_QueryInterface;
    config_vtbl[1]  = (void *)cfg_AddRef;
    config_vtbl[2]  = (void *)cfg_Release;
    config_vtbl[3]  = (void *)cfg_GetIids;
    config_vtbl[4]  = (void *)cfg_GetRuntimeClassName;
    config_vtbl[5]  = (void *)cfg_GetTrustLevel;
    config_vtbl[6]  = (void *)cfg_get_DeviceId;
    config_vtbl[7]  = (void *)cfg_get_IsSpatialAudioSupported;
    config_vtbl[8]  = (void *)cfg_IsSpatialAudioFormatSupported;
    config_vtbl[9]  = (void *)cfg_get_ActiveSpatialAudioFormat;
    config_vtbl[10] = (void *)cfg_get_DefaultSpatialAudioFormat;
    config_vtbl[11] = (void *)cfg_SetDefaultSpatialAudioFormatAsync;
    config_vtbl[12] = (void *)cfg_add_ConfigurationChanged;
    config_vtbl[13] = (void *)cfg_remove_ConfigurationChanged;
}

struct Statics { const void **vtbl; LONG ref; };
static Statics statics_obj;
static const void *statics_vtbl[7];

static HRESULT WINAPI s_QueryInterface(void *i, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_ISpatialAudioDeviceConfigurationStatics_))
    {
        InterlockedIncrement(&statics_obj.ref);
        *out = &statics_obj.vtbl;
        return S_OK;
    }
    if (!IsEqualGUID(iid, IID_IReferenceTracker_))
        trace("statics QI for an interface we do not serve");
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(void *i)  { return InterlockedIncrement(&statics_obj.ref); }
static ULONG WINAPI s_Release(void *i) { return InterlockedDecrement(&statics_obj.ref); }

static HRESULT WINAPI s_GetForDeviceId(void *iface, HSTRING device_id, void **config)
{
    Config *c;
    HRESULT hr;

    if (!config) return E_POINTER;
    *config = nullptr;
    if (!(c = (Config *)calloc(1, sizeof(*c)))) return E_OUTOFMEMORY;
    if (FAILED(hr = WindowsDuplicateString(device_id, &c->device_id))) { free(c); return hr; }
    init_config_vtbl();
    c->vtbl = config_vtbl;
    c->ref = 1;
    *config = &c->vtbl;
    trace("GetForDeviceId(%ls) -> %p",
          device_id ? WindowsGetStringRawBuffer(device_id, nullptr) : L"(null)", *config);
    return S_OK;
}


/* ---------------------------------------------------------------------------------------------
 * Windows.Media.Audio.SpatialAudioFormatSubtype - the constant strings.
 *
 * The app goes straight on to this class once it can get a SpatialAudioDeviceConfiguration, and
 * refusing it throws in exactly the same way. It is nothing but seven read-only string properties.
 *
 * **These are our values, not Microsoft's.** The shipping constants appear nowhere in the Windows
 * image here - a UTF-16 search of every DLL in System32 for "Sonic", "DolbyAtmos", "HeadphoneX"
 * and "DTSX" finds nothing - so there is nothing to copy, and AGENTS.md is explicit about not
 * inventing a value and presenting it as the real one. What makes this safe is that these strings
 * are only ever compared against *each other* and against
 * SpatialAudioDeviceConfiguration.ActiveSpatialAudioFormat, which this same shim returns. They
 * need to be distinct and stable within the process, not byte-identical to Windows. The active and
 * default formats above are empty, so they match none of these - which is the truthful answer,
 * since IsSpatialAudioSupported is false.
 *
 * If something ever passes one of these strings *out* of the process or into a real audio API,
 * this reasoning stops holding and the true constants have to be recovered from a machine that
 * ships them.
 *
 *   ISpatialAudioFormatSubtypeStatics  {b3de8a47-...}: 6 WindowsSonic 7 DolbyAtmosForHeadphones
 *      8 DolbyAtmosForHomeTheater 9 DolbyAtmosForSpeakers 10 DTSHeadphoneX 11 DTSXUltra
 *   ISpatialAudioFormatSubtypeStatics2 {4565e6cb-...}: 6 DTSXForHomeTheater
 * ------------------------------------------------------------------------------------------- */
struct Subtypes { const void **vtbl; const void **vtbl2; LONG ref; };
static Subtypes subtypes_obj;
static const void *subtypes_vtbl[12];
static const void *subtypes2_vtbl[7];

static Subtypes *from_st(void *i)  { return (Subtypes *)i; }
static Subtypes *from_st2(void *i) { return (Subtypes *)((char *)i - sizeof(void *)); }

static HRESULT subtypes_qi(REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory) ||
        IsEqualGUID(iid, IID_ISpatialAudioFormatSubtypeStatics_))
    {
        InterlockedIncrement(&subtypes_obj.ref);
        *out = &subtypes_obj.vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_ISpatialAudioFormatSubtypeStatics2_))
    {
        InterlockedIncrement(&subtypes_obj.ref);
        *out = &subtypes_obj.vtbl2;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static HRESULT WINAPI st_QueryInterface(void *i, REFIID iid, void **o)  { return subtypes_qi(iid, o); }
static ULONG   WINAPI st_AddRef(void *i)  { return InterlockedIncrement(&subtypes_obj.ref); }
static ULONG   WINAPI st_Release(void *i) { return InterlockedDecrement(&subtypes_obj.ref); }
static HRESULT WINAPI st_GetRuntimeClassName(void *i, HSTRING *n)
{
    static const WCHAR cls[] = L"Windows.Media.Audio.SpatialAudioFormatSubtype";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, n);
}

#define SUBTYPE_GETTER(fn, text)                                                     \
    static HRESULT WINAPI fn(void *iface, HSTRING *value)                            \
    {                                                                                \
        static const WCHAR s[] = text;                                               \
        if (!value) return E_POINTER;                                                \
        return WindowsCreateString(s, ARRAYSIZE(s) - 1, value);                      \
    }
SUBTYPE_GETTER(st_get_WindowsSonic,            L"Windows.Spatial.Sound.WindowsSonic")
SUBTYPE_GETTER(st_get_DolbyAtmosForHeadphones, L"Windows.Spatial.Sound.DolbyAtmosForHeadphones")
SUBTYPE_GETTER(st_get_DolbyAtmosForHomeTheater,L"Windows.Spatial.Sound.DolbyAtmosForHomeTheater")
SUBTYPE_GETTER(st_get_DolbyAtmosForSpeakers,   L"Windows.Spatial.Sound.DolbyAtmosForSpeakers")
SUBTYPE_GETTER(st_get_DTSHeadphoneX,           L"Windows.Spatial.Sound.DTSHeadphoneX")
SUBTYPE_GETTER(st_get_DTSXUltra,               L"Windows.Spatial.Sound.DTSXUltra")
SUBTYPE_GETTER(st_get_DTSXForHomeTheater,      L"Windows.Spatial.Sound.DTSXForHomeTheater")

static HRESULT WINAPI st2_QueryInterface(void *i, REFIID iid, void **o) { return subtypes_qi(iid, o); }
static ULONG   WINAPI st2_AddRef(void *i)  { return InterlockedIncrement(&subtypes_obj.ref); }
static ULONG   WINAPI st2_Release(void *i) { return InterlockedDecrement(&subtypes_obj.ref); }

static void init_subtypes(void)
{
    if (subtypes_obj.vtbl) return;
    subtypes_vtbl[0]  = (void *)st_QueryInterface;
    subtypes_vtbl[1]  = (void *)st_AddRef;
    subtypes_vtbl[2]  = (void *)st_Release;
    subtypes_vtbl[3]  = (void *)cfg_GetIids;
    subtypes_vtbl[4]  = (void *)st_GetRuntimeClassName;
    subtypes_vtbl[5]  = (void *)cfg_GetTrustLevel;
    subtypes_vtbl[6]  = (void *)st_get_WindowsSonic;
    subtypes_vtbl[7]  = (void *)st_get_DolbyAtmosForHeadphones;
    subtypes_vtbl[8]  = (void *)st_get_DolbyAtmosForHomeTheater;
    subtypes_vtbl[9]  = (void *)st_get_DolbyAtmosForSpeakers;
    subtypes_vtbl[10] = (void *)st_get_DTSHeadphoneX;
    subtypes_vtbl[11] = (void *)st_get_DTSXUltra;

    subtypes2_vtbl[0] = (void *)st2_QueryInterface;
    subtypes2_vtbl[1] = (void *)st2_AddRef;
    subtypes2_vtbl[2] = (void *)st2_Release;
    subtypes2_vtbl[3] = (void *)cfg_GetIids;
    subtypes2_vtbl[4] = (void *)st_GetRuntimeClassName;
    subtypes2_vtbl[5] = (void *)cfg_GetTrustLevel;
    subtypes2_vtbl[6] = (void *)st_get_DTSXForHomeTheater;

    subtypes_obj.vtbl2 = subtypes2_vtbl;
    subtypes_obj.vtbl = subtypes_vtbl;   /* last: doubles as the initialised flag */
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (cls && !wcscmp(cls, L"Windows.Media.Audio.SpatialAudioFormatSubtype"))
    {
        init_subtypes();
        InterlockedIncrement(&subtypes_obj.ref);
        *out = (IActivationFactory *)&subtypes_obj.vtbl;
        trace("serving SpatialAudioFormatSubtype");
        return S_OK;
    }
    if (!cls || wcscmp(cls, L"Windows.Media.Audio.SpatialAudioDeviceConfiguration"))
    {
        trace("not ours: %ls", cls ? cls : L"(null)");
        return REGDB_E_CLASSNOTREG;
    }
    statics_vtbl[0] = (void *)s_QueryInterface;
    statics_vtbl[1] = (void *)s_AddRef;
    statics_vtbl[2] = (void *)s_Release;
    statics_vtbl[3] = (void *)cfg_GetIids;
    statics_vtbl[4] = (void *)cfg_GetRuntimeClassName;
    statics_vtbl[5] = (void *)cfg_GetTrustLevel;
    statics_vtbl[6] = (void *)s_GetForDeviceId;
    statics_obj.vtbl = statics_vtbl;
    InterlockedIncrement(&statics_obj.ref);
    *out = (IActivationFactory *)&statics_obj.vtbl;
    trace("serving SpatialAudioDeviceConfiguration");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

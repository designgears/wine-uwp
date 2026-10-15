// Microsoft.GamePlatform.Services.*: a stand-in for the Gaming Services client classes.
//
// The Xbox app ships Microsoft.GamePlatform.Services.dll, a client of the Gaming Services NT
// services (GamingServices.exe, LocalSystem, with the xvdd.sys and gameflt.sys kernel drivers).
// Those cannot run under Wine, so the client reports GamePlatformApiStatus "Missing" and the app
// tries to install Gaming Services and never leaves its splash.
//
// This stand-in answers for the client classes instead. It is registered with OverridesPackage=1,
// which makes combase prefer it to the package's own DLL. What it says is honest for a machine
// with a game platform that has no games and does nothing:
//   PlatformStatus.ApiStatus = Ready, its change event registers and never fires;
//   every event registration succeeds; everything else is traced (WINE_UWP_GAMEPLATFORM_TRACE=1
//   is not needed: tracing is always on) and returns E_NOTIMPL until the app shows it needs more.
//
// Slot numbers come from Microsoft.GamePlatform.Services.winmd (winmd.py --guid).
//   IPlatformStatusStatics {c3d6128d-a890-5627-93ae-3f25c8fc50ad}:
//      6 get_ApiStatus  7 add_ApiStatusChanged  8 remove_ApiStatusChanged  9 ConfigureReload
//     10 SuspendUpdates
//   IPackageQueueStatics {de4e4989-2bc5-5d62-8475-435f7a588bbb}:
//      6 SubmitRequestAsync ... 24 CreateView, 25/27/29/31/33 add_*, 26/28/30/32/34 remove_*
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IPlatformStatusStatics = {0xc3d6128d,0xa890,0x5627,{0x93,0xae,0x3f,0x25,0xc8,0xfc,0x50,0xad}};
static const GUID IID_IPackageQueueStatics = {0xde4e4989,0x2bc5,0x5d62,{0x84,0x75,0x43,0x5f,0x7a,0x58,0x8b,0xbb}};

static void tracef(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(stderr, "[gameplatform] %s\n", buf);
    fflush(stderr);
}

enum kind { K_PLATFORM_STATUS, K_PACKAGE_QUEUE, K_COUNT };

struct cls_info
{
    const wchar_t *name;
    const GUID *statics;
    unsigned char slot_kind[64];   /* 0 traced E_NOTIMPL, 1 add_ event, 2 remove_ event, 3 special */
};

static cls_info classes[K_COUNT] =
{
    { L"Microsoft.GamePlatform.Services.PlatformStatus", &IID_IPlatformStatusStatics, {} },
    { L"Microsoft.GamePlatform.Services.PackageQueue", &IID_IPackageQueueStatics, {} },
};

struct Factory
{
    const void **vtbl;
    LONG ref;
    int kind;
    LONGLONG next_token;
};

#define SLOTS 64
static const void *factory_vtbl[K_COUNT][SLOTS];
static Factory factories[K_COUNT];

static void log_guid(const char *what, int kind, REFIID g)
{
    tracef("%ls: %s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", classes[kind].name, what,
           (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
           g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static HRESULT WINAPI f_QueryInterface(Factory *f, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IActivationFactory) || IsEqualGUID(iid, *classes[f->kind].statics))
    {
        InterlockedIncrement(&f->ref);
        *out = f;
        return S_OK;
    }
    log_guid("QueryInterface refused", f->kind, iid);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI f_AddRef(Factory *f) { return InterlockedIncrement(&f->ref); }
static ULONG WINAPI f_Release(Factory *f) { return InterlockedDecrement(&f->ref); }
static HRESULT WINAPI f_GetIids(Factory *, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI f_GetRuntimeClassName(Factory *f, HSTRING *n)
{
    return WindowsCreateString(classes[f->kind].name, (UINT32)wcslen(classes[f->kind].name), n);
}
static HRESULT WINAPI f_GetTrustLevel(Factory *, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* PlatformStatus: the platform is ready */
static HRESULT WINAPI ps_get_ApiStatus(Factory *f, INT32 *value)
{
    tracef("PlatformStatus.ApiStatus -> Ready");
    if (!value) return E_POINTER;
    *value = 0; /* GamePlatformApiStatus.Ready */
    return S_OK;
}

/* an add_ event: (handler, EventRegistrationToken *) */
static HRESULT WINAPI ev_add(Factory *f, IUnknown *handler, LONGLONG *token)
{
    if (!token) return E_POINTER;
    *token = InterlockedIncrement64(&f->next_token);
    tracef("%ls: event registered (token %lld)", classes[f->kind].name, *token);
    return S_OK;
}
static HRESULT WINAPI ev_remove(Factory *, LONGLONG) { return S_OK; }

/* one thunk per slot and per class; nothing is read or written through the arguments */
#define THUNK(k, n) \
    static HRESULT WINAPI t_##k##_##n(Factory *f, void *, void *, void *, void *, void *) \
    { tracef("%ls: slot %d called -> E_NOTIMPL", classes[k].name, n); return E_NOTIMPL; }
#define THUNKS(k) \
    THUNK(k,6) THUNK(k,7) THUNK(k,8) THUNK(k,9) THUNK(k,10) THUNK(k,11) THUNK(k,12) THUNK(k,13) \
    THUNK(k,14) THUNK(k,15) THUNK(k,16) THUNK(k,17) THUNK(k,18) THUNK(k,19) THUNK(k,20) THUNK(k,21) \
    THUNK(k,22) THUNK(k,23) THUNK(k,24) THUNK(k,25) THUNK(k,26) THUNK(k,27) THUNK(k,28) THUNK(k,29) \
    THUNK(k,30) THUNK(k,31) THUNK(k,32) THUNK(k,33) THUNK(k,34) THUNK(k,35) THUNK(k,36) THUNK(k,37) \
    THUNK(k,38) THUNK(k,39) THUNK(k,40) THUNK(k,41) THUNK(k,42) THUNK(k,43) THUNK(k,44) THUNK(k,45) \
    THUNK(k,46) THUNK(k,47) THUNK(k,48) THUNK(k,49) THUNK(k,50) THUNK(k,51) THUNK(k,52) THUNK(k,53) \
    THUNK(k,54) THUNK(k,55) THUNK(k,56) THUNK(k,57) THUNK(k,58) THUNK(k,59) THUNK(k,60) THUNK(k,61) \
    THUNK(k,62) THUNK(k,63)
THUNKS(0) THUNKS(1)
#define SETT(k) \
    { const void *t[] = { (void*)t_##k##_6,(void*)t_##k##_7,(void*)t_##k##_8,(void*)t_##k##_9,(void*)t_##k##_10, \
      (void*)t_##k##_11,(void*)t_##k##_12,(void*)t_##k##_13,(void*)t_##k##_14,(void*)t_##k##_15,(void*)t_##k##_16, \
      (void*)t_##k##_17,(void*)t_##k##_18,(void*)t_##k##_19,(void*)t_##k##_20,(void*)t_##k##_21,(void*)t_##k##_22, \
      (void*)t_##k##_23,(void*)t_##k##_24,(void*)t_##k##_25,(void*)t_##k##_26,(void*)t_##k##_27,(void*)t_##k##_28, \
      (void*)t_##k##_29,(void*)t_##k##_30,(void*)t_##k##_31,(void*)t_##k##_32,(void*)t_##k##_33,(void*)t_##k##_34, \
      (void*)t_##k##_35,(void*)t_##k##_36,(void*)t_##k##_37,(void*)t_##k##_38,(void*)t_##k##_39,(void*)t_##k##_40, \
      (void*)t_##k##_41,(void*)t_##k##_42,(void*)t_##k##_43,(void*)t_##k##_44,(void*)t_##k##_45,(void*)t_##k##_46, \
      (void*)t_##k##_47,(void*)t_##k##_48,(void*)t_##k##_49,(void*)t_##k##_50,(void*)t_##k##_51,(void*)t_##k##_52, \
      (void*)t_##k##_53,(void*)t_##k##_54,(void*)t_##k##_55,(void*)t_##k##_56,(void*)t_##k##_57,(void*)t_##k##_58, \
      (void*)t_##k##_59,(void*)t_##k##_60,(void*)t_##k##_61,(void*)t_##k##_62,(void*)t_##k##_63 }; \
      memcpy(&factory_vtbl[k][6], t, sizeof(t)); }

static void init(void)
{
    static bool done;
    if (done) return;
    done = true;
    SETT(0) SETT(1)
    for (int k = 0; k < K_COUNT; k++)
    {
        factory_vtbl[k][0] = (void *)f_QueryInterface;
        factory_vtbl[k][1] = (void *)f_AddRef;
        factory_vtbl[k][2] = (void *)f_Release;
        factory_vtbl[k][3] = (void *)f_GetIids;
        factory_vtbl[k][4] = (void *)f_GetRuntimeClassName;
        factory_vtbl[k][5] = (void *)f_GetTrustLevel;
        factories[k].vtbl = factory_vtbl[k];
        factories[k].ref = 1;
        factories[k].kind = k;
    }
    factory_vtbl[K_PLATFORM_STATUS][6] = (void *)ps_get_ApiStatus;
    factory_vtbl[K_PLATFORM_STATUS][7] = (void *)ev_add;
    factory_vtbl[K_PLATFORM_STATUS][8] = (void *)ev_remove;
    for (int n = 25; n <= 33; n += 2)
    {
        factory_vtbl[K_PACKAGE_QUEUE][n] = (void *)ev_add;
        factory_vtbl[K_PACKAGE_QUEUE][n + 1] = (void *)ev_remove;
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const wchar_t *name = WindowsGetStringRawBuffer(classid, nullptr);

    init();
    for (int k = 0; k < K_COUNT; k++)
    {
        if (wcscmp(name, classes[k].name)) continue;
        tracef("%ls: factory", name);
        InterlockedIncrement(&factories[k].ref);
        *factory = (IActivationFactory *)&factories[k];
        return S_OK;
    }
    tracef("%ls: no stand-in", name);
    *factory = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

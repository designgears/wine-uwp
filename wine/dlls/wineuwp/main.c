/*
 * wineuwp: in-process stand-ins for the Windows shell brokers a packaged (UWP) app talks to.
 *
 * ActivatableApplicationRegistrar: on Windows the app's CoreApplication registers an activation object
 * with the shell's activation manager (out of process), which then calls back Activate() with the launch
 * arguments. Here the registrar lives in the app process and posts the activation to the calling thread's
 * message queue, so it arrives once CoreApplication::Run is pumping messages, like the real callback does.
 *
 * Interface shapes were recovered from twinapi.appcore.dll (CoreApplication::RegisterActivatableApplication,
 * ApplicationActivationFactory::Activate) with Microsoft's public symbols.
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include "windows.h"
#include "winternl.h"
#include "tlhelp32.h"
#include "initguid.h"
#include "objbase.h"
#include "appmodel.h"
#include "roapi.h"
#include "winstring.h"
#include "inspectable.h"
#include "wine/debug.h"
#include "wine/exception.h"
#include <stdio.h>
#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(wineuwp);

static void maybe_start_stack_dumper(void);

DEFINE_GUID(CLSID_ActivatableApplicationRegistrar, 0xdea794e0, 0x1c1d, 0x4363, 0xb1, 0x71, 0x98, 0xd0, 0xb1, 0x70, 0x35, 0x86);
DEFINE_GUID(IID_IActivatableApplicationRegistrar,  0x036c57fc, 0xe73d, 0x4f3a, 0xb0, 0x95, 0x8b, 0x81, 0x3f, 0xa2, 0xf7, 0x64);
/* Cloaked IActivatableApplication on ApplicationActivationFactory (QI first dword 0x92696c00).
 * IInspectable {af86e2e0-...} is the default vtable; its slot 6 is ActivateInstance, not Activate. */
DEFINE_GUID(IID_IActivatableApplication,           0x92696c00, 0x7578, 0x48e1, 0xac, 0x1a, 0x2c, 0xa9, 0x09, 0xe2, 0xc8, 0xcf);
/* Windows.ApplicationModel.Activation */
DEFINE_GUID(IID_IActivatedEventArgs,                0xcf651713, 0xcd08, 0x4fd8, 0xb6, 0x97, 0xa2, 0x81, 0xb6, 0x54, 0x4e, 0x2e);
DEFINE_GUID(IID_IActivatedEventArgsWithUser,        0x1cf09b9e, 0x9962, 0x4936, 0x80, 0xff, 0xaf, 0xc8, 0xe8, 0xae, 0x5c, 0x8c);
DEFINE_GUID(IID_IStorageItem,                       0x4207a996, 0xca2f, 0x42f7, 0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30);
DEFINE_GUID(IID_IFileActivatedEventArgs,            0xbb2afc33, 0x93b1, 0x42ed, 0x8b, 0x26, 0x23, 0x6d, 0xd9, 0xc7, 0x84, 0x96);
/* IVectorView<IStorageItem>, IIterable<IStorageItem>, IIterator<IStorageItem>: the parameterised
 * IIDs, derived as in tools/wine-uwp/piid.py in the calculator tree. */
DEFINE_GUID(IID_IVectorView_IStorageItem,           0x85575a41, 0x06cb, 0x58d0, 0xb9, 0x8a, 0x7c, 0x8f, 0x06, 0xe6, 0xe9, 0xd7);
DEFINE_GUID(IID_IIterable_IStorageItem,             0xbb8b8418, 0x65d1, 0x544b, 0xb0, 0x83, 0x6d, 0x17, 0x2f, 0x56, 0x8c, 0x73);
DEFINE_GUID(IID_IIterator_IStorageItem,             0x05b487c2, 0x3830, 0x5d3c, 0x98, 0xda, 0x25, 0xfa, 0x11, 0x54, 0x2d, 0xbd);
DEFINE_GUID(IID_IApplicationViewActivatedEventArgs, 0x930cef4b, 0xb829, 0x40fc, 0x88, 0xf4, 0x85, 0x13, 0xe8, 0xa6, 0x47, 0x38);
DEFINE_GUID(IID_ILaunchActivatedEventArgs,          0xfbc93e26, 0xa14a, 0x4b4f, 0x82, 0xb0, 0x33, 0xbe, 0xd9, 0x20, 0xaf, 0x52);
DEFINE_GUID(IID_ILaunchActivatedEventArgs2,         0x0fd37ebc, 0x9dc9, 0x46b5, 0x9a, 0xce, 0xbd, 0x95, 0xd4, 0x56, 0x53, 0x45);
DEFINE_GUID(IID_IPrelaunchActivatedEventArgs,       0x0c44717b, 0x19f7, 0x48d6, 0xb0, 0x46, 0xcf, 0x22, 0x82, 0x6e, 0xaa, 0x74);
DEFINE_GUID(IID_IViewSwitcherProvider,              0x33f288a6, 0x5c2c, 0x4d27, 0xba, 0xc7, 0x75, 0x36, 0x08, 0x8f, 0x12, 0x19);
DEFINE_GUID(IID_ISplashScreen,                      0xca4d975c, 0xd4d6, 0x43f0, 0x97, 0xc0, 0x08, 0x33, 0xc6, 0x39, 0x1c, 0x24);
/* Private, and not a splash screen: named from OneCoreUAPCommonProxyStub's proxy vtable. */
DEFINE_GUID(IID_IInitializeActivatedEventArgs,      0x99fc44e3, 0xce9a, 0x4284, 0xbf, 0x04, 0x76, 0x33, 0x1d, 0x1b, 0x17, 0x88);
/* CoreApplication::GetWindowFactory CoCreates this local-server CLSID (Windows broker). */
DEFINE_GUID(CLSID_CoreWindowBroker,                 0x3480a401, 0xbde9, 0x4407, 0xbc, 0x02, 0x79, 0x8a, 0x86, 0x6a, 0xc0, 0x51);
/* RegisterWithPSM CoCreates this in-proc after incrementing gPsmRegistered. */
DEFINE_GUID(CLSID_PsmBroker,                        0x02844640, 0xe37c, 0x4322, 0xa3, 0xb8, 0x4c, 0x61, 0xa2, 0xe5, 0x88, 0x79);
DEFINE_GUID(IID_IPsmBroker,                         0x33c255c3, 0x1a86, 0x45fc, 0x94, 0x98, 0xf6, 0x0d, 0x8f, 0x56, 0xb9, 0xd3);
DEFINE_GUID(IID_ICoreWindowBroker,                  0x0f4accb1, 0xd8f9, 0x4011, 0xba, 0x37, 0x25, 0x57, 0x92, 0x5a, 0x78, 0xcf);
DEFINE_GUID(IID_ICoreWindowFactory,                 0xcd292360, 0x2763, 0x4085, 0x8a, 0x9f, 0x74, 0xb2, 0x24, 0xa2, 0x91, 0x75);
DEFINE_GUID(IID_IConfigureWindowFactory,            0x601d51e3, 0x801e, 0x49c9, 0xbb, 0xfa, 0xfe, 0x29, 0xa6, 0x62, 0xae, 0xad);
DEFINE_GUID(IID_ICoreWindow,                        0x79b9d5f2, 0x879e, 0x4b89, 0xb7, 0x98, 0x79, 0xe4, 0x75, 0x98, 0x03, 0x0c);
DEFINE_GUID(IID_ICoreWindowStatic,                  0x4d239005, 0x3c2a, 0x41b1, 0x90, 0x22, 0x53, 0x6b, 0xb9, 0xcf, 0x93, 0xb1);
/* Windows.Phone.UI.Core.IImmersiveNavigationClientFactory: Create(VIEW_KIND, flags, INavigationClient**) */
/* CoreApplicationView::PrepareToActivateAsync QIs the navigation client for this and calls its
 * first method, PrepareToActivate(UINT64, IAsyncAction **). */
/* Marshalling the activation onto the view thread: CoreApplication (ICoreImmersiveApplication)
 * -> MainView -> its CoreDispatcher -> RunAsync. */
/* IActivationEventCallback: CoreApplicationView::Activate QIs its second argument for this and
 * calls ActivationHandled (slot 6) when the app has taken the activation. */
DEFINE_GUID(IID_IActivationEventCallback,           0xfefdc269, 0xa317, 0x4e33, 0x9f, 0x50, 0xc2, 0x85, 0xc9, 0xc2, 0x97, 0x25);
DEFINE_GUID(IID_IApplicationStatics,                0x06499997, 0xf7b4, 0x45fe, 0xb7, 0x63, 0x75, 0x77, 0xd1, 0xd3, 0xcb, 0x4a);
DEFINE_GUID(IID_IProtocolActivatedEventArgs,         0x6095f4dd, 0xb7c0, 0x46ab, 0x81, 0xfe, 0xd9, 0x0f, 0x36, 0xd0, 0x0d, 0x24);
DEFINE_GUID(IID_IProtocolActivatedEventArgsWithCallerPackageFamilyNameAndData,
                                                     0xd84a0c12, 0x5c8f, 0x438c, 0x83, 0xcb, 0xc2, 0x8f, 0xcc, 0x0b, 0x2f, 0xdb);
DEFINE_GUID(IID_IUriRuntimeClassFactory,            0x44a9796f, 0x723e, 0x4fdf, 0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84);
DEFINE_GUID(IID_ICoreApplication,                   0x0aacf7a4, 0x5e1d, 0x49df, 0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1);
DEFINE_GUID(IID_ICoreImmersiveApplication,         0x1ada0e3e, 0xe4a2, 0x4123, 0xb4, 0x51, 0xdc, 0x96, 0xbf, 0x80, 0x04, 0x19);
DEFINE_GUID(IID_ICoreApplicationView2,             0x68eb7adf, 0x917f, 0x48eb, 0x9a, 0xeb, 0x7d, 0xe5, 0x3e, 0x08, 0x6a, 0xb1);
DEFINE_GUID(IID_ICoreDispatcher,                   0x60db2fa8, 0xb705, 0x4fde, 0xa7, 0xd6, 0xeb, 0xbb, 0x18, 0x91, 0xd3, 0x9e);
DEFINE_GUID(IID_IDispatchedHandler,                0xd1f276c4, 0x98d8, 0x4636, 0xbf, 0x49, 0xeb, 0x79, 0x50, 0x75, 0x48, 0xe9);
DEFINE_GUID(IID_INavigationClientPrepareToActivate, 0x075be329, 0x825a, 0x4f2a, 0xa8, 0xaf, 0xcd, 0x2f, 0x9a, 0x81, 0x02, 0xec);
DEFINE_GUID(IID_IAsyncActionStandIn,               0x5a648006, 0x843a, 0x4da9, 0x86, 0x5b, 0x9d, 0x26, 0xe5, 0xdf, 0xad, 0x7b);
DEFINE_GUID(IID_IAsyncInfoStandIn,                 0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
/* WindowServer's navigation client property: slot 6 get_NavigationClient, slot 7 put_NavigationClient */
DEFINE_GUID(IID_ICoreWindowNavigation,              0xbb4cbbe9, 0x65af, 0x4730, 0x9e, 0xd2, 0xab, 0xbd, 0xb5, 0xcc, 0x15, 0xb9);

/* ---------------------------------------------------------------------------------------------
 * Interface layouts (C, no headers exist for the private ones)
 */
typedef struct IActivatableApplication IActivatableApplication;
struct IActivatableApplicationVtbl
{
    HRESULT (WINAPI *QueryInterface)(IActivatableApplication *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IActivatableApplication *);
    ULONG   (WINAPI *Release)(IActivatableApplication *);
    HRESULT (WINAPI *GetIids)(IActivatableApplication *, ULONG *, IID **);
    HRESULT (WINAPI *GetRuntimeClassName)(IActivatableApplication *, HSTRING *);
    HRESULT (WINAPI *GetTrustLevel)(IActivatableApplication *, TrustLevel *);
    /* ApplicationActivationFactory::Activate(HSTRING, HSTRING, HSTRING, HSTRING, IActivatedEventArgs*, UINT64, UINT32)
     * Four HSTRINGs: aumid, contract, activation-id, extra. Missing the fourth shifts IActivatedEventArgs
     * onto a stack 0, so ActivateForeground never sees launch args and skips GetWindowFactory. */
    HRESULT (WINAPI *Activate)(IActivatableApplication *, HSTRING aumid, HSTRING contract, HSTRING activation_id,
                               HSTRING extra, IInspectable *args, UINT64 cookie, UINT32 options);
};
struct IActivatableApplication { const struct IActivatableApplicationVtbl *lpVtbl; };

/* ---------------------------------------------------------------------------------------------
 * LaunchActivatedEventArgs
 */
struct launch_args
{
    const void *vtbl_activated;   /* IActivatedEventArgs (IInspectable-derived) */
    const void *vtbl_launch;      /* ILaunchActivatedEventArgs */
    const void *vtbl_launch2;     /* ILaunchActivatedEventArgs2 */
    const void *vtbl_appview;     /* IApplicationViewActivatedEventArgs */
    const void *vtbl_prelaunch;   /* IPrelaunchActivatedEventArgs */
    const void *vtbl_viewswitch;  /* IViewSwitcherProvider */
    const void *vtbl_withuser;    /* IActivatedEventArgsWithUser */
    const void *vtbl_init;        /* IInitializeActivatedEventArgs (private) */
    IUnknown *splash;             /* ISplashScreen, a separate object (see args_get_splash) */
    IUnknown *ftm;
    LONG refcount;
    HSTRING arguments;
    HSTRING tile_id;
    const void *vtbl_file;        /* IFileActivatedEventArgs, when launched on a file */
    WCHAR *file_path;             /* NULL for a plain Launch activation */
    const void *vtbl_protocol;    /* IProtocolActivatedEventArgs, when activated on a URI */
    const void *vtbl_protocol2;   /* IProtocolActivatedEventArgsWithCallerPackageFamilyNameAndData */
    IUnknown *protocol_uri;       /* the Windows.Foundation.Uri; NULL unless a Protocol activation */
};

#define ARGS_FROM(iface, field) ((struct launch_args *)((char *)(iface) - offsetof(struct launch_args, field)))

static HRESULT WINAPI args_QueryInterface(struct launch_args *args, REFIID iid, void **out)
{
    TRACE("%p, %s, %p\n", args, debugstr_guid(iid), out);
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) || IsEqualGUID(iid, &IID_IActivatedEventArgs))
        *out = &args->vtbl_activated;
    /* ProtocolActivatedEventArgs is not a launch: it has no Arguments, TileId or Prelaunch */
    else if (args->protocol_uri && (IsEqualGUID(iid, &IID_ILaunchActivatedEventArgs) ||
             IsEqualGUID(iid, &IID_ILaunchActivatedEventArgs2) || IsEqualGUID(iid, &IID_IPrelaunchActivatedEventArgs)))
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    else if (IsEqualGUID(iid, &IID_IProtocolActivatedEventArgs) && args->protocol_uri) *out = &args->vtbl_protocol;
    else if (IsEqualGUID(iid, &IID_IProtocolActivatedEventArgsWithCallerPackageFamilyNameAndData) && args->protocol_uri)
        *out = &args->vtbl_protocol2;
    else if (IsEqualGUID(iid, &IID_ILaunchActivatedEventArgs)) *out = &args->vtbl_launch;
    else if (IsEqualGUID(iid, &IID_ILaunchActivatedEventArgs2)) *out = &args->vtbl_launch2;
    else if (IsEqualGUID(iid, &IID_IApplicationViewActivatedEventArgs)) *out = &args->vtbl_appview;
    else if (IsEqualGUID(iid, &IID_IPrelaunchActivatedEventArgs)) *out = &args->vtbl_prelaunch;
    else if (IsEqualGUID(iid, &IID_IViewSwitcherProvider)) *out = &args->vtbl_viewswitch;
    else if (IsEqualGUID(iid, &IID_IActivatedEventArgsWithUser)) *out = &args->vtbl_withuser;
    else if (IsEqualGUID(iid, &IID_IFileActivatedEventArgs) && args->file_path) *out = &args->vtbl_file;
    else if (IsEqualGUID(iid, &IID_ISplashScreen))
    {
        /* Not a view of this object: see args_get_splash. */
        IUnknown_AddRef(args->splash);
        *out = args->splash;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IInitializeActivatedEventArgs)) *out = &args->vtbl_init;
    /* These args have no thread affinity; being proxied into the app's apartment only turns
     * every QI the app makes into a marshalling failure. */
    else if (IsEqualGUID(iid, &IID_IMarshal) || IsEqualGUID(iid, &IID_IAgileObject))
        return IUnknown_QueryInterface(args->ftm, iid, out);
    else
    {
        /* {7ca84d06-f54c-4ba8-bd6f-9abaf9380d51} is
         * Windows.Foundation.Marshaling.Internal.IValueMarshalByPropertySet - a cloaked WRL
         * interface that lets args marshal themselves into a PropertySet instead of being
         * COM-proxied (twinapi.appcore's CLaunchActivatedEventArgs carries it as a CloakedIid,
         * with MarshalObjectToPropertySet / UnmarshalObjectFromPropertySet). Only Solitaire asks
         * for it, and refusing is right: the caller falls back to ordinary marshalling, which the
         * FTM above provides. Answering it with stubs made CoreApplicationView::Activate itself
         * fail with E_NOTIMPL and no window was created at all. */
        FIXME("LaunchActivatedEventArgs: unsupported interface %s\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&args->refcount);
    return S_OK;
}
static ULONG WINAPI args_AddRef(struct launch_args *args) { return InterlockedIncrement(&args->refcount); }
static ULONG WINAPI args_Release(struct launch_args *args)
{
    ULONG ref = InterlockedDecrement(&args->refcount);
    if (!ref)
    {
        WindowsDeleteString(args->arguments);
        WindowsDeleteString(args->tile_id);
        if (args->protocol_uri) IUnknown_Release(args->protocol_uri);
        if (args->splash) IUnknown_Release(args->splash);
        if (args->ftm) IUnknown_Release(args->ftm);
        free(args);
    }
    return ref;
}
static HRESULT WINAPI args_GetIids(void *iface, ULONG *count, IID **iids) { *count = 0; *iids = NULL; return S_OK; }
/* The class name is how .NET types the managed wrapper, so a protocol activation must say
 * ProtocolActivatedEventArgs. Every view of the object shares this function, and a view pointer
 * cannot be mapped back to its object without knowing which view it is, so the protocol args
 * answer through their own copies of the table (see protocol_name_vtbl below). */
static HRESULT WINAPI args_GetRuntimeClassName(void *iface, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs";
    return WindowsCreateString(cls, ARRAY_SIZE(cls) - 1, name);
}
static HRESULT WINAPI protocol_args_GetRuntimeClassName(void *iface, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.ApplicationModel.Activation.ProtocolActivatedEventArgs";
    return WindowsCreateString(cls, ARRAY_SIZE(cls) - 1, name);
}
static HRESULT WINAPI args_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }

#define ARGS_IUNKNOWN(field) \
    static HRESULT WINAPI field##_QI(void *iface, REFIID iid, void **out) { return args_QueryInterface(ARGS_FROM(iface, field), iid, out); } \
    static ULONG WINAPI field##_AddRef(void *iface) { return args_AddRef(ARGS_FROM(iface, field)); } \
    static ULONG WINAPI field##_Release(void *iface) { return args_Release(ARGS_FROM(iface, field)); }
ARGS_IUNKNOWN(vtbl_activated) ARGS_IUNKNOWN(vtbl_launch) ARGS_IUNKNOWN(vtbl_launch2) ARGS_IUNKNOWN(vtbl_appview)
ARGS_IUNKNOWN(vtbl_prelaunch) ARGS_IUNKNOWN(vtbl_viewswitch) ARGS_IUNKNOWN(vtbl_withuser)

/* IActivatedEventArgs */
/* ActivationKind: Launch 0, Search 1, ShareTarget 2, File 3, Protocol 4. An app that owns a file
 * type or a scheme branches on this before it looks at the args at all. This used to say File 1
 * and Protocol 2, i.e. Search and ShareTarget: Shapr3D took its sign-in callback for a share,
 * asked for IShareTargetActivatedEventArgs, got E_NOINTERFACE and dropped the callback silently. */
static HRESULT WINAPI activated_get_Kind(void *iface, INT32 *kind)
{
    struct launch_args *args = ARGS_FROM(iface, vtbl_activated);
    *kind = args->protocol_uri ? 4 /* Protocol */ : args->file_path ? 3 /* File */ : 0 /* Launch */;
    TRACE("kind %d\n", *kind);
    return S_OK;
}
/* Protocol args only ever go to the instance that is already running (see activation_inbox_thread). */
static HRESULT WINAPI activated_get_PreviousExecutionState(void *iface, INT32 *state)
{
    struct launch_args *args = ARGS_FROM(iface, vtbl_activated);
    TRACE("\n");
    *state = args->protocol_uri ? 1 /* Running */ : 0 /* NotRunning */;
    return S_OK;
}
/* The args object is its own splash screen: a managed OnLaunched dereferences
 * LaunchActivatedEventArgs.SplashScreen without checking it. */
static HRESULT args_get_splash(struct launch_args *args, void **screen)
{
    TRACE("%p\n", args);
    if (!screen) return E_POINTER;
    /* A separate object on purpose. The projection resolves an interface pointer to its canonical
     * IUnknown and builds the managed wrapper from that object's IInspectable::GetRuntimeClassName,
     * so a SplashScreen sharing identity with the args would be cast to LaunchActivatedEventArgs
     * and the app's `args.SplashScreen` would throw. */
    IUnknown_AddRef(args->splash);
    *screen = args->splash;
    return S_OK;
}
#define ARGS_SPLASH(field) \
    static HRESULT WINAPI field##_get_SplashScreen(void *iface, void **screen) \
    { return args_get_splash(ARGS_FROM(iface, field), screen); }
ARGS_SPLASH(vtbl_activated)
static const void *activated_vtbl[] = { vtbl_activated_QI, vtbl_activated_AddRef, vtbl_activated_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    activated_get_Kind, activated_get_PreviousExecutionState, vtbl_activated_get_SplashScreen };
/* ILaunchActivatedEventArgs */
static HRESULT WINAPI launch_get_Arguments(void *iface, HSTRING *value) { TRACE("\n"); return WindowsDuplicateString(ARGS_FROM(iface, vtbl_launch)->arguments, value); }
static HRESULT WINAPI launch_get_TileId(void *iface, HSTRING *value) { TRACE("\n"); return WindowsDuplicateString(ARGS_FROM(iface, vtbl_launch)->tile_id, value); }
/* ILaunchActivatedEventArgs derives from IInspectable, not from IActivatedEventArgs: Kind,
 * PreviousExecutionState and SplashScreen are only on IActivatedEventArgs {cf651713}. Repeating
 * them here put get_Arguments at slot 9, so a caller asking for the launch arguments (slot 6)
 * got get_Kind and an ActivationKind written through its HSTRING *. */
static const void *launch_vtbl[] = { vtbl_launch_QI, vtbl_launch_AddRef, vtbl_launch_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    launch_get_Arguments, launch_get_TileId };
/* ILaunchActivatedEventArgs2 */
static HRESULT WINAPI launch2_get_TileActivatedInfo(void *iface, void **info) { TRACE("\n"); *info = NULL; return S_OK; }
static const void *launch2_vtbl[] = { vtbl_launch2_QI, vtbl_launch2_AddRef, vtbl_launch2_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    launch2_get_TileActivatedInfo };
/* IApplicationViewActivatedEventArgs */
static HRESULT WINAPI appview_get_CurrentlyShownApplicationViewId(void *iface, INT32 *id) { TRACE("\n"); *id = 0; return S_OK; }
static const void *appview_vtbl[] = { vtbl_appview_QI, vtbl_appview_AddRef, vtbl_appview_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    appview_get_CurrentlyShownApplicationViewId };
/* IPrelaunchActivatedEventArgs */
static HRESULT WINAPI prelaunch_get_PrelaunchActivated(void *iface, boolean *value) { TRACE("\n"); *value = FALSE; return S_OK; }
static const void *prelaunch_vtbl[] = { vtbl_prelaunch_QI, vtbl_prelaunch_AddRef, vtbl_prelaunch_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    prelaunch_get_PrelaunchActivated };
/* IViewSwitcherProvider */
static HRESULT WINAPI viewswitch_get_ViewSwitcher(void *iface, void **switcher) { FIXME("no view switcher\n"); *switcher = NULL; return S_OK; }
static const void *viewswitch_vtbl[] = { vtbl_viewswitch_QI, vtbl_viewswitch_AddRef, vtbl_viewswitch_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    viewswitch_get_ViewSwitcher };
/* IActivatedEventArgsWithUser */
/* Windows.System.User: a packaged app reads args.User.NonRoamableId to key its per-user state,
 * so this has to be a real object. Wine has a single local user. */
DEFINE_GUID(IID_IUserStandIn, 0xdf9a26c6, 0xe746, 0x4bcd, 0xb5, 0xd4, 0x12, 0x01, 0x03, 0xc4, 0x20, 0x9b);

struct sys_user
{
    const void **vtbl;
    IUnknown *ftm;
    LONG refcount;
};

static HRESULT WINAPI user_QueryInterface(struct sys_user *user, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IUserStandIn))
        *out = &user->vtbl;
    else if (IsEqualGUID(iid, &IID_IMarshal) || IsEqualGUID(iid, &IID_IAgileObject))
        return IUnknown_QueryInterface(user->ftm, iid, out);
    else
    {
        FIXME("Windows.System.User: unsupported interface %s\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&user->refcount);
    return S_OK;
}
static ULONG WINAPI user_AddRef(struct sys_user *user) { return InterlockedIncrement(&user->refcount); }
static ULONG WINAPI user_Release(struct sys_user *user)
{
    ULONG ref = InterlockedDecrement(&user->refcount);
    if (!ref)
    {
        if (user->ftm) IUnknown_Release(user->ftm);
        free(user);
    }
    return ref;
}
static HRESULT WINAPI user_GetIids(void *iface, ULONG *count, IID **iids) { *count = 0; *iids = NULL; return S_OK; }
static HRESULT WINAPI user_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.System.User", 19, name);
}
static HRESULT WINAPI user_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI user_get_NonRoamableId(void *iface, HSTRING *value)
{
    /* Stable for the lifetime of the prefix; apps only use it as a key. */
    return WindowsCreateString(L"wine-uwp-local-user", 19, value);
}
static HRESULT WINAPI user_get_AuthenticationStatus(void *iface, INT32 *value)
{
    *value = 1 /* LocallyAuthenticated */; return S_OK;
}
static HRESULT WINAPI user_get_Type(void *iface, INT32 *value) { *value = 0 /* LocalUser */; return S_OK; }
static HRESULT WINAPI user_not_impl(void *iface) { FIXME("stub\n"); return E_NOTIMPL; }
static const void *user_vtbl[] = { user_QueryInterface, user_AddRef, user_Release,
    user_GetIids, user_GetRuntimeClassName, user_GetTrustLevel,
    user_get_NonRoamableId, user_get_AuthenticationStatus, user_get_Type,
    user_not_impl, user_not_impl, user_not_impl };

static HRESULT WINAPI withuser_get_User(void *iface, void **out)
{
    struct sys_user *user;

    TRACE("%p\n", out);
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(user = calloc(1, sizeof(*user)))) return E_OUTOFMEMORY;
    user->vtbl = user_vtbl;
    user->refcount = 1;
    if (FAILED(CoCreateFreeThreadedMarshaler((IUnknown *)&user->vtbl, &user->ftm)))
    {
        free(user);
        return E_OUTOFMEMORY;
    }
    *out = &user->vtbl;
    return S_OK;
}
static const void *withuser_vtbl[] = { vtbl_withuser_QI, vtbl_withuser_AddRef, vtbl_withuser_Release, args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    withuser_get_User };
/* Windows.Foundation.Rect: four floats, not a RECT. */
struct foundation_rect { FLOAT x, y, width, height; };

/* The one view this host creates (PrivateCreateCoreWindow below), in physical pixels at 100%. */
#define CORE_WINDOW_WIDTH  1280
#define CORE_WINDOW_HEIGHT 800

/* The hosted CoreWindow, once shown. The view bounds reported to windows.ui follow its client
 * area, so the window can be resized through the normal frame it is given. */
static HWND hosted_core_window;
/* posted to the CoreWindow to report it visible; see core_window_subclass_proc */
#define WM_WINE_UWP_VISIBLE (WM_APP + 0x270)
/* WINE_UWP_WINDOW_SIZE=WxH overrides the launch size (a window that covers the whole screen is not
 * resizable through the frame, which matters on small test displays). */
static void launch_window_size( int *width, int *height )
{
    WCHAR env[32];
    int w, h;
    if (GetEnvironmentVariableW( L"WINE_UWP_WINDOW_SIZE", env, ARRAY_SIZE(env) ) &&
        swscanf( env, L"%dx%d", &w, &h ) == 2 && w > 100 && h > 100)
    {
        *width = w;
        *height = h;
        return;
    }
    *width = CORE_WINDOW_WIDTH;
    *height = CORE_WINDOW_HEIGHT;
}
static void current_view_size( FLOAT *width, FLOAT *height )
{
    RECT rc;
    int w, h;
    if (hosted_core_window && GetClientRect( hosted_core_window, &rc ) && rc.right > 0 && rc.bottom > 0)
    {
        *width = (FLOAT)rc.right;
        *height = (FLOAT)rc.bottom;
        return;
    }
    launch_window_size( &w, &h );
    *width = (FLOAT)w;
    *height = (FLOAT)h;
}

/* A WinRT delegate is IUnknown + Invoke: three slots then Invoke, not IInspectable's six. */
struct splash_handler;
struct splash_handler_vtbl
{
    HRESULT (WINAPI *QueryInterface)( struct splash_handler *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( struct splash_handler * );
    ULONG (WINAPI *Release)( struct splash_handler * );
    HRESULT (WINAPI *Invoke)( struct splash_handler *, void *sender, void *args );
};
struct splash_handler { const struct splash_handler_vtbl *lpVtbl; };

struct splash_screen
{
    const void **vtbl;
    IUnknown *ftm;
    LONG refcount;
    struct splash_handler *dismissed;   /* the app's Dismissed handler, if it registered one */
    LONG fired;
};

static HRESULT WINAPI splash_QueryInterface(struct splash_screen *splash, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_ISplashScreen))
        *out = &splash->vtbl;
    else if (IsEqualGUID(iid, &IID_IMarshal) || IsEqualGUID(iid, &IID_IAgileObject))
        return IUnknown_QueryInterface(splash->ftm, iid, out);
    else
    {
        FIXME("SplashScreen: unsupported interface %s\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&splash->refcount);
    return S_OK;
}
static ULONG WINAPI splash_AddRef(struct splash_screen *splash) { return InterlockedIncrement(&splash->refcount); }
static ULONG WINAPI splash_Release(struct splash_screen *splash)
{
    ULONG ref = InterlockedDecrement(&splash->refcount);
    if (!ref)
    {
        if (splash->dismissed) splash->dismissed->lpVtbl->Release(splash->dismissed);
        if (splash->ftm) IUnknown_Release(splash->ftm);
        free(splash);
    }
    return ref;
}
static HRESULT WINAPI splash_GetIids(void *iface, ULONG *count, IID **iids) { *count = 0; *iids = NULL; return S_OK; }
static HRESULT WINAPI splash_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.ApplicationModel.Activation.SplashScreen", 47, name);
}
static HRESULT WINAPI splash_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI splash_get_ImageLocation(void *iface, struct foundation_rect *rect)
{
    TRACE("%p\n", rect);
    if (!rect) return E_POINTER;
    rect->x = rect->y = 0.0f;
    rect->width = CORE_WINDOW_WIDTH;
    rect->height = CORE_WINDOW_HEIGHT;
    return S_OK;
}
/* The extended-splash pattern: an app registers Dismissed and only then sets Window.Content, so a
 * registration that is accepted and never fired leaves the app running with no UI forever. That is
 * what Solitaire does - it builds a Page, never calls Window.put_Content (confirmed with a ud2
 * probe on WindowGenerated::put_Content) and idles at 60 Hz.
 *
 * Windows raises Dismissed on a background thread once the splash is taken down, and apps are
 * expected to marshal to the dispatcher themselves, so firing from a worker here matches the real
 * contract. The delay gives the view thread time to finish activating first. */
static DWORD WINAPI splash_dismiss_thread(void *arg)
{
    struct splash_screen *splash = arg;
    struct splash_handler *handler;

    Sleep(1500);

    handler = splash->dismissed;
    if (handler && !InterlockedExchange(&splash->fired, 1))
    {
        HRESULT hr;
        ERR("raising SplashScreen.Dismissed\n");
        hr = handler->lpVtbl->Invoke(handler, &splash->vtbl, NULL);
        if (FAILED(hr)) WARN("Dismissed handler returned %#lx\n", hr);
    }
    splash_Release(splash);
    return 0;
}

static HRESULT WINAPI splash_add_Dismissed(void *iface, void *handler, UINT64 *token)
{
    struct splash_screen *splash = iface;
    HANDLE thread;

    TRACE("%p\n", handler);

    if (splash->dismissed) splash->dismissed->lpVtbl->Release(splash->dismissed);
    if ((splash->dismissed = handler)) splash->dismissed->lpVtbl->AddRef(splash->dismissed);

    if (splash->dismissed)
    {
        InterlockedIncrement(&splash->refcount);
        if ((thread = CreateThread(NULL, 0, splash_dismiss_thread, splash, 0, NULL)))
            CloseHandle(thread);
        else
            splash_Release(splash);
    }
    if (token) *token = 1;
    return S_OK;
}
static HRESULT WINAPI splash_remove_Dismissed(void *iface, UINT64 token)
{
    struct splash_screen *splash = iface;

    if (splash->dismissed) splash->dismissed->lpVtbl->Release(splash->dismissed);
    splash->dismissed = NULL;
    return S_OK;
}
static const void *splash_vtbl[] = { splash_QueryInterface, splash_AddRef, splash_Release,
    splash_GetIids, splash_GetRuntimeClassName, splash_GetTrustLevel,
    splash_get_ImageLocation, splash_add_Dismissed, splash_remove_Dismissed };

static IUnknown *create_splash_screen(void)
{
    struct splash_screen *splash;

    if (!(splash = calloc(1, sizeof(*splash)))) return NULL;
    splash->vtbl = splash_vtbl;
    splash->refcount = 1;
    if (FAILED(CoCreateFreeThreadedMarshaler((IUnknown *)&splash->vtbl, &splash->ftm)))
    {
        free(splash);
        return NULL;
    }
    return (IUnknown *)&splash->vtbl;
}

/* IInitializeActivatedEventArgs {99fc44e3}: how CoreApplicationView hands an args object the
 * details it could not know at construction. Slots from CActivatedEventArgsBase's vtable in
 * twinapi.appcore (RVA 0x1a6568); the signatures are the ones in its public symbols:
 *   Initialize( UINT64, HSTRING, ApplicationExecutionState, byte, byte, HSTRING, GUID )
 *   InitializeViewDependent( ApplicationExecutionState, int viewId, ISplashScreen *, byte )
 * The splash screen arrives through the second one, so keep it: it is the one the app will see.
 * (This IID was previously mistaken for ISplashScreen - the real one is {ca4d975c} - and
 * answered with the splash object, which gave the caller a vtable of the wrong shape.) */
static HRESULT WINAPI args_init_QueryInterface(void *iface, REFIID iid, void **out)
{
    return args_QueryInterface(ARGS_FROM(iface, vtbl_init), iid, out);
}
static ULONG WINAPI args_init_AddRef(void *iface)
{
    return args_AddRef(ARGS_FROM(iface, vtbl_init));
}
static ULONG WINAPI args_init_Release(void *iface)
{
    return args_Release(ARGS_FROM(iface, vtbl_init));
}
static HRESULT WINAPI args_Initialize(void *iface, UINT64 unknown, HSTRING arguments, INT32 state,
                                      BYTE flag1, BYTE flag2, HSTRING tile_id, const GUID *id)
{
    struct launch_args *args = ARGS_FROM(iface, vtbl_init);

    TRACE("%p %#I64x %s state %d %u %u %s %s\n", args, unknown, debugstr_hstring(arguments), state,
          flag1, flag2, debugstr_hstring(tile_id), debugstr_guid(id));
    if (arguments)
    {
        WindowsDeleteString(args->arguments);
        WindowsDuplicateString(arguments, &args->arguments);
    }
    if (tile_id)
    {
        WindowsDeleteString(args->tile_id);
        WindowsDuplicateString(tile_id, &args->tile_id);
    }
    return S_OK;
}
static HRESULT WINAPI args_InitializeViewDependent(void *iface, INT32 state, INT32 view_id,
                                                   IUnknown *splash, BYTE flag)
{
    struct launch_args *args = ARGS_FROM(iface, vtbl_init);

    TRACE("%p state %d view %d splash %p %u\n", args, state, view_id, splash, flag);
    if (splash)
    {
        IUnknown_AddRef(splash);
        if (args->splash) IUnknown_Release(args->splash);
        args->splash = splash;
    }
    return S_OK;
}
static const void *args_init_vtbl[] = {
    args_init_QueryInterface, args_init_AddRef, args_init_Release,
    args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    args_Initialize, args_InitializeViewDependent };

/* ---------------------------------------------------------------------------------------------
 * FileActivatedEventArgs
 *
 * Launching a packaged app on a file is how "open with" works, and the app that owns the file type
 * expects ActivationKind_File with the file in Files. Without it the only activation the host
 * could produce was a plain Launch, so an app that can only start playing from a file activation
 * had no way in.
 */
struct file_vector
{
    const void *vtbl_view;        /* IVectorView<IStorageItem> */
    const void *vtbl_iterable;    /* IIterable<IStorageItem> */
    LONG refcount;
    IUnknown *item;               /* the single activated file, as IStorageItem */
};

struct file_iterator
{
    const void *vtbl;             /* IIterator<IStorageItem> */
    LONG refcount;
    struct file_vector *vector;
    BOOL done;
};

#define FV_FROM(iface, field) ((struct file_vector *)((char *)(iface) - offsetof(struct file_vector, field)))

static HRESULT WINAPI fv_QueryInterface(void *iface, REFIID iid, void **out);
static ULONG WINAPI fv_AddRef(void *iface);
static ULONG WINAPI fv_Release(void *iface);

static HRESULT WINAPI fv_view_QueryInterface(void *iface, REFIID iid, void **out)
{ return fv_QueryInterface(iface, iid, out); }
static ULONG WINAPI fv_view_AddRef(void *iface) { return fv_AddRef(iface); }
static ULONG WINAPI fv_view_Release(void *iface) { return fv_Release(iface); }
static HRESULT WINAPI fv_iterable_QueryInterface(void *iface, REFIID iid, void **out)
{ return fv_QueryInterface(FV_FROM(iface, vtbl_iterable), iid, out); }
static ULONG WINAPI fv_iterable_AddRef(void *iface) { return fv_AddRef(FV_FROM(iface, vtbl_iterable)); }
static ULONG WINAPI fv_iterable_Release(void *iface) { return fv_Release(FV_FROM(iface, vtbl_iterable)); }

static HRESULT WINAPI fv_QueryInterface(void *iface, REFIID iid, void **out)
{
    struct file_vector *fv = iface;

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IVectorView_IStorageItem))
        *out = &fv->vtbl_view;
    else if (IsEqualGUID(iid, &IID_IIterable_IStorageItem))
        *out = &fv->vtbl_iterable;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&fv->refcount);
    return S_OK;
}

static ULONG WINAPI fv_AddRef(void *iface)
{
    struct file_vector *fv = iface;
    return InterlockedIncrement(&fv->refcount);
}

static ULONG WINAPI fv_Release(void *iface)
{
    struct file_vector *fv = iface;
    ULONG ref = InterlockedDecrement(&fv->refcount);
    if (!ref) { if (fv->item) IUnknown_Release(fv->item); free(fv); }
    return ref;
}

static HRESULT WINAPI fv_GetIids(void *iface, ULONG *count, IID **iids)
{ if (count) *count = 0; if (iids) *iids = NULL; return S_OK; }
static HRESULT WINAPI fv_GetRuntimeClassName(void *iface, HSTRING *name)
{ return WindowsCreateString(L"Windows.Foundation.Collections.IVectorView`1<Windows.Storage.IStorageItem>", 68, name); }
static HRESULT WINAPI fv_GetTrustLevel(void *iface, INT32 *level) { if (level) *level = 0; return S_OK; }

static HRESULT WINAPI fv_GetAt(void *iface, UINT32 index, IUnknown **value)
{
    struct file_vector *fv = iface;
    if (!value) return E_POINTER;
    *value = NULL;
    if (index || !fv->item) return E_BOUNDS;
    IUnknown_AddRef((*value = fv->item));
    return S_OK;
}

static HRESULT WINAPI fv_get_Size(void *iface, UINT32 *value)
{
    struct file_vector *fv = iface;
    if (!value) return E_POINTER;
    *value = fv->item ? 1 : 0;
    return S_OK;
}

static HRESULT WINAPI fv_IndexOf(void *iface, IUnknown *item, UINT32 *index, BOOLEAN *found)
{
    struct file_vector *fv = iface;
    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = (fv->item && item == fv->item);
    return S_OK;
}

static HRESULT WINAPI fv_GetMany(void *iface, UINT32 start, UINT32 count, IUnknown **items, UINT32 *written)
{
    struct file_vector *fv = iface;
    if (!written) return E_POINTER;
    *written = 0;
    if (!start && count && fv->item) { IUnknown_AddRef((items[0] = fv->item)); *written = 1; }
    return S_OK;
}

static HRESULT WINAPI fi_QueryInterface(void *iface, REFIID iid, void **out)
{
    struct file_iterator *it = iface;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IIterator_IStorageItem))
    {
        InterlockedIncrement(&it->refcount);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI fi_AddRef(void *iface) { return InterlockedIncrement(&((struct file_iterator *)iface)->refcount); }
static ULONG WINAPI fi_Release(void *iface)
{
    struct file_iterator *it = iface;
    ULONG ref = InterlockedDecrement(&it->refcount);
    if (!ref) { fv_Release(it->vector); free(it); }
    return ref;
}
static HRESULT WINAPI fi_get_Current(void *iface, IUnknown **value)
{
    struct file_iterator *it = iface;
    if (!value) return E_POINTER;
    *value = NULL;
    if (it->done || !it->vector->item) return E_BOUNDS;
    IUnknown_AddRef((*value = it->vector->item));
    return S_OK;
}
static HRESULT WINAPI fi_get_HasCurrent(void *iface, BOOLEAN *value)
{
    struct file_iterator *it = iface;
    if (!value) return E_POINTER;
    *value = !it->done && it->vector->item;
    return S_OK;
}
static HRESULT WINAPI fi_MoveNext(void *iface, BOOLEAN *value)
{
    struct file_iterator *it = iface;
    it->done = TRUE;                 /* one element */
    if (value) *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI fi_GetMany(void *iface, UINT32 count, IUnknown **items, UINT32 *written)
{
    struct file_iterator *it = iface;
    if (!written) return E_POINTER;
    *written = 0;
    if (!it->done && count && it->vector->item)
    { IUnknown_AddRef((items[0] = it->vector->item)); it->done = TRUE; *written = 1; }
    return S_OK;
}

static const void *file_iterator_vtbl[] = {
    fi_QueryInterface, fi_AddRef, fi_Release,
    fv_GetIids, fv_GetRuntimeClassName, fv_GetTrustLevel,
    fi_get_Current, fi_get_HasCurrent, fi_MoveNext, fi_GetMany };

static HRESULT WINAPI fv_First(void *iface, void **value)
{
    struct file_vector *fv = FV_FROM(iface, vtbl_iterable);
    struct file_iterator *it;

    if (!value) return E_POINTER;
    if (!(it = calloc(1, sizeof(*it)))) return E_OUTOFMEMORY;
    it->vtbl = file_iterator_vtbl;
    it->refcount = 1;
    it->vector = fv;
    fv_AddRef(fv);
    *value = it;
    return S_OK;
}

static const void *file_view_vtbl[] = {
    fv_view_QueryInterface, fv_view_AddRef, fv_view_Release,
    fv_GetIids, fv_GetRuntimeClassName, fv_GetTrustLevel,
    fv_GetAt, fv_get_Size, fv_IndexOf, fv_GetMany };

static const void *file_iterable_vtbl[] = {
    fv_iterable_QueryInterface, fv_iterable_AddRef, fv_iterable_Release,
    fv_GetIids, fv_GetRuntimeClassName, fv_GetTrustLevel,
    fv_First };

ARGS_IUNKNOWN(vtbl_file)

static struct file_vector *create_file_vector(const WCHAR *path);

/* The StorageFile is made here rather than when the args are built. Creating it needs
 * windows.storage loaded, and doing that while the app is being activated takes the loader lock at
 * the worst possible moment - the same shape of problem as msftedit's DllMain, and it stopped the
 * app painting every time. By the time an app asks for Files it is running and the load is safe. */
static HRESULT WINAPI file_args_get_Files(void *iface, void **value)
{
    struct launch_args *args = ARGS_FROM(iface, vtbl_file);
    struct file_vector *fv;

    TRACE("%p\n", value);

    if (!value) return E_POINTER;
    *value = NULL;
    if (!args->file_path) return E_FAIL;
    if (!(fv = create_file_vector(args->file_path))) return E_FAIL;
    *value = &fv->vtbl_view;
    return S_OK;
}

/* The verb says which of the app's registered actions was chosen; only the default one exists
 * here, and Windows reports an empty string for it. */
static HRESULT WINAPI file_args_get_Verb(void *iface, HSTRING *value)
{
    return WindowsCreateString(L"", 0, value);
}

static const void *file_args_vtbl[] = {
    vtbl_file_QI, vtbl_file_AddRef, vtbl_file_Release,
    args_GetIids, args_GetRuntimeClassName, args_GetTrustLevel,
    file_args_get_Files, file_args_get_Verb };

/* Build the IStorageItem for a file activation. windows.storage makes the object directly through
 * __wine_storage_file_create; going through GetFileFromPathAsync here would mean driving an async
 * operation from inside activation. */
static struct file_vector *create_file_vector(const WCHAR *path)
{
    HRESULT (WINAPI *create_file)(const WCHAR *, IUnknown **);
    struct file_vector *fv;
    IUnknown *file = NULL, *item = NULL;
    HMODULE storage;

    if (!(storage = LoadLibraryW(L"windows.storage.dll")))
    {
        ERR("cannot load windows.storage.dll, error %lu\n", GetLastError());
        return NULL;
    }
    if (!(create_file = (void *)GetProcAddress(storage, "__wine_storage_file_create")))
    {
        ERR("windows.storage.dll has no __wine_storage_file_create\n");
        return NULL;
    }
    if (FAILED(create_file(path, &file)))
    {
        ERR("cannot make a StorageFile for %s\n", debugstr_w(path));
        return NULL;
    }
    /* Files is a vector of IStorageItem, not of IStorageFile. */
    if (FAILED(IUnknown_QueryInterface(file, &IID_IStorageItem, (void **)&item)))
    {
        IUnknown_Release(file);
        return NULL;
    }
    IUnknown_Release(file);

    if (!(fv = calloc(1, sizeof(*fv))))
    {
        IUnknown_Release(item);
        return NULL;
    }
    fv->vtbl_view = file_view_vtbl;
    fv->vtbl_iterable = file_iterable_vtbl;
    fv->refcount = 1;
    fv->item = item;
    return fv;
}

static IInspectable *create_launch_args_for_file(const WCHAR *arguments, const WCHAR *file);

/* ---------------------------------------------------------------------------------------------
 * ProtocolActivatedEventArgs: the same object as the launch args, in protocol mode. It is how an
 * app that owns a URI scheme gets that URI - Shapr3D's sign-in comes back as shapr3d:... from the
 * browser, to the instance that started it (only that instance holds the PKCE verifier).
 */
static HRESULT WINAPI protocol_get_Uri(void *iface, IUnknown **value)
{
    struct launch_args *args = ARGS_FROM(iface, vtbl_protocol);
    TRACE("\n");
    if (!value) return E_POINTER;
    IUnknown_AddRef((*value = args->protocol_uri));
    return S_OK;
}
static HRESULT WINAPI protocol2_get_CallerPackageFamilyName(void *iface, HSTRING *value)
{
    TRACE("\n");
    return WindowsCreateString(L"", 0, value);   /* the browser is not a package */
}
/* A ValueSet the app can read and find empty, rather than a NULL it would dereference. */
static HRESULT WINAPI protocol2_get_Data(void *iface, IInspectable **value)
{
    HSTRING_HEADER header;
    HSTRING name;
    HRESULT hr;

    TRACE("\n");
    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = WindowsCreateStringReference(L"Windows.Foundation.Collections.ValueSet", 39, &header, &name)))
        return hr;
    return RoActivateInstance(name, value);
}
ARGS_IUNKNOWN(vtbl_protocol) ARGS_IUNKNOWN(vtbl_protocol2)
static const void *protocol_vtbl[] = { vtbl_protocol_QI, vtbl_protocol_AddRef, vtbl_protocol_Release, args_GetIids,
    protocol_args_GetRuntimeClassName, args_GetTrustLevel, protocol_get_Uri };
static const void *protocol2_vtbl[] = { vtbl_protocol2_QI, vtbl_protocol2_AddRef, vtbl_protocol2_Release, args_GetIids,
    protocol_args_GetRuntimeClassName, args_GetTrustLevel, protocol2_get_CallerPackageFamilyName, protocol2_get_Data };

/* Copies of the shared view tables whose GetRuntimeClassName says ProtocolActivatedEventArgs. */
static const void **protocol_view_vtbl(const void **src, size_t count, const void ***cache)
{
    const void **copy;

    if (*cache) return *cache;
    if (!(copy = malloc(count * sizeof(*copy)))) return src;
    memcpy(copy, src, count * sizeof(*copy));
    copy[4] = protocol_args_GetRuntimeClassName;
    if (InterlockedCompareExchangePointer((void **)cache, copy, NULL)) free(copy);
    return *cache;
}

static IInspectable *create_protocol_args(const WCHAR *uri)
{
    static const void **activated_copy, **appview_copy, **viewswitch_copy, **withuser_copy, **init_copy;
    IUnknown **uri_factory;
    HSTRING_HEADER header;
    struct launch_args *args;
    IInspectable *inspectable;
    HSTRING name, value;
    HRESULT hr;

    if (!(inspectable = create_launch_args_for_file(L"", NULL))) return NULL;
    args = ARGS_FROM(inspectable, vtbl_activated);

    if (SUCCEEDED(hr = WindowsCreateStringReference(L"Windows.Foundation.Uri", 22, &header, &name)) &&
        SUCCEEDED(hr = RoGetActivationFactory(name, &IID_IUriRuntimeClassFactory, (void **)&uri_factory)))
    {
        if (SUCCEEDED(hr = WindowsCreateString(uri, lstrlenW(uri), &value)))
        {
            /* IUriRuntimeClassFactory::CreateUri is slot 6 */
            hr = ((HRESULT (WINAPI *)(void *, HSTRING, IUnknown **))(*(void ***)uri_factory)[6])(uri_factory, value, &args->protocol_uri);
            WindowsDeleteString(value);
        }
        IUnknown_Release((IUnknown *)uri_factory);
    }
    if (FAILED(hr) || !args->protocol_uri)
    {
        ERR("cannot make a Uri of %s, hr %#lx\n", debugstr_w(uri), hr);
        IInspectable_Release(inspectable);
        return NULL;
    }
    args->vtbl_protocol = protocol_vtbl;
    args->vtbl_protocol2 = protocol2_vtbl;
    args->vtbl_activated = protocol_view_vtbl(activated_vtbl, ARRAY_SIZE(activated_vtbl), &activated_copy);
    args->vtbl_appview = protocol_view_vtbl(appview_vtbl, ARRAY_SIZE(appview_vtbl), &appview_copy);
    args->vtbl_viewswitch = protocol_view_vtbl(viewswitch_vtbl, ARRAY_SIZE(viewswitch_vtbl), &viewswitch_copy);
    args->vtbl_withuser = protocol_view_vtbl(withuser_vtbl, ARRAY_SIZE(withuser_vtbl), &withuser_copy);
    args->vtbl_init = protocol_view_vtbl(args_init_vtbl, ARRAY_SIZE(args_init_vtbl), &init_copy);
    return inspectable;
}

static IInspectable *create_launch_args_for_file(const WCHAR *arguments, const WCHAR *file);

/* Both activation paths come through here, so the file is picked up wherever the activation is
 * driven from. WINE_UWP_ACTIVATE_FILE is how a caller says "open this file with the app". */
static IInspectable *create_launch_args(const WCHAR *arguments)
{
    WCHAR file[MAX_PATH];

    if (GetEnvironmentVariableW(L"WINE_UWP_ACTIVATE_FILE", file, ARRAY_SIZE(file)))
    {
        ERR("activating on file %s\n", debugstr_w(file));
        return create_launch_args_for_file(arguments, file);
    }
    return create_launch_args_for_file(arguments, NULL);
}

static IInspectable *create_launch_args_for_file(const WCHAR *arguments, const WCHAR *file)
{
    struct launch_args *args = calloc(1, sizeof(*args));
    if (!args) return NULL;
    args->vtbl_activated = activated_vtbl; args->vtbl_launch = launch_vtbl; args->vtbl_launch2 = launch2_vtbl;
    args->vtbl_appview = appview_vtbl; args->vtbl_prelaunch = prelaunch_vtbl; args->vtbl_viewswitch = viewswitch_vtbl;
    args->vtbl_withuser = withuser_vtbl; args->vtbl_init = args_init_vtbl;
    args->vtbl_file = file_args_vtbl;
    if (file && *file && !(args->file_path = wcsdup(file)))
        WARN("cannot activate on %s; falling back to a plain launch\n", debugstr_w(file));
    if (!(args->splash = create_splash_screen())) { free(args); return NULL; }
    args->refcount = 1;
    if (FAILED(CoCreateFreeThreadedMarshaler((IUnknown *)&args->vtbl_activated, &args->ftm)))
    {
        free(args);
        return NULL;
    }
    WindowsCreateString(arguments, lstrlenW(arguments), &args->arguments);
    WindowsCreateString(L"", 0, &args->tile_id);
    return (IInspectable *)&args->vtbl_activated;
}

/* ---------------------------------------------------------------------------------------------
 * ActivatableApplicationRegistrar
 */
struct registrar
{
    const void *vtbl;
    LONG refcount;
    IUnknown *app;
    /* CoreApplication::RegisterActivatableApplication runs on the thread that called
     * CoreApplication::Run, which is the apartment the CoreApplicationView belongs to. */
    DWORD owner_tid;
};
static ULONG WINAPI registrar_Release( struct registrar *reg );

/* Same-thread execute breakpoints in twinapi (hardware DRs are per-thread). */
static LONG CALLBACK twinapi_break_handler( EXCEPTION_POINTERS *info )
{
    if (info->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP)
    {
        CONTEXT *ctx = info->ContextRecord;
        ERR( "twinapi break rip %p rcx %p rdx %p r8 %p r9 %p rax %p\n",
             (void *)ctx->Rip, (void *)ctx->Rcx, (void *)ctx->Rdx,
             (void *)ctx->R8, (void *)ctx->R9, (void *)ctx->Rax );
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000; /* RF: skip the breakpoint instruction once */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* WINE_UWP_BREAK_TWINAPI is a comma separated list of up to four twinapi.appcore RVAs to arm as
 * execute breakpoints on the activation thread; empty means the three sites this milestone kept
 * coming back to. Every hit reports its arguments through twinapi_break_handler. */
static void arm_twinapi_breaks(void)
{
    static const ULONG_PTR defaults[] =
    {
        0x5fc20,  /* ApplicationActivationFactory::Activate */
        0x4fc30,  /* CoreApplication::ActivateForeground */
        0x288a0,  /* CoreApplicationView::SetWindowAndGetDispatcher */
    };
    ULONG_PTR offsets[4];
    unsigned int count = 0, i;
    char env[128], *p;
    HMODULE mod;
    CONTEXT ctx;
    static LONG handler_added;

    if (!GetEnvironmentVariableA( "WINE_UWP_BREAK_TWINAPI", env, sizeof(env) )) return;
    for (p = env; *p && count < ARRAY_SIZE(offsets); )
    {
        char *next;
        ULONG_PTR off = strtoull( p, &next, 16 );
        if (next == p) break;
        offsets[count++] = off;
        for (p = next; *p == ',' || *p == ' '; p++) ;
    }
    if (!count)
        for (count = 0; count < ARRAY_SIZE(defaults); count++) offsets[count] = defaults[count];
    if (!(mod = GetModuleHandleW( L"twinapi.appcore.dll" )))
    {
        ERR( "twinapi.appcore.dll not loaded\n" );
        return;
    }
    if (!InterlockedExchange( &handler_added, 1 ))
        AddVectoredExceptionHandler( 1, twinapi_break_handler );
    memset( &ctx, 0, sizeof(ctx) );
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    for (i = 0; i < count; i++)
    {
        ULONG_PTR addr = (ULONG_PTR)mod + offsets[i];
        switch (i)
        {
        case 0: ctx.Dr0 = addr; break;
        case 1: ctx.Dr1 = addr; break;
        case 2: ctx.Dr2 = addr; break;
        case 3: ctx.Dr3 = addr; break;
        }
        ctx.Dr7 |= 1u << (2 * i);
        ERR( "twinapi break %u at %p (rva %#Ix)\n", i, (void *)addr, offsets[i] );
    }
    if (!SetThreadContext( GetCurrentThread(), &ctx ))
        ERR( "could not arm twinapi breakpoints, err %lu\n", GetLastError() );
}

/* The patched CoreMessaging.dll hands due-now message timers (XAML's tick timer) to the UI thread
 * through one static slot that every dispatcher's DrainSignaledWaits used to take: when the main
 * thread's dispatcher got there first, the timer's dispatch item was activated on the wrong
 * dispatcher and XAML's tick never ran (about one click in five). The dequeue now only runs on the
 * thread whose id wineuwp stores at CoreMessaging+0x10dee4, in HotPatchSpareGlobals, where the
 * patch also keeps a 16-entry ring of due-now timers; zero means "any thread", which covers startup
 * before the view thread exists. Both used to live at +0x10d000..+0x10d010, which is not free BSS:
 * it is wil's error counters and its cached s_pfnRtlDllShutdownInProgress. When CoreMessaging
 * cached that pointer after this wrote the thread id, no thread ever matched, no timer was ever
 * drained again, and XAML's window stayed blank - about one Solitaire run in ten. See
 * docs/Findings-M4.md, "the lost-tick race" and "the mailbox was wil's globals". */
/* WINE_UWP_CM_STATS=1: log the patched CoreMessaging timer ring's counters every 2 s. */
static DWORD WINAPI cm_stats_thread( void *arg )
{
    const BYTE *g = (const BYTE *)arg + 0x10dee0;
    int i;

    for (i = 0; i < 20; i++)
    {
        ERR( "cm ring: tid %lx head %lu tail %lu enq %lu deq %lu drop %lu\n", *(const DWORD *)(g + 4),
             *(const DWORD *)(g + 8), *(const DWORD *)(g + 12), *(const DWORD *)(g + 16),
             *(const DWORD *)(g + 20), *(const DWORD *)(g + 24) );
        Sleep( 2000 );
    }
    return 0;
}

static void publish_ui_thread_to_coremessaging( void )
{
    HMODULE cm = GetModuleHandleW( L"coremessaging.dll" );
    DWORD old;

    if (!cm) return;
    if (!VirtualProtect( (char *)cm + 0x10dee4, sizeof(DWORD), PAGE_READWRITE, &old )) return;
    *(DWORD *)((char *)cm + 0x10dee4) = GetCurrentThreadId();
    VirtualProtect( (char *)cm + 0x10dee4, sizeof(DWORD), old, &old );
    TRACE( "CoreMessaging timer drain bound to thread %lx\n", GetCurrentThreadId() );
    if (GetEnvironmentVariableW( L"WINE_UWP_CM_STATS", NULL, 0 ))
    {
        static LONG started;
        if (!InterlockedExchange( &started, 1 )) CloseHandle( CreateThread( NULL, 0, cm_stats_thread, cm, 0, NULL ) );
    }
}

/* The shell shows an activated app's CoreWindow on Windows: the app itself only calls
 * Window.Current.Activate(), which tells the window server the view is ready. Nothing plays that
 * role here, so show the view's window once activation has returned. */
static BOOL CALLBACK show_core_window( HWND hwnd, LPARAM param )
{
    WCHAR class_name[64];
    DWORD pid = 0;

    GetWindowThreadProcessId( hwnd, &pid );
    if (pid == GetCurrentProcessId() &&
        GetClassNameW( hwnd, class_name, ARRAY_SIZE(class_name) ) &&
        !wcscmp( class_name, L"Windows.UI.Core.CoreWindow" ))
    {
        LONG_PTR style = GetWindowLongPtrW( hwnd, GWL_STYLE );
        RECT rc = { 0, 0, 0, 0 };
        int w, h;

        launch_window_size( &w, &h );
        rc.right = w;
        rc.bottom = h;

        TRACE( "showing core window %p\n", hwnd );
        /* Windows draws no frame around a CoreWindow (the shell does). Here the X window manager
         * needs a normal frame to let the user move and resize it. */
        style = (style & ~(WS_POPUP | WS_CHILD)) | WS_OVERLAPPEDWINDOW;
        SetWindowLongPtrW( hwnd, GWL_STYLE, style );
        AdjustWindowRectEx( &rc, style, FALSE, GetWindowLongPtrW( hwnd, GWL_EXSTYLE ) );
        SetWindowPos( hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
        hosted_core_window = hwnd;
        {   /* the CoreWindow's text is the app model entry id ("App"); the frame deserves a name */
            WCHAR title[128];
            if (GetEnvironmentVariableW( L"WINE_UWP_WINDOW_TITLE", title, ARRAY_SIZE(title) ) && title[0])
                SetWindowTextW( hwnd, title );
        }
        ShowWindow( hwnd, SW_SHOW );
        SetForegroundWindow( hwnd );
        SetTimer( hwnd, WM_WINE_UWP_VISIBLE, 3000, NULL );   /* see core_window_subclass_proc */

        *(HWND *)param = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* The CoreWindow's own procedure (windows.ui's WindowServer::WndProc) does nothing for WM_PAINT;
 * XAML draws on its commits. After an expose the window stayed black until the next commit, so
 * repaint from dcomp's back buffer here. */
static WNDPROC core_window_proc_orig;

/* On Windows the shell tells the window server its window is visible with the private message
 * 0x270: WindowServer::WndProc passes lParam to OnVisibilityChangedEvent as flags, bit 0 =
 * visible, and CoreWindow.VisibilityChanged is raised only when that changes. Nothing sends it
 * here, so an app that renders only while visible - Solitaire turns continuous rendering on from
 * that event - sits on its first frame forever. (An earlier attempt put the flag in wParam.)
 *
 * The shell shows the window after the app has subscribed; here the window is shown before the
 * app's own startup has run, and a handler added after the one change is never told. So
 * add_VisibilityChanged is hooked, and every registration made after the window was shown gets a
 * hidden -> visible pulse, which the window server delivers to all subscribers. */
static BOOL core_window_visible;
static HRESULT (WINAPI *add_visibility_changed_orig)( void *, void *, INT64 * );

static HRESULT WINAPI add_visibility_changed_hook( void *iface, void *handler, INT64 *token )
{
    HRESULT hr = add_visibility_changed_orig( iface, handler, token );

    TRACE( "add_VisibilityChanged %p -> %#lx, visible %d\n", handler, hr, core_window_visible );
    if (SUCCEEDED( hr ) && core_window_visible && hosted_core_window)
        PostMessageW( hosted_core_window, WM_WINE_UWP_VISIBLE, 0, 0 );
    return hr;
}

/* Runs on the CoreWindow's thread, which is the only place GetForCurrentThread answers. */
static void hook_add_visibility_changed( void )
{
    static const WCHAR name_str[] = L"Windows.UI.Core.CoreWindow";
    struct statics_vtbl { HRESULT (WINAPI *slot[7])( void *, ... ); } **statics = NULL;
    void ***window = NULL, **slot;
    HSTRING_HEADER header;
    HSTRING name;
    HMODULE module;
    DWORD old;

    if (add_visibility_changed_orig) return;
    if (FAILED( WindowsCreateStringReference( name_str, ARRAY_SIZE(name_str) - 1, &header, &name ) )) return;
    if (FAILED( RoGetActivationFactory( name, &IID_ICoreWindowStatic, (void **)&statics ) ) || !statics) return;
    (*statics)->slot[6]( statics, &window );    /* GetForCurrentThread */
    IUnknown_Release( (IUnknown *)statics );
    if (!window) return;
    /* ICoreWindow slot 56 is add_VisibilityChanged; refuse anything that is not windows.ui code */
    slot = &(*window)[56];
    if (GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const WCHAR *)*slot, &module ) &&
        module == GetModuleHandleW( L"windows.ui.dll" ) &&
        VirtualProtect( slot, sizeof(*slot), PAGE_READWRITE, &old ))
    {
        add_visibility_changed_orig = *slot;
        *slot = add_visibility_changed_hook;
        VirtualProtect( slot, sizeof(*slot), old, &old );
        TRACE( "hooked add_VisibilityChanged at %p\n", add_visibility_changed_orig );
    }
    else WARN( "not hooking add_VisibilityChanged: slot %p does not point into windows.ui\n", *slot );
    IUnknown_Release( (IUnknown *)window );
}

static LRESULT CALLBACK core_window_subclass_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    hook_add_visibility_changed();
    if (msg == WM_TIMER && wparam == WM_WINE_UWP_VISIBLE)
    {
        /* Fallback: the window has been up for a while and the app's Window.Activate() has not
         * reached us (XAML defers it until its first frame, and the app may be waiting to be told
         * it is visible before it draws that frame). Report it visible anyway. */
        KillTimer( hwnd, WM_WINE_UWP_VISIBLE );
        if (!core_window_visible)
        {
            ERR( "no Window.Activate() %u ms after showing %p; reporting it visible\n", 3000, hwnd );
            PostMessageW( hwnd, WM_WINE_UWP_VISIBLE, 0, 0 );
        }
        return 0;
    }
    if (msg == WM_WINE_UWP_VISIBLE)
    {
        TRACE( "visibility %s\n", core_window_visible ? "pulse" : "on" );
        if (core_window_visible) CallWindowProcW( core_window_proc_orig, hwnd, 0x270, 0, 0 );
        core_window_visible = TRUE;
        return CallWindowProcW( core_window_proc_orig, hwnd, 0x270, 0, 1 );
    }
    if (msg == WM_PAINT)
    {
        typedef void (WINAPI *repaint_t)( HWND hwnd, const RECT *rect );
        HMODULE dcomp = GetModuleHandleW( L"dcomp.dll" );
        repaint_t repaint = dcomp ? (repaint_t)GetProcAddress( dcomp, "__wine_dcomp_repaint_window_rect" ) : NULL;
        RECT update;
        /* dcomp repaints only the region that needs it: an expose's, or the pixel a swap chain frame
         * invalidates to wake this thread */
        if (!GetUpdateRect( hwnd, &update, FALSE )) GetClientRect( hwnd, &update );
        ValidateRect( hwnd, NULL );
        TRACE( "WM_PAINT on the CoreWindow %p, repainting %s from dcomp (%p)\n", hwnd, wine_dbgstr_rect( &update ), repaint );
        if (repaint) repaint( hwnd, &update );
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return CallWindowProcW( core_window_proc_orig, hwnd, msg, wparam, lparam );
}

static void connect_dcomp_target( HWND hwnd )
{
    typedef void (WINAPI *set_target_window_t)( HWND hwnd );
    HMODULE dcomp;
    set_target_window_t set_target_window;

    if (!hwnd || !(dcomp = GetModuleHandleW( L"dcomp.dll" ))) return;
    if (!(set_target_window = (void *)GetProcAddress( dcomp, "__wine_dcomp_set_target_window" )))
    {
        WARN( "dcomp has no hosted-window target hook\n" );
        return;
    }
    set_target_window( hwnd );
    if (!core_window_proc_orig)
        core_window_proc_orig = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)core_window_subclass_proc );
}

/* The shell's own view startup sequence, run after Activate; see drive_view_startup below. */
static HRESULT drive_view_startup( IInspectable *args );

static void start_activation_inbox( struct registrar *reg );

/* The registrar drops its app object once the first activation is over, so later activations
 * keep their own reference, taken while it was still valid. */
static IActivatableApplication *inbox_app;

/* A later activation of the app that is already running - on Windows the shell hands it to the
 * existing instance, which raises Activated on its view rather than starting again. */
static HRESULT activate_existing_view( IInspectable *args );

/* A later activation of the app that is already running. On Windows the shell hands it to the
 * running instance, which raises Activated on the view it already has - Application.OnActivated
 * for a XAML app. Going through ApplicationActivationFactory::Activate again instead made twinapi
 * build a *second* view and CoreWindow for it, which nothing showed, and the app went black. So
 * activate the existing view directly; this runs on its thread (see activation_inbox_thread). */
static HRESULT do_activate_protocol( struct registrar *reg, const WCHAR *uri )
{
    IInspectable *args;
    HRESULT hr;

    if (!(args = create_protocol_args(uri))) return E_FAIL;
    hr = activate_existing_view( args );
    ERR("Protocol activation on %s returned %#lx\n", debugstr_w(uri), hr);
    IInspectable_Release(args);
    return hr;
}

static HRESULT do_activate(struct registrar *reg)
{
    IActivatableApplication *app;
    IInspectable *args;
    HSTRING aumid, contract, activation_id, extra;
    WCHAR aumid_buf[512];
    UINT32 len = ARRAY_SIZE(aumid_buf);
    HRESULT hr;

    if (!GetEnvironmentVariableW(L"WINE_UWP_AUMID", aumid_buf, len)) lstrcpyW(aumid_buf, L"App");
    hr = IUnknown_QueryInterface(reg->app, &IID_IActivatableApplication, (void **)&app);
    if (FAILED(hr))
    {
        ERR("app object does not expose IActivatableApplication, hr %#lx\n", hr);
        return hr;
    }
    WindowsCreateString(aumid_buf, lstrlenW(aumid_buf), &aumid);
    WindowsCreateString(L"Windows.Launch", 14, &contract);
    WindowsCreateString(L"", 0, &activation_id);
    WindowsCreateString(L"", 0, &extra);
    args = create_launch_args(L"");

    TRACE("activating %s with contract Windows.Launch\n", debugstr_w(aumid_buf));
    arm_twinapi_breaks();
    hr = app->lpVtbl->Activate(app, aumid, contract, activation_id, extra, args, 0, 0);
    if (FAILED(hr)) ERR("Activate failed, hr %#lx\n", hr);
    else
    {
        HWND core_window = NULL;

        TRACE("Activate returned %#lx\n", hr);
        EnumWindows( show_core_window, (LPARAM)&core_window );
        if (!core_window) WARN("no core window in this process to show\n");
        drive_view_startup( args );
        start_activation_inbox( reg );
    }

    IInspectable_Release(args);
    WindowsDeleteString(aumid); WindowsDeleteString(contract);
    WindowsDeleteString(activation_id); WindowsDeleteString(extra);
    app->lpVtbl->Release(app);
    return hr;
}

/* Activate has to run on the view thread: it creates the view's CoreWindow and ends up in
 * DXamlCore, which is per-thread. On Windows COM marshals the shell's call into the view's
 * apartment; here the call arrives in-process, so hand it to the view's CoreDispatcher. */
struct dispatched_handler
{
    const struct dispatched_handler_vtbl *vtbl;
    struct registrar *reg;
    LONG refcount;
    WCHAR *protocol_uri;          /* set for a later Protocol activation */
};

struct dispatched_handler_vtbl
{
    HRESULT (WINAPI *QueryInterface)( struct dispatched_handler *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( struct dispatched_handler * );
    ULONG (WINAPI *Release)( struct dispatched_handler * );
    HRESULT (WINAPI *Invoke)( struct dispatched_handler * );
};

static HRESULT WINAPI handler_QueryInterface( struct dispatched_handler *handler, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatchedHandler ))
    {
        InterlockedIncrement( &handler->refcount );
        *out = handler;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI handler_AddRef( struct dispatched_handler *handler )
{
    return InterlockedIncrement( &handler->refcount );
}
static ULONG WINAPI handler_Release( struct dispatched_handler *handler )
{
    ULONG ref = InterlockedDecrement( &handler->refcount );
    if (!ref)
    {
        free( handler->protocol_uri );
        free( handler );
    }
    return ref;
}
static HRESULT WINAPI handler_Invoke( struct dispatched_handler *handler )
{
    HRESULT hr;
    unsigned int tries;

    if (handler->protocol_uri)
    {
        do_activate_protocol( handler->reg, handler->protocol_uri );
        return S_OK;
    }

    /* ApplicationActivationFactory::Activate reads a field filled by RunInternal;
     * early calls return E_POINTER. Retry briefly on the view thread. */
    for (tries = 0; tries < 40; tries++)
    {
        hr = do_activate( handler->reg );
        if (hr != E_POINTER) break;
        Sleep( 50 );
    }
    return S_OK;
}
static const struct dispatched_handler_vtbl dispatched_handler_vtbl =
{
    handler_QueryInterface, handler_AddRef, handler_Release, handler_Invoke,
};

/* ICoreImmersiveApplication slot 8 (get_MainView), ICoreApplicationView2 slot 6 (get_Dispatcher),
 * ICoreDispatcher slot 8 (RunAsync). */
struct slot_vtbl { HRESULT (WINAPI *slot[16])( void *, ... ); };

static HRESULT get_core_application( struct slot_vtbl ***app )
{
    HSTRING_HEADER header;
    HSTRING name;
    HRESULT hr;

    if (FAILED( hr = WindowsCreateStringReference( L"Windows.ApplicationModel.Core.CoreApplication",
                                                   45, &header, &name ) ))
        return hr;
    return RoGetActivationFactory( name, &IID_ICoreImmersiveApplication, (void **)app );
}

/* CoreApplication only has a main view once Run has built one, and Activate reads state that is
 * filled along the way; activating before that faults inside ApplicationActivationFactory. */
static BOOL wait_for_main_view( void )
{
    struct slot_vtbl **app, **view;
    unsigned int tries;

    /* Poll hard rather than politely: the window between the main view existing and the app
     * tearing its views down again is a couple of milliseconds, and an activation that arrives
     * after UninitializeView has started deadlocks against the view thread it has to call into. */
    for (tries = 0; tries < 2000; tries++)
    {
        if (SUCCEEDED( get_core_application( &app ) ))
        {
            view = NULL;
            if (SUCCEEDED( (*app)->slot[8]( app, &view ) ) && view)
            {
                IUnknown_Release( (IUnknown *)view );
                IUnknown_Release( (IUnknown *)app );
                TRACE( "main view is up after %u tries\n", tries );
                return TRUE;
            }
            IUnknown_Release( (IUnknown *)app );
        }
        Sleep( 1 );
    }
    ERR( "timed out waiting for CoreApplication's main view\n" );
    return FALSE;
}

static HRESULT get_main_view_dispatcher( void **dispatcher )
{
    struct slot_vtbl **app, **view = NULL;
    HRESULT hr;

    *dispatcher = NULL;
    if (FAILED( hr = get_core_application( &app ) ))
    {
        ERR( "no ICoreImmersiveApplication, hr %#lx\n", hr );
        return hr;
    }
    hr = (*app)->slot[8]( app, &view );
    IUnknown_Release( (IUnknown *)app );
    if (FAILED( hr ) || !view)
    {
        ERR( "no main view, hr %#lx view %p\n", hr, view );
        return FAILED(hr) ? hr : E_FAIL;
    }

    {
        struct slot_vtbl **view2 = NULL;
        if (SUCCEEDED( hr = IUnknown_QueryInterface( (IUnknown *)view, &IID_ICoreApplicationView2,
                                                     (void **)&view2 ) ))
        {
            hr = (*view2)->slot[6]( view2, dispatcher );
            if (FAILED( hr ) || !*dispatcher) ERR( "no dispatcher, hr %#lx %p\n", hr, *dispatcher );
            IUnknown_Release( (IUnknown *)view2 );
        }
        else ERR( "no ICoreApplicationView2, hr %#lx\n", hr );
    }
    IUnknown_Release( (IUnknown *)view );
    return hr;
}

/* WINE_UWP_ACTIVATE_INBOX (default C:\\users\\Public\\wineuwp-activate.uri): a URI written to that
 * file is delivered to the running app as a Protocol activation, on its main view's thread, and
 * the file is deleted. This is what the shell's single-instance redirection does on Windows; a
 * host-side handler for the app's scheme only has to write the file (see activate-uri.sh). */
static HRESULT do_activate_protocol( struct registrar *reg, const WCHAR *uri );

static void CALLBACK protocol_activation_apc( ULONG_PTR param )
{
    WCHAR *uri = (WCHAR *)param;

    do_activate_protocol( NULL, uri );
    free( uri );
}

static DWORD WINAPI activation_inbox_thread( void *param )
{
    struct registrar *reg = param;
    WCHAR path[MAX_PATH];

    if (!GetEnvironmentVariableW( L"WINE_UWP_ACTIVATE_INBOX", path, ARRAY_SIZE(path) ))
        lstrcpyW( path, L"C:\\users\\Public\\wineuwp-activate.uri" );
    SetThreadDescription( GetCurrentThread(), L"wineuwp_activation_inbox" );
    DeleteFileW( path );   /* a leftover from an earlier run is not for this one */
    ERR( "watching %s for protocol activations\n", debugstr_w( path ) );
    for (;;)
    {
        char buffer[8192];
        DWORD size = 0;
        HANDLE file;
        WCHAR *uri;
        int len;

        Sleep( 500 );
        file = CreateFileW( path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL );
        if (file == INVALID_HANDLE_VALUE) continue;
        ReadFile( file, buffer, sizeof(buffer) - 1, &size, NULL );
        CloseHandle( file );
        DeleteFileW( path );
        while (size && (buffer[size - 1] == '\n' || buffer[size - 1] == '\r' || buffer[size - 1] == ' ')) size--;
        buffer[size] = 0;
        if (!size) continue;
        len = MultiByteToWideChar( CP_UTF8, 0, buffer, -1, NULL, 0 );
        if (!(uri = malloc( len * sizeof(WCHAR) ))) continue;
        MultiByteToWideChar( CP_UTF8, 0, buffer, -1, uri, len );
        ERR( "inbox: activating on %s\n", debugstr_w( uri ) );
        {
            /* The view to activate is the one whose CoreWindow is on screen. An app can have more
             * than one CoreApplicationView (Calculator and Shapr3D both do), and the main view's
             * dispatcher ran this on a thread with no window. That thread waits alertably, which is
             * how the first activation reaches it too (see activate_apc). */
            DWORD tid = hosted_core_window ? GetWindowThreadProcessId( hosted_core_window, NULL ) : 0;
            HANDLE thread = tid ? OpenThread( THREAD_SET_CONTEXT, FALSE, tid ) : NULL;

            if (!thread || !QueueUserAPC( protocol_activation_apc, thread, (ULONG_PTR)uri ))
            {
                ERR( "no view thread to deliver %s to (window %p, tid %lu)\n", debugstr_w( uri ),
                     hosted_core_window, tid );
                free( uri );
            }
            if (thread) CloseHandle( thread );
        }
    }
    return 0;
}

static void start_activation_inbox( struct registrar *reg )
{
    static LONG started;
    HANDLE thread;

    if (InterlockedExchange( &started, 1 )) return;
    if (!reg->app || FAILED( IUnknown_QueryInterface( reg->app, &IID_IActivatableApplication, (void **)&inbox_app ) ))
    {
        ERR( "no IActivatableApplication to deliver later activations to\n" );
        return;
    }
    if ((thread = CreateThread( NULL, 0, activation_inbox_thread, reg, 0, NULL ))) CloseHandle( thread );
}

/* ---------------------------------------------------------------------------------------------
 * The rest of the shell's view startup: Load then Activate on the view itself
 *
 * ApplicationActivationFactory::Activate gets the app as far as creating the view and its
 * CoreWindow, but twinapi's own path from there on (CoreApplicationViewAgileContainer::
 * ActivateInternal -> PrepareToActivateInternal) waits on the shell's CoreUI server and never
 * reaches the three calls that actually start a XAML app. Without them XAML's FrameworkView is
 * initialised and then torn down again: its INavigationClient gets OnCloseWindow/OnDestroyWindow
 * with no OnActivateWindow in between, and Application::OnLaunched never runs.
 *
 * The three calls live on CoreApplicationView's private ICoreApplicationViewInternal, whose
 * vtable is at twinapi.appcore RVA 0x1a4ce8 in the pinned Windows 11 build:
 *
 *   slot 7  SetWindowAndGetDispatcher( ICoreWindowFactory *, ICoreWindow **, ICoreDispatcher ** )
 *   slot 8  Load( HSTRING entryPoint )
 *   slot 9  Activate( IActivationEventCallback *, IActivatedEventArgs * )
 *
 * The interface subobject sits at +0x28 in the class (CoreApplicationView::Activate starts with
 * lea r15,[rsi-0x28]), but rather than trust that offset we look for the vtable pointer itself.
 */
#define TWINAPI_VIEW_VTBL          0x1a4b60   /* ??_7CoreApplicationView...6BICoreApplicationView@ */
#define TWINAPI_VIEW_INTERNAL_VTBL 0x1a4ce8   /* ...6B...ICoreApplicationViewInternal... */
/* ICoreApplicationViewInternal3: IInspectable plus CreateCoreWindow( ICoreWindowFactory * ).
 * This is what fills the view's CoreWindow slot. SetWindowAndGetDispatcher only creates the window
 * itself when IsApplicationActivationWatcherEnabled() is false, and on the desktop device family
 * it is true - so with the family we report, the window has to be made through here first or
 * SetWindowAndGetDispatcher reads the empty slot and faults. */
#define TWINAPI_VIEW_INTERNAL3_VTBL 0x1a4e68
struct view_internal3_vtbl
{
    void *inspectable[6];
    HRESULT (WINAPI *CreateCoreWindow)( void *iface, void *factory );
};
struct view_internal3 { const struct view_internal3_vtbl *vtbl; };

#define TWINAPI_VIEW_ACTIVATION_VTBL 0x1a4cb0 /* ...6B...ICoreApplicationView_Activation... */
#define VIEW_INTERNAL_SEARCH       0x100

/* ICoreApplicationView_Activation: slot 6 is PrepareToActivateAsync, which is how the shell tells
 * a view that an activation is on its way before sending it. */
struct view_activation_vtbl
{
    void *inspectable[6];
    HRESULT (WINAPI *PrepareToActivateAsync)( void *iface, UINT64 options, void **action );
};
struct view_activation { const struct view_activation_vtbl *vtbl; };

struct view_internal_vtbl
{
    void *inspectable[6];
    HRESULT (WINAPI *get_PrivateProperties)( void *iface, void **properties );
    HRESULT (WINAPI *SetWindowAndGetDispatcher)( void *iface, void *factory, void **window,
                                                 void **dispatcher );
    HRESULT (WINAPI *Load)( void *iface, HSTRING entry_point );
    /* Activate is declared with two parameters, but the call lands in a dispatcher that reads a
     * fourth register argument, so pass an explicit zero rather than leaving r9 whatever the
     * caller's frame had in it. */
    HRESULT (WINAPI *Activate)( void *iface, void *callback, IInspectable *args, void *reserved );
};
struct view_internal { const struct view_internal_vtbl *vtbl; };

/* The private interface is cloaked, so QueryInterface cannot reach it without its IID. Find the
 * subobject by its vtable pointer instead: it is a fixed address in the loaded twinapi image. */
/* Find one of the view's interface subobjects by the vtable it points at, which is a fixed
 * address in the loaded twinapi image. QueryInterface cannot reach the cloaked ones. */
static void *find_view_subobject( void *view, ULONG_PTR vtbl_rva, const char *name )
{
    const void *want, *primary;
    HMODULE twinapi;
    unsigned int i;

    if (!view) return NULL;
    if (!(twinapi = GetModuleHandleW( L"twinapi.appcore.dll" )))
    {
        ERR( "twinapi.appcore.dll not loaded\n" );
        return NULL;
    }
    want = (const char *)twinapi + vtbl_rva;
    primary = (const char *)twinapi + TWINAPI_VIEW_VTBL;
    /* Off the view's own thread, CoreApplication hands back a marshalled proxy, whose vtable is
     * built on the heap rather than being in twinapi's image - and none of the subobjects this
     * scans for are in it. A vtable that is merely a *different* twinapi one is fine and must
     * keep scanning: that is the ordinary case for the calculator. */
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)twinapi;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)twinapi + dos->e_lfanew);
        ULONG_PTR off = (ULONG_PTR)*(const void **)view - (ULONG_PTR)twinapi;

        if (off >= nt->OptionalHeader.SizeOfImage)
        {
            TRACE( "view %p vtable %p is outside twinapi: a proxy, not scanning\n",
                   view, *(const void **)view );
            return NULL;
        }
    }
    if (*(const void **)view != primary)
        WARN( "view %p vtable %p is not CoreApplicationView's %p; continuing anyway\n",
              view, *(const void **)view, primary );
    for (i = 0; i < VIEW_INTERNAL_SEARCH; i += sizeof(void *))
    {
        void **candidate = (void **)((char *)view + i);
        if (*candidate == want)
        {
            TRACE( "%s at %p (+%#x from view %p)\n", name, candidate, i, view );
            return candidate;
        }
    }
    ERR( "no %s vtable %p within %#x of view %p\n", name, want, VIEW_INTERNAL_SEARCH, view );
    return NULL;
}

static struct view_internal *find_view_internal( void *view )
{
    return find_view_subobject( view, TWINAPI_VIEW_INTERNAL_VTBL, "ICoreApplicationViewInternal" );
}

/* The shell passes CoreApplicationView::Activate a callback it uses to report that the app
 * handled the activation; passing NULL faults, because Activate QIs the pointer without a null
 * check (twinapi.appcore RVA 0x1b87f).  Only ActivationHandled is ever called on it. */
struct activation_callback
{
    const void **vtbl;
    IUnknown *ftm;
    LONG refcount;
};

static HRESULT WINAPI activation_cb_QueryInterface( struct activation_callback *cb, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivationEventCallback ))
        *out = &cb->vtbl;
    else if (IsEqualGUID( iid, &IID_IMarshal ) || IsEqualGUID( iid, &IID_IAgileObject ))
        return IUnknown_QueryInterface( cb->ftm, iid, out );
    else
    {
        WARN( "activation callback has no interface %s\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &cb->refcount );
    return S_OK;
}
static ULONG WINAPI activation_cb_AddRef( struct activation_callback *cb )
{
    return InterlockedIncrement( &cb->refcount );
}
static ULONG WINAPI activation_cb_Release( struct activation_callback *cb )
{
    ULONG ref = InterlockedDecrement( &cb->refcount );
    if (!ref)
    {
        if (cb->ftm) IUnknown_Release( cb->ftm );
        free( cb );
    }
    return ref;
}
static HRESULT WINAPI activation_cb_GetIids( struct activation_callback *cb, ULONG *count, IID **iids )
{
    *count = 0; *iids = NULL; return S_OK;
}
static HRESULT WINAPI activation_cb_GetRuntimeClassName( struct activation_callback *cb, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.Core.ActivationEventCallback", 53, name );
}
static HRESULT WINAPI activation_cb_GetTrustLevel( struct activation_callback *cb, TrustLevel *level )
{
    *level = BaseTrust; return S_OK;
}
static HRESULT WINAPI activation_cb_ActivationHandled( struct activation_callback *cb )
{
    ERR( "the app handled the activation\n" );
    maybe_start_stack_dumper();
    return S_OK;
}

static const void *activation_cb_vtbl[] =
{
    activation_cb_QueryInterface, activation_cb_AddRef, activation_cb_Release,
    activation_cb_GetIids, activation_cb_GetRuntimeClassName, activation_cb_GetTrustLevel,
    activation_cb_ActivationHandled,
};

static HRESULT create_activation_callback( void **out )
{
    struct activation_callback *cb;

    if (!(cb = calloc( 1, sizeof(*cb) ))) return E_OUTOFMEMORY;
    cb->vtbl = activation_cb_vtbl;
    cb->refcount = 1;
    if (FAILED( CoCreateFreeThreadedMarshaler( (IUnknown *)&cb->vtbl, &cb->ftm ) ))
    {
        free( cb );
        return E_OUTOFMEMORY;
    }
    *out = &cb->vtbl;
    return S_OK;
}

/* CoreApplication.GetCurrentView() is the view for the calling thread, which is the one this
 * runs on; ICoreImmersiveApplication::get_MainView answers E_ILLEGAL_METHOD_CALL until the shell
 * has established a main view, so it is only a fallback. ICoreApplication slot 12 is
 * GetCurrentView (IInspectable, get_Id, four Suspending/Resuming slots, get_Properties). */
static HRESULT get_current_core_application_view( struct slot_vtbl ***view )
{
    struct slot_vtbl **statics, **app;
    HSTRING_HEADER header;
    HSTRING name;
    HRESULT hr;

    *view = NULL;
    if (SUCCEEDED( hr = WindowsCreateStringReference( L"Windows.ApplicationModel.Core.CoreApplication",
                                                      45, &header, &name ) ) &&
        SUCCEEDED( hr = RoGetActivationFactory( name, &IID_ICoreApplication, (void **)&statics ) ))
    {
        hr = (*statics)->slot[12]( statics, view );
        IUnknown_Release( (IUnknown *)statics );
        if (SUCCEEDED( hr ) && *view) return S_OK;
        WARN( "GetCurrentView on tid %lu, hr %#lx view %p\n", GetCurrentThreadId(), hr, *view );
    }
    else ERR( "no ICoreApplication, hr %#lx\n", hr );

    if (FAILED( hr = get_core_application( &app ) )) return hr;
    hr = (*app)->slot[8]( app, view );   /* get_MainView */
    IUnknown_Release( (IUnknown *)app );
    if (FAILED( hr ) || !*view)
    {
        ERR( "no view to start, hr %#lx view %p\n", hr, *view );
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}

static LONG view_startup_done;
static HRESULT WINAPI wffactory_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID iid, void **out );

/* WINE_UWP_DRIVE_FIRST_VIEW: let any view claim the startup, and give it a CoreWindow of its own
 * first. The app's App object is constructed on the first view, so the activation has to land
 * there too; this is the path that deadlocks, instrumented step by step to find out where. */
static unsigned queue_drive_view_apcs( void );
static void prepare_view_for_activation( void );
static void try_configure_from_get_for_current_thread( void );

static BOOL drive_first_view( void )
{
    return GetEnvironmentVariableW( L"WINE_UWP_DRIVE_FIRST_VIEW", NULL, 0 ) != 0;
}

static HRESULT drive_view_startup( IInspectable *args )
{
    struct slot_vtbl **view = NULL;
    struct view_internal *internal;
    IInspectable *own_args = NULL;
    WCHAR entry[256], mode[16], *app_id;
    HSTRING entry_point;
    HRESULT hr;

    /* WINE_UWP_DRIVE_VIEW=load stops after Load; any other value also runs Activate, which
     * currently faults inside windows.ui's CDispatcher (see Findings-M4.md). */
    if (!GetEnvironmentVariableW( L"WINE_UWP_DRIVE_VIEW", mode, ARRAY_SIZE(mode) )) return S_FALSE;
    if (view_startup_done) return S_FALSE;
    if (!args && !(args = own_args = create_launch_args( L"" ))) return E_OUTOFMEMORY;
    if (FAILED( hr = get_current_core_application_view( &view ) ))
    {
        if (own_args) IInspectable_Release( own_args );
        return hr;
    }
    if (!(internal = find_view_internal( view )))
    {
        /* Not the view thread - this is a proxy. Leave the one-shot unclaimed so the call from
         * the view thread (after ConfigureCoreWindow, where the DXamlCore and CoreWindow are)
         * still gets to run: claiming it here is what stopped the startup sequence from ever
         * reaching the real view, and left the app split across two of them. */
        TRACE( "no view internal on tid %lu; leaving the startup to the view thread\n",
               GetCurrentThreadId() );
        IUnknown_Release( (IUnknown *)view );
        if (own_args) IInspectable_Release( own_args );
        return E_NOINTERFACE;
    }
    if (InterlockedCompareExchange( &view_startup_done, 1, 0 ))
    {
        IUnknown_Release( (IUnknown *)view );
        if (own_args) IInspectable_Release( own_args );
        return S_FALSE;
    }

    if (drive_first_view())
    {
        void *factory = NULL, *win = NULL, *disp = NULL;
        HRESULT hr2;

        /* Tell the view an activation is coming first, the way the shell does. Nothing on this
         * path had done it, and it is the documented precondition for the calls that follow. */
        ERR( "STEP tid %lu: PrepareToActivateAsync\n", GetCurrentThreadId() );
        prepare_view_for_activation();
        ERR( "STEP tid %lu: creating the window factory\n", GetCurrentThreadId() );
        hr2 = wffactory_CreateInstance( NULL, NULL, &IID_ICoreWindowFactory, &factory );
        ERR( "STEP tid %lu: factory %p hr %#lx\n", GetCurrentThreadId(), factory, hr2 );
        if (SUCCEEDED( hr2 ))
        {
            {
                struct view_internal3 *i3 = find_view_subobject( view, TWINAPI_VIEW_INTERNAL3_VTBL,
                                                                 "ICoreApplicationViewInternal3" );
                if (i3)
                {
                    hr2 = i3->vtbl->CreateCoreWindow( i3, factory );
                    ERR( "STEP tid %lu: CreateCoreWindow hr %#lx\n", GetCurrentThreadId(), hr2 );
                }
                else
                {
                    hr2 = E_NOINTERFACE;
                    ERR( "STEP tid %lu: no ICoreApplicationViewInternal3\n", GetCurrentThreadId() );
                }
            }
            /* CreateCoreWindow makes the window and gives it to the *view*, but what tells XAML
             * about it is FrameworkView::SetWindow, which only runs as part of
             * SetWindowAndGetDispatcher - and that does not return when called from here. Without
             * it DirectUI::Window keeps a null CoreWindow (+0x148) and a null HWND (+0x1e8), and
             * Window::get_BoundsImpl answers E_FAIL, which is what stopped Media Player's
             * ViewportStateTrigger. Configure it directly instead; this is the one place that has
             * to do it, so the double-configure that fail-fasts XAML cannot happen. */
            if (SUCCEEDED( hr2 ))
            {
                ERR( "STEP tid %lu: configuring the core window for XAML\n", GetCurrentThreadId() );
                try_configure_from_get_for_current_thread();
            }

            /* SetWindowAndGetDispatcher is only needed for the dispatcher it hands back, and it
             * does not return when called from here. */
            if (FAILED( hr2 ))
            {
                ERR( "STEP tid %lu: SetWindowAndGetDispatcher on view %p\n", GetCurrentThreadId(), view );
                hr2 = internal->vtbl->SetWindowAndGetDispatcher( internal, factory, &win, &disp );
                ERR( "STEP tid %lu: SetWindowAndGetDispatcher hr %#lx window %p dispatcher %p\n",
                     GetCurrentThreadId(), hr2, win, disp );
            }
            if (win) IUnknown_Release( (IUnknown *)win );
            if (disp) IUnknown_Release( (IUnknown *)disp );
            IUnknown_Release( (IUnknown *)factory );
        }
    }

    /* IFrameworkView::Load takes the activated entry point, which is the part of the AUMID
     * after the '!'; XAML only records it, but it must not be empty. */
    if (!GetEnvironmentVariableW( L"WINE_UWP_AUMID", entry, ARRAY_SIZE(entry) )) entry[0] = 0;
    app_id = wcsrchr( entry, '!' );
    app_id = app_id ? app_id + 1 : entry;
    if (!*app_id) app_id = (WCHAR *)L"App";
    if (SUCCEEDED( hr = WindowsCreateString( app_id, lstrlenW( app_id ), &entry_point ) ))
    {
        hr = internal->vtbl->Load( internal, entry_point );
        ERR( "Load(%s) on view %p, hr %#lx\n", debugstr_w(app_id), view, hr );
        WindowsDeleteString( entry_point );
    }
    if (SUCCEEDED( hr ) && wcsicmp( mode, L"load" ))
    {
        void *callback = NULL;

        if (SUCCEEDED( hr = create_activation_callback( &callback ) ))
        {
            hr = internal->vtbl->Activate( internal, callback, args, NULL );
            ERR( "view Activate(callback %p, args %p), hr %#lx\n", callback, args, hr );
            IUnknown_Release( (IUnknown *)callback );
        }
    }
    IUnknown_Release( (IUnknown *)view );
    if (own_args) IInspectable_Release( own_args );
    return hr;
}

static HRESULT activate_existing_view( IInspectable *args )
{
    struct slot_vtbl **view = NULL;
    struct view_internal *internal;
    void *callback = NULL;
    HRESULT hr;

    if (FAILED( hr = get_current_core_application_view( &view ) )) return hr;
    if (!(internal = find_view_internal( view )))
    {
        ERR( "no view internal on tid %lu\n", GetCurrentThreadId() );
        IUnknown_Release( (IUnknown *)view );
        return E_NOINTERFACE;
    }
    if (SUCCEEDED( hr = create_activation_callback( &callback ) ))
    {
        hr = internal->vtbl->Activate( internal, callback, args, NULL );
        IUnknown_Release( (IUnknown *)callback );
    }
    IUnknown_Release( (IUnknown *)view );
    return hr;
}

/* CoreDispatcher::RunAsync posts work that our HostModeRun/Drain path never
 * drains. Queue an APC onto every other thread in the process instead: the view
 * thread's CoMsgWait is alertable, so Activate runs there with FrameworkApplication
 * TLS set. Avoid EnumWindows here — it deadlocks against a busy UI thread. */
static LONG activate_apc_done;

/* Which thread activates matters: CoreApplicationViewAgileContainer holds the
 * CoreApplicationView through an agile reference, so ActivateInternal's
 * ICoreApplicationViewInternal3::CreateCoreWindow becomes a marshalled call whenever the
 * container and the view are in different apartments, and nothing in the prefix services it
 * (see Findings-M4.md). Selecting the apartment from here did not help - the proxy is the
 * container's, not ours - so the activation still goes to whichever thread takes the APC first,
 * and the apartment it ran in is logged for the next attempt. */

/* The view thread is the one twinapi built a DXamlCore on: CoreApplicationViewAgileContainer
 * creates the view and calls FrameworkView::Initialize there long before any activation. It is
 * also the only thread that can run the activation without deadlocking, because
 * ActivateInternal's calls into the view (CreateCoreWindow, Load, Activate) are marshalled from
 * anywhere else, and nothing pumps that apartment until FrameworkView::Run - which only starts
 * after the activation. On Windows the shell's activation arrives on the view thread, so these
 * are direct calls. */
#define XAML_DXAMLCORE_GETCURRENT_RVA 0x1134d4

static void *xaml_dxaml_core_current( void )
{
    void * (WINAPI *get_current)( void );
    HMODULE xaml;

    xaml = GetModuleHandleW( L"Windows.UI.Xaml.dll" );
    if (!xaml) xaml = GetModuleHandleW( L"windows.ui.xaml.dll" );
    if (!xaml) return NULL;
    get_current = (void *)((char *)xaml + XAML_DXAMLCORE_GETCURRENT_RVA);
    return get_current();
}

static LONG activate_any_thread;

static void broadcast_load_app_xaml( void );
static LONG app_xaml_loaded;

/* Tell the view an activation is coming, the way the shell does, before sending it. Without this
 * the container can decide the view is idle and tear it down while the activation is in flight,
 * and Activate comes back E_APPLICATION_VIEW_EXITING. */
static void prepare_view_for_activation( void )
{
    struct slot_vtbl **view = NULL;
    struct view_activation *activation;
    void *action = NULL;
    HRESULT hr;

    if (FAILED( get_current_core_application_view( &view ) ) || !view) return;
    if ((activation = find_view_subobject( view, TWINAPI_VIEW_ACTIVATION_VTBL,
                                           "ICoreApplicationView_Activation" )))
    {
        hr = activation->vtbl->PrepareToActivateAsync( activation, 0, &action );
        ERR( "PrepareToActivateAsync on tid %lu, hr %#lx action %p\n",
             GetCurrentThreadId(), hr, action );
        if (action) IUnknown_Release( (IUnknown *)action );
    }
    IUnknown_Release( (IUnknown *)view );
}

/* Runs the activation exactly once, wherever it is called from. Returns FALSE when another
 * thread already took it. */
static BOOL try_activate_once( struct registrar *reg )
{
    unsigned int tries;
    HRESULT hr = E_FAIL;

    if (activate_apc_done) return FALSE;
    if (!activate_any_thread && !xaml_dxaml_core_current())
    {
        TRACE( "thread %lu has no DXamlCore; leaving the activation to the view thread\n",
               GetCurrentThreadId() );
        return FALSE;
    }
    if (InterlockedCompareExchange( &activate_apc_done, 1, 0 ) != 0) return FALSE;
    {
        APTTYPE type = 0;
        APTTYPEQUALIFIER qualifier = 0;
        CoGetApartmentType( &type, &qualifier );
        ERR( "running Activate on thread %lu (apartment type %d qualifier %d, registering thread %lu)\n",
             GetCurrentThreadId(), type, qualifier, reg->owner_tid );
    }
    if (GetEnvironmentVariableW( L"WINE_UWP_PREPARE_ACTIVATE", NULL, 0 )) prepare_view_for_activation();

    /* E_POINTER means ApplicationActivationFactory's state is not filled in yet. Retrying
     * E_APPLICATION_VIEW_EXITING was tried and is worse: CoreApplication does not build a new
     * view for a second activation, and an attempt that blocks before reaching the window
     * factory never gets a view pump, so the whole run hangs. */
    for (tries = 0; tries < 4000; tries++)
    {
        hr = do_activate( reg );
        if (hr != E_POINTER) break;
        Sleep( 1 );
    }
    if (FAILED( hr )) ERR( "Activate failed, hr %#lx\n", hr );
    else ERR( "Activate returned %#lx\n", hr );
    return TRUE;
}

static void CALLBACK activate_apc( ULONG_PTR param )
{
    struct registrar *reg = (struct registrar *)param;
    APTTYPE type = 0;
    APTTYPEQUALIFIER qualifier = 0;

    /* The APC is commonly delivered on MAINSTA just before FrameworkView::Initialize has
     * installed its DXamlCore.  Dropping it here leaves only the much-later MTA fallback,
     * which loses the view to UninitializeView.  Keep one APC pending on MAINSTA until the
     * per-thread core exists; then try_activate_once performs the existing E_POINTER retry. */
    if (!activate_apc_done && !xaml_dxaml_core_current() &&
        SUCCEEDED( CoGetApartmentType( &type, &qualifier ) ) && type == APTTYPE_MAINSTA)
    {
        Sleep( 1 );
        InterlockedIncrement( &reg->refcount );
        if (!QueueUserAPC( activate_apc, GetCurrentThread(), param )) registrar_Release( reg );
        registrar_Release( reg );
        return;
    }
    try_activate_once( reg );
    registrar_Release( reg );
}

static BOOL queue_activate_apc_to( struct registrar *reg, DWORD tid )
{
    HANDLE thr = OpenThread( THREAD_SET_CONTEXT, FALSE, tid );

    if (!thr) return FALSE;
    InterlockedIncrement( &reg->refcount );
    if (!QueueUserAPC( activate_apc, thr, (ULONG_PTR)reg ))
    {
        registrar_Release( reg );
        CloseHandle( thr );
        return FALSE;
    }
    CloseHandle( thr );
    return TRUE;
}

/* CoreApplicationViewAgileContainer::ActivateInternal reaches the CoreApplicationView through an
 * agile reference, so activating from any other apartment turns ICoreApplicationViewInternal3
 * CreateCoreWindow into a marshalled call that the owning thread - parked in
 * CoreApplication::WaitForActivate - never services, and the whole activation hangs there. Run it
 * on the owning thread when we know which one that is. */
static BOOL queue_activate_apc_to_owner( struct registrar *reg )
{
    unsigned int tries;

    if (!GetEnvironmentVariableW( L"WINE_UWP_ACTIVATE_ON_OWNER", NULL, 0 )) return FALSE;
    if (!reg->owner_tid || reg->owner_tid == GetCurrentThreadId()) return FALSE;
    if (!queue_activate_apc_to( reg, reg->owner_tid )) return FALSE;
    ERR( "queued Activate APC onto the registering thread %lu\n", reg->owner_tid );
    for (tries = 0; tries < 20 && !activate_apc_done; tries++) Sleep( 50 );
    if (activate_apc_done) return TRUE;
    ERR( "thread %lu did not run the Activate APC; falling back to every thread\n", reg->owner_tid );
    return FALSE;
}

/* Wine's internal worker threads sit in *alertable* waits, so an APC queued onto one runs inside
 * that wait. That is fatal for the RPC server thread: drive_view_startup_apc reaches
 * CoUnmarshalInterface on the global interface table, which makes an outgoing RPC call - and the
 * thread that has to service that call is the very thread now blocked inside it. Everything that
 * later needs RPC (XAML's RoGetAgileReference from ConfigureCoreWindow) then blocks forever.
 * Wine names these threads, so open app threads through here and let the service threads be. */
static HANDLE open_app_thread( DWORD tid, DWORD access )
{
    HANDLE thr = OpenThread( access | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid );
    WCHAR *desc = NULL;
    BOOL service = FALSE;

    if (!thr) return NULL;
    if (SUCCEEDED( GetThreadDescription( thr, &desc ) ) && desc)
    {
        service = !wcsnicmp( desc, L"wine", 4 );
        if (service) TRACE( "not queueing onto Wine service thread %s\n", debugstr_w( desc ) );
        LocalFree( desc );
    }
    if (service)
    {
        CloseHandle( thr );
        return NULL;
    }
    return thr;
}

static unsigned queue_activate_apcs( struct registrar *reg )
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap, thr;
    THREADENTRY32 te;
    unsigned queued = 0;

    if (queue_activate_apc_to_owner( reg )) return 1;

    snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if (snap == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te ))
    {
        do
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            thr = open_app_thread( te.th32ThreadID, THREAD_SET_CONTEXT );
            if (!thr) continue;
            InterlockedIncrement( &reg->refcount );
            if (QueueUserAPC( activate_apc, thr, (ULONG_PTR)reg ))
            {
                queued++;
                TRACE( "queued Activate APC onto tid %lu\n", te.th32ThreadID );
            }
            else
            {
                registrar_Release( reg );
            }
            CloseHandle( thr );
        } while (Thread32Next( snap, &te ));
    }
    CloseHandle( snap );
    return queued;
}

static void broadcast_configure_from_get_for_current_thread( void );

static DWORD WINAPI activation_thread( void *arg )
{
    struct registrar *reg = arg;
    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    struct dispatched_handler *handler;
    struct slot_vtbl **dispatcher;
    unsigned int tries;
    DWORD start;

    /* CoreWindow reads mouse input as WM_POINTER* messages (GetPointerInfo). On Windows, mouse-in-pointer is on
     * for UWP processes; win32u only converts mouse messages after EnableMouseInPointer(TRUE), and nothing in the
     * hosted app calls it, so clicks never reached XAML. */
    if (!EnableMouseInPointer( TRUE ))
        WARN( "EnableMouseInPointer failed, error %lu\n", GetLastError() );

    /* Native msftedit's DllMain runs a WIL feature check (UseRichEditCore) on DLL_THREAD_ATTACH.
     * Its once-initialisation on the UI thread reaches the loader (feature-staging API set
     * lookup) while a thread XAML has just created (CompositorScheduler's render thread, after
     * the graphing calculator rebuilt its D3D device) holds the loader lock inside that same
     * check: the two wait for each other and input stops for a minute at a time. The handler
     * only manages a lazily created TLS block, so skip it: load msftedit first and turn its
     * thread callouts off; XAML's later LoadLibrary gets the same module. */
    {
        HMODULE msftedit = LoadLibraryW( L"msftedit.dll" );
        /* DisableThreadLibraryCalls refuses modules with static TLS (msftedit has a .tls
         * directory), on Windows as in Wine. The loader allocates the TLS slots for new threads
         * on its own, so flag the loader entry directly. */
        if (msftedit && !DisableThreadLibraryCalls( msftedit ))
        {
            LIST_ENTRY *mark = &NtCurrentTeb()->Peb->LdrData->InLoadOrderModuleList, *entry;
            for (entry = mark->Flink; entry != mark; entry = entry->Flink)
            {
                LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks );
                if (mod->DllBase == msftedit)
                {
                    mod->Flags |= LDR_NO_DLL_CALLS;
                    ERR( "msftedit thread callouts disabled on its loader entry.\n" );
                    break;
                }
            }
        }
    }

    /* Activating on the view thread is what Windows does. Prefer that when
     * WINE_UWP_ACTIVATE_ON_VIEW is set; otherwise Activate immediately on this
     * thread as soon as the main view exists. Waiting for a dispatcher used to
     * lose the race with the UI thread crashing before SetWindow/SetCoreWindow. */
    dispatcher = NULL;

    /* Offer the startup to the view that already exists, before the activation factory is called
     * and the container starts standing up a second one. Timing is the whole point: driven from
     * wf_CreateCoreWindow, this asks the container for a window on one view while it is mid-way
     * through creating another, and SetWindowAndGetDispatcher never returns. */
    if (drive_first_view())
    {
        /* A wall-clock deadline rather than a fixed tick count. Anything that stalls the whole
         * process for a minute - the loader lock being held while some DllMain blocks, which is
         * what a dead X display does to the first RegisterClassW - used to expire the old 600 x
         * 25 ms budget and drop the startup into the much worse activation path with no
         * CoreWindow. The loop still exits the moment the startup is claimed (about 12 tries on
         * a healthy run), so the longer deadline costs a working run nothing and only changes
         * how a sick one reports itself. */
        start = GetTickCount();
        for (tries = 0; GetTickCount() - start < 90000 && !view_startup_done; tries++)
        {
            if (!(tries % 8)) queue_drive_view_apcs();
            if (tries && !(tries % 600))
                ERR( "still waiting for a view to claim the startup, %lu ms\n", GetTickCount() - start );
            Sleep( 25 );
        }
        ERR( "first-view startup %s after %u tries, %lu ms\n",
             view_startup_done ? "claimed" : "not taken", tries, GetTickCount() - start );
        if (view_startup_done)
        {
            registrar_Release( reg );
            if (SUCCEEDED(hr)) CoUninitialize();
            return 0;
        }
    }

    if (GetEnvironmentVariableW( L"WINE_UWP_ACTIVATE_ON_VIEW", NULL, 0 ))
    {
        /* Do not wait for the main view here: by the time a poll from this thread sees it, the
         * container is already a millisecond or two into tearing the views down again, and an
         * activation that lands then deadlocks. Queue the APCs now and let them retry on the
         * E_POINTER that Activate returns until the view is ready. */
        /* Loading App.xaml from here, before the activation - which is when Windows does it -
         * was tried: broadcasting the APC repeatedly from this thread costs the activation its
         * race with the view teardown, and no run completed. It is left to the window factory,
         * which runs on the view thread and is still before the Activated event. */
        unsigned queued = queue_activate_apcs( reg );
        ERR( "queued %u Activate APCs for view-thread delivery\n", queued );
        if (queued)
        {
            /* Give a view thread a moment to enter an alertable wait and run the APC; the
             * activation is better off there. But the window in which the container will still
             * accept one is only a few hundred milliseconds wide, so take it here rather than
             * miss it. */
            /* The APC is used up on threads without a DXamlCore, so keep re-queuing until the
             * view thread takes it. The any-thread fallback below ran Activate on an STA in
             * every run that reached it, and none of those runs ever created the CoreWindow. */
            for (tries = 0; tries < 400 && !activate_apc_done; tries++)
            {
                Sleep( 25 );
                if (tries % 20 == 19 && !activate_apc_done) queue_activate_apcs( reg );
            }
            if (!activate_apc_done)
            {
                /* Nobody with a DXamlCore took it. Activating from the wrong thread deadlocks,
                 * but not activating at all is certain failure, so try here and then let any
                 * thread have it. */
                ERR( "no view thread took the activation; allowing any thread\n" );
                InterlockedExchange( &activate_any_thread, 1 );
                queue_activate_apcs( reg );
                for (tries = 0; tries < 20 && !activate_apc_done; tries++) Sleep( 25 );
                if (!activate_apc_done) try_activate_once( reg );
            }
            for (tries = 0; tries < 100 && !activate_apc_done; tries++) Sleep( 50 );

            registrar_Release( reg );
            if (SUCCEEDED(hr)) CoUninitialize();
            return 0;
        }
        WARN( "no threads for APC Activate; falling back to RunAsync\n" );

        for (tries = 0; tries < 40; tries++)
        {
            if (SUCCEEDED( get_main_view_dispatcher( (void **)&dispatcher ) ) && dispatcher) break;
            Sleep( 25 );
            dispatcher = NULL;
        }
        if (!dispatcher) WARN( "the main view has no dispatcher yet\n" );
    }

    if (!dispatcher)
    {
        unsigned int activate_tries;

        /* No APC path here, so there is no view thread to race: wait for the main view. */
        wait_for_main_view();
        /* Same E_POINTER retry as handler_Invoke: factory field may still be unset. */
        for (activate_tries = 0; activate_tries < 40; activate_tries++)
        {
            hr = do_activate( reg );
            if (hr != E_POINTER) break;
            Sleep( 50 );
        }
    }
    else if (!(handler = calloc( 1, sizeof(*handler) )))
        ERR( "out of memory\n" );
    else
    {
        void *action = NULL;

        handler->vtbl = &dispatched_handler_vtbl;
        handler->reg = reg;
        handler->refcount = 1;
        /* CoreDispatcherPriority_Normal */
        hr = (*dispatcher)->slot[8]( dispatcher, 0, handler, &action );
        if (FAILED( hr )) ERR( "RunAsync failed, hr %#lx\n", hr );
        if (action) IUnknown_Release( (IUnknown *)action );
        handler->vtbl->Release( handler );
        IUnknown_Release( (IUnknown *)dispatcher );
    }

    registrar_Release( reg );
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

static HRESULT WINAPI registrar_QueryInterface(struct registrar *reg, REFIID iid, void **out)
{
    TRACE("%p, %s, %p\n", reg, debugstr_guid(iid), out);
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IActivatableApplicationRegistrar))
    {
        *out = reg;
        InterlockedIncrement(&reg->refcount);
        return S_OK;
    }
    FIXME("unsupported interface %s\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI registrar_AddRef(struct registrar *reg) { return InterlockedIncrement(&reg->refcount); }
static ULONG WINAPI registrar_Release(struct registrar *reg)
{
    ULONG ref = InterlockedDecrement(&reg->refcount);
    if (!ref)
    {
        if (reg->app) IUnknown_Release(reg->app);
        free(reg);
    }
    return ref;
}

/* slot 3: called by CoreApplication::RegisterActivatableApplication with the app's activation object */
static HRESULT WINAPI registrar_RegisterActivatableApplication(struct registrar *reg, IUnknown *app)
{
    HANDLE thread;

    TRACE("%p, %p\n", reg, app);
    if (!app) return E_INVALIDARG;
    if (reg->app) IUnknown_Release(reg->app);
    IUnknown_AddRef(app);
    reg->app = app;
    reg->owner_tid = GetCurrentThreadId();

    InterlockedIncrement(&reg->refcount);
    if (!(thread = CreateThread(NULL, 0, activation_thread, reg, 0, NULL)))
    {
        registrar_Release(reg);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(thread);
    return S_OK;
}
static HRESULT WINAPI registrar_unknown_method(struct registrar *reg)
{
    FIXME("unexpected IActivatableApplicationRegistrar method called\n");
    return E_NOTIMPL;
}
static const void *registrar_vtbl[] = { registrar_QueryInterface, registrar_AddRef, registrar_Release,
    registrar_RegisterActivatableApplication, registrar_unknown_method, registrar_unknown_method, registrar_unknown_method };

/* ---------------------------------------------------------------------------------------------
 * Class factory / exports
 */
static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IClassFactory)) { *out = iface; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI factory_AddRef(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_Release(IClassFactory *iface) { return 1; }
static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    struct registrar *reg;
    HRESULT hr;
    TRACE("%p, %s, %p\n", outer, debugstr_guid(iid), out);
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(reg = calloc(1, sizeof(*reg)))) return E_OUTOFMEMORY;
    reg->vtbl = registrar_vtbl;
    reg->refcount = 1;
    hr = registrar_QueryInterface(reg, iid, out);
    registrar_Release(reg);
    return hr;
}
static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock) { return S_OK; }
static const IClassFactoryVtbl factory_vtbl = { factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer };
static IClassFactory registrar_factory = { &factory_vtbl };

/* ---------------------------------------------------------------------------------------------
 * ICoreWindowFactory stand-in (Windows hosts this as a local-server broker)
 *
 * GetWindowFactory CoCreates CLSID_CoreWindowBroker as CLSCTX_LOCAL_SERVER with IID
 * ICoreWindowBroker, QueryService-likes it, then QIs ICoreWindowFactory. CreateCoreWindow
 * calls windows.ui.dll ordinal 1500 (PrivateCreateCoreWindow).
 */
typedef HRESULT (WINAPI *PrivateCreateCoreWindow_t)( DWORD type, const WCHAR *title, INT x, INT y,
        UINT width, UINT height, DWORD flags, HWND parent, const GUID *iid, void **out );

struct window_factory
{
    const void *vtbl_broker;
    const void *vtbl_factory;
    const void *vtbl_configure;
    IUnknown *ftm;
    LONG refcount;
};

static struct window_factory *factory_from_factory( void *iface )
{
    return (struct window_factory *)((char *)iface - offsetof( struct window_factory, vtbl_factory ));
}
static struct window_factory *factory_from_configure( void *iface )
{
    return (struct window_factory *)((char *)iface - offsetof( struct window_factory, vtbl_configure ));
}

static HRESULT WINAPI wf_QueryInterface( struct window_factory *wf, REFIID iid, void **out );

static ULONG WINAPI wf_AddRef( struct window_factory *wf )
{
    return InterlockedIncrement( &wf->refcount );
}

static ULONG WINAPI wf_Release( struct window_factory *wf )
{
    ULONG ref = InterlockedDecrement( &wf->refcount );
    if (!ref)
    {
        if (wf->ftm) IUnknown_Release( wf->ftm );
        free( wf );
    }
    return ref;
}

static HRESULT WINAPI wf_GetIids( struct window_factory *wf, ULONG *count, IID **iids )
{
    *count = 0; *iids = NULL; return S_OK;
}
static HRESULT WINAPI wf_GetRuntimeClassName( struct window_factory *wf, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.UAPCoreWindowFactory", 37, name );
}
static HRESULT WINAPI wf_GetTrustLevel( struct window_factory *wf, TrustLevel *level )
{
    *level = BaseTrust; return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * INavigationClient stand-in
 *
 * Windows creates the CoreWindow through CImmersiveWindowFactoryProxy, which also activates a
 * Windows.Phone.UI.Core.ImmersiveNavigationClient and stores it on the window's WindowServer.
 * CoreApplicationView::PrepareToActivateAsync fails the whole activation with E_NOTIMPL when that
 * property is empty, so a client has to be there.  The real one (CoreUIComponents.dll) creates a
 * view in the shell's CoreUI server, which a Wine prefix has no counterpart for, so this is a
 * local object with the same shape.  Only PrepareToActivate is meaningful; the rest of the
 * interface is logged as it gets used.
 */

#define ASYNC_STATUS_COMPLETED 1

/* IAsyncAction that is already complete: PrepareToActivate has nothing to wait for here. */
struct done_action
{
    const struct done_action_vtbl *vtbl_action;
    const struct done_info_vtbl *vtbl_info;
    IUnknown *handler;
    IUnknown *ftm;
    LONG refcount;
};

struct done_action_vtbl
{
    HRESULT (WINAPI *QueryInterface)( struct done_action *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( struct done_action * );
    ULONG (WINAPI *Release)( struct done_action * );
    HRESULT (WINAPI *GetIids)( struct done_action *, ULONG *, IID ** );
    HRESULT (WINAPI *GetRuntimeClassName)( struct done_action *, HSTRING * );
    HRESULT (WINAPI *GetTrustLevel)( struct done_action *, TrustLevel * );
    HRESULT (WINAPI *put_Completed)( struct done_action *, IUnknown * );
    HRESULT (WINAPI *get_Completed)( struct done_action *, IUnknown ** );
    HRESULT (WINAPI *GetResults)( struct done_action * );
};

struct done_info_vtbl
{
    HRESULT (WINAPI *QueryInterface)( void *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( void * );
    ULONG (WINAPI *Release)( void * );
    HRESULT (WINAPI *get_Id)( void *, UINT32 * );
    HRESULT (WINAPI *get_Status)( void *, INT32 * );
    HRESULT (WINAPI *get_ErrorCode)( void *, HRESULT * );
    HRESULT (WINAPI *Cancel)( void * );
    HRESULT (WINAPI *Close)( void * );
};

/* IAsyncActionCompletedHandler::Invoke( IAsyncAction *, AsyncStatus ) */
struct completed_handler_vtbl
{
    HRESULT (WINAPI *QueryInterface)( IUnknown *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( IUnknown * );
    ULONG (WINAPI *Release)( IUnknown * );
    HRESULT (WINAPI *Invoke)( IUnknown *, void *action, INT32 status );
};

static struct done_action *action_from_info( void *iface )
{
    return (struct done_action *)((char *)iface - offsetof( struct done_action, vtbl_info ));
}

static HRESULT WINAPI action_QueryInterface( struct done_action *action, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAsyncActionStandIn ))
        *out = &action->vtbl_action;
    else if (IsEqualGUID( iid, &IID_IAsyncInfoStandIn ))
        *out = &action->vtbl_info;
    else if (IsEqualGUID( iid, &IID_IMarshal ) || IsEqualGUID( iid, &IID_IAgileObject ))
        return IUnknown_QueryInterface( action->ftm, iid, out );
    else
    {
        WARN( "no interface %s\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &action->refcount );
    return S_OK;
}

static ULONG WINAPI action_AddRef( struct done_action *action )
{
    return InterlockedIncrement( &action->refcount );
}

static ULONG WINAPI action_Release( struct done_action *action )
{
    ULONG ref = InterlockedDecrement( &action->refcount );
    if (!ref)
    {
        if (action->handler) IUnknown_Release( action->handler );
        if (action->ftm) IUnknown_Release( action->ftm );
        free( action );
    }
    return ref;
}

static HRESULT WINAPI action_GetIids( struct done_action *action, ULONG *count, IID **iids )
{
    *count = 0; *iids = NULL; return S_OK;
}
static HRESULT WINAPI action_GetRuntimeClassName( struct done_action *action, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncAction", 31, name );
}
static HRESULT WINAPI action_GetTrustLevel( struct done_action *action, TrustLevel *level )
{
    *level = BaseTrust; return S_OK;
}

static HRESULT WINAPI action_put_Completed( struct done_action *action, IUnknown *handler )
{
    const struct completed_handler_vtbl **vtbl = (const struct completed_handler_vtbl **)handler;

    TRACE( "%p %p\n", action, handler );
    if (!handler) return S_OK;
    /* The action completed before it was handed out, so call the handler straight away. */
    return (*vtbl)->Invoke( handler, &action->vtbl_action, ASYNC_STATUS_COMPLETED );
}

static HRESULT WINAPI action_get_Completed( struct done_action *action, IUnknown **handler )
{
    *handler = NULL; return S_OK;
}
static HRESULT WINAPI action_GetResults( struct done_action *action ) { return S_OK; }

static HRESULT WINAPI info_QueryInterface( void *iface, REFIID iid, void **out )
{
    return action_QueryInterface( action_from_info( iface ), iid, out );
}
static ULONG WINAPI info_AddRef( void *iface ) { return action_AddRef( action_from_info( iface ) ); }
static ULONG WINAPI info_Release( void *iface ) { return action_Release( action_from_info( iface ) ); }
static HRESULT WINAPI info_get_Id( void *iface, UINT32 *id ) { *id = 1; return S_OK; }
static HRESULT WINAPI info_get_Status( void *iface, INT32 *status )
{
    *status = ASYNC_STATUS_COMPLETED; return S_OK;
}
static HRESULT WINAPI info_get_ErrorCode( void *iface, HRESULT *hr ) { *hr = S_OK; return S_OK; }
static HRESULT WINAPI info_Cancel( void *iface ) { return S_OK; }
static HRESULT WINAPI info_Close( void *iface ) { return S_OK; }

static const struct done_action_vtbl done_action_vtbl =
{
    action_QueryInterface, action_AddRef, action_Release, action_GetIids,
    action_GetRuntimeClassName, action_GetTrustLevel,
    action_put_Completed, action_get_Completed, action_GetResults,
};
static const struct done_info_vtbl done_info_vtbl =
{
    info_QueryInterface, info_AddRef, info_Release,
    info_get_Id, info_get_Status, info_get_ErrorCode, info_Cancel, info_Close,
};

static HRESULT create_completed_action( void **out )
{
    struct done_action *action;

    if (!(action = calloc( 1, sizeof(*action) ))) return E_OUTOFMEMORY;
    action->vtbl_action = &done_action_vtbl;
    action->vtbl_info = &done_info_vtbl;
    action->refcount = 1;
    if (FAILED( CoCreateFreeThreadedMarshaler( (IUnknown *)&action->vtbl_action, &action->ftm ) ))
    {
        free( action );
        return E_OUTOFMEMORY;
    }
    *out = &action->vtbl_action;
    return S_OK;
}

/* The navigation client itself.  NAV_CLIENT_SLOTS matches the real vtable slot for slot, so a
 * caller reaching a high slot lands on a method of ours instead of off the end of the array. */
struct nav_client
{
    const void **vtbl_client;
    const void **vtbl_prepare;
    IUnknown *ftm;
    LONG refcount;
};

static struct nav_client *nav_from_prepare( void *iface )
{
    return (struct nav_client *)((char *)iface - offsetof( struct nav_client, vtbl_prepare ));
}

static HRESULT WINAPI nav_QueryInterface( struct nav_client *nav, REFIID iid, void **out );

static ULONG WINAPI nav_AddRef( struct nav_client *nav )
{
    return InterlockedIncrement( &nav->refcount );
}
static ULONG WINAPI nav_Release( struct nav_client *nav )
{
    ULONG ref = InterlockedDecrement( &nav->refcount );
    if (!ref)
    {
        if (nav->ftm) IUnknown_Release( nav->ftm );
        free( nav );
    }
    return ref;
}
static HRESULT WINAPI nav_GetIids( struct nav_client *nav, ULONG *count, IID **iids )
{
    *count = 0; *iids = NULL; return S_OK;
}
static HRESULT WINAPI nav_GetRuntimeClassName( struct nav_client *nav, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Phone.UI.Core.ImmersiveNavigationClient", 47, name );
}
static HRESULT WINAPI nav_GetTrustLevel( struct nav_client *nav, TrustLevel *level )
{
    *level = BaseTrust; return S_OK;
}

/* {075be329-825a-4f2a-a8af-cd2f9a8102ec} slot 6: hand the caller a completed IAsyncAction. */
static HRESULT WINAPI nav_PrepareToActivate( void *iface, UINT64 arg, void **action )
{
    TRACE( "%p %#I64x %p\n", iface, arg, action );
    if (!action) return E_POINTER;
    return create_completed_action( action );
}

static HRESULT WINAPI prepare_QueryInterface( void *iface, REFIID iid, void **out )
{
    return nav_QueryInterface( nav_from_prepare( iface ), iid, out );
}
static ULONG WINAPI prepare_AddRef( void *iface ) { return nav_AddRef( nav_from_prepare( iface ) ); }
static ULONG WINAPI prepare_Release( void *iface ) { return nav_Release( nav_from_prepare( iface ) ); }
static HRESULT WINAPI prepare_GetIids( void *iface, ULONG *count, IID **iids )
{
    return nav_GetIids( nav_from_prepare( iface ), count, iids );
}
static HRESULT WINAPI prepare_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return nav_GetRuntimeClassName( nav_from_prepare( iface ), name );
}
static HRESULT WINAPI prepare_GetTrustLevel( void *iface, TrustLevel *level )
{
    return nav_GetTrustLevel( nav_from_prepare( iface ), level );
}

/* Every slot below is named from CoreUIComponents' real NavigationClient vtable
 * (??_7NavigationClient@Core@UI@Phone@Windows@@6BINavigationClient@1234@@, RVA 0x1af3f8 in the
 * pinned Windows 11 build, 138 slots), resolved against Microsoft's public symbols.  The values
 * describe the one view this host has: a single top-level 1280x800 landscape window with no
 * system title bar.  Slots whose parameter shape is not recoverable from the symbol name alone
 * stay numbered stubs, because writing a guessed struct would corrupt the caller's stack.
 */
#define DISPLAY_ORIENTATION_LANDSCAPE 1

/* Windows.Foundation.Size */
struct foundation_size { FLOAT width, height; };

/* VIEW_BOUNDS: nine 32-bit fields copied straight out of the client (GetViewBounds at RVA
 * 0x7dc40 moves [this+0x2c0..+0x2e0] to [out+0x00..+0x20]).  get_VisibleBounds proves the first
 * two are the window width and height in pixels; the rest are unidentified, so they read 0. */
struct view_bounds { INT32 width, height, unknown[7]; };

static HRESULT WINAPI nav_get_SplashScreen( void *iface, void **value )
{
    FIXME( "%p %p: stub\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI nav_put_SplashScreen( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsScreenCaptureEnabled( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsScreenCaptureEnabled( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_SuppressSystemOverlays( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_put_SuppressSystemOverlays( void *iface, INT32 value )
{
    TRACE( "%p %d\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_VisibleBounds( void *iface, struct foundation_rect *bounds )
{
    TRACE( "%p %p\n", iface, bounds );
    if (!bounds) return E_POINTER;
    bounds->x = bounds->y = 0.0f;
    current_view_size( &bounds->width, &bounds->height );
    return S_OK;
}

static HRESULT WINAPI nav_add_OrientationChanged( void *iface, void *handler, UINT64 *token )
{
    TRACE( "%p handler %p\n", iface, handler );
    if (!token) return E_POINTER;
    /* Nothing here raises these events, so hand back a token that removes cleanly. */
    *token = 0;
    return S_OK;
}

static HRESULT WINAPI nav_remove_OrientationChanged( void *iface, UINT64 token )
{
    TRACE( "%p token %#I64x\n", iface, token );
    return S_OK;
}

static HRESULT WINAPI nav_add_TitleBarVisibilityChanged( void *iface, void *handler, UINT64 *token )
{
    TRACE( "%p handler %p\n", iface, handler );
    if (!token) return E_POINTER;
    /* Nothing here raises these events, so hand back a token that removes cleanly. */
    *token = 0;
    return S_OK;
}

static HRESULT WINAPI nav_remove_TitleBarVisibilityChanged( void *iface, UINT64 token )
{
    TRACE( "%p token %#I64x\n", iface, token );
    return S_OK;
}

static HRESULT WINAPI nav_add_TitleBarLayoutMetricsChanged( void *iface, void *handler, UINT64 *token )
{
    TRACE( "%p handler %p\n", iface, handler );
    if (!token) return E_POINTER;
    /* Nothing here raises these events, so hand back a token that removes cleanly. */
    *token = 0;
    return S_OK;
}

static HRESULT WINAPI nav_remove_TitleBarLayoutMetricsChanged( void *iface, UINT64 token )
{
    TRACE( "%p token %#I64x\n", iface, token );
    return S_OK;
}

static HRESULT WINAPI nav_add_TitleBarInputRoutingInfoChanged( void *iface, void *handler, UINT64 *token )
{
    TRACE( "%p handler %p\n", iface, handler );
    if (!token) return E_POINTER;
    /* Nothing here raises these events, so hand back a token that removes cleanly. */
    *token = 0;
    return S_OK;
}

static HRESULT WINAPI nav_remove_TitleBarInputRoutingInfoChanged( void *iface, UINT64 token )
{
    TRACE( "%p token %#I64x\n", iface, token );
    return S_OK;
}

static HRESULT WINAPI nav_GetCurrentOrientation( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = DISPLAY_ORIENTATION_LANDSCAPE;
    return S_OK;
}

static DWORD view_activated_tick;   /* GetTickCount() of the app's Window.Activate(), 0 before */

static HRESULT WINAPI nav_OnActivateWindow( void *iface, void *window )
{
    TRACE( "%s: core window %p, caller %p\n", "OnActivateWindow", window, __builtin_return_address( 0 ) );
    if (!view_activated_tick) view_activated_tick = GetTickCount() | 1;
    /* The app's Window.Activate(): on Windows the shell activates the CoreWindow now, and the
     * window's keyboard-cue state is initialised at that point. The host shows the window earlier
     * (before XAML subclasses it), so re-initialise here: XAML reads the resulting
     * WM_UPDATEUISTATE to decide whether focus visuals should be drawn. */
    if (hosted_core_window)
    {
        SendMessageW( hosted_core_window, WM_CHANGEUISTATE, MAKEWPARAM( UIS_INITIALIZE, 0 ), 0 );
        /* ...and the shell reports it visible; see WM_WINE_UWP_VISIBLE */
        PostMessageW( hosted_core_window, WM_WINE_UWP_VISIBLE, 0, 0 );
    }
    return S_OK;
}

static HRESULT WINAPI nav_OnCloseWindow( void *iface, void *window )
{
    ERR( "%s: core window %p\n", "OnCloseWindow", window );
    return S_OK;
}

static HRESULT WINAPI nav_OnDestroyWindow( void *iface, void *window )
{
    ERR( "%s: core window %p\n", "OnDestroyWindow", window );
    return S_OK;
}

static HRESULT WINAPI nav_GetNativeOrientation( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = DISPLAY_ORIENTATION_LANDSCAPE;
    return S_OK;
}

static HRESULT WINAPI nav_ShowStandardSystemOverlays( void *iface )
{
    TRACE( "%p\n", iface );
    return S_OK;
}

static HRESULT WINAPI nav_GetViewInstanceId( void *iface, UINT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI nav_GetViewBounds( void *iface, struct view_bounds *bounds )
{
    TRACE( "%p %p\n", iface, bounds );
    if (!bounds) return E_POINTER;
    memset( bounds, 0, sizeof(*bounds) );
    {
        FLOAT w, h;
        current_view_size( &w, &h );
        bounds->width = (int)w;
        bounds->height = (int)h;
    }
    return S_OK;
}

static HRESULT WINAPI nav_SetPreferredMinSize( void *iface, struct foundation_size size )
{
    TRACE( "%p %gx%g\n", iface, size.width, size.height );
    return S_OK;
}

static HRESULT WINAPI nav_get_PreferredLaunchWindowingMode( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_put_PreferredLaunchWindowingMode( void *iface, INT32 value )
{
    TRACE( "%p %d\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_PreferredLaunchViewSize( void *iface, struct foundation_size *size )
{
    TRACE( "%p %p\n", iface, size );
    if (!size) return E_POINTER;
    size->width = CORE_WINDOW_WIDTH;
    size->height = CORE_WINDOW_HEIGHT;
    return S_OK;
}

static HRESULT WINAPI nav_put_PreferredLaunchViewSize( void *iface, struct foundation_size size )
{
    TRACE( "%p %gx%g\n", iface, size.width, size.height );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsViewExtendedIntoTitleBar( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsViewExtendedIntoTitleBar( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarHeight( void *iface, DOUBLE *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0.0;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsTitleBarVisible( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarSystemOverlayLeftInset( void *iface, DOUBLE *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0.0;
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarSystemOverlayRightInset( void *iface, DOUBLE *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0.0;
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarInputRoutingInfo( void *iface, void *value )
{
    FIXME( "%p %p: stub, parameter shape unknown\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI nav_get_TitleBarInputRoutingHwnd( void *iface, UINT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsTitleBarInputVisualSet( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsTitleBarInputVisualSet( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarTextColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarTextColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarBackgroundColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarBackgroundColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarButtonTextColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarButtonTextColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarButtonBackgroundColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarButtonBackgroundColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarButtonHoverTextColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarButtonHoverTextColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarButtonHoverBackgroundColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarButtonHoverBackgroundColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarButtonPressedTextColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarButtonPressedTextColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarButtonPressedBackgroundColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarButtonPressedBackgroundColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarInactiveTextColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarInactiveTextColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarInactiveBackgroundColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarInactiveBackgroundColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarInactiveButtonTextColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarInactiveButtonTextColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_TitleBarInactiveButtonBackgroundColorOverride( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_TitleBarInactiveButtonBackgroundColorOverride( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_OnNavigateBackRequest( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_SetAppAsComponent( void *iface )
{
    TRACE( "%p\n", iface );
    return S_OK;
}

static HRESULT WINAPI nav_DisableResponsiveAppViewPolicy( void *iface )
{
    TRACE( "%p\n", iface );
    return S_OK;
}

static HRESULT WINAPI nav_Uninitialize( void *iface, void *window )
{
    ERR( "%s: core window %p\n", "Uninitialize", window );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsCloaked( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsDisabled( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsDisabled( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsMaximizable( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsMaximizable( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsMinimizable( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsMinimizable( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsMovable( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsMovable( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsPopup( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsPopup( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsProxied( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsProxied( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsResizable( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsResizable( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsWin32NoActivate( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsWin32NoActivate( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsWin32ToolWindow( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsWin32ToolWindow( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsWin32TopLevelAppWindow( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_IsWin32TopLevelAppWindow( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_HideInTaskSwitcher( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_HideInTaskSwitcher( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_PreferDisableSystemUXPolicy( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_put_PreferDisableSystemUXPolicy( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_ShouldSuppressCaptionControls( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_put_ShouldSuppressCaptionControls( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_RemoteConfigurationDescription( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_RemoteConfigurationDescription( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_RemoteConfigurationName( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_RemoteConfigurationName( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_SetPreferredMaxSize( void *iface, struct foundation_size size )
{
    TRACE( "%p %gx%g\n", iface, size.width, size.height );
    return S_OK;
}

static HRESULT WINAPI nav_SetPreferredScale( void *iface, INT32 value )
{
    TRACE( "%p %d\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_GetWindowInstanceId( void *iface, UINT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI nav_GetServerTaskInstanceId( void *iface, UINT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI nav_RequestHide( void *iface, INT32 value )
{
    TRACE( "%p %d\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_GetSpanningRects( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_get_ContainerId( void *iface, GUID *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    memset( value, 0, sizeof(*value) );
    return S_OK;
}

static HRESULT WINAPI nav_put_ContainerId( void *iface, GUID value )
{
    TRACE( "%p %s\n", iface, debugstr_guid( &value ) );
    return S_OK;
}

static HRESULT WINAPI nav_get_IsTopMost( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_get_Placement( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_get_ProxiedAppId( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_put_ProxiedAppId( void *iface, void *value )
{
    TRACE( "%p %p\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_ProxiedProcessId( void *iface, UINT64 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_put_ProxiedProcessId( void *iface, UINT64 value )
{
    TRACE( "%p %#I64x\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_get_ProxiedHwnd( void *iface, UINT64 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_put_ProxiedHwnd( void *iface, UINT64 value )
{
    TRACE( "%p %#I64x\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_TrySetTopMost( void *iface, boolean value )
{
    TRACE( "%p %u\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI nav_PresentationLoadingComplete( void *iface )
{
    ERR( "%s\n", "PresentationLoadingComplete" );
    return S_OK;
}

static HRESULT WINAPI nav_PresentationUnloadedComplete( void *iface )
{
    ERR( "%s\n", "PresentationUnloadedComplete" );
    return S_OK;
}

static HRESULT WINAPI nav_add_ScaleChanged( void *iface, void *handler, UINT64 *token )
{
    TRACE( "%p handler %p\n", iface, handler );
    if (!token) return E_POINTER;
    /* Nothing here raises these events, so hand back a token that removes cleanly. */
    *token = 0;
    return S_OK;
}

static HRESULT WINAPI nav_remove_ScaleChanged( void *iface, UINT64 token )
{
    TRACE( "%p token %#I64x\n", iface, token );
    return S_OK;
}

static HRESULT WINAPI nav_add_OrientationChangedInternal( void *iface, void *handler, UINT64 *token )
{
    TRACE( "%p handler %p\n", iface, handler );
    if (!token) return E_POINTER;
    /* Nothing here raises these events, so hand back a token that removes cleanly. */
    *token = 0;
    return S_OK;
}

static HRESULT WINAPI nav_remove_OrientationChangedInternal( void *iface, UINT64 token )
{
    TRACE( "%p token %#I64x\n", iface, token );
    return S_OK;
}

static HRESULT WINAPI nav_get_ConsolidationStatus( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsConsolidated( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsMinimized( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsOnLockScreen( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI nav_get_IsVisible( void *iface, boolean *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI nav_get_PreferredMaxSize( void *iface, struct foundation_size *size )
{
    TRACE( "%p %p\n", iface, size );
    if (!size) return E_POINTER;
    size->width = CORE_WINDOW_WIDTH;
    size->height = CORE_WINDOW_HEIGHT;
    return S_OK;
}

static HRESULT WINAPI nav_get_PreferredMinSize( void *iface, struct foundation_size *size )
{
    TRACE( "%p %p\n", iface, size );
    if (!size) return E_POINTER;
    size->width = 0.0f;
    size->height = 0.0f;
    return S_OK;
}

static HRESULT WINAPI nav_get_PreferredScale( void *iface, UINT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 100;
    return S_OK;
}

static HRESULT WINAPI nav_get_ViewPresentationMode( void *iface, INT32 *value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI nav_get_PrimaryDisplayRegion( void *iface, void **value )
{
    TRACE( "%p %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI nav_NotifyScaleChanged( void *iface )
{
    TRACE( "%p\n", iface );
    return S_OK;
}

static HRESULT WINAPI nav_stub_72( void *iface ) { FIXME( "INavigationClient slot 72: stub\n" ); return E_NOTIMPL; }

#define NAV_CLIENT_SLOTS 138

static const void *nav_client_vtbl[NAV_CLIENT_SLOTS] =
{
    nav_QueryInterface, nav_AddRef, nav_Release,
    nav_GetIids, nav_GetRuntimeClassName, nav_GetTrustLevel,
    nav_get_SplashScreen, nav_put_SplashScreen, nav_get_IsScreenCaptureEnabled,
    nav_put_IsScreenCaptureEnabled, nav_get_SuppressSystemOverlays, nav_put_SuppressSystemOverlays,
    nav_get_VisibleBounds, nav_add_OrientationChanged, nav_remove_OrientationChanged,
    nav_add_TitleBarVisibilityChanged, nav_remove_TitleBarVisibilityChanged,
    nav_add_TitleBarLayoutMetricsChanged, nav_remove_TitleBarLayoutMetricsChanged,
    nav_add_TitleBarInputRoutingInfoChanged, nav_remove_TitleBarInputRoutingInfoChanged,
    nav_GetCurrentOrientation, nav_OnActivateWindow, nav_OnCloseWindow, nav_OnDestroyWindow,
    nav_GetNativeOrientation, nav_ShowStandardSystemOverlays, nav_GetViewInstanceId, nav_GetViewBounds,
    nav_SetPreferredMinSize, nav_get_PreferredLaunchWindowingMode, nav_put_PreferredLaunchWindowingMode,
    nav_get_PreferredLaunchViewSize, nav_put_PreferredLaunchViewSize, nav_get_IsViewExtendedIntoTitleBar,
    nav_put_IsViewExtendedIntoTitleBar, nav_get_TitleBarHeight, nav_get_IsTitleBarVisible,
    nav_get_TitleBarSystemOverlayLeftInset, nav_get_TitleBarSystemOverlayRightInset,
    nav_get_TitleBarInputRoutingInfo, nav_get_TitleBarInputRoutingHwnd, nav_get_IsTitleBarInputVisualSet,
    nav_put_IsTitleBarInputVisualSet, nav_get_TitleBarTextColorOverride, nav_put_TitleBarTextColorOverride,
    nav_get_TitleBarBackgroundColorOverride, nav_put_TitleBarBackgroundColorOverride,
    nav_get_TitleBarButtonTextColorOverride, nav_put_TitleBarButtonTextColorOverride,
    nav_get_TitleBarButtonBackgroundColorOverride, nav_put_TitleBarButtonBackgroundColorOverride,
    nav_get_TitleBarButtonHoverTextColorOverride, nav_put_TitleBarButtonHoverTextColorOverride,
    nav_get_TitleBarButtonHoverBackgroundColorOverride, nav_put_TitleBarButtonHoverBackgroundColorOverride,
    nav_get_TitleBarButtonPressedTextColorOverride, nav_put_TitleBarButtonPressedTextColorOverride,
    nav_get_TitleBarButtonPressedBackgroundColorOverride,
    nav_put_TitleBarButtonPressedBackgroundColorOverride, nav_get_TitleBarInactiveTextColorOverride,
    nav_put_TitleBarInactiveTextColorOverride, nav_get_TitleBarInactiveBackgroundColorOverride,
    nav_put_TitleBarInactiveBackgroundColorOverride, nav_get_TitleBarInactiveButtonTextColorOverride,
    nav_put_TitleBarInactiveButtonTextColorOverride, nav_get_TitleBarInactiveButtonBackgroundColorOverride,
    nav_put_TitleBarInactiveButtonBackgroundColorOverride, nav_OnNavigateBackRequest, nav_SetAppAsComponent,
    nav_DisableResponsiveAppViewPolicy, nav_Uninitialize, nav_stub_72, nav_get_IsCloaked,
    nav_get_IsDisabled, nav_put_IsDisabled, nav_get_IsMaximizable, nav_put_IsMaximizable,
    nav_get_IsMinimizable, nav_put_IsMinimizable, nav_get_IsMovable, nav_put_IsMovable, nav_get_IsPopup,
    nav_put_IsPopup, nav_get_IsProxied, nav_put_IsProxied, nav_get_IsResizable, nav_put_IsResizable,
    nav_get_IsWin32NoActivate, nav_put_IsWin32NoActivate, nav_get_IsWin32ToolWindow,
    nav_put_IsWin32ToolWindow, nav_get_IsWin32TopLevelAppWindow, nav_put_IsWin32TopLevelAppWindow,
    nav_get_HideInTaskSwitcher, nav_put_HideInTaskSwitcher, nav_get_PreferDisableSystemUXPolicy,
    nav_put_PreferDisableSystemUXPolicy, nav_get_ShouldSuppressCaptionControls,
    nav_put_ShouldSuppressCaptionControls, nav_get_RemoteConfigurationDescription,
    nav_put_RemoteConfigurationDescription, nav_get_RemoteConfigurationName,
    nav_put_RemoteConfigurationName, nav_SetPreferredMaxSize, nav_SetPreferredScale,
    nav_GetWindowInstanceId, nav_GetServerTaskInstanceId, nav_RequestHide, nav_GetSpanningRects,
    nav_get_ContainerId, nav_put_ContainerId, nav_get_IsTopMost, nav_get_Placement, nav_get_ProxiedAppId,
    nav_put_ProxiedAppId, nav_get_ProxiedProcessId, nav_put_ProxiedProcessId, nav_get_ProxiedHwnd,
    nav_put_ProxiedHwnd, nav_TrySetTopMost, nav_PresentationLoadingComplete,
    nav_PresentationUnloadedComplete, nav_add_ScaleChanged, nav_remove_ScaleChanged,
    nav_add_OrientationChangedInternal, nav_remove_OrientationChangedInternal, nav_get_ConsolidationStatus,
    nav_get_IsConsolidated, nav_get_IsMinimized, nav_get_IsOnLockScreen, nav_get_IsVisible,
    nav_get_PreferredMaxSize, nav_get_PreferredMinSize, nav_get_PreferredScale,
    nav_get_ViewPresentationMode, nav_get_PrimaryDisplayRegion, nav_NotifyScaleChanged,
};

static const void *nav_prepare_vtbl[] =
{
    prepare_QueryInterface, prepare_AddRef, prepare_Release,
    prepare_GetIids, prepare_GetRuntimeClassName, prepare_GetTrustLevel,
    nav_PrepareToActivate,
};

static HRESULT WINAPI nav_QueryInterface( struct nav_client *nav, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ))
        *out = &nav->vtbl_client;
    else if (IsEqualGUID( iid, &IID_INavigationClientPrepareToActivate ))
        *out = &nav->vtbl_prepare;
    else if (IsEqualGUID( iid, &IID_IMarshal ) || IsEqualGUID( iid, &IID_IAgileObject ))
        return IUnknown_QueryInterface( nav->ftm, iid, out );
    else
    {
        FIXME( "navigation client has no interface %s\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &nav->refcount );
    return S_OK;
}

static HRESULT create_navigation_client( IUnknown **out )
{
    struct nav_client *nav;

    if (!(nav = calloc( 1, sizeof(*nav) ))) return E_OUTOFMEMORY;
    nav->vtbl_client = nav_client_vtbl;
    nav->vtbl_prepare = nav_prepare_vtbl;
    nav->refcount = 1;
    if (FAILED( CoCreateFreeThreadedMarshaler( (IUnknown *)&nav->vtbl_client, &nav->ftm ) ))
    {
        free( nav );
        return E_OUTOFMEMORY;
    }
    *out = (IUnknown *)&nav->vtbl_client;
    return S_OK;
}

/* Put the stand-in client on the window's WindowServer, the way CImmersiveWindowFactoryProxy
 * puts the real one there. */
static HRESULT attach_navigation_client( void *window )
{
    struct navigation_host_vtbl
    {
        void *unknown_inspectable[6];
        HRESULT (WINAPI *get_NavigationClient)( void *iface, IUnknown **client );
        HRESULT (WINAPI *put_NavigationClient)( void *iface, IUnknown *client );
    };
    struct navigation_host { const struct navigation_host_vtbl *vtbl; } *host;
    IUnknown *client;
    HRESULT hr;

    if (FAILED( hr = IUnknown_QueryInterface( (IUnknown *)window, &IID_ICoreWindowNavigation, (void **)&host ) ))
    {
        ERR( "CoreWindow has no navigation client property, hr %#lx\n", hr );
        return hr;
    }
    if (SUCCEEDED( hr = create_navigation_client( &client ) ))
    {
        hr = host->vtbl->put_NavigationClient( host, client );
        TRACE( "navigation client %p attached to %p, hr %#lx\n", client, window, hr );
        IUnknown_Release( client );
    }
    IUnknown_Release( (IUnknown *)host );
    return hr;
}

/* FrameworkView::SetWindow is DXamlCore::GetCurrent + ConfigureCoreWindow(core, window, TRUE)
 * (+ InvokeOnWindowCreated). Activate reaches CreateCoreWindow@View but never
 * SetWindowAndGetDispatcher, so wire the window into every thread that has a DXamlCore
 * (create thread + main UI thread where CreateContentVisual runs). */
static void try_configure_core_window( void *window )
{
    HMODULE xaml;
    void * (WINAPI *get_current)( void );
    HRESULT (WINAPI *configure)( void *core, void *core_window, boolean );
    HRESULT (WINAPI *invoke_created)( void *framework_app );
    void * (WINAPI *get_fa)( void );
    void *core, *fa;
    HRESULT hr;

    if (!window) return;
    xaml = GetModuleHandleW( L"Windows.UI.Xaml.dll" );
    if (!xaml) xaml = GetModuleHandleW( L"windows.ui.xaml.dll" );
    if (!xaml)
    {
        ERR( "Windows.UI.Xaml.dll not loaded; cannot ConfigureCoreWindow\n" );
        return;
    }
    get_current = (void *)((char *)xaml + 0x1134d4);      /* DXamlCore::GetCurrent */
    configure = (void *)((char *)xaml + 0x3bd0b4);        /* DXamlCore::ConfigureCoreWindow */
    get_fa = (void *)((char *)xaml + 0x19cb14);           /* FrameworkApplication::GetCurrentNoRef */
    invoke_created = (void *)((char *)xaml + 0x58f9f8);   /* FrameworkApplication::InvokeOnWindowCreated */

    core = get_current();
    if (!core)
    {
        TRACE( "DXamlCore::GetCurrent NULL on tid %lu\n", GetCurrentThreadId() );
        return;
    }

    hr = configure( core, window, TRUE );
    ERR( "ConfigureCoreWindow core %p window %p hr %#lx (tid %lu)\n",
         core, window, hr, GetCurrentThreadId() );
    if (FAILED( hr )) return;

    if (!(fa = get_fa()))
        WARN( "FrameworkApplication::GetCurrentNoRef NULL on tid %lu; skip InvokeOnWindowCreated\n",
              GetCurrentThreadId() );
    else
    {
        hr = invoke_created( fa );
        ERR( "InvokeOnWindowCreated fa %p hr %#lx (tid %lu)\n", fa, hr, GetCurrentThreadId() );
    }

    /* This is the view thread with a live DXamlCore and CoreWindow, i.e. the point the shell
     * would have reached SetWindow; ApplicationActivationFactory::Activate does not return
     * until the app exits, so the rest of the startup sequence has to happen from here. */
    drive_view_startup( NULL );
}

static void CALLBACK configure_core_window_apc( ULONG_PTR param )
{
    void *window = (void *)param;
    try_configure_core_window( window );
    IUnknown_Release( (IUnknown *)window );
}

static void publish_core_window_mailbox( void *window );
static void broadcast_configure_from_get_for_current_thread( void );

/* On the main UI thread a CoreWindow already exists (GetForCurrentThread) before
 * Activate creates another. Wire that one into DXamlCore so early CreateContentVisual
 * ticks see a real ICoreWindow. */
static void try_configure_from_get_for_current_thread( void )
{
    HSTRING_HEADER header;
    HSTRING name;
    struct slot_vtbl **statics = NULL;
    void *window = NULL;
    HRESULT hr;

    if (FAILED( hr = WindowsCreateStringReference( L"Windows.UI.Core.CoreWindow", 27, &header, &name ) ))
        return;
    if (FAILED( hr = RoGetActivationFactory( name, &IID_ICoreWindowStatic, (void **)&statics ) ) || !statics)
    {
        WARN( "no ICoreWindowStatic, hr %#lx\n", hr );
        return;
    }
    /* IInspectable slots 0-5, GetForCurrentThread is slot 6. */
    hr = (*statics)->slot[6]( statics, &window );
    IUnknown_Release( (IUnknown *)statics );
    if (FAILED( hr ) || !window)
    {
        TRACE( "GetForCurrentThread hr %#lx window %p on tid %lu\n", hr, window, GetCurrentThreadId() );
        return;
    }
    ERR( "GetForCurrentThread -> %p on tid %lu; configuring\n", window, GetCurrentThreadId() );
    publish_core_window_mailbox( window );
    try_configure_core_window( window );
    IUnknown_Release( (IUnknown *)window );
}

static void CALLBACK configure_from_gct_apc( ULONG_PTR param )
{
    (void)param;
    try_configure_from_get_for_current_thread();
}

static void broadcast_configure_from_get_for_current_thread( void )
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap, thr;
    THREADENTRY32 te;
    unsigned queued = 0;

    try_configure_from_get_for_current_thread();

    snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te ))
    {
        do
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            thr = open_app_thread( te.th32ThreadID, THREAD_SET_CONTEXT );
            if (!thr) continue;
            if (QueueUserAPC( configure_from_gct_apc, thr, 0 ))
            {
                queued++;
                TRACE( "queued GetForCurrentThread configure APC onto tid %lu\n", te.th32ThreadID );
            }
            CloseHandle( thr );
        } while (Thread32Next( snap, &te ));
    }
    CloseHandle( snap );
    ERR( "queued %u GetForCurrentThread configure APCs\n", queued );
}

/* XAML GetCurrentCoreWindowNoRef cave reads this BSS mailbox when DXamlCore's
 * CoreWindow slot is empty, ConfigureCoreWindow's it, and returns it. */
#define XAML_COREWINDOW_MAILBOX_RVA 0xfd3450

static void publish_core_window_mailbox( void *window )
{
    HMODULE xaml;
    void **mailbox;

    xaml = GetModuleHandleW( L"Windows.UI.Xaml.dll" );
    if (!xaml) xaml = GetModuleHandleW( L"windows.ui.xaml.dll" );
    if (!xaml || !window) return;
    mailbox = (void **)((char *)xaml + XAML_COREWINDOW_MAILBOX_RVA);
    if (*mailbox) IUnknown_Release( (IUnknown *)*mailbox );
    IUnknown_AddRef( (IUnknown *)window );
    *mailbox = window;
    ERR( "published CoreWindow %p to XAML mailbox\n", window );
}

/* Calling DXamlCore::ConfigureCoreWindow by hand stood in for a SetWindow that never arrived.
 * The activation now reaches CoreApplicationView::SetWindowAndGetDispatcher and XAML does it
 * itself, and doing it twice is a real failure: the second one comes back E_UNEXPECTED from
 * CJupiterWindow::ConfigureCoreWindow and XAML fail-fasts (Findings-M4.md). So this is off by
 * default now; WINE_UWP_MANUAL_CONFIGURE puts it back for comparison. */
static void broadcast_configure_core_window( void *window )
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap, thr;
    THREADENTRY32 te;
    unsigned queued = 0, skipped = 0;

    publish_core_window_mailbox( window );

    if (!GetEnvironmentVariableW( L"WINE_UWP_MANUAL_CONFIGURE", NULL, 0 ))
    {
        TRACE( "leaving ConfigureCoreWindow to FrameworkView::SetWindow\n" );
        return;
    }

    /* Creating thread first (has the new-view DXamlCore). */
    try_configure_core_window( window );

    snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te ))
    {
        do
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            thr = open_app_thread( te.th32ThreadID, THREAD_SET_CONTEXT );
            if (!thr) { skipped++; continue; }

            IUnknown_AddRef( (IUnknown *)window );
            if (QueueUserAPC( configure_core_window_apc, thr, (ULONG_PTR)window ))
            {
                queued++;
                TRACE( "queued ConfigureCoreWindow APC onto tid %lu\n", te.th32ThreadID );
            }
            else
                IUnknown_Release( (IUnknown *)window );
            CloseHandle( thr );
        } while (Thread32Next( snap, &te ));
    }
    CloseHandle( snap );
    ERR( "queued %u ConfigureCoreWindow APCs for other threads (%u Wine service threads skipped)\n",
         queued, skipped );
}

/* ---------------------------------------------------------------------------------------------
 * Application.LoadComponent("ms-appx:///App.xaml")
 *
 * Nothing in this app ever asks XAML to load a XAML document: LoadComponent, LoadXamlResource,
 * TryLoadXamlResourceHelper and GetBinaryResourceForXamlUri are all unused on a run that
 * activates, while Application.Resources is read exactly once - and comes back empty, so the
 * app's first Resources[...] lookup throws and OnLaunched fails (Findings-M4.md). App.xbf is in
 * the package's resources.pri, so load it here in the app's place and let the app find its
 * resources. IApplicationStatics {06499997}: slot 6 get_Current, 7 Start, 8 LoadComponent,
 * 9 LoadComponentWithResourceLocation.
 */
static HRESULT load_application_xaml( void )
{
    struct slot_vtbl **statics = NULL, **uri_factory = NULL, *app = NULL, *uri = NULL;
    HSTRING_HEADER header;
    HSTRING name, uri_string;
    HRESULT hr;

    if (app_xaml_loaded) return S_FALSE;
    if (!xaml_dxaml_core_current()) return S_FALSE;

    if (FAILED( hr = WindowsCreateStringReference( L"Windows.UI.Xaml.Application", 27, &header, &name ) ))
        return hr;
    if (FAILED( hr = RoGetActivationFactory( name, &IID_IApplicationStatics, (void **)&statics ) ))
    {
        WARN( "no IApplicationStatics, hr %#lx\n", hr );
        return hr;
    }
    hr = (*statics)->slot[6]( statics, &app );   /* get_Current */
    if (FAILED( hr ) || !app)
    {
        TRACE( "no Application on tid %lu, hr %#lx\n", GetCurrentThreadId(), hr );
        IUnknown_Release( (IUnknown *)statics );
        return FAILED(hr) ? hr : S_FALSE;
    }
    if (InterlockedCompareExchange( &app_xaml_loaded, 1, 0 ))
    {
        IUnknown_Release( (IUnknown *)app );
        IUnknown_Release( (IUnknown *)statics );
        return S_FALSE;
    }

    {
        HSTRING_HEADER uri_header;
        if (SUCCEEDED( hr = WindowsCreateStringReference( L"Windows.Foundation.Uri", 22, &uri_header, &name ) ) &&
            SUCCEEDED( hr = RoGetActivationFactory( name, &IID_IUriRuntimeClassFactory, (void **)&uri_factory ) ))
        {
            HSTRING_HEADER value_header;
            if (SUCCEEDED( hr = WindowsCreateStringReference( L"ms-appx:///App.xaml", 19, &value_header, &uri_string ) ))
                hr = (*uri_factory)->slot[6]( uri_factory, uri_string, &uri );   /* CreateUri */
            IUnknown_Release( (IUnknown *)uri_factory );
        }
    }
    if (SUCCEEDED( hr ) && uri)
    {
        /* ComponentResourceLocation_Application == 0 */
        hr = (*statics)->slot[9]( statics, app, uri, 0 );
        ERR( "LoadComponent(ms-appx:///App.xaml) on tid %lu, hr %#lx\n", GetCurrentThreadId(), hr );
        IUnknown_Release( (IUnknown *)uri );
    }
    else ERR( "could not make the App.xaml uri, hr %#lx\n", hr );

    IUnknown_Release( (IUnknown *)app );
    IUnknown_Release( (IUnknown *)statics );
    return hr;
}

static void CALLBACK load_app_xaml_apc( ULONG_PTR param )
{
    load_application_xaml();
}

/* XAML objects belong to the apartment that made them, so offer this to every thread and let the
 * one holding the Application take it. */
static void broadcast_load_app_xaml( void )
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 te;
    HANDLE snap, thr;

    if (GetEnvironmentVariableW( L"WINE_UWP_NO_APP_XAML", NULL, 0 )) return;
    if (SUCCEEDED( load_application_xaml() ) && app_xaml_loaded) return;
    if ((snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 )) == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te )) do
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (!(thr = open_app_thread( te.th32ThreadID, THREAD_SET_CONTEXT ))) continue;
        QueueUserAPC( load_app_xaml_apc, thr, 0 );
        CloseHandle( thr );
    } while (Thread32Next( snap, &te ));
    CloseHandle( snap );
}

/* The activation's calls into the view (CreateCoreWindow, SetWindowAndGetDispatcher, Load,
 * Activate) are marshalled whenever it does not run on the view thread itself, and Wine delivers
 * a cross-apartment call to an STA by posting to the apartment window. The view thread services
 * the first two and then stops pumping, so Load never arrives (Findings-M4.md). Queue an APC to
 * ourselves here - we are on the view thread, inside CreateCoreWindow - so that the next time
 * this thread waits alertably it drains its message queue for a while and the rest of the
 * activation gets delivered. */
static DWORD view_pump_deadline;

/* WINE_UWP_DUMP_STACKS=<seconds>: suspend every thread in the process after that many seconds and
 * unwind it, printing "STACK tid N frame M rip=..." lines. winedbg does not run in this build and
 * gdb cannot attach, so this is the only way to see what an app's threads are actually doing - and
 * it works for an *idle* app, not just a deadlocked one, which is where it earned its keep:
 * comparing Solitaire's threads against Media Player's is what showed the difference was missing
 * engine worker threads rather than anything on the main thread.
 *
 * Resolve the addresses against /proc/<pid>/maps captured from the SAME process (a map dies with
 * its run), and read the result against a working app before concluding anything - the main
 * thread's Start/RunInternal/TearDown/WaitForExit chain looks alarming and is completely normal. */
static void dump_one_thread_stack( HANDLE thr, DWORD tid )
{
    CONTEXT ctx;
    unsigned int i;

    if (SuspendThread( thr ) == (DWORD)-1) return;
    memset( &ctx, 0, sizeof(ctx) );
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext( thr, &ctx ))
    {
        __TRY
        {
            CONTEXT c = ctx;
            for (i = 0; i < 40; i++)
            {
                RUNTIME_FUNCTION *func;
                ULONG64 base = 0, frame = 0;
                void *handler_data;

                ERR( "STACK tid %lu frame %2u rip=%p\n", tid, i, (void *)c.Rip );
                if (!c.Rip) break;
                if (!(func = RtlLookupFunctionEntry( c.Rip, &base, NULL )))
                {
                    c.Rip = *(ULONG64 *)c.Rsp;
                    c.Rsp += 8;
                    continue;
                }
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, base, c.Rip, func, &c, &handler_data, &frame, NULL );
            }
        }
        __EXCEPT_ALL
        {
            ERR( "STACK tid %lu unwind faulted\n", tid );
        }
        __ENDTRY
    }
    ResumeThread( thr );
}

static DWORD WINAPI dump_stacks_thread( void *arg )
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId(), delay = (DWORD)(ULONG_PTR)arg;
    THREADENTRY32 te;
    HANDLE snap, thr;

    Sleep( delay * 1000 );
    ERR( "STACK ==== dumping every thread after %lu s ====\n", delay );
    snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if (snap == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te )) do
    {
        WCHAR *desc = NULL;

        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (!(thr = OpenThread( THREAD_ALL_ACCESS, FALSE, te.th32ThreadID ))) continue;
        if (SUCCEEDED( GetThreadDescription( thr, &desc ) ) && desc)
        {
            ERR( "STACK tid %lu name %s\n", te.th32ThreadID, debugstr_w( desc ) );
            LocalFree( desc );
        }
        dump_one_thread_stack( thr, te.th32ThreadID );
        CloseHandle( thr );
    } while (Thread32Next( snap, &te ));
    CloseHandle( snap );
    ERR( "STACK ==== end ====\n" );
    return 0;
}

static void maybe_start_stack_dumper(void)
{
    WCHAR buf[16];
    DWORD secs;
    HANDLE h;

    if (!GetEnvironmentVariableW( L"WINE_UWP_DUMP_STACKS", buf, ARRAY_SIZE(buf) )) return;
    if (!(secs = wcstoul( buf, NULL, 10 ))) return;
    if ((h = CreateThread( NULL, 0, dump_stacks_thread, (void *)(ULONG_PTR)secs, 0, NULL )))
        CloseHandle( h );
}

static void CALLBACK view_pump_apc( ULONG_PTR param )
{
    MSG msg;
    unsigned int dispatched = 0;
    DWORD last_dispatch = GetTickCount();

    ERR( "pumping the view thread %lu for %lu ms\n", GetCurrentThreadId(),
         view_pump_deadline - GetTickCount() );
    while ((int)(view_pump_deadline - GetTickCount()) > 0)
    {
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
            dispatched++;
            last_dispatch = GetTickCount();
        }
        /* The queue going quiet is the sign the view thread has caught up; the full deadline is only
         * a safety net (it used to be five seconds of every start). */
        if (GetTickCount() - last_dispatch > 500)
        {
            ERR( "view thread %lu: queue idle, ending the pump early\n", GetCurrentThreadId() );
            break;
        }
        /* The pump covers the time until the app has activated its window and runs its own
         * dispatcher loop; once Window.Activate() has been seen, a short grace period is enough. */
        if (view_activated_tick && GetTickCount() - view_activated_tick > 300)
        {
            ERR( "view thread %lu: window activated, ending the pump early\n", GetCurrentThreadId() );
            break;
        }
        MsgWaitForMultipleObjectsEx( 0, NULL, 20, QS_ALLINPUT, MWMO_ALERTABLE | MWMO_INPUTAVAILABLE );
    }
    ERR( "view thread %lu dispatched %u messages\n", GetCurrentThreadId(), dispatched );
}

/* Load and Activate are the rest of the startup sequence, and on Windows they arrive on the view
 * thread. Here the container stops after SetWindowAndGetDispatcher, so drive them from the view
 * thread ourselves - queued behind the pump above, which is what delivers SetWindowAndGetDispatcher,
 * because APCs run in the order they were queued. Doing it anywhere else gets a marshalled proxy of
 * the view and, worse, used to consume the one-shot so the view thread never got its turn: the app
 * then started on whichever thread the activation landed on and finished on this one, with its
 * DependencyObjects split across two DXamlCores. */
static void CALLBACK drive_view_startup_apc( ULONG_PTR param )
{
    HRESULT hr = drive_view_startup( NULL );
    if (hr != S_FALSE) ERR( "view startup on tid %lu, hr %#lx\n", GetCurrentThreadId(), hr );
}

static void drive_view_startup_later( void )
{
    WCHAR mode[16];

    if (!GetEnvironmentVariableW( L"WINE_UWP_DRIVE_VIEW", mode, ARRAY_SIZE(mode) )) return;
    if (!QueueUserAPC( drive_view_startup_apc, GetCurrentThread(), 0 ))
        ERR( "could not queue the view startup, err %lu\n", GetLastError() );
}

/* WINE_UWP_REPORT_VIEWS: read-only census. Each thread reports whether CoreApplication hands it a
 * real view (vtable in twinapi) or a proxy, and which view object, so the number of distinct
 * CoreApplicationViews in the process and the threads they belong to can be counted. */
static void CALLBACK report_view_apc( ULONG_PTR param )
{
    struct slot_vtbl **view = NULL;
    HMODULE twinapi = GetModuleHandleW( L"twinapi.appcore.dll" );
    HRESULT hr = get_current_core_application_view( &view );
    const void *vt = (view && !IsBadReadPtr( view, sizeof(void *) )) ? *(const void **)view : NULL;
    BOOL real = FALSE;

    if (vt && twinapi)
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)twinapi;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)twinapi + dos->e_lfanew);
        real = (ULONG_PTR)vt - (ULONG_PTR)twinapi < nt->OptionalHeader.SizeOfImage;
    }
    ERR( "VIEWCENSUS tid %lu hr %#lx view %p vtbl %p %s core %p\n", GetCurrentThreadId(), hr, view, vt,
         real ? "REAL" : "proxy", xaml_dxaml_core_current() );
    if (view) IUnknown_Release( (IUnknown *)view );
}

static void report_views( void )
{
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    HANDLE snap, thr;

    if (!GetEnvironmentVariableW( L"WINE_UWP_REPORT_VIEWS", NULL, 0 )) return;
    if ((snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 )) == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te )) do
    {
        if (te.th32OwnerProcessID != pid) continue;
        if (!(thr = open_app_thread( te.th32ThreadID, THREAD_SET_CONTEXT ))) continue;
        QueueUserAPC( report_view_apc, thr, 0 );
        CloseHandle( thr );
    } while (Thread32Next( snap, &te ));
    CloseHandle( snap );
}

/* Offer the startup to every thread; only one that owns a real view can take it (elsewhere
 * GetCurrentView returns a proxy and drive_view_startup leaves the one-shot unclaimed). */
static unsigned queue_drive_view_apcs( void )
{
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    unsigned queued = 0;
    HANDLE snap, thr;

    if (!drive_first_view()) return 0;
    if ((snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 )) == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof(te);
    if (Thread32First( snap, &te )) do
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == GetCurrentThreadId()) continue;
        if (!(thr = open_app_thread( te.th32ThreadID, THREAD_SET_CONTEXT ))) continue;
        if (QueueUserAPC( drive_view_startup_apc, thr, 0 )) queued++;
        CloseHandle( thr );
    } while (Thread32Next( snap, &te ));
    CloseHandle( snap );
    return queued;
}

static void pump_view_thread_later( DWORD ms )
{
    if (GetEnvironmentVariableW( L"WINE_UWP_NO_VIEW_PUMP", NULL, 0 )) return;
    view_pump_deadline = GetTickCount() + ms;
    if (!QueueUserAPC( view_pump_apc, GetCurrentThread(), 0 ))
        ERR( "could not queue the view pump, err %lu\n", GetLastError() );
}

/* Tried and rejected: queueing the same pump to every other thread as the activation starts, so
 * the view thread would be pumping for the calls that come before CreateCoreWindow. It reaches
 * threads that must not pump, and no run got a window at all. */

static HRESULT WINAPI wf_CreateCoreWindow( struct window_factory *wf, HSTRING title, void **window )
{
    static PrivateCreateCoreWindow_t pcreate;
    const WCHAR *str;
    UINT32 len = 0;
    HRESULT hr;

    ERR( "CreateCoreWindow %s on thread %lu\n", debugstr_hstring( title ), GetCurrentThreadId() );
    if (!window) return E_POINTER;
    *window = NULL;
    if (!pcreate)
    {
        HMODULE ui = LoadLibraryW( L"windows.ui.dll" );
        if (!ui) return HRESULT_FROM_WIN32( GetLastError() );
        pcreate = (PrivateCreateCoreWindow_t)GetProcAddress( ui, (const char *)(ULONG_PTR)1500 );
        if (!pcreate) return E_FAIL;
    }
    str = WindowsGetStringRawBuffer( title, &len );
    if (!str || !len) str = L"App";
    /* WINDOW_TYPE 1 = main UAP window (PrivateCreateCoreWindow switch). */
    hr = pcreate( 1, str, 0, 0, CORE_WINDOW_WIDTH, CORE_WINDOW_HEIGHT, 0, NULL, &IID_ICoreWindow, window );
    if (FAILED( hr ))
    {
        ERR( "PrivateCreateCoreWindow failed, hr %#lx\n", hr );
        return hr;
    }
    TRACE( "PrivateCreateCoreWindow -> %p hr %#lx\n", *window, hr );
    {
        HWND core_window = NULL;
        EnumWindows( show_core_window, (LPARAM)&core_window );
        connect_dcomp_target( core_window );
        publish_ui_thread_to_coremessaging();
    }
    if (FAILED( hr = attach_navigation_client( *window ) ))
    {
        ERR( "could not attach a navigation client, hr %#lx\n", hr );
        IUnknown_Release( (IUnknown *)*window );
        *window = NULL;
        return hr;
    }
    broadcast_configure_core_window( *window );
    broadcast_load_app_xaml();
    pump_view_thread_later( 5000 );
    drive_view_startup_later();
    report_views();
    if (drive_first_view()) ERR( "queued %u first-view startup APCs\n", queue_drive_view_apcs() );
    return hr;
}

static HRESULT WINAPI wf_get_WindowReuseAllowed( struct window_factory *wf, boolean *value )
{
    TRACE( "%p\n", value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI wf_QueryService( struct window_factory *wf, const GUID *service, const GUID *iid, void **out )
{
    /* the private service GetWindowFactory asks for, and then calls slot 5 on */
    static const GUID SID_window_factory = { 0xd98fd14a, 0x522a, 0x4d59, { 0xb8, 0x75, 0x81, 0x1e, 0x83, 0x91, 0x9a, 0x9e } };

    TRACE( "service %s iid %s\n", debugstr_guid( service ), debugstr_guid( iid ) );
    if (service && IsEqualGUID( service, &SID_window_factory )) return wf_QueryInterface( wf, &IID_IUnknown, out );
    /* Anything else is a broker service this process does not have, e.g. the share UI's
     * IApplicationInvocableFlow {f230b678}: handing back the window factory made twinapi.appcore
     * treat it as one and fault. */
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI wf_CreateFromCookie( struct window_factory *wf, UINT64 cookie, void *opts, void **out )
{
    TRACE( "cookie %#I64x opts %p\n", cookie, opts );
    return wf_QueryInterface( wf, &IID_IUnknown, out );
}

static HRESULT WINAPI wf_QueryInterface( struct window_factory *wf, REFIID iid, void **out )
{
    TRACE( "%s\n", debugstr_guid( iid ) );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_ICoreWindowBroker ))
        *out = &wf->vtbl_broker;
    else if (IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_ICoreWindowFactory ))
        *out = &wf->vtbl_factory;
    else if (IsEqualGUID( iid, &IID_IConfigureWindowFactory ))
        *out = &wf->vtbl_configure;
    /* The window factory has no thread affinity of its own and has to make the CoreWindow on the
     * calling (view) thread, so it must never be proxied into another apartment. */
    else if (IsEqualGUID( iid, &IID_IMarshal ) || IsEqualGUID( iid, &IID_IAgileObject ))
        return IUnknown_QueryInterface( wf->ftm, iid, out );
    else
    {
        FIXME( "unsupported %s\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    wf_AddRef( wf );
    return S_OK;
}

static const void *wf_broker_vtbl[] = {
    wf_QueryInterface, wf_AddRef, wf_Release,
    wf_QueryService, wf_GetIids, wf_CreateFromCookie
};
static HRESULT WINAPI fac_QueryInterface( void *iface, REFIID iid, void **out )
{
    return wf_QueryInterface( factory_from_factory( iface ), iid, out );
}
static ULONG WINAPI fac_AddRef( void *iface )
{
    return wf_AddRef( factory_from_factory( iface ) );
}
static ULONG WINAPI fac_Release( void *iface )
{
    return wf_Release( factory_from_factory( iface ) );
}
static HRESULT WINAPI fac_GetIids( void *iface, ULONG *count, IID **iids )
{
    return wf_GetIids( factory_from_factory( iface ), count, iids );
}
static HRESULT WINAPI fac_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return wf_GetRuntimeClassName( factory_from_factory( iface ), name );
}
static HRESULT WINAPI fac_GetTrustLevel( void *iface, TrustLevel *level )
{
    return wf_GetTrustLevel( factory_from_factory( iface ), level );
}
static HRESULT WINAPI fac_CreateCoreWindow( void *iface, HSTRING title, void **window )
{
    return wf_CreateCoreWindow( factory_from_factory( iface ), title, window );
}
static HRESULT WINAPI fac_get_WindowReuseAllowed( void *iface, boolean *value )
{
    return wf_get_WindowReuseAllowed( factory_from_factory( iface ), value );
}
static const void *wf_factory_vtbl[] = {
    fac_QueryInterface, fac_AddRef, fac_Release,
    fac_GetIids, fac_GetRuntimeClassName, fac_GetTrustLevel,
    fac_CreateCoreWindow, fac_get_WindowReuseAllowed
};

static HRESULT WINAPI cfg_QueryInterface( void *iface, REFIID iid, void **out )
{
    return wf_QueryInterface( factory_from_configure( iface ), iid, out );
}
static ULONG WINAPI cfg_AddRef( void *iface )
{
    return wf_AddRef( factory_from_configure( iface ) );
}
static ULONG WINAPI cfg_Release( void *iface )
{
    return wf_Release( factory_from_configure( iface ) );
}
static HRESULT WINAPI cfg_Initialize( void *iface, void *monitor, const WCHAR *title )
{
    TRACE( "monitor %p title %p\n", monitor, title );
    return S_OK;
}
static HRESULT WINAPI cfg_InitializeWithPosition( void *iface, void *monitor, const WCHAR *title, RECT *rect )
{
    TRACE( "monitor %p title %p rect %p\n", monitor, title, rect );
    return S_OK;
}
static HRESULT WINAPI cfg_CreateSplashScreen( void *iface, DWORD flags, void **splash )
{
    TRACE( "flags %#lx\n", flags );
    /* Activate QIs ISplashScreen on the launch args; a NULL here is fine if that QI succeeds. */
    if (splash) *splash = NULL;
    return S_OK;
}
static HRESULT WINAPI cfg_GetAppWindow( void *iface, HWND *hwnd )
{
    TRACE( "\n" );
    if (hwnd) *hwnd = NULL;
    return S_OK;
}
static HRESULT WINAPI cfg_HasExistingSplashScreen( void *iface, BOOL *has )
{
    if (has) *has = FALSE;
    return S_OK;
}
static HRESULT WINAPI cfg_SetViewId( void *iface, ULONG id )
{
    TRACE( "id %lu\n", id );
    return S_OK;
}
static const void *wf_configure_vtbl[] = {
    cfg_QueryInterface, cfg_AddRef, cfg_Release,
    cfg_Initialize, cfg_InitializeWithPosition, cfg_CreateSplashScreen,
    cfg_GetAppWindow, cfg_HasExistingSplashScreen, cfg_SetViewId
};

static HRESULT WINAPI wffactory_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID iid, void **out )
{
    struct window_factory *wf;
    HRESULT hr;

    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(wf = calloc( 1, sizeof(*wf) ))) return E_OUTOFMEMORY;
    wf->vtbl_broker = wf_broker_vtbl;
    wf->vtbl_factory = wf_factory_vtbl;
    wf->vtbl_configure = wf_configure_vtbl;
    wf->refcount = 1;
    if (FAILED(hr = CoCreateFreeThreadedMarshaler( (IUnknown *)&wf->vtbl_broker, &wf->ftm )))
    {
        free( wf );
        return hr;
    }
    hr = wf_QueryInterface( wf, iid, out );
    wf_Release( wf );
    return hr;
}
static const IClassFactoryVtbl wffactory_vtbl = { factory_QueryInterface, factory_AddRef, factory_Release,
    wffactory_CreateInstance, factory_LockServer };
static IClassFactory window_factory_factory = { &wffactory_vtbl };

struct psm_broker
{
    const void *vtbl;
    LONG refcount;
};

static HRESULT WINAPI psm_QueryInterface( struct psm_broker *psm, REFIID iid, void **out )
{
    TRACE( "%s\n", debugstr_guid( iid ) );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IPsmBroker ))
    {
        *out = psm;
        InterlockedIncrement( &psm->refcount );
        return S_OK;
    }
    FIXME( "psm unsupported %s\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI psm_AddRef( struct psm_broker *psm ) { return InterlockedIncrement( &psm->refcount ); }
static ULONG WINAPI psm_Release( struct psm_broker *psm )
{
    ULONG ref = InterlockedDecrement( &psm->refcount );
    if (!ref) free( psm );
    return ref;
}
static HRESULT WINAPI psm_stub( struct psm_broker *psm )
{
    FIXME( "psm broker method stub\n" );
    return S_OK;
}
static const void *psm_vtbl[] = {
    psm_QueryInterface, psm_AddRef, psm_Release,
    psm_stub, psm_stub, psm_stub, psm_stub, psm_stub, psm_stub, psm_stub, psm_stub
};

static HRESULT WINAPI psmfactory_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID iid, void **out )
{
    struct psm_broker *psm;
    HRESULT hr;

    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(psm = calloc( 1, sizeof(*psm) ))) return E_OUTOFMEMORY;
    psm->vtbl = psm_vtbl;
    psm->refcount = 1;
    hr = psm_QueryInterface( psm, iid, out );
    psm_Release( psm );
    return hr;
}
static const IClassFactoryVtbl psmfactory_vtbl = { factory_QueryInterface, factory_AddRef, factory_Release,
    psmfactory_CreateInstance, factory_LockServer };
static IClassFactory psm_broker_factory = { &psmfactory_vtbl };

/* WINE_UWP_PROFILE=<file prefix>: a sampling profiler for performance work. A thread suspends the other
 * threads every WINE_UWP_PROFILE_MS (default 5) milliseconds and counts their instruction pointers; every two
 * seconds <prefix>.<pid> is rewritten with the module list and the counts. Threads blocked in a system call
 * report the call's user-mode caller. */
#define PROFILE_BUCKETS 65536
#define PROFILE_CALLERS 4
static struct { DWORD64 rip, caller[PROFILE_CALLERS]; DWORD tid; DWORD count; } profile_buckets[PROFILE_BUCKETS];

static struct { DWORD64 start, end; } profile_images[512];
static unsigned int profile_image_count;

static void profile_refresh_images( void )
{
    HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE, 0 );
    MODULEENTRY32W me = { sizeof(me) };
    unsigned int n = 0;
    if (snap == INVALID_HANDLE_VALUE) return;
    if (Module32FirstW( snap, &me ))
        do
        {
            profile_images[n].start = (DWORD64)(ULONG_PTR)me.modBaseAddr;
            profile_images[n].end = profile_images[n].start + me.modBaseSize;
            n++;
        } while (n < ARRAY_SIZE(profile_images) && Module32NextW( snap, &me ));
    CloseHandle( snap );
    profile_image_count = n;
}

/* The words on the stack that return right after a call instruction inside a loaded image: the leaf's
 * callers, usually - enough of them to read a hang, where one frame rarely says who is waiting. */
static void profile_callers( DWORD64 rsp, DWORD64 *out )
{
    unsigned int found = 0;
    unsigned int i, j;
    __TRY
    {
        for (i = 0; i < 256 && found < PROFILE_CALLERS; i++)
        {
            const BYTE *r = (const BYTE *)(ULONG_PTR)((DWORD64 *)(ULONG_PTR)rsp)[i];
            for (j = 0; j < profile_image_count; j++)
                if ((DWORD64)(ULONG_PTR)r >= profile_images[j].start + 0x1000 && (DWORD64)(ULONG_PTR)r < profile_images[j].end) break;
            if (j == profile_image_count) continue;
            if (r[-5] == 0xe8 || (r[-6] == 0xff && r[-5] == 0x15) || (r[-2] == 0xff && (r[-1] & 0x38) == 0x10) ||
                (r[-3] == 0xff && (r[-2] & 0x38) == 0x10))
            {
                /* a return address repeated down the stack is the same frame seen twice */
                for (j = 0; j < found; j++) if (out[j] == (DWORD64)(ULONG_PTR)r) break;
                if (j == found) out[found++] = (DWORD64)(ULONG_PTR)r;
            }
        }
    }
    __EXCEPT_ALL { }
    __ENDTRY
}

static void profile_write( const char *path )
{
    HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE, 0 );
    MODULEENTRY32W me = { sizeof(me) };
    FILE *f;
    unsigned int i;

    if (!(f = fopen( path, "w" ))) { if (snap != INVALID_HANDLE_VALUE) CloseHandle( snap ); return; }
    if (snap != INVALID_HANDLE_VALUE && Module32FirstW( snap, &me ))
    {
        do fprintf( f, "M %p %lx %ls\n", me.modBaseAddr, me.modBaseSize, me.szExePath );
        while (Module32NextW( snap, &me ));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle( snap );
    for (i = 0; i < PROFILE_BUCKETS; i++)
        if (profile_buckets[i].count)
            fprintf( f, "S %lx %I64x %lu %I64x %I64x %I64x %I64x\n", profile_buckets[i].tid, profile_buckets[i].rip,
                     profile_buckets[i].count, profile_buckets[i].caller[0], profile_buckets[i].caller[1],
                     profile_buckets[i].caller[2], profile_buckets[i].caller[3] );
    fclose( f );
}

static DWORD WINAPI profile_thread( void *arg )
{
    const char *prefix = arg;
    char path[MAX_PATH];
    struct { DWORD tid; HANDLE handle; } threads[256];
    unsigned int count = 0, i, tick = 0;
    DWORD self = GetCurrentThreadId(), interval = 5;
    const char *ms = getenv( "WINE_UWP_PROFILE_MS" );
    CONTEXT ctx;

    if (ms && atoi( ms ) > 0) interval = atoi( ms );
    snprintf( path, sizeof(path), "%s.%lu", prefix, GetCurrentProcessId() );
    for (;;)
    {
        if (!(tick % (200 / interval + 1)))
        {
            HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
            THREADENTRY32 te = { sizeof(te) };
            for (i = 0; i < count; i++) CloseHandle( threads[i].handle );
            count = 0;
            if (snap != INVALID_HANDLE_VALUE && Thread32First( snap, &te ))
            {
                do
                {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self || count == ARRAY_SIZE(threads))
                        continue;
                    if ((threads[count].handle = OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID )))
                        threads[count++].tid = te.th32ThreadID;
                } while (Thread32Next( snap, &te ));
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle( snap );
            if (!(tick % (1000 / interval + 1))) profile_refresh_images();
        }
        for (i = 0; i < count; i++)
        {
            DWORD64 rip = 0, caller[PROFILE_CALLERS] = { 0 };
            unsigned int slot, probe, k;
            if (SuspendThread( threads[i].handle ) == (DWORD)-1) continue;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext( threads[i].handle, &ctx ))
            {
                rip = ctx.Rip;
                profile_callers( ctx.Rsp, caller );
            }
            ResumeThread( threads[i].handle );
            if (!rip) continue;
            slot = (unsigned int)((rip * 0x9e3779b97f4a7c15ull ^ caller[0] ^ caller[1] ^ threads[i].tid) >> 40) & (PROFILE_BUCKETS - 1);
            for (probe = 0; probe < 64; probe++, slot = (slot + 1) & (PROFILE_BUCKETS - 1))
            {
                if (!profile_buckets[slot].count)
                {
                    profile_buckets[slot].rip = rip;
                    for (k = 0; k < PROFILE_CALLERS; k++) profile_buckets[slot].caller[k] = caller[k];
                    profile_buckets[slot].tid = threads[i].tid;
                }
                if (profile_buckets[slot].rip == rip && profile_buckets[slot].tid == threads[i].tid)
                {
                    for (k = 0; k < PROFILE_CALLERS; k++) if (profile_buckets[slot].caller[k] != caller[k]) break;
                    if (k < PROFILE_CALLERS) continue;
                    profile_buckets[slot].count++;
                    break;
                }
            }
        }
        if (!(tick % (100 / interval + 1)))
        {
            /* <prefix>.reset: start counting afresh (isolate one interaction) */
            char reset[MAX_PATH];
            snprintf( reset, sizeof(reset), "%s.reset", prefix );
            if (GetFileAttributesA( reset ) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileA( reset );
                memset( profile_buckets, 0, sizeof(profile_buckets) );
            }
            snprintf( reset, sizeof(reset), "%s.dump", prefix );
            if (GetFileAttributesA( reset ) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileA( reset );
                profile_write( path );
            }
        }
        if (!(++tick % (2000 / interval))) profile_write( path );
        Sleep( interval );
    }
    return 0;
}

/* Temporary read-only exception diagnostic. */
static LONG CALLBACK resume_exception_trace( EXCEPTION_POINTERS *info )
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT c = *info->ContextRecord;
    unsigned int i;
    if (rec->ExceptionCode != 0xe06d7363 || rec->NumberParameters < 4 ||
        rec->ExceptionInformation[3] != (ULONG_PTR)GetModuleHandleW(NULL)) return EXCEPTION_CONTINUE_SEARCH;
    __TRY
    {
        const DWORD *obj = (void *)rec->ExceptionInformation[1];
        if (obj) ERR("[DEBUG-resume] CXX object %08lx %08lx %08lx %08lx %08lx %08lx\n",
                     obj[0], obj[1], obj[2], obj[3], obj[4], obj[5]);
        for (i = 0; i < 28 && c.Rip; i++)
        {
            RUNTIME_FUNCTION *func;
            ULONG64 base = 0, frame = 0;
            void *handler_data;
            ERR("[DEBUG-resume] frame %u rip %p\n", i, (void *)c.Rip);
            if (!(func = RtlLookupFunctionEntry(c.Rip, &base, NULL)))
            {
                c.Rip = *(ULONG64 *)c.Rsp;
                c.Rsp += 8;
            }
            else RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, func, &c, &handler_data, &frame, NULL);
        }
    }
    __EXCEPT_ALL { ERR("[DEBUG-resume] unwind fault\n"); }
    __ENDTRY
    return EXCEPTION_CONTINUE_SEARCH;
}

/* WINE_UWP_BP=module+rva[,module+rva...]: non-fatal code probes. Each address gets an int3; a vectored
 * handler (which runs before any of the target's own handlers, so nothing can swallow it) logs the hit
 * with its argument registers and return address, puts the original byte back, single-steps over it
 * and re-arms it. Unlike a ud2 probe the run carries on, so one run answers several questions.
 * WINE_UWP_BP_MAX caps the lines per breakpoint (default 20). Hits on other threads while one is being
 * stepped over can be missed; counts are therefore lower bounds. */
struct code_bp
{
    char module[64];
    ULONG_PTR rva;
    BYTE *addr;
    BYTE orig;
    LONG hits;
};
static struct code_bp code_bps[32];
static unsigned int code_bp_count;
static LONG code_bp_max = 20;
static DWORD code_bp_step_tls = TLS_OUT_OF_INDEXES;

static void code_bp_write( BYTE *addr, BYTE value )
{
    DWORD old;
    if (!VirtualProtect( addr, 1, PAGE_EXECUTE_READWRITE, &old )) return;
    *addr = value;
    VirtualProtect( addr, 1, old, &old );
    FlushInstructionCache( GetCurrentProcess(), addr, 1 );
}

static void code_bp_describe( ULONG_PTR addr, char *buf, size_t size )
{
    HMODULE mod;
    WCHAR path[MAX_PATH], *name;

    if (GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (const WCHAR *)addr, &mod ) && GetModuleFileNameW( mod, path, MAX_PATH ))
    {
        name = wcsrchr( path, '\\' );
        snprintf( buf, size, "%ls+%#Ix", name ? name + 1 : path, addr - (ULONG_PTR)mod );
    }
    else snprintf( buf, size, "%#Ix", addr );
}

static LONG CALLBACK code_bp_handler( EXCEPTION_POINTERS *info )
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT *ctx = info->ContextRecord;
    unsigned int i;

    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT)
    {
        for (i = 0; i < code_bp_count; i++)
        {
            struct code_bp *bp = &code_bps[i];
            char ret[128];
            LONG n;

            if (!bp->addr || (BYTE *)rec->ExceptionAddress != bp->addr) continue;
            n = InterlockedIncrement( &bp->hits );
            if (n <= code_bp_max)
            {
                code_bp_describe( *(ULONG_PTR *)ctx->Rsp, ret, sizeof(ret) );
                ERR( "BP %s+%#Ix hit %ld tid %04lx rcx %Ix rdx %Ix r8 %Ix r9 %Ix rax %Ix ret %s\n", bp->module,
                     bp->rva, n, GetCurrentThreadId(), ctx->Rcx, ctx->Rdx, ctx->R8, ctx->R9, ctx->Rax, ret );
            }
            code_bp_write( bp->addr, bp->orig );
            ctx->Rip = (ULONG_PTR)bp->addr;
            ctx->EFlags |= 0x100;   /* trap after the original instruction, then re-arm */
            TlsSetValue( code_bp_step_tls, (void *)(ULONG_PTR)(i + 1) );
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    else if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && code_bp_step_tls != TLS_OUT_OF_INDEXES)
    {
        ULONG_PTR index = (ULONG_PTR)TlsGetValue( code_bp_step_tls );
        if (index)
        {
            TlsSetValue( code_bp_step_tls, NULL );
            code_bp_write( code_bps[index - 1].addr, 0xcc );
            ctx->EFlags &= ~0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The target modules may load after us: poll until each is present, for up to two minutes. */
static DWORD WINAPI code_bp_thread( void *arg )
{
    unsigned int armed = 0, i, tries;
    const char *max = getenv( "WINE_UWP_BP_MAX" );

    if (max) code_bp_max = atol( max );
    for (tries = 0; tries < 2400 && armed < code_bp_count; tries++)
    {
        for (i = 0; i < code_bp_count; i++)
        {
            struct code_bp *bp = &code_bps[i];
            HMODULE mod;

            if (bp->addr || !(mod = GetModuleHandleA( bp->module ))) continue;
            bp->addr = (BYTE *)mod + bp->rva;
            bp->orig = *bp->addr;
            code_bp_write( bp->addr, 0xcc );
            ERR( "BP armed %s+%#Ix at %p (byte %02x)\n", bp->module, bp->rva, bp->addr, bp->orig );
            armed++;
        }
        if (armed < code_bp_count) Sleep( 50 );
    }
    return 0;
}

static void maybe_start_code_bps(void)
{
    char env[1024], *p, *item;
    HANDLE thread;

    if (!GetEnvironmentVariableA( "WINE_UWP_BP", env, sizeof(env) )) return;
    for (item = strtok_s( env, ",", &p ); item && code_bp_count < ARRAY_SIZE(code_bps); item = strtok_s( NULL, ",", &p ))
    {
        char *plus = strrchr( item, '+' );
        struct code_bp *bp = &code_bps[code_bp_count];
        if (!plus) continue;
        *plus = 0;
        lstrcpynA( bp->module, item, sizeof(bp->module) );
        bp->rva = strtoull( plus + 1, NULL, 16 );
        code_bp_count++;
    }
    if (!code_bp_count) return;
    code_bp_step_tls = TlsAlloc();
    AddVectoredExceptionHandler( 1, code_bp_handler );
    if ((thread = CreateThread( NULL, 0, code_bp_thread, NULL, 0, NULL ))) CloseHandle( thread );
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    static LONG resume_trace;
    if (GetEnvironmentVariableW(L"WINE_UWP_TRACE_CXX", NULL, 0) && !InterlockedExchange(&resume_trace, 1))
        AddVectoredExceptionHandler(1, resume_exception_trace);
    static LONG profiling, dumping;
    const char *profile = getenv( "WINE_UWP_PROFILE" );
    if (profile && !InterlockedExchange( &profiling, 1 ))
    {
        HANDLE thread = CreateThread( NULL, 0, profile_thread, (void *)profile, 0, NULL );
        if (thread) CloseHandle( thread );
    }
    /* also arm the stack dumper here: a run that never reaches ActivationHandled is exactly
     * the one worth dumping, and that hook fires too late to see it. */
    if (!InterlockedExchange( &dumping, 1 ))
    {
        maybe_start_stack_dumper();
        maybe_start_code_bps();
    }
    TRACE("%s, %s, %p\n", debugstr_guid(clsid), debugstr_guid(iid), out);
    if (IsEqualGUID(clsid, &CLSID_ActivatableApplicationRegistrar))
        return IClassFactory_QueryInterface(&registrar_factory, iid, out);
    if (IsEqualGUID(clsid, &CLSID_CoreWindowBroker))
        return IClassFactory_QueryInterface(&window_factory_factory, iid, out);
    if (IsEqualGUID(clsid, &CLSID_PsmBroker))
        return IClassFactory_QueryInterface(&psm_broker_factory, iid, out);
    FIXME("class %s not provided\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

/* ext-ms-win-sysmain-pfsapi-l1-1-0: SysMain's "offerable heap", which Windows.UI.Xaml's OfferableHeap
 * delay-loads (a failed delay load aborted the process). Signatures from XAML's wrappers: Initialize
 * and Start take the heap and return a Win32 status; Alloc(heap, size) returns the block or NULL;
 * Free/Offer/Reclaim take (heap, block); Reclaim returns 0 when the block's contents survived. Nothing
 * is ever offered to the system here, so every block survives. */
DWORD WINAPI PfsOfferableHeapInitialize( void *heap, void *a, void *b, void *c )
{
    TRACE( "%p %p %p %p\n", heap, a, b, c );
    return ERROR_SUCCESS;
}
DWORD WINAPI PfsOfferableHeapStart( void *heap, void *a, void *b, void *c )
{
    TRACE( "%p\n", heap );
    return ERROR_SUCCESS;
}
void * WINAPI PfsOfferableHeapAlloc( void *heap, SIZE_T size, void *a, void *b )
{
    void *block = HeapAlloc( GetProcessHeap(), 0, size );
    TRACE( "%p size %Iu -> %p\n", heap, size, block );
    return block;
}
DWORD WINAPI PfsOfferableHeapFree( void *heap, void *block, void *a, void *b )
{
    TRACE( "%p %p\n", heap, block );
    if (block) HeapFree( GetProcessHeap(), 0, block );
    return ERROR_SUCCESS;
}
DWORD WINAPI PfsOfferableHeapOffer( void *heap, void *block, void *a, void *b )
{
    TRACE( "%p %p\n", heap, block );
    return ERROR_SUCCESS;
}
DWORD WINAPI PfsOfferableHeapReclaim( void *heap, void *block, void *a, void *b )
{
    TRACE( "%p %p\n", heap, block );
    return ERROR_SUCCESS;
}
DWORD WINAPI PfsOfferableHeapCleanup( void *heap, void *a, void *b, void *c )
{
    TRACE( "%p\n", heap );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *  Repairing imports Wine's apiset table cannot satisfy
 *
 * twinapi.appcore imports GetPackageFamilyNameFromToken from
 * api-ms-win-appmodel-runtime-l1-1-1. Wine's kernelbase implements the other seven functions this
 * apiset is used for but not that one, so the loader points the thunk at allocate_stub() and the
 * first call raises EXCEPTION_WINE_STUB (0x80000100). In Solitaire that kills the thread running
 * XblAccountStatusHelperWinRT::PopulateXblAccountStatusAsync, so the async that
 * StoreAppComponent::Connect is waiting on is never completed and startup stops.
 *
 * Dropping a DLL with the apiset's file name into system32 does *not* fix this, which is worth
 * recording because it does work for apisets Wine has never heard of (see the advertising-id
 * stand-in). find_apiset_dll() consults Peb->ApiSetMap first, and apiset name matching ignores the
 * trailing version fields - so "l1-1-1" matches the schema's "l1-1-2 = kernelbase.dll" entry, the
 * import resolves to kernelbase, and the file is never looked at.
 *
 * The right home for the function is kernelbase, and for the mapping apisetschema.spec - but both
 * are another session's modified files. So the thunk is repaired here instead: walk each module's
 * import table and replace the stub with a real implementation. That generalises, which is the
 * point: any packaged app can import an apiset function Wine happens to lack, and this is a
 * one-line table entry rather than a new investigation each time.
 */

/* Resolved dynamically: importing it here would bind to the same apiset. */
static LONG package_full_name_from_token( HANDLE token, UINT32 *length, WCHAR *name )
{
    static LONG (WINAPI *fn)( HANDLE, UINT32 *, WCHAR * );
    static BOOL tried;

    if (!tried)
    {
        HMODULE kb = GetModuleHandleW( L"kernelbase.dll" );
        if (kb) fn = (void *)GetProcAddress( kb, "GetPackageFullNameFromToken" );
        tried = TRUE;
    }
    if (!fn) return APPMODEL_ERROR_NO_PACKAGE;
    return fn( token, length, name );
}

/* A package family name is the first and last fields of the full name:
 * Name_Version_Architecture_ResourceId_PublisherId -> Name_PublisherId. */
static LONG WINAPI uwp_GetPackageFamilyNameFromToken( HANDLE token, UINT32 *length, WCHAR *name )
{
    WCHAR full[PACKAGE_FULL_NAME_MAX_LENGTH + 1], family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 full_len = ARRAY_SIZE(full), needed;
    const WCHAR *first, *last;
    size_t name_len, pub_len;
    LONG ret;

    if (!length) return ERROR_INVALID_PARAMETER;
    if ((ret = package_full_name_from_token( token, &full_len, full )))
    {
        TRACE( "GetPackageFullNameFromToken returned %ld\n", ret );
        return ret;
    }
    first = wcschr( full, '_' );
    last = wcsrchr( full, '_' );
    if (!first || !last || last <= first) return APPMODEL_ERROR_NO_PACKAGE;
    name_len = first - full;
    pub_len = wcslen( last + 1 );
    if (!name_len || !pub_len || name_len + pub_len + 2 > ARRAY_SIZE(family))
        return APPMODEL_ERROR_NO_PACKAGE;
    memcpy( family, full, name_len * sizeof(WCHAR) );
    family[name_len] = '_';
    memcpy( family + name_len + 1, last + 1, (pub_len + 1) * sizeof(WCHAR) );

    needed = name_len + pub_len + 2;   /* the documented count includes the terminating NUL */
    if (!name || *length < needed)
    {
        *length = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    memcpy( name, family, needed * sizeof(WCHAR) );
    *length = needed;
    TRACE( "token %p -> %s\n", token, debugstr_w(family) );
    return ERROR_SUCCESS;
}

static const struct
{
    const char *dll;
    const char *func;
    void *impl;
}
import_fixups[] =
{
    { "api-ms-win-appmodel-runtime-l1-1-1.dll", "GetPackageFamilyNameFromToken",
      uwp_GetPackageFamilyNameFromToken },
};

static void patch_module_imports( HMODULE mod )
{
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_IMPORT_DESCRIPTOR *desc;
    BYTE *base = (BYTE *)mod;
    ULONG size;
    unsigned int i;

    if (!mod) return;
    if (!(nt = RtlImageNtHeader( mod ))) return;
    if (!(desc = RtlImageDirectoryEntryToData( mod, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size )))
        return;

    for (; desc->Name && desc->FirstThunk; desc++)
    {
        const char *dll = (const char *)(base + desc->Name);
        const IMAGE_THUNK_DATA *names;
        IMAGE_THUNK_DATA *addrs;

        for (i = 0; i < ARRAY_SIZE(import_fixups); i++)
            if (!_stricmp( dll, import_fixups[i].dll )) break;
        if (i == ARRAY_SIZE(import_fixups)) continue;
        if (!desc->OriginalFirstThunk) continue;   /* no names to match against */

        names = (const IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);
        addrs = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        for (; names->u1.Ordinal; names++, addrs++)
        {
            const IMAGE_IMPORT_BY_NAME *by_name;
            unsigned int f;

            if (IMAGE_SNAP_BY_ORDINAL( names->u1.Ordinal )) continue;
            by_name = (const IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            for (f = 0; f < ARRAY_SIZE(import_fixups); f++)
            {
                void *old = (void *)addrs->u1.Function;
                DWORD prot;

                if (_stricmp( dll, import_fixups[f].dll )) continue;
                if (strcmp( (const char *)by_name->Name, import_fixups[f].func )) continue;
                if (old == import_fixups[f].impl) break;   /* already done */
                if (!VirtualProtect( addrs, sizeof(*addrs), PAGE_READWRITE, &prot )) break;
                addrs->u1.Function = (ULONG_PTR)import_fixups[f].impl;
                VirtualProtect( addrs, sizeof(*addrs), prot, &prot );
                ERR( "repaired %s.%s in %p (was %p, Wine has no implementation)\n",
                     dll, import_fixups[f].func, mod, old );
                break;
            }
        }
    }
}

/* The apiset imports that matter here are *delay* imports, so the normal import table does not
 * mention them at all. (This is also why dropping a file named after the apiset into system32 works
 * for an apiset Wine has never heard of: the delay-load helper does a runtime LoadLibrary, which
 * finds the file. It cannot work here, because this apiset *is* in the schema and resolves to
 * kernelbase before any file is looked at.)
 *
 * Overwriting the delay IAT entry before the first call means the call site's `call [IAT]` goes
 * straight to our implementation and the delay-load helper never runs for this function. */
static void patch_module_delay_imports( HMODULE mod )
{
    const IMAGE_DELAYLOAD_DESCRIPTOR *desc;
    BYTE *base = (BYTE *)mod;
    ULONG size;
    unsigned int i;

    if (!(desc = RtlImageDirectoryEntryToData( mod, TRUE, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
                                               &size )))
        return;

    for (; desc->DllNameRVA; desc++)
    {
        const char *dll;
        const IMAGE_THUNK_DATA *names;
        IMAGE_THUNK_DATA *addrs;

        /* Attributes bit 0 clear means the fields are absolute addresses, from a linker old enough
         * that nothing we load uses it. Skip rather than misread them as RVAs. */
        if (!(desc->Attributes.AllAttributes & 1)) continue;
        if (!desc->ImportNameTableRVA || !desc->ImportAddressTableRVA) continue;

        dll = (const char *)(base + desc->DllNameRVA);
        for (i = 0; i < ARRAY_SIZE(import_fixups); i++)
            if (!_stricmp( dll, import_fixups[i].dll )) break;
        if (i == ARRAY_SIZE(import_fixups)) continue;

        names = (const IMAGE_THUNK_DATA *)(base + desc->ImportNameTableRVA);
        addrs = (IMAGE_THUNK_DATA *)(base + desc->ImportAddressTableRVA);
        for (; names->u1.Ordinal; names++, addrs++)
        {
            const IMAGE_IMPORT_BY_NAME *by_name;
            unsigned int f;

            if (IMAGE_SNAP_BY_ORDINAL( names->u1.Ordinal )) continue;
            by_name = (const IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            for (f = 0; f < ARRAY_SIZE(import_fixups); f++)
            {
                void *old = (void *)addrs->u1.Function;
                DWORD prot;

                if (_stricmp( dll, import_fixups[f].dll )) continue;
                if (strcmp( (const char *)by_name->Name, import_fixups[f].func )) continue;
                if (old == import_fixups[f].impl) break;
                if (!VirtualProtect( addrs, sizeof(*addrs), PAGE_READWRITE, &prot )) break;
                addrs->u1.Function = (ULONG_PTR)import_fixups[f].impl;
                VirtualProtect( addrs, sizeof(*addrs), prot, &prot );
                ERR( "repaired delay-import %s.%s in %p (was %p, Wine has no implementation)\n",
                     dll, import_fixups[f].func, mod, old );
                break;
            }
        }
    }
}

static void patch_loaded_module_imports( void )
{
    PEB_LDR_DATA *ldr = NtCurrentTeb()->Peb->LdrData;
    LIST_ENTRY *mark = &ldr->InMemoryOrderModuleList, *entry;

    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY,
                                                       InMemoryOrderLinks );
        patch_module_imports( mod->DllBase );
        patch_module_delay_imports( mod->DllBase );
    }
}

/* twinapi.appcore loads after wineuwp, so patching only at attach time would miss it. */
static void CALLBACK dll_load_notification( ULONG reason, const void *data, void *context )
{
    const struct
    {
        ULONG flags;
        const UNICODE_STRING *full_name;
        const UNICODE_STRING *base_name;
        void *base;
        ULONG size;
    } *info = data;

    if (reason == 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */ && info)
    {
        patch_module_imports( info->base );
        patch_module_delay_imports( info->base );
    }
}

static void install_import_fixups( void )
{
    static NTSTATUS (WINAPI *reg)( ULONG, void *, void *, void ** );
    static void *cookie;
    HMODULE ntdll;

    patch_loaded_module_imports();
    if (cookie) return;
    if (!(ntdll = GetModuleHandleW( L"ntdll.dll" ))) return;
    if (!(reg = (void *)GetProcAddress( ntdll, "LdrRegisterDllNotification" ))) return;
    reg( 0, dll_load_notification, NULL, &cookie );
}

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls( inst );
        install_import_fixups();
    }
    return TRUE;
}

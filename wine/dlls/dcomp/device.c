/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "objidl.h"
#include "winternl.h"
#include "dxgi.h"
#include <stdlib.h>
#define COBJMACROS
#include "winstring.h"
#include "activation.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

extern void dcomp_install_fault_logger(void);

/* Install the composition fault logger as soon as the DLL is loaded, so faults that happen before the first
 * composition object is created (e.g. during app startup) are logged too. */
BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, LPVOID reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls( instance );
        dcomp_install_fault_logger();
    }
    return TRUE;
}

extern HRESULT dcomp_create_device(IUnknown *rendering_device, REFIID iid, void **out);

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", dxgi_device, debugstr_guid(iid), device);
    return dcomp_create_device((IUnknown *)dxgi_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);
    return dcomp_create_device(rendering_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);
    return dcomp_create_device(rendering_device, iid, device);
}

/***********************************************************************
 *      Windows.UI.UIContext
 *
 * An opaque per-view identity token. It carries no state of its own; CoreWindow asks the class for
 * one and hands it out as Window.UIContext, and callers only ever compare them. Windows implements
 * it in dcomp.dll, so it lives here.
 */
static const GUID IID_IUIContextStatics_private =
    { 0x97473389, 0x5b37, 0x5c96, { 0x9c, 0x2b, 0x0f, 0xd2, 0x9d, 0x43, 0xa6, 0xc1 } };
/* Windows.UI.IUIContext, the default interface: no members, callers only compare identities. */
static const GUID IID_IUIContext =
    { 0xf21e14c1, 0xe669, 0x52af, { 0x99, 0xcd, 0x17, 0x1e, 0xee, 0x8e, 0x94, 0x0d } };

struct uicontext
{
    IInspectable IInspectable_iface;
    LONG ref;
    HWND hwnd;
};

/* ICoreWindowInterop, the documented way from a CoreWindow to its HWND. */
static const GUID IID_ICoreWindowInterop =
    { 0x45d64a29, 0xa63e, 0x4cb6, { 0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f } };
struct corewindow_interop_vtbl
{
    HRESULT (WINAPI *QueryInterface)(void *iface, REFIID iid, void **out);
    ULONG (WINAPI *AddRef)(void *iface);
    ULONG (WINAPI *Release)(void *iface);
    HRESULT (WINAPI *get_WindowHandle)(void *iface, HWND *hwnd);
};

static HWND hwnd_from_view(void *view)
{
    struct corewindow_interop_vtbl **interop;
    HWND hwnd = NULL;

    if (view && SUCCEEDED(((IUnknown *)view)->lpVtbl->QueryInterface((IUnknown *)view,
            &IID_ICoreWindowInterop, (void **)&interop)))
    {
        if (FAILED((*interop)->get_WindowHandle(interop, &hwnd))) hwnd = NULL;
        (*interop)->Release(interop);
    }
    return hwnd;
}

static struct uicontext *impl_from_IInspectable(IInspectable *iface)
{
    return CONTAINING_RECORD(iface, struct uicontext, IInspectable_iface);
}

static HRESULT WINAPI uicontext_QueryInterface(IInspectable *iface, REFIID iid, void **out)
{
    struct uicontext *impl = impl_from_IInspectable(iface);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IUIContext))
    {
        InterlockedIncrement(&impl->ref);
        *out = &impl->IInspectable_iface;
        return S_OK;
    }

    FIXME("UIContext: no interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI uicontext_AddRef(IInspectable *iface)
{
    return InterlockedIncrement(&impl_from_IInspectable(iface)->ref);
}

static ULONG WINAPI uicontext_Release(IInspectable *iface)
{
    struct uicontext *impl = impl_from_IInspectable(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    if (!ref) free(impl);
    return ref;
}

static HRESULT WINAPI uicontext_GetIids(IInspectable *iface, ULONG *count, IID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI uicontext_GetRuntimeClassName(IInspectable *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.UI.UIContext", 20, name);
}

static HRESULT WINAPI uicontext_GetTrustLevel(IInspectable *iface, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

/* The interface dcomp.dll hands out here is IUIContextPartner, not the empty public IUIContext: the
 * vtable in the shipping dcomp.dll (??_7UIContext@UI@Windows@@6B<partner>, slots 6 and 7) is
 * IInspectable followed by get_UIContentRoot and get_WindowContext. Slot 6 has no answer here -
 * there is no UIContentRoot - and callers handle the failure. */
static HRESULT WINAPI uicontext_method(void *iface)
{
    FIXME("stub.\n");
    return E_NOTIMPL;
}

/*
 * The window context handed back by get_WindowContext. Callers reach it for interfaces we do not
 * implement and check the QI (MrmCoreR's CoreWindowContainer::Initialize logs and carries on), but
 * they do want an object: XAML fails to create a control that owns an InputPane when the getter
 * itself fails, and MRM faults if it succeeds with a null out parameter.
 */
/*
 * twinapi's CInputPaneEventSourceFactory::GetForUIContext() does not want any state from the
 * UIContext: it walks UIContext -> (IID_IUIContext) -> get_WindowContext -> (IID_IWindowHostPartner)
 * -> slot 7 -> slot 7 purely to arrive at an HWND, which it then hands to
 * CInputPaneEventSource_GetInstance(). Each hop is IInspectable plus one unused method, with the
 * getter at slot 7. Answering the first QI with E_NOINTERFACE ends the walk, and that is what made
 * every AutoSuggestBox fail to construct (DirectUI::AutoSuggestBox::PrepareState subscribes to the
 * input pane and propagates the failure). So carry the HWND down the chain and hand it over.
 */
static const GUID IID_IWindowHostPartner =
    { 0x84acbbfe, 0x649c, 0x5ac7, { 0x81, 0x30, 0x7b, 0x6b, 0x76, 0xdb, 0x46, 0x3c } };

struct window_host
{
    const void **lpVtbl;
    LONG ref;
    HWND hwnd;
    BOOL last;  /* the last hop returns the HWND itself rather than another hop */
};

static const void *window_host_vtbl[];

static HRESULT WINAPI window_host_QueryInterface(void *iface, REFIID iid, void **out)
{
    struct window_host *impl = iface;

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IWindowHostPartner))
    {
        InterlockedIncrement(&impl->ref);
        *out = iface;
        return S_OK;
    }

    FIXME("window host: no interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI window_host_AddRef(void *iface)
{
    return InterlockedIncrement(&((struct window_host *)iface)->ref);
}

static ULONG WINAPI window_host_Release(void *iface)
{
    struct window_host *impl = iface;
    ULONG ref = InterlockedDecrement(&impl->ref);

    if (!ref) free(impl);
    return ref;
}

static HRESULT WINAPI window_host_GetIids(void *iface, ULONG *count, IID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI window_host_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.UI.WindowContext", 24, name);
}

static HRESULT WINAPI window_host_GetTrustLevel(void *iface, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT create_window_host(HWND hwnd, BOOL last, void **out);

/* Slot 6 is another getter: MrmCoreR's CoreWindowContainer::Initialize calls it with a single
 * out-parameter, stores what comes back and releases it later, and treats a failure as fatal -
 * which is what E_NOTIMPL here did to Media Player's ViewportStateTrigger. Hand back another hop
 * carrying the same window handle, the way the QI and slot 7 do. */
static HRESULT WINAPI window_host_method(void *iface, void **out)
{
    struct window_host *impl = iface;

    TRACE("iface %p, out %p.\n", iface, out);

    if (!out) return E_POINTER;
    return create_window_host(impl->hwnd, FALSE, out);
}

static HRESULT create_window_host(HWND hwnd, BOOL last, void **out)
{
    struct window_host *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->lpVtbl = window_host_vtbl;
    impl->ref = 1;
    impl->hwnd = hwnd;
    impl->last = last;
    *out = impl;
    return S_OK;
}

static HRESULT WINAPI window_host_get_next(void *iface, void **out)
{
    struct window_host *impl = iface;

    TRACE("iface %p, out %p.\n", iface, out);

    if (!out) return E_POINTER;
    if (impl->last)
    {
        /* The caller stores this as an HWND, not as an interface pointer. */
        *out = impl->hwnd;
        return impl->hwnd ? S_OK : E_FAIL;
    }
    return create_window_host(impl->hwnd, TRUE, out);
}

/* The real interface is longer than the eight slots implemented above: MrmCoreR calls slot 17
 * (`mov 0x88(%rax),%rax; call`), which read nine entries past the end of this array and jumped to
 * whatever .data held next - a NULL, caught only by dcomp's NX handler. The interface is in no
 * .winmd and unnamed in HKCR, so give every slot up to 32 a traced thunk and let the app say which
 * ones it actually needs. A thunk that returns E_NOTIMPL is an answer a caller can handle; a jump
 * through NULL is not. */
#define WINDOW_HOST_THUNK(n)                                                                  \
    static HRESULT WINAPI window_host_slot_##n( void *iface, void *a, void *b, void *c )      \
    {                                                                                         \
        FIXME( "window host %p: unimplemented slot " #n " (a %p, b %p, c %p).\n",              \
               iface, a, b, c );                                                              \
        return E_NOTIMPL;                                                                     \
    }
WINDOW_HOST_THUNK(8)  WINDOW_HOST_THUNK(9)  WINDOW_HOST_THUNK(10) WINDOW_HOST_THUNK(11)
WINDOW_HOST_THUNK(12) WINDOW_HOST_THUNK(13) WINDOW_HOST_THUNK(14) WINDOW_HOST_THUNK(15)
WINDOW_HOST_THUNK(16) WINDOW_HOST_THUNK(17) WINDOW_HOST_THUNK(18) WINDOW_HOST_THUNK(19)
WINDOW_HOST_THUNK(20) WINDOW_HOST_THUNK(21) WINDOW_HOST_THUNK(22) WINDOW_HOST_THUNK(23)
WINDOW_HOST_THUNK(24) WINDOW_HOST_THUNK(25) WINDOW_HOST_THUNK(26) WINDOW_HOST_THUNK(27)
WINDOW_HOST_THUNK(28) WINDOW_HOST_THUNK(29) WINDOW_HOST_THUNK(30) WINDOW_HOST_THUNK(31)

static const void *window_host_vtbl[] =
{
    window_host_QueryInterface,
    window_host_AddRef,
    window_host_Release,
    window_host_GetIids,
    window_host_GetRuntimeClassName,
    window_host_GetTrustLevel,
    window_host_method,
    window_host_get_next,
    window_host_slot_8,  window_host_slot_9,  window_host_slot_10, window_host_slot_11,
    window_host_slot_12, window_host_slot_13, window_host_slot_14, window_host_slot_15,
    window_host_slot_16, window_host_slot_17, window_host_slot_18, window_host_slot_19,
    window_host_slot_20, window_host_slot_21, window_host_slot_22, window_host_slot_23,
    window_host_slot_24, window_host_slot_25, window_host_slot_26, window_host_slot_27,
    window_host_slot_28, window_host_slot_29, window_host_slot_30, window_host_slot_31,
};

struct window_context
{
    IInspectable IInspectable_iface;
    LONG ref;
    HWND hwnd;
};

static HRESULT WINAPI window_context_QueryInterface(IInspectable *iface, REFIID iid, void **out)
{
    struct window_context *impl = CONTAINING_RECORD(iface, struct window_context, IInspectable_iface);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject))
    {
        InterlockedIncrement(&impl->ref);
        *out = &impl->IInspectable_iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IWindowHostPartner))
        return create_window_host(impl->hwnd ? impl->hwnd : GetActiveWindow(), FALSE, out);

    FIXME("window context: no interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI window_context_AddRef(IInspectable *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, struct window_context, IInspectable_iface)->ref);
}

static ULONG WINAPI window_context_Release(IInspectable *iface)
{
    struct window_context *impl = CONTAINING_RECORD(iface, struct window_context, IInspectable_iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    if (!ref) free(impl);
    return ref;
}

static HRESULT WINAPI window_context_GetIids(IInspectable *iface, ULONG *count, IID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI window_context_GetRuntimeClassName(IInspectable *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.UI.WindowContext", 24, name);
}

static HRESULT WINAPI window_context_GetTrustLevel(IInspectable *iface, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

static const IInspectableVtbl window_context_vtbl =
{
    window_context_QueryInterface,
    window_context_AddRef,
    window_context_Release,
    window_context_GetIids,
    window_context_GetRuntimeClassName,
    window_context_GetTrustLevel,
};

static HRESULT WINAPI uicontext_get_WindowContext(void *iface, void **out)
{
    struct window_context *impl;

    TRACE("iface %p, out %p.\n", iface, out);

    if (!out) return E_POINTER;
    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->IInspectable_iface.lpVtbl = &window_context_vtbl;
    impl->ref = 1;
    impl->hwnd = impl_from_IInspectable(iface)->hwnd;

    *out = &impl->IInspectable_iface;
    return S_OK;
}


static const void *uicontext_vtbl[] =
{
    uicontext_QueryInterface,
    uicontext_AddRef,
    uicontext_Release,
    uicontext_GetIids,
    uicontext_GetRuntimeClassName,
    uicontext_GetTrustLevel,
    uicontext_method, uicontext_get_WindowContext, uicontext_method, uicontext_method,
    uicontext_method, uicontext_method, uicontext_method, uicontext_method,
};

static HRESULT create_uicontext(void *view, IInspectable **out)
{
    struct uicontext *impl;

    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->IInspectable_iface.lpVtbl = (const IInspectableVtbl *)uicontext_vtbl;
    impl->ref = 1;
    impl->hwnd = hwnd_from_view(view);
    *out = &impl->IInspectable_iface;
    return S_OK;
}

/* The private statics interface CoreWindow uses. Its second method makes a context for a view;
 * the first one is not used here. */
struct uicontext_statics_vtbl
{
    HRESULT (WINAPI *QueryInterface)(void *iface, REFIID iid, void **out);
    ULONG (WINAPI *AddRef)(void *iface);
    ULONG (WINAPI *Release)(void *iface);
    HRESULT (WINAPI *GetIids)(void *iface, ULONG *count, IID **iids);
    HRESULT (WINAPI *GetRuntimeClassName)(void *iface, HSTRING *name);
    HRESULT (WINAPI *GetTrustLevel)(void *iface, TrustLevel *level);
    HRESULT (WINAPI *unknown_method)(void *iface);
    HRESULT (WINAPI *CreateForView)(void *iface, void *view, UINT32 flags, IInspectable **out);
};

struct uicontext_factory
{
    IActivationFactory IActivationFactory_iface;
    const struct uicontext_statics_vtbl *statics_vtbl;
};

static HRESULT uicontext_factory_query(REFIID iid, void **out);

static HRESULT WINAPI uicontext_factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    return uicontext_factory_query(iid, out);
}
static ULONG WINAPI uicontext_factory_AddRef(IActivationFactory *iface) { return 2; }
static ULONG WINAPI uicontext_factory_Release(IActivationFactory *iface) { return 1; }
static HRESULT WINAPI uicontext_factory_GetIids(IActivationFactory *iface, ULONG *count, IID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI uicontext_factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.UI.UIContext", 20, name);
}
static HRESULT WINAPI uicontext_factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI uicontext_factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    return create_uicontext(NULL, instance);
}

static const IActivationFactoryVtbl uicontext_factory_vtbl =
{
    uicontext_factory_QueryInterface,
    uicontext_factory_AddRef,
    uicontext_factory_Release,
    uicontext_factory_GetIids,
    uicontext_factory_GetRuntimeClassName,
    uicontext_factory_GetTrustLevel,
    uicontext_factory_ActivateInstance,
};

static HRESULT WINAPI uicontext_statics_QueryInterface(void *iface, REFIID iid, void **out)
{
    return uicontext_factory_query(iid, out);
}
static ULONG WINAPI uicontext_statics_AddRef(void *iface) { return 2; }
static ULONG WINAPI uicontext_statics_Release(void *iface) { return 1; }
static HRESULT WINAPI uicontext_statics_GetIids(void *iface, ULONG *count, IID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI uicontext_statics_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.UI.UIContext", 20, name);
}
static HRESULT WINAPI uicontext_statics_GetTrustLevel(void *iface, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI uicontext_statics_unknown(void *iface)
{
    FIXME("stub.\n");
    return E_NOTIMPL;
}
static HRESULT WINAPI uicontext_statics_CreateForView(void *iface, void *view, UINT32 flags, IInspectable **out)
{
    TRACE("%p, %#x, %p.\n", view, flags, out);

    if (!out) return E_POINTER;
    return create_uicontext(view, out);
}

static const struct uicontext_statics_vtbl uicontext_statics_vtbl =
{
    uicontext_statics_QueryInterface,
    uicontext_statics_AddRef,
    uicontext_statics_Release,
    uicontext_statics_GetIids,
    uicontext_statics_GetRuntimeClassName,
    uicontext_statics_GetTrustLevel,
    uicontext_statics_unknown,
    uicontext_statics_CreateForView,
};

static struct uicontext_factory uicontext_factory =
{
    { &uicontext_factory_vtbl },
    &uicontext_statics_vtbl,
};

static HRESULT uicontext_factory_query(REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IActivationFactory) || IsEqualGUID(iid, &IID_IAgileObject))
        *out = &uicontext_factory.IActivationFactory_iface;
    else if (IsEqualGUID(iid, &IID_IUIContextStatics_private))
        *out = &uicontext_factory.statics_vtbl;
    else
    {
        FIXME("UIContext factory: no interface %s.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

/***********************************************************************
 *      DllGetActivationFactory (dcomp.@)
 *
 * Windows.UI.Composition also lives in dcomp.dll and is not implemented here yet; asking for one
 * gets a clean REGDB_E_CLASSNOTREG rather than tearing the process down on a missing entry point.
 */
extern IActivationFactory *compositor_activation_factory;
extern IActivationFactory *capabilities_activation_factory;
extern IActivationFactory *composition_object_activation_factory;
extern IActivationFactory *composition_island_activation_factory;
extern IActivationFactory *effect_source_parameter_activation_factory;
extern IActivationFactory *scene_lighting_effect_activation_factory;
extern IActivationFactory *composition_path_activation_factory;
extern IActivationFactory *interactions_activation_factory;

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const WCHAR *name = WindowsGetStringRawBuffer(classid, NULL);

    TRACE("%s, %p.\n", debugstr_w(name), factory);

    if (!factory) return E_INVALIDARG;
    if (name && !wcscmp(name, L"Windows.UI.UIContext"))
    {
        *factory = &uicontext_factory.IActivationFactory_iface;
        return S_OK;
    }
    if (name && !wcscmp(name, L"Windows.UI.Composition.Compositor"))
    {
        IActivationFactory_AddRef( (*factory = compositor_activation_factory) );
        return S_OK;
    }

    if (name && !wcscmp(name, L"Windows.UI.Composition.CompositionCapabilities"))
    {
        IActivationFactory_AddRef( (*factory = capabilities_activation_factory) );
        return S_OK;
    }

    if (name && !wcscmp(name, L"Windows.UI.Composition.CompositionObject"))
    {
        IActivationFactory_AddRef( (*factory = composition_object_activation_factory) );
        return S_OK;
    }

    if (name && !wcscmp(name, L"Windows.UI.Composition.CompositionPath"))
    {
        IActivationFactory_AddRef( (*factory = composition_path_activation_factory) );
        return S_OK;
    }
    if (name && !wcscmp(name, L"Windows.UI.Composition.Effects.SceneLightingEffect"))
    {
        IActivationFactory_AddRef( (*factory = scene_lighting_effect_activation_factory) );
        return S_OK;
    }
    if (name && !wcscmp(name, L"Windows.UI.Composition.CompositionEffectSourceParameter"))
    {
        IActivationFactory_AddRef( (*factory = effect_source_parameter_activation_factory) );
        return S_OK;
    }
    if (name && !wcscmp(name, L"Windows.UI.Composition.CompositionIsland"))
    {
        IActivationFactory_AddRef( (*factory = composition_island_activation_factory) );
        return S_OK;
    }

    if (name && !wcsncmp(name, L"Windows.UI.Composition.Interactions.", 36))
    {
        TRACE("%s: recording factory.\n", debugstr_w(name));
        IActivationFactory_AddRef( (*factory = interactions_activation_factory) );
        return S_OK;
    }

    FIXME("%s not implemented.\n", debugstr_w(name));
    *factory = NULL;
    return REGDB_E_CLASSNOTREG;
}

/***********************************************************************
 *      NtCreateCompositionInputSink (dcomp.@)
 *
 * Ordinal 1019, on Windows a forwarder to win32u.NtCreateCompositionInputSink. XAML asks for an
 * input sink when it attaches hit-testing to a visual (user32's AddInputSinkToVisual does the same);
 * the caller only keeps the handle and checks the status, so hand out a closable handle of our own.
 * The first DWORD of the 0xe8-byte descriptor is its size.
 */
/* Pointer input for an input sink. On Windows the kernel sends input that lands on the sink's visual
 * to the sink's target window instead of the window underneath; SwapChainPanel's
 * CreateCoreIndependentInputSource depends on it, since that input source (and Shapr3D's whole 3D
 * viewport, which reads PointerPressed/Moved/Released from one) only ever sees its own window. The
 * descriptor carries the target window at +0x10 (class Windows.UI.Core.CoreIndependentInputSource,
 * a hidden popup owned by the input thread).
 *
 * Here a WH_GETMESSAGE hook on the thread that created the sink - XAML's UI thread, which also
 * paints - reposts mouse messages to the target when the topmost thing painted at that point is
 * swap chain content, and from a button press to its release so drags stay with the sink. */
BOOL dcomp_point_on_swapchain( POINT pt, POINT *origin );

static HWND input_sink_target;
static HHOOK input_sink_hook;
static BOOL input_sink_captured;
static POINT input_sink_origin;   /* the panel's top left in the window, from the last hit */

/* The pointer message win32u would have produced for this mouse message (process_mouse_message):
 * Windows.UI's input sources read pointer messages and GetPointerInfo, never mouse messages.
 * Pointer id 1 is win32u's mouse pointer, whose state the same mouse input keeps current. */
static BOOL pointer_message_from_mouse( const MSG *msg, UINT *message, WPARAM *wparam )
{
    static const struct { WORD mk; WORD flag; } buttons[] =
    {
        { MK_LBUTTON, POINTER_MESSAGE_FLAG_FIRSTBUTTON }, { MK_RBUTTON, POINTER_MESSAGE_FLAG_SECONDBUTTON },
        { MK_MBUTTON, POINTER_MESSAGE_FLAG_THIRDBUTTON }, { MK_XBUTTON1, POINTER_MESSAGE_FLAG_FOURTHBUTTON },
        { MK_XBUTTON2, POINTER_MESSAGE_FLAG_FIFTHBUTTON },
    };
    WORD flags = POINTER_MESSAGE_FLAG_INRANGE, pressed = 0;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(buttons); i++) if (LOWORD(msg->wParam) & buttons[i].mk) pressed |= buttons[i].flag;
    if (pressed) flags |= pressed | POINTER_MESSAGE_FLAG_INCONTACT;
    switch (msg->message)
    {
    case WM_MOUSEMOVE:
        *message = WM_POINTERUPDATE;
        if (!pressed) flags |= POINTER_MESSAGE_FLAG_PRIMARY;
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (pressed & (pressed - 1)) *message = WM_POINTERUPDATE;   /* another button was already down */
        else
        {
            *message = WM_POINTERDOWN;
            flags |= POINTER_MESSAGE_FLAG_PRIMARY;
        }
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        *message = pressed ? WM_POINTERUPDATE : WM_POINTERUP;
        break;
    case WM_MOUSEWHEEL:
        *message = WM_POINTERWHEEL;
        flags = HIWORD(msg->wParam);
        break;
    case WM_MOUSEHWHEEL:
        *message = WM_POINTERHWHEEL;
        flags = HIWORD(msg->wParam);
        break;
    default:
        return FALSE;
    }
    *wparam = MAKEWPARAM( 1, flags );
    return TRUE;
}

/* The input source turns a pointer message's screen position into a point on the panel against
 * its own window, so that window is kept at the panel's top left on screen and positions go out in
 * screen coordinates. XAML routes a gesture's first press to the input source itself, in screen
 * coordinates, so the two routes have to agree: with the window left at 0,0, Shapr3D put the first
 * point of every line the window's screen offset plus the panel's top away from the click.
 *
 * A press and a release are each preceded by an update at the same point, as a real pointer hovers
 * before it presses; the input source takes an event's position from the points gathered before it. */
static LRESULT CALLBACK input_sink_hook_proc( int code, WPARAM wparam, LPARAM lparam )
{
    MSG *msg = (MSG *)lparam;

    if (code == HC_ACTION && wparam == PM_REMOVE && input_sink_target && msg->hwnd != input_sink_target &&
        msg->message >= WM_MOUSEFIRST && msg->message <= WM_MOUSELAST)
    {
        POINT pt = { (short)LOWORD(msg->lParam), (short)HIWORD(msg->lParam) }, screen;
        BOOL down = msg->message == WM_LBUTTONDOWN || msg->message == WM_RBUTTONDOWN || msg->message == WM_MBUTTONDOWN;
        BOOL up = msg->message == WM_LBUTTONUP || msg->message == WM_RBUTTONUP || msg->message == WM_MBUTTONUP;
        UINT pointer_message;
        WPARAM pointer_wparam;
        POINT origin = input_sink_origin;

        /* the wheel carries screen coordinates, the rest client ones */
        if (msg->message == WM_MOUSEWHEEL || msg->message == WM_MOUSEHWHEEL) ScreenToClient( msg->hwnd, &pt );
        screen = pt;
        ClientToScreen( msg->hwnd, &screen );

        if ((dcomp_point_on_swapchain( pt, &origin ) || input_sink_captured) &&
            pointer_message_from_mouse( msg, &pointer_message, &pointer_wparam ))
        {
            LPARAM pos = MAKELPARAM( screen.x, screen.y );
            POINT panel = origin;
            RECT rect;

            ClientToScreen( msg->hwnd, &panel );
            GetWindowRect( input_sink_target, &rect );
            if (rect.left != panel.x || rect.top != panel.y)
                SetWindowPos( input_sink_target, NULL, panel.x, panel.y, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING );
            input_sink_origin = origin;
            if (down) input_sink_captured = TRUE;
            if (up && !(LOWORD(msg->wParam) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) input_sink_captured = FALSE;
            if (pointer_message == WM_POINTERDOWN || pointer_message == WM_POINTERUP)
            {
                /* the buttons as they were just before this message */
                WORD before = pointer_message == WM_POINTERDOWN ? POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_PRIMARY
                                                                : HIWORD(pointer_wparam) | POINTER_MESSAGE_FLAG_INCONTACT |
                                                                  POINTER_MESSAGE_FLAG_FIRSTBUTTON;
                PostMessageW( input_sink_target, WM_POINTERUPDATE, MAKEWPARAM( 1, before ), pos );
            }
            TRACE( "routing %#x at %s (screen %s, panel at %s) as %#x to input sink %p\n", msg->message,
                   wine_dbgstr_point( &pt ), wine_dbgstr_point( &screen ), wine_dbgstr_point( &panel ),
                   pointer_message, input_sink_target );
            PostMessageW( input_sink_target, pointer_message, pointer_wparam, pos );
            msg->message = WM_NULL;
        }
    }
    return CallNextHookEx( input_sink_hook, code, wparam, lparam );
}

NTSTATUS WINAPI create_composition_input_sink( const void *desc, HANDLE *sink )
{
    const BYTE *bytes = desc;
    HWND target = NULL;

    TRACE( "desc %p, sink %p.\n", desc, sink );

    if (!sink) return STATUS_INVALID_PARAMETER;
    if (bytes && *(const DWORD *)bytes >= 0x18)
    {
        ULONG_PTR value = *(const ULONG_PTR *)(bytes + 0x10);
        if (value && value < 0x100000000ull && IsWindow( (HWND)value )) target = (HWND)value;
    }
    if (target)
    {
        TRACE( "input sink for window %p, hooking thread %04lx.\n", target, GetCurrentThreadId() );
        input_sink_target = target;
        /* win32u copies every pointer message on the CoreWindow to this window; this hook routes
         * only the ones over the swapchain, so tell win32u to leave it alone */
        SetPropW( target, L"__wine_dcomp_input_sink", (HANDLE)1 );
        if (!input_sink_hook)
            input_sink_hook = SetWindowsHookExW( WH_GETMESSAGE, input_sink_hook_proc, NULL, GetCurrentThreadId() );
    }
    if (!(*sink = CreateEventW( NULL, TRUE, FALSE, NULL ))) return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *      DCompositionCreateAnimationStats (dcomp.@)
 *
 * Ordinal 2502. XAML asks for an animation statistics object while it sets up its animation
 * scenario tracing. Nothing consumes the statistics here, and the caller copes with a failure.
 */
HRESULT WINAPI create_animation_stats( void *arg, void **stats )
{
    FIXME( "arg %p, stats %p: stub.\n", arg, stats );

    if (stats) *stats = NULL;
    return E_NOTIMPL;
}

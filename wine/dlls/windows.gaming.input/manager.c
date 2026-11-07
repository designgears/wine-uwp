/* WinRT Windows.Gaming.Input implementation
 *
 * Copyright 2022 Rémi Bernon for CodeWeavers
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

#define WIDL_using_Windows_System
#include "private.h"
#include "provider.h"
#include "roapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(input);

static CRITICAL_SECTION manager_cs;
static CRITICAL_SECTION_DEBUG manager_cs_debug =
{
    0, 0, &manager_cs,
    { &manager_cs_debug.ProcessLocksList, &manager_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": manager_cs") }
};
static CRITICAL_SECTION manager_cs = { &manager_cs_debug, -1, 0, 0, 0, 0 };

static struct list controller_list = LIST_INIT( controller_list );

struct hardware_factory_registration
{
    struct list entry;
    UINT16 vendor_id, product_id;
    IAgileReference *reference;
};
static struct list hardware_factories = LIST_INIT( hardware_factories );

static void hardware_factory_add_controller( struct hardware_factory_registration *registration,
                                            IGameControllerProvider *provider );

struct gip_factory_registration
{
    struct list entry;
    GUID interface_id;
    ICustomGameControllerFactory *factory;
};
static struct list gip_factories = LIST_INIT( gip_factories );

struct controller
{
    IGameController IGameController_iface;
    IGameControllerBatteryInfo IGameControllerBatteryInfo_iface;
    IInspectable *IInspectable_inner;
    IAgileReference *custom_ref;
    struct hardware_factory_registration *registration;
    BOOL busy, removed;
    LONG ref;

    struct list entry;
    IGameControllerProvider *provider;
    ICustomGameControllerFactory *factory;
};

static inline struct controller *impl_from_IGameController( IGameController *iface )
{
    return CONTAINING_RECORD( iface, struct controller, IGameController_iface );
}

static HRESULT WINAPI controller_QueryInterface( IGameController *iface, REFIID iid, void **out )
{
    struct controller *impl = impl_from_IGameController( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGameController ))
    {
        IInspectable_AddRef( (*out = &impl->IGameController_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameControllerBatteryInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IGameControllerBatteryInfo_iface) );
        return S_OK;
    }

    return IInspectable_QueryInterface( impl->IInspectable_inner, iid, out );
}

static ULONG WINAPI controller_AddRef( IGameController *iface )
{
    struct controller *impl = impl_from_IGameController( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI controller_Release( IGameController *iface )
{
    struct controller *impl = impl_from_IGameController( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        if (impl->custom_ref) IAgileReference_Release( impl->custom_ref );
        if (impl->IInspectable_inner) IInspectable_Release( impl->IInspectable_inner );
        if (impl->factory) ICustomGameControllerFactory_Release( impl->factory );
        IGameControllerProvider_Release( impl->provider );
        free( impl );
    }

    return ref;
}

/* Snapshot references, never external COM calls, are taken under manager_cs. */
static HRESULT controller_snapshot( struct controller ***out, unsigned int *count )
{
    struct controller *entry, **items;
    unsigned int size = 0, i = 0;

    *out = NULL;
    *count = 0;
    EnterCriticalSection( &manager_cs );
    LIST_FOR_EACH_ENTRY( entry, &controller_list, struct controller, entry ) size++;
    if (!(items = calloc( size ? size : 1, sizeof(*items) )))
    {
        LeaveCriticalSection( &manager_cs );
        return E_OUTOFMEMORY;
    }
    LIST_FOR_EACH_ENTRY( entry, &controller_list, struct controller, entry )
    {
        IGameController_AddRef( &entry->IGameController_iface );
        items[i++] = entry;
    }
    LeaveCriticalSection( &manager_cs );
    *out = items;
    *count = size;
    return S_OK;
}

static BOOL object_matches( IUnknown *left, IUnknown *right )
{
    IUnknown *a, *b;
    BOOL match = FALSE;

    if (FAILED(IUnknown_QueryInterface( left, &IID_IUnknown, (void **)&a ))) return FALSE;
    if (SUCCEEDED(IUnknown_QueryInterface( right, &IID_IUnknown, (void **)&b )))
    {
        match = a == b;
        IUnknown_Release( b );
    }
    IUnknown_Release( a );
    return match;
}

static HRESULT controller_get_public_iface( struct controller *controller, IGameController **out )
{
    IAgileReference *reference;

    *out = NULL;
    if (!controller->registration)
    {
        IGameController_AddRef( (*out = &controller->IGameController_iface) );
        return S_OK;
    }
    EnterCriticalSection( &manager_cs );
    reference = controller->custom_ref;
    LeaveCriticalSection( &manager_cs );
    /* The caller owns a snapshot reference, so custom_ref cannot be destroyed here. */
    if (!reference) return E_PENDING;
    return IAgileReference_Resolve( reference, &IID_IGameController, (void **)out );
}

static BOOL controller_matches_factory( struct controller *controller, ICustomGameControllerFactory *factory )
{
    ICustomGameControllerFactory *resolved;
    BOOL match;

    if (!controller->registration) return controller->factory == factory;
    if (FAILED(IAgileReference_Resolve( controller->registration->reference,
                                       &IID_ICustomGameControllerFactory, (void **)&resolved ))) return FALSE;
    match = object_matches( (IUnknown *)resolved, (IUnknown *)factory );
    ICustomGameControllerFactory_Release( resolved );
    return match;
}

static HRESULT WINAPI controller_GetIids( IGameController *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI controller_GetRuntimeClassName( IGameController *iface, HSTRING *class_name )
{
    struct controller *impl = impl_from_IGameController( iface );
    return IInspectable_GetRuntimeClassName( impl->IInspectable_inner, class_name );
}

static HRESULT WINAPI controller_GetTrustLevel( IGameController *iface, TrustLevel *trust_level )
{
    struct controller *impl = impl_from_IGameController( iface );
    return IInspectable_GetTrustLevel( impl->IInspectable_inner, trust_level );
}

static HRESULT WINAPI controller_add_HeadsetConnected( IGameController *iface, ITypedEventHandler_IGameController_Headset *handler,
                                                       EventRegistrationToken *token )
{
    static LONG64 next_token;

    /* never raised; C++/CX callers throw on a failed registration */
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI controller_remove_HeadsetConnected( IGameController *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI controller_add_HeadsetDisconnected( IGameController *iface, ITypedEventHandler_IGameController_Headset *handler,
                                                          EventRegistrationToken *token )
{
    static LONG64 next_token;

    /* never raised; C++/CX callers throw on a failed registration */
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI controller_remove_HeadsetDisconnected( IGameController *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI controller_add_UserChanged( IGameController *iface,
                                                  ITypedEventHandler_IGameController_UserChangedEventArgs *handler,
                                                  EventRegistrationToken *token )
{
    static LONG64 next_token;

    /* never raised; C++/CX callers throw on a failed registration */
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI controller_remove_UserChanged( IGameController *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI controller_get_Headset( IGameController *iface, IHeadset **value )
{
    TRACE( "iface %p, value %p: no headset.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI controller_get_IsWireless( IGameController *iface, boolean *value )
{
    FIXME( "iface %p, value %p semi-stub, reporting wired.\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI controller_get_User( IGameController *iface, __x_ABI_CWindows_CSystem_CIUser **value )
{
    /* every controller belongs to the prefix's one user: User.GetDefault() */
    static const GUID iid_IUserStatics2 = {0x74a37e11, 0x2eb5, 0x4487, {0xb0, 0xd5, 0x2c, 0x67, 0x90, 0xe0, 0x13, 0xe9}};
    static const WCHAR user_class[] = L"Windows.System.User";
    __x_ABI_CWindows_CSystem_CIUserStatics2 *statics;
    HSTRING str;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = WindowsCreateString( user_class, wcslen( user_class ), &str ))) return hr;
    hr = RoGetActivationFactory( str, &iid_IUserStatics2, (void **)&statics );
    WindowsDeleteString( str );
    if (FAILED(hr))
    {
        WARN( "no Windows.System.User, hr %#lx: no user.\n", hr );
        return S_OK;
    }
    hr = __x_ABI_CWindows_CSystem_CIUserStatics2_GetDefault( statics, value );
    __x_ABI_CWindows_CSystem_CIUserStatics2_Release( statics );
    return hr;
}

static const struct IGameControllerVtbl controller_vtbl =
{
    controller_QueryInterface,
    controller_AddRef,
    controller_Release,
    /* IInspectable methods */
    controller_GetIids,
    controller_GetRuntimeClassName,
    controller_GetTrustLevel,
    /* IGameController methods */
    controller_add_HeadsetConnected,
    controller_remove_HeadsetConnected,
    controller_add_HeadsetDisconnected,
    controller_remove_HeadsetDisconnected,
    controller_add_UserChanged,
    controller_remove_UserChanged,
    controller_get_Headset,
    controller_get_IsWireless,
    controller_get_User,
};

DEFINE_IINSPECTABLE( battery, IGameControllerBatteryInfo, struct controller, IGameController_iface )

static HRESULT WINAPI battery_TryGetBatteryReport( IGameControllerBatteryInfo *iface, IBatteryReport **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct IGameControllerBatteryInfoVtbl battery_vtbl =
{
    battery_QueryInterface,
    battery_AddRef,
    battery_Release,
    /* IInspectable methods */
    battery_GetIids,
    battery_GetRuntimeClassName,
    battery_GetTrustLevel,
    /* IGameControllerBatteryInfo methods */
    battery_TryGetBatteryReport,
};

struct manager_statics
{
    IActivationFactory IActivationFactory_iface;
    IGameControllerFactoryManagerStatics IGameControllerFactoryManagerStatics_iface;
    IGameControllerFactoryManagerStatics2 IGameControllerFactoryManagerStatics2_iface;
    LONG ref;
};

static inline struct manager_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct manager_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct manager_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameControllerFactoryManagerStatics ))
    {
        IInspectable_AddRef( (*out = &impl->IGameControllerFactoryManagerStatics_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IGameControllerFactoryManagerStatics2 ))
    {
        IInspectable_AddRef( (*out = &impl->IGameControllerFactoryManagerStatics2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( statics, IGameControllerFactoryManagerStatics, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI
statics_RegisterCustomFactoryForGipInterface( IGameControllerFactoryManagerStatics *iface,
                                              ICustomGameControllerFactory *factory,
                                              GUID interface_id )
{
    struct gip_factory_registration *entry, *registration;

    TRACE( "iface %p, factory %p, interface_id %s.\n", iface, factory, debugstr_guid(&interface_id) );
    if (!factory) return E_INVALIDARG;
    if (!(registration = malloc(sizeof(*registration)))) return E_OUTOFMEMORY;
    registration->interface_id = interface_id;
    ICustomGameControllerFactory_AddRef( (registration->factory = factory) );

    EnterCriticalSection( &manager_cs );
    LIST_FOR_EACH_ENTRY( entry, &gip_factories, struct gip_factory_registration, entry )
    {
        if (entry->factory == factory && IsEqualGUID( &entry->interface_id, &interface_id ))
        {
            LeaveCriticalSection( &manager_cs );
            ICustomGameControllerFactory_Release( factory );
            free( registration );
            return S_OK;
        }
    }
    /* Registrations last for the process lifetime. Wine currently exposes no GIP
     * providers, so no existing controller matches this factory. */
    list_add_tail( &gip_factories, &registration->entry );
    LeaveCriticalSection( &manager_cs );
    return S_OK;
}

static HRESULT WINAPI
statics_RegisterCustomFactoryForHardwareId( IGameControllerFactoryManagerStatics *iface,
                                            ICustomGameControllerFactory *factory,
                                            UINT16 vendor_id, UINT16 product_id )
{
    struct hardware_factory_registration *entry, *registration;
    ICustomGameControllerFactory *resolved;
    struct controller **items;
    struct list *cursor;
    unsigned int count, i, j;
    BOOL match;
    HRESULT hr;

    TRACE( "iface %p, factory %p, vendor_id %u, product_id %u.\n", iface, factory, vendor_id, product_id );
    if (!factory) return E_INVALIDARG;
    if (!(registration = calloc( 1, sizeof(*registration) ))) return E_OUTOFMEMORY;
    registration->vendor_id = vendor_id;
    registration->product_id = product_id;
    hr = RoGetAgileReference( AGILEREFERENCE_DEFAULT, &IID_ICustomGameControllerFactory,
                             (IUnknown *)factory, &registration->reference );
    if (FAILED(hr))
    {
        free( registration );
        return hr;
    }

    EnterCriticalSection( &manager_cs );
    cursor = hardware_factories.next;
    while (cursor != &hardware_factories)
    {
        entry = LIST_ENTRY( cursor, struct hardware_factory_registration, entry );
        LeaveCriticalSection( &manager_cs );
        match = FALSE;
        if (entry->vendor_id == vendor_id && entry->product_id == product_id &&
            SUCCEEDED(IAgileReference_Resolve( entry->reference, &IID_ICustomGameControllerFactory, (void **)&resolved )))
        {
            match = object_matches( (IUnknown *)resolved, (IUnknown *)factory );
            ICustomGameControllerFactory_Release( resolved );
        }
        if (match)
        {
            IAgileReference_Release( registration->reference );
            free( registration );
            return S_OK;
        }
        EnterCriticalSection( &manager_cs );
        /* Registrations are never removed. Read next after resolving, so concurrent appends are included. */
        cursor = entry->entry.next;
    }
    hr = controller_snapshot( &items, &count );
    if (SUCCEEDED(hr)) list_add_tail( &hardware_factories, &registration->entry );
    LeaveCriticalSection( &manager_cs );
    if (FAILED(hr))
    {
        IAgileReference_Release( registration->reference );
        free( registration );
        return hr;
    }
    for (i = 0; i < count; ++i)
    {
        for (j = 0; j < i; ++j) if (items[j]->provider == items[i]->provider) break;
        if (j == i) hardware_factory_add_controller( registration, items[i]->provider );
    }
    for (i = 0; i < count; ++i) IGameController_Release( &items[i]->IGameController_iface );
    free( items );
    return S_OK;
}

static HRESULT WINAPI
statics_RegisterCustomFactoryForXusbType( IGameControllerFactoryManagerStatics *iface,
                                          ICustomGameControllerFactory *factory,
                                          XusbDeviceType type, XusbDeviceSubtype subtype )
{
    FIXME( "iface %p, factory %p, type %d, subtype %d stub!\n", iface, factory, type, subtype );
    return E_NOTIMPL;
}

static const struct IGameControllerFactoryManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IGameControllerFactoryManagerStatics methods */
    statics_RegisterCustomFactoryForGipInterface,
    statics_RegisterCustomFactoryForHardwareId,
    statics_RegisterCustomFactoryForXusbType,
};

DEFINE_IINSPECTABLE( statics2, IGameControllerFactoryManagerStatics2, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI
statics2_TryGetFactoryControllerFromGameController( IGameControllerFactoryManagerStatics2 *iface,
                                                    ICustomGameControllerFactory *factory,
                                                    IGameController *controller, IGameController **value )
{
    struct controller **items;
    IGameControllerProvider *provider = NULL;
    IGameController *current;
    unsigned int count, i;
    BOOL removed;
    HRESULT hr;

    TRACE( "iface %p, factory %p, controller %p, value %p.\n", iface, factory, controller, value );
    if (!value) return E_POINTER;
    *value = NULL;
    if (!factory || !controller) return E_INVALIDARG;
    if (FAILED(hr = controller_snapshot( &items, &count ))) return hr;
    for (i = 0; i < count; ++i)
    {
        if (FAILED(controller_get_public_iface( items[i], &current ))) continue;
        if (object_matches( (IUnknown *)current, (IUnknown *)controller )) provider = items[i]->provider;
        IGameController_Release( current );
        if (provider) break;
    }
    if (provider) for (i = 0; i < count; ++i)
    {
        if (items[i]->provider != provider || !controller_matches_factory( items[i], factory )) continue;
        if (FAILED(controller_get_public_iface( items[i], &current ))) continue;
        EnterCriticalSection( &manager_cs );
        removed = items[i]->removed;
        LeaveCriticalSection( &manager_cs );
        if (removed) IGameController_Release( current );
        else *value = current;
        break;
    }
    for (i = 0; i < count; ++i) IGameController_Release( &items[i]->IGameController_iface );
    free( items );
    return S_OK;
}

static const struct IGameControllerFactoryManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IGameControllerFactoryManagerStatics2 methods */
    statics2_TryGetFactoryControllerFromGameController,
};

static struct manager_statics manager_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    1,
};

IGameControllerFactoryManagerStatics2 *manager_factory = &manager_statics.IGameControllerFactoryManagerStatics2_iface;

static HRESULT controller_create( ICustomGameControllerFactory *factory, IGameControllerProvider *provider,
                                  struct controller **out )
{
    IGameControllerImpl *inner_impl;
    struct controller *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IGameController_iface.lpVtbl = &controller_vtbl;
    impl->IGameControllerBatteryInfo_iface.lpVtbl = &battery_vtbl;
    impl->ref = 1;
    impl->IInspectable_inner = NULL;

    if (FAILED(hr = ICustomGameControllerFactory_CreateGameController( factory, provider, &impl->IInspectable_inner )))
        WARN( "Failed to create game controller, hr %#lx\n", hr );
    else if (!impl->IInspectable_inner) hr = E_FAIL;
    else if (FAILED(hr = IInspectable_QueryInterface( impl->IInspectable_inner, &IID_IGameControllerImpl, (void **)&inner_impl )))
        WARN( "Failed to find IGameControllerImpl iface, hr %#lx\n", hr );
    else
    {
        if (FAILED(hr = IGameControllerImpl_Initialize( inner_impl, &impl->IGameController_iface, provider )))
            WARN( "Failed to initialize game controller, hr %#lx\n", hr );
        IGameControllerImpl_Release( inner_impl );
    }

    if (FAILED(hr))
    {
        if (impl->IInspectable_inner) IInspectable_Release( impl->IInspectable_inner );
        free( impl );
        return hr;
    }

    ICustomGameControllerFactory_AddRef( (impl->factory = factory) );
    IGameControllerProvider_AddRef( (impl->provider = provider) );

    *out = impl;
    return S_OK;
}

static void custom_controller_removed( struct controller *controller )
{
    ICustomGameControllerFactory *factory;
    IGameControllerInputSink *sink;
    IGameController *public_iface;

    if (FAILED(IAgileReference_Resolve( controller->registration->reference,
                                       &IID_ICustomGameControllerFactory, (void **)&factory ))) return;
    if (SUCCEEDED(controller_get_public_iface( controller, &public_iface )))
    {
        if (SUCCEEDED(IGameController_QueryInterface( public_iface, &IID_IGameControllerInputSink, (void **)&sink )))
        {
            IGameControllerInputSink_OnInputSuspended( sink, GetTickCount64() * 10000 );
            IGameControllerInputSink_Release( sink );
        }
        ICustomGameControllerFactory_OnGameControllerRemoved( factory, public_iface );
        IGameController_Release( public_iface );
    }
    ICustomGameControllerFactory_Release( factory );
}

/* Reserve the provider/factory pair under the lock. Marshal and notify without it. */
static void hardware_factory_add_controller( struct hardware_factory_registration *registration,
                                            IGameControllerProvider *provider )
{
    ICustomGameControllerFactory *factory = NULL;
    IGameController *public_iface = NULL;
    IInspectable *inner = NULL;
    IGameControllerInputSink *sink;
    IAgileReference *reference = NULL;
    struct controller *entry, *controller;
    BOOL present = FALSE, duplicate = FALSE, notify, release_list = FALSE;
    UINT16 vendor_id, product_id;
    HRESULT hr;

    if (FAILED(IGameControllerProvider_get_HardwareVendorId( provider, &vendor_id )) ||
        FAILED(IGameControllerProvider_get_HardwareProductId( provider, &product_id )) ||
        vendor_id != registration->vendor_id || product_id != registration->product_id) return;
    if (!(controller = calloc( 1, sizeof(*controller) ))) return;
    controller->IGameController_iface.lpVtbl = &controller_vtbl;
    controller->ref = 1;
    controller->registration = registration;
    controller->busy = TRUE;
    IGameControllerProvider_AddRef( (controller->provider = provider) );

    EnterCriticalSection( &manager_cs );
    LIST_FOR_EACH_ENTRY( entry, &controller_list, struct controller, entry )
    {
        if (entry->provider != provider) continue;
        present = TRUE;
        if (entry->registration == registration) { duplicate = TRUE; break; }
    }
    if (!present || duplicate)
    {
        LeaveCriticalSection( &manager_cs );
        IGameController_Release( &controller->IGameController_iface );
        return;
    }
    IGameController_AddRef( &controller->IGameController_iface ); /* list reference */
    list_add_tail( &controller_list, &controller->entry );
    LeaveCriticalSection( &manager_cs );

    hr = IAgileReference_Resolve( registration->reference, &IID_ICustomGameControllerFactory, (void **)&factory );
    if (SUCCEEDED(hr)) hr = ICustomGameControllerFactory_CreateGameController( factory, provider, &inner );
    if (SUCCEEDED(hr) && !inner) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IInspectable_QueryInterface( inner, &IID_IGameController, (void **)&public_iface );
    if (SUCCEEDED(hr) && !public_iface) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = RoGetAgileReference( AGILEREFERENCE_DEFAULT, &IID_IGameController,
                                               (IUnknown *)public_iface, &reference );
    EnterCriticalSection( &manager_cs );
    controller->custom_ref = reference;
    notify = SUCCEEDED(hr) && !controller->removed;
    if (FAILED(hr) && !controller->removed)
    {
        list_remove( &controller->entry );
        controller->removed = TRUE;
        release_list = TRUE;
    }
    LeaveCriticalSection( &manager_cs );
    if (notify)
    {
        if (SUCCEEDED(IGameController_QueryInterface( public_iface, &IID_IGameControllerInputSink, (void **)&sink )))
        {
            IGameControllerInputSink_OnInputResumed( sink, GetTickCount64() * 10000 );
            IGameControllerInputSink_Release( sink );
        }
        ICustomGameControllerFactory_OnGameControllerAdded( factory, public_iface );
    }
    else if (FAILED(hr)) WARN( "Custom controller creation failed, hr %#lx\n", hr );
    EnterCriticalSection( &manager_cs );
    controller->busy = FALSE;
    notify = notify && controller->removed;
    LeaveCriticalSection( &manager_cs );
    if (notify) custom_controller_removed( controller );
    if (public_iface) IGameController_Release( public_iface );
    if (inner) IInspectable_Release( inner );
    if (factory) ICustomGameControllerFactory_Release( factory );
    if (release_list) IGameController_Release( &controller->IGameController_iface );
    IGameController_Release( &controller->IGameController_iface );
}

void manager_on_provider_created( IGameControllerProvider *provider )
{
    struct hardware_factory_registration *registration;
    IWineGameControllerProvider *wine_provider;
    struct controller *created[2];
    struct list *cursor;
    WineGameControllerType type;
    unsigned int count = 0, i;
    HRESULT hr;

    TRACE( "provider %p\n", provider );
    if (FAILED(IGameControllerProvider_QueryInterface( provider, &IID_IWineGameControllerProvider,
                                                       (void **)&wine_provider ))) return;
    if (FAILED(hr = IWineGameControllerProvider_get_Type( wine_provider, &type ))) type = WineGameControllerType_Joystick;
    IWineGameControllerProvider_Release( wine_provider );

    if (SUCCEEDED(controller_create( controller_factory, provider, &created[count] ))) count++;
    if (type == WineGameControllerType_Gamepad &&
        SUCCEEDED(controller_create( gamepad_factory, provider, &created[count] ))) count++;
    if (type == WineGameControllerType_RacingWheel &&
        SUCCEEDED(controller_create( racing_wheel_factory, provider, &created[count] ))) count++;
    EnterCriticalSection( &manager_cs );
    for (i = 0; i < count; ++i)
    {
        IGameController_AddRef( &created[i]->IGameController_iface );
        list_add_tail( &controller_list, &created[i]->entry );
    }
    LeaveCriticalSection( &manager_cs );
    for (i = 0; i < count; ++i)
    {
        ICustomGameControllerFactory_OnGameControllerAdded( created[i]->factory, &created[i]->IGameController_iface );
        IGameController_Release( &created[i]->IGameController_iface );
    }

    EnterCriticalSection( &manager_cs );
    cursor = hardware_factories.next;
    while (cursor != &hardware_factories)
    {
        registration = LIST_ENTRY( cursor, struct hardware_factory_registration, entry );
        LeaveCriticalSection( &manager_cs );
        hardware_factory_add_controller( registration, provider );
        EnterCriticalSection( &manager_cs );
        cursor = registration->entry.next;
    }
    LeaveCriticalSection( &manager_cs );
}

void manager_on_provider_removed( IGameControllerProvider *provider )
{
    struct controller *controller, *next;
    struct list notify = LIST_INIT( notify ), pending = LIST_INIT( pending );

    TRACE( "provider %p\n", provider );
    EnterCriticalSection( &manager_cs );
    LIST_FOR_EACH_ENTRY_SAFE( controller, next, &controller_list, struct controller, entry )
    {
        if (controller->provider != provider) continue;
        controller->removed = TRUE;
        list_remove( &controller->entry );
        list_add_tail( controller->busy ? &pending : &notify, &controller->entry );
    }
    LeaveCriticalSection( &manager_cs );
    LIST_FOR_EACH_ENTRY_SAFE( controller, next, &notify, struct controller, entry )
    {
        list_remove( &controller->entry );
        if (controller->registration) custom_controller_removed( controller );
        else ICustomGameControllerFactory_OnGameControllerRemoved( controller->factory, &controller->IGameController_iface );
        IGameController_Release( &controller->IGameController_iface );
    }
    LIST_FOR_EACH_ENTRY_SAFE( controller, next, &pending, struct controller, entry )
    {
        list_remove( &controller->entry );
        /* The creating thread sends removal after its added callback returns. */
        IGameController_Release( &controller->IGameController_iface );
    }
}

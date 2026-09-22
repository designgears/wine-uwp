/* WinRT Windows.Networking.EndpointPair Implementation
 *
 * Copyright (C) 2026 the Wine project
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

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(hostname);

struct endpoint_pair
{
    IEndpointPair IEndpointPair_iface;
    LONG ref;

    IHostName *local_host;
    HSTRING local_service;
    IHostName *remote_host;
    HSTRING remote_service;
};

static inline struct endpoint_pair *impl_from_IEndpointPair( IEndpointPair *iface )
{
    return CONTAINING_RECORD( iface, struct endpoint_pair, IEndpointPair_iface );
}

static HRESULT WINAPI endpoint_pair_QueryInterface( IEndpointPair *iface, REFIID iid, void **out )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IEndpointPair ))
    {
        *out = &impl->IEndpointPair_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI endpoint_pair_AddRef( IEndpointPair *iface )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI endpoint_pair_Release( IEndpointPair *iface )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->local_host) IHostName_Release( impl->local_host );
        if (impl->remote_host) IHostName_Release( impl->remote_host );
        WindowsDeleteString( impl->local_service );
        WindowsDeleteString( impl->remote_service );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI endpoint_pair_GetIids( IEndpointPair *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI endpoint_pair_GetRuntimeClassName( IEndpointPair *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_EndpointPair,
                                ARRAY_SIZE(RuntimeClass_Windows_Networking_EndpointPair) - 1, class_name );
}

static HRESULT WINAPI endpoint_pair_GetTrustLevel( IEndpointPair *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT get_host( IHostName *host, IHostName **value )
{
    if (!value) return E_POINTER;
    *value = host;
    if (host) IHostName_AddRef( host );
    return S_OK;
}

static HRESULT put_host( IHostName **slot, IHostName *value )
{
    if (value) IHostName_AddRef( value );
    if (*slot) IHostName_Release( *slot );
    *slot = value;
    return S_OK;
}

static HRESULT get_service( HSTRING service, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( service, value );
}

static HRESULT put_service( HSTRING *slot, HSTRING value )
{
    HSTRING copy = NULL;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *slot );
    *slot = copy;
    return S_OK;
}

static HRESULT WINAPI endpoint_pair_get_LocalHostName( IEndpointPair *iface, IHostName **value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_host( impl->local_host, value );
}

static HRESULT WINAPI endpoint_pair_put_LocalHostName( IEndpointPair *iface, IHostName *value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return put_host( &impl->local_host, value );
}

static HRESULT WINAPI endpoint_pair_get_LocalServiceName( IEndpointPair *iface, HSTRING *value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_service( impl->local_service, value );
}

static HRESULT WINAPI endpoint_pair_put_LocalServiceName( IEndpointPair *iface, HSTRING value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return put_service( &impl->local_service, value );
}

static HRESULT WINAPI endpoint_pair_get_RemoteHostName( IEndpointPair *iface, IHostName **value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_host( impl->remote_host, value );
}

static HRESULT WINAPI endpoint_pair_put_RemoteHostName( IEndpointPair *iface, IHostName *value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return put_host( &impl->remote_host, value );
}

static HRESULT WINAPI endpoint_pair_get_RemoteServiceName( IEndpointPair *iface, HSTRING *value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_service( impl->remote_service, value );
}

static HRESULT WINAPI endpoint_pair_put_RemoteServiceName( IEndpointPair *iface, HSTRING value )
{
    struct endpoint_pair *impl = impl_from_IEndpointPair( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return put_service( &impl->remote_service, value );
}

static const struct IEndpointPairVtbl endpoint_pair_vtbl =
{
    endpoint_pair_QueryInterface,
    endpoint_pair_AddRef,
    endpoint_pair_Release,
    /* IInspectable methods */
    endpoint_pair_GetIids,
    endpoint_pair_GetRuntimeClassName,
    endpoint_pair_GetTrustLevel,
    /* IEndpointPair methods */
    endpoint_pair_get_LocalHostName,
    endpoint_pair_put_LocalHostName,
    endpoint_pair_get_LocalServiceName,
    endpoint_pair_put_LocalServiceName,
    endpoint_pair_get_RemoteHostName,
    endpoint_pair_put_RemoteHostName,
    endpoint_pair_get_RemoteServiceName,
    endpoint_pair_put_RemoteServiceName,
};

struct endpoint_pair_statics
{
    IActivationFactory IActivationFactory_iface;
    IEndpointPairFactory IEndpointPairFactory_iface;
    LONG ref;
};

static inline struct endpoint_pair_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct endpoint_pair_statics, IActivationFactory_iface );
}

static HRESULT WINAPI endpoint_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct endpoint_pair_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IEndpointPairFactory ))
    {
        *out = &impl->IEndpointPairFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI endpoint_factory_AddRef( IActivationFactory *iface )
{
    struct endpoint_pair_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI endpoint_factory_Release( IActivationFactory *iface )
{
    struct endpoint_pair_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI endpoint_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI endpoint_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_EndpointPair,
                                ARRAY_SIZE(RuntimeClass_Windows_Networking_EndpointPair) - 1, class_name );
}

static HRESULT WINAPI endpoint_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT create_endpoint_pair( IHostName *local_host, HSTRING local_service,
                                     IHostName *remote_host, HSTRING remote_service,
                                     IEndpointPair **value )
{
    struct endpoint_pair *impl;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = NULL;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IEndpointPair_iface.lpVtbl = &endpoint_pair_vtbl;
    impl->ref = 1;

    if (FAILED(hr = WindowsDuplicateString( local_service, &impl->local_service )) ||
        FAILED(hr = WindowsDuplicateString( remote_service, &impl->remote_service )))
    {
        WindowsDeleteString( impl->local_service );
        free( impl );
        return hr;
    }

    if ((impl->local_host = local_host)) IHostName_AddRef( local_host );
    if ((impl->remote_host = remote_host)) IHostName_AddRef( remote_host );

    *value = &impl->IEndpointPair_iface;
    return S_OK;
}

/* IEndpointPairFactory */

static inline struct endpoint_pair_statics *impl_from_IEndpointPairFactory( IEndpointPairFactory *iface )
{
    return CONTAINING_RECORD( iface, struct endpoint_pair_statics, IEndpointPairFactory_iface );
}

static HRESULT WINAPI endpoint_pair_factory_QueryInterface( IEndpointPairFactory *iface, REFIID iid, void **out )
{
    struct endpoint_pair_statics *impl = impl_from_IEndpointPairFactory( iface );
    return IActivationFactory_QueryInterface( &impl->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI endpoint_pair_factory_AddRef( IEndpointPairFactory *iface )
{
    struct endpoint_pair_statics *impl = impl_from_IEndpointPairFactory( iface );
    return IActivationFactory_AddRef( &impl->IActivationFactory_iface );
}

static ULONG WINAPI endpoint_pair_factory_Release( IEndpointPairFactory *iface )
{
    struct endpoint_pair_statics *impl = impl_from_IEndpointPairFactory( iface );
    return IActivationFactory_Release( &impl->IActivationFactory_iface );
}

static HRESULT WINAPI endpoint_pair_factory_GetIids( IEndpointPairFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI endpoint_pair_factory_GetRuntimeClassName( IEndpointPairFactory *iface, HSTRING *class_name )
{
    struct endpoint_pair_statics *impl = impl_from_IEndpointPairFactory( iface );
    return IActivationFactory_GetRuntimeClassName( &impl->IActivationFactory_iface, class_name );
}

static HRESULT WINAPI endpoint_pair_factory_GetTrustLevel( IEndpointPairFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI endpoint_pair_factory_CreateEndpointPair( IEndpointPairFactory *iface, IHostName *local_host,
                                                                HSTRING local_service, IHostName *remote_host,
                                                                HSTRING remote_service, IEndpointPair **value )
{
    TRACE( "iface %p, local_host %p, local_service %s, remote_host %p, remote_service %s, value %p.\n",
           iface, local_host, debugstr_hstring( local_service ), remote_host,
           debugstr_hstring( remote_service ), value );
    return create_endpoint_pair( local_host, local_service, remote_host, remote_service, value );
}

static const struct IEndpointPairFactoryVtbl endpoint_pair_factory_vtbl =
{
    endpoint_pair_factory_QueryInterface,
    endpoint_pair_factory_AddRef,
    endpoint_pair_factory_Release,
    /* IInspectable methods */
    endpoint_pair_factory_GetIids,
    endpoint_pair_factory_GetRuntimeClassName,
    endpoint_pair_factory_GetTrustLevel,
    /* IEndpointPairFactory methods */
    endpoint_pair_factory_CreateEndpointPair,
};

static HRESULT WINAPI endpoint_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    IEndpointPair *pair;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (FAILED(hr = create_endpoint_pair( NULL, NULL, NULL, NULL, &pair ))) return hr;
    *instance = (IInspectable *)pair;
    return S_OK;
}

static const struct IActivationFactoryVtbl endpoint_factory_vtbl =
{
    endpoint_factory_QueryInterface,
    endpoint_factory_AddRef,
    endpoint_factory_Release,
    /* IInspectable methods */
    endpoint_factory_GetIids,
    endpoint_factory_GetRuntimeClassName,
    endpoint_factory_GetTrustLevel,
    /* IActivationFactory methods */
    endpoint_factory_ActivateInstance,
};

static struct endpoint_pair_statics endpoint_pair_statics =
{
    {&endpoint_factory_vtbl},
    {&endpoint_pair_factory_vtbl},
    1,
};

IActivationFactory *endpoint_pair_factory = &endpoint_pair_statics.IActivationFactory_iface;

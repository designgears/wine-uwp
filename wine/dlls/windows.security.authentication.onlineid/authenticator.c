/* WinRT Windows.Security.Authentication.Onlineid Implementation
 *
 * Copyright (C) 2024 Mohamad Al-Jaf
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

WINE_DEFAULT_DEBUG_CHANNEL(onlineid);

struct authenticator_statics
{
    IActivationFactory IActivationFactory_iface;
    IOnlineIdSystemAuthenticatorStatics IOnlineIdSystemAuthenticatorStatics_iface;
    LONG ref;
};

static inline struct authenticator_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct authenticator_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct authenticator_statics *impl = impl_from_IActivationFactory( iface );

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

    if (IsEqualGUID( iid, &IID_IOnlineIdSystemAuthenticatorStatics ))
    {
        *out = &impl->IOnlineIdSystemAuthenticatorStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct authenticator_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct authenticator_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
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

struct authenticator
{
    IOnlineIdSystemAuthenticatorForUser IOnlineIdSystemAuthenticatorForUser_iface;
    LONG ref;
    __x_ABI_CWindows_CSystem_CIUser *user;   /* GetForUser's; NULL for Default */
    GUID application_id;
};

static inline struct authenticator *impl_from_IOnlineIdSystemAuthenticatorForUser( IOnlineIdSystemAuthenticatorForUser *iface )
{
    return CONTAINING_RECORD( iface, struct authenticator, IOnlineIdSystemAuthenticatorForUser_iface );
}

static HRESULT WINAPI authenticator_QueryInterface( IOnlineIdSystemAuthenticatorForUser *iface, REFIID iid, void **out )
{
    struct authenticator *impl = impl_from_IOnlineIdSystemAuthenticatorForUser( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IOnlineIdSystemAuthenticatorForUser ))
    {
        *out = &impl->IOnlineIdSystemAuthenticatorForUser_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI authenticator_AddRef( IOnlineIdSystemAuthenticatorForUser *iface )
{
    struct authenticator *impl = impl_from_IOnlineIdSystemAuthenticatorForUser( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI authenticator_Release( IOnlineIdSystemAuthenticatorForUser *iface )
{
    struct authenticator *impl = impl_from_IOnlineIdSystemAuthenticatorForUser( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->user) __x_ABI_CWindows_CSystem_CIUser_Release( impl->user );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI authenticator_GetIids( IOnlineIdSystemAuthenticatorForUser *iface, ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI authenticator_GetRuntimeClassName( IOnlineIdSystemAuthenticatorForUser *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticatorForUser";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI authenticator_GetTrustLevel( IOnlineIdSystemAuthenticatorForUser *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* No Microsoft account is connected to this Windows user, so every ticket request ends in an error result, as it does
 * on Windows for a local account: the operation completes at once (a never-completing one would park the caller). */
static HRESULT WINAPI authenticator_GetTicketAsync( IOnlineIdSystemAuthenticatorForUser *iface, IOnlineIdServiceTicketRequest *request,
                                                    IAsyncOperation_OnlineIdSystemTicketResult **operation )
{
    TRACE( "iface %p, request %p, operation %p.\n", iface, request, operation );
    if (!operation) return E_POINTER;
    return ticket_result_operation_create( OnlineIdSystemTicketStatus_Error,
                                           HRESULT_FROM_WIN32( ERROR_NO_SUCH_LOGON_SESSION ), operation );
}

static HRESULT WINAPI authenticator_put_ApplicationId( IOnlineIdSystemAuthenticatorForUser *iface, GUID value )
{
    struct authenticator *impl = impl_from_IOnlineIdSystemAuthenticatorForUser( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_guid( &value ) );
    impl->application_id = value;
    return S_OK;
}

static HRESULT WINAPI authenticator_get_ApplicationId( IOnlineIdSystemAuthenticatorForUser *iface, GUID *value )
{
    struct authenticator *impl = impl_from_IOnlineIdSystemAuthenticatorForUser( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->application_id;
    return S_OK;
}

static HRESULT WINAPI authenticator_get_User( IOnlineIdSystemAuthenticatorForUser *iface, __x_ABI_CWindows_CSystem_CIUser **user )
{
    struct authenticator *impl = impl_from_IOnlineIdSystemAuthenticatorForUser( iface );
    TRACE( "iface %p, user %p.\n", iface, user );
    if (!user) return E_POINTER;
    if ((*user = impl->user)) __x_ABI_CWindows_CSystem_CIUser_AddRef( *user );
    return S_OK;
}

static const struct IOnlineIdSystemAuthenticatorForUserVtbl authenticator_vtbl =
{
    authenticator_QueryInterface,
    authenticator_AddRef,
    authenticator_Release,
    /* IInspectable methods */
    authenticator_GetIids,
    authenticator_GetRuntimeClassName,
    authenticator_GetTrustLevel,
    /* IOnlineIdSystemAuthenticatorForUser methods */
    authenticator_GetTicketAsync,
    authenticator_put_ApplicationId,
    authenticator_get_ApplicationId,
    authenticator_get_User,
};

DEFINE_IINSPECTABLE( authenticator_statics, IOnlineIdSystemAuthenticatorStatics, struct authenticator_statics, IActivationFactory_iface )

static HRESULT authenticator_create( __x_ABI_CWindows_CSystem_CIUser *user, IOnlineIdSystemAuthenticatorForUser **value )
{
    struct authenticator *impl;

    if (!value) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IOnlineIdSystemAuthenticatorForUser_iface.lpVtbl = &authenticator_vtbl;
    impl->ref = 1;
    if ((impl->user = user)) __x_ABI_CWindows_CSystem_CIUser_AddRef( user );

    *value = &impl->IOnlineIdSystemAuthenticatorForUser_iface;
    TRACE( "created IOnlineIdSystemAuthenticatorForUser %p for user %p.\n", *value, user );
    return S_OK;
}

static HRESULT WINAPI authenticator_statics_get_Default( IOnlineIdSystemAuthenticatorStatics *iface,
                                                         IOnlineIdSystemAuthenticatorForUser **value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    return authenticator_create( NULL, value );
}

/* The Microsoft Store asks for the signed-in user's authenticator as a product page opens; E_NOTIMPL here was
 * rethrown and the page lost its buy box. */
static HRESULT WINAPI authenticator_statics_GetForUser( IOnlineIdSystemAuthenticatorStatics *iface, __x_ABI_CWindows_CSystem_CIUser *user,
                                                        IOnlineIdSystemAuthenticatorForUser **value )
{
    TRACE( "iface %p, user %p, value %p.\n", iface, user, value );
    if (!user) return E_INVALIDARG;
    return authenticator_create( user, value );
}

static const struct IOnlineIdSystemAuthenticatorStaticsVtbl authenticator_statics_vtbl =
{
    authenticator_statics_QueryInterface,
    authenticator_statics_AddRef,
    authenticator_statics_Release,
    /* IInspectable methods */
    authenticator_statics_GetIids,
    authenticator_statics_GetRuntimeClassName,
    authenticator_statics_GetTrustLevel,
    /* IOnlineIdSystemAuthenticatorStatics methods */
    authenticator_statics_get_Default,
    authenticator_statics_GetForUser,
};

static struct authenticator_statics authenticator_statics =
{
    {&factory_vtbl},
    {&authenticator_statics_vtbl},
    1,
};

IActivationFactory *authenticator_factory = &authenticator_statics.IActivationFactory_iface;

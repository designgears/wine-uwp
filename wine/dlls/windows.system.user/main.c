/*
 * Windows.System.User and Windows.System.UserPicker
 *
 * Copyright 2026 wine-uwp contributors
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

/*
 * Windows serves both classes out of process from the UserManager service, which a prefix does not have, so they
 * could not be activated at all: the Xbox app could not find its shell user and its configuration never loaded, and
 * XAL (Solitaire's Xbox sign-in) failed to initialise. There is one user here, the one running the prefix, signed in
 * locally: GetDefault, FindAllAsync and GetFromId answer with it. Its NonRoamableId is the account's SID; its
 * AccountName, DisplayName and FirstName are the account name. There is no picker UI, so UserPicker.IsSupported is
 * false, the answer callers are written to handle ("do not offer to switch users").
 */

#include "initguid.h"
#include "private.h"
#include "sddl.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(user);

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT make_string( const WCHAR *text, HSTRING *out )
{
    return WindowsCreateString( text, wcslen( text ), out );
}

static BOOL is_base_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject );
}

static HRESULT no_interface( REFIID iid, void **out )
{
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

#define STATIC_IINSPECTABLE( prefix, type, query, class ) \
    static HRESULT WINAPI prefix##_QueryInterface( type *iface, REFIID iid, void **out ) { return query( iid, out ); } \
    static ULONG WINAPI prefix##_AddRef( type *iface ) { return 2; } \
    static ULONG WINAPI prefix##_Release( type *iface ) { return 1; } \
    static HRESULT WINAPI prefix##_GetIids( type *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); } \
    static HRESULT WINAPI prefix##_GetRuntimeClassName( type *iface, HSTRING *name ) { return make_string( class, name ); } \
    static HRESULT WINAPI prefix##_GetTrustLevel( type *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

/* ---- the user ---- */

static struct
{
    IUser IUser_iface;
    IUser2 IUser2_iface;
} user;

static HRESULT user_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IUser )) *out = &user.IUser_iface;
    else if (IsEqualGUID( iid, &IID_IUser2 )) *out = &user.IUser2_iface;
    else return no_interface( iid, out );
    return S_OK;
}

STATIC_IINSPECTABLE( user_main, IUser, user_query, RuntimeClass_Windows_System_User )
STATIC_IINSPECTABLE( user2, IUser2, user_query, RuntimeClass_Windows_System_User )

static HRESULT WINAPI user_main_get_NonRoamableId( IUser *iface, HSTRING *value )
{
    WCHAR *sid = NULL;
    BYTE buffer[256];
    HANDLE token;
    DWORD len;
    HRESULT hr = E_FAIL;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ))
    {
        if (GetTokenInformation( token, TokenUser, buffer, sizeof(buffer), &len ) &&
            ConvertSidToStringSidW( ((TOKEN_USER *)buffer)->User.Sid, &sid ))
        {
            hr = make_string( sid, value );
            LocalFree( sid );
        }
        CloseHandle( token );
    }
    return hr;
}

static HRESULT WINAPI user_main_get_AuthenticationStatus( IUser *iface, UserAuthenticationStatus *value )
{
    if (!value) return E_POINTER;
    *value = UserAuthenticationStatus_LocallyAuthenticated;
    return S_OK;
}

static HRESULT WINAPI user_main_get_Type( IUser *iface, UserType *value )
{
    if (!value) return E_POINTER;
    *value = UserType_LocalUser;
    return S_OK;
}

/* the value of a KnownUserProperties name for the local account; NULL when there is none */
static HRESULT property_value( const WCHAR *name, IInspectable **out )
{
    IPropertyValueStatics *statics;
    WCHAR account[256] = L"";
    DWORD len = ARRAY_SIZE(account);
    const WCHAR *value = NULL;
    HSTRING class, str;
    HRESULT hr;

    *out = NULL;
    if (!name) return S_OK;
    GetUserNameW( account, &len );
    if (!wcscmp( name, L"AccountName" ) || !wcscmp( name, L"DisplayName" ) || !wcscmp( name, L"FirstName" )) value = account;
    else if (!wcscmp( name, L"LastName" ) || !wcscmp( name, L"ProviderName" ) || !wcscmp( name, L"PrincipalName" ) ||
             !wcscmp( name, L"DomainName" ) || !wcscmp( name, L"GuestHost" ) || !wcscmp( name, L"SessionInitiationProtocolUri" ))
        value = L"";
    TRACE( "%s -> %s\n", debugstr_w(name), debugstr_w(value) );
    if (!value) return S_OK;

    if (FAILED(hr = make_string( RuntimeClass_Windows_Foundation_PropertyValue, &class ))) return hr;
    hr = RoGetActivationFactory( class, &IID_IPropertyValueStatics, (void **)&statics );
    WindowsDeleteString( class );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = make_string( value, &str )))
    {
        hr = IPropertyValueStatics_CreateString( statics, str, out );
        WindowsDeleteString( str );
    }
    IPropertyValueStatics_Release( statics );
    return hr;
}

/* the operations here are answered when they are made: the callback hands back what was computed */
static HRESULT return_param( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    if ((result->punkVal = param)) IUnknown_AddRef( param );
    return S_OK;
}

static HRESULT WINAPI user_main_GetPropertyAsync( IUser *iface, HSTRING name, IAsyncOperation_IInspectable **operation )
{
    IInspectable *value;
    HRESULT hr;

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );
    if (!operation) return E_POINTER;
    if (FAILED(hr = property_value( WindowsGetStringRawBuffer( name, NULL ), &value ))) return hr;
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_IInspectable, (IUnknown *)iface, (IUnknown *)value,
                                             return_param, operation );
    if (value) IInspectable_Release( value );
    return hr;
}

static HRESULT WINAPI user_main_GetPropertiesAsync( IUser *iface, IVectorView_HSTRING *names, IAsyncOperation_IPropertySet **operation )
{
    IMap_HSTRING_IInspectable *map = NULL;
    IPropertySet *set;
    UINT32 count = 0, i;
    HSTRING class;
    HRESULT hr;

    TRACE( "iface %p, names %p, operation %p.\n", iface, names, operation );
    if (!operation) return E_POINTER;
    if (FAILED(hr = make_string( RuntimeClass_Windows_Foundation_Collections_PropertySet, &class ))) return hr;
    hr = RoActivateInstance( class, (IInspectable **)&set );
    WindowsDeleteString( class );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = IPropertySet_QueryInterface( set, &IID_IMap_HSTRING_IInspectable, (void **)&map )) && names)
    {
        IVectorView_HSTRING_get_Size( names, &count );
        for (i = 0; i < count; i++)
        {
            IInspectable *value;
            HSTRING name = NULL;
            boolean replaced;

            if (FAILED(IVectorView_HSTRING_GetAt( names, i, &name ))) continue;
            if (SUCCEEDED(property_value( WindowsGetStringRawBuffer( name, NULL ), &value )) && value)
            {
                IMap_HSTRING_IInspectable_Insert( map, name, value, &replaced );
                IInspectable_Release( value );
            }
            WindowsDeleteString( name );
        }
    }
    if (map) IMap_HSTRING_IInspectable_Release( map );
    if (SUCCEEDED(hr))
        hr = async_operation_inspectable_create( &IID_IAsyncOperation_IPropertySet, (IUnknown *)iface, (IUnknown *)set,
                                                 return_param, (IAsyncOperation_IInspectable **)operation );
    IPropertySet_Release( set );
    return hr;
}

/* there is no account picture: the operation completes with none */
static HRESULT WINAPI user_main_GetPictureAsync( IUser *iface, UserPictureSize size, IAsyncOperation_IRandomAccessStreamReference **operation )
{
    TRACE( "iface %p, size %d, operation %p.\n", iface, size, operation );
    if (!operation) return E_POINTER;
    return async_operation_inspectable_create( &IID_IAsyncOperation_IRandomAccessStreamReference, (IUnknown *)iface, NULL,
                                               return_param, (IAsyncOperation_IInspectable **)operation );
}

static const struct IUserVtbl user_vtbl =
{
    user_main_QueryInterface,
    user_main_AddRef,
    user_main_Release,
    user_main_GetIids,
    user_main_GetRuntimeClassName,
    user_main_GetTrustLevel,
    user_main_get_NonRoamableId,
    user_main_get_AuthenticationStatus,
    user_main_get_Type,
    user_main_GetPropertyAsync,
    user_main_GetPropertiesAsync,
    user_main_GetPictureAsync,
};

static HRESULT WINAPI user2_CheckUserAgeConsentGroupAsync( IUser2 *iface, UserAgeConsentGroup group,
                                                          IAsyncOperation_UserAgeConsentResult **operation )
{
    FIXME( "iface %p, group %d, operation %p: not implemented.\n", iface, group, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IUser2Vtbl user2_vtbl =
{
    user2_QueryInterface,
    user2_AddRef,
    user2_Release,
    user2_GetIids,
    user2_GetRuntimeClassName,
    user2_GetTrustLevel,
    user2_CheckUserAgeConsentGroupAsync,
};

/* ---- the list of users: this one ---- */

static struct
{
    IVectorView_User IVectorView_User_iface;
    IIterable_User IIterable_User_iface;
} users;

struct users_iterator
{
    IIterator_User IIterator_User_iface;
    LONG ref;
    UINT32 index;
};

static HRESULT users_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IVectorView_User )) *out = &users.IVectorView_User_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_User )) *out = &users.IIterable_User_iface;
    else return no_interface( iid, out );
    return S_OK;
}

STATIC_IINSPECTABLE( users_view, IVectorView_User, users_query, L"Windows.Foundation.Collections.IVectorView`1<Windows.System.User>" )
STATIC_IINSPECTABLE( users_iterable, IIterable_User, users_query, L"Windows.Foundation.Collections.IIterable`1<Windows.System.User>" )

static HRESULT WINAPI users_view_GetAt( IVectorView_User *iface, UINT32 index, IUser **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    if (index) return E_BOUNDS;
    *value = &user.IUser_iface;
    return S_OK;
}

static HRESULT WINAPI users_view_get_Size( IVectorView_User *iface, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI users_view_IndexOf( IVectorView_User *iface, IUser *element, UINT32 *index, boolean *found )
{
    *index = 0;
    *found = element == &user.IUser_iface;
    return S_OK;
}

static HRESULT WINAPI users_view_GetMany( IVectorView_User *iface, UINT32 start, UINT32 size, IUser **items, UINT32 *count )
{
    *count = 0;
    if (start == 0 && size)
    {
        items[0] = &user.IUser_iface;
        *count = 1;
    }
    return S_OK;
}

static const struct IVectorView_UserVtbl users_view_vtbl =
{
    users_view_QueryInterface,
    users_view_AddRef,
    users_view_Release,
    users_view_GetIids,
    users_view_GetRuntimeClassName,
    users_view_GetTrustLevel,
    users_view_GetAt,
    users_view_get_Size,
    users_view_IndexOf,
    users_view_GetMany,
};

static inline struct users_iterator *impl_from_IIterator_User( IIterator_User *iface )
{
    return CONTAINING_RECORD( iface, struct users_iterator, IIterator_User_iface );
}

static HRESULT WINAPI iterator_QueryInterface( IIterator_User *iface, REFIID iid, void **out )
{
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_IIterator_User )) return no_interface( iid, out );
    *out = iface;
    IIterator_User_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI iterator_AddRef( IIterator_User *iface ) { return InterlockedIncrement( &impl_from_IIterator_User( iface )->ref ); }

static ULONG WINAPI iterator_Release( IIterator_User *iface )
{
    struct users_iterator *impl = impl_from_IIterator_User( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI iterator_GetIids( IIterator_User *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_User *iface, HSTRING *name )
{
    return make_string( L"Windows.Foundation.Collections.IIterator`1<Windows.System.User>", name );
}
static HRESULT WINAPI iterator_GetTrustLevel( IIterator_User *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI iterator_get_Current( IIterator_User *iface, IUser **value )
{
    return users_view_GetAt( &users.IVectorView_User_iface, impl_from_IIterator_User( iface )->index, value );
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_User *iface, boolean *value )
{
    *value = !impl_from_IIterator_User( iface )->index;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_User *iface, boolean *value )
{
    struct users_iterator *impl = impl_from_IIterator_User( iface );
    if (!impl->index) impl->index++;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI iterator_GetMany( IIterator_User *iface, UINT32 size, IUser **items, UINT32 *count )
{
    struct users_iterator *impl = impl_from_IIterator_User( iface );
    return users_view_GetMany( &users.IVectorView_User_iface, impl->index, size, items, count );
}

static const struct IIterator_UserVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

static HRESULT WINAPI users_iterable_First( IIterable_User *iface, IIterator_User **value )
{
    struct users_iterator *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IIterator_User_iface.lpVtbl = &iterator_vtbl;
    impl->ref = 1;
    *value = &impl->IIterator_User_iface;
    return S_OK;
}

static const struct IIterable_UserVtbl users_iterable_vtbl =
{
    users_iterable_QueryInterface,
    users_iterable_AddRef,
    users_iterable_Release,
    users_iterable_GetIids,
    users_iterable_GetRuntimeClassName,
    users_iterable_GetTrustLevel,
    users_iterable_First,
};

/* ---- User's statics ---- */

static struct
{
    IActivationFactory IActivationFactory_iface;
    IUserStatics IUserStatics_iface;
    IUserStatics2 IUserStatics2_iface;
} user_factory;

static HRESULT user_factory_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &user_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IUserStatics )) *out = &user_factory.IUserStatics_iface;
    else if (IsEqualGUID( iid, &IID_IUserStatics2 )) *out = &user_factory.IUserStatics2_iface;
    else return no_interface( iid, out );
    return S_OK;
}

STATIC_IINSPECTABLE( user_activation, IActivationFactory, user_factory_query, RuntimeClass_Windows_System_User )
STATIC_IINSPECTABLE( user_statics, IUserStatics, user_factory_query, RuntimeClass_Windows_System_User )
STATIC_IINSPECTABLE( user_statics2, IUserStatics2, user_factory_query, RuntimeClass_Windows_System_User )

static HRESULT WINAPI not_activatable( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl user_activation_vtbl =
{
    user_activation_QueryInterface,
    user_activation_AddRef,
    user_activation_Release,
    user_activation_GetIids,
    user_activation_GetRuntimeClassName,
    user_activation_GetTrustLevel,
    not_activatable,
};

static HRESULT WINAPI user_statics_CreateWatcher( IUserStatics *iface, __x_ABI_CWindows_CSystem_CIUserWatcher **result )
{
    FIXME( "iface %p, result %p: no watcher, the one user never changes.\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    return E_NOTIMPL;
}

static HRESULT find_all( IUnknown *invoker, IAsyncOperation_IVectorView_User **operation )
{
    if (!operation) return E_POINTER;
    return async_operation_inspectable_create( &IID_IAsyncOperation_IVectorView_User, invoker,
                                               (IUnknown *)&users.IVectorView_User_iface, return_param,
                                               (IAsyncOperation_IInspectable **)operation );
}

static HRESULT WINAPI user_statics_FindAllAsync( IUserStatics *iface, IAsyncOperation_IVectorView_User **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return find_all( (IUnknown *)iface, operation );
}

static HRESULT WINAPI user_statics_FindAllAsyncByType( IUserStatics *iface, UserType type, IAsyncOperation_IVectorView_User **operation )
{
    TRACE( "iface %p, type %d, operation %p.\n", iface, type, operation );
    return find_all( (IUnknown *)iface, operation );
}

static HRESULT WINAPI user_statics_FindAllAsyncByTypeAndStatus( IUserStatics *iface, UserType type, UserAuthenticationStatus status,
                                                                IAsyncOperation_IVectorView_User **operation )
{
    TRACE( "iface %p, type %d, status %d, operation %p.\n", iface, type, status, operation );
    return find_all( (IUnknown *)iface, operation );
}

static HRESULT WINAPI user_statics_GetFromId( IUserStatics *iface, HSTRING id, IUser **result )
{
    TRACE( "iface %p, id %s, result %p.\n", iface, debugstr_hstring( id ), result );
    if (!result) return E_POINTER;
    *result = &user.IUser_iface;
    return S_OK;
}

static const struct IUserStaticsVtbl user_statics_vtbl =
{
    user_statics_QueryInterface,
    user_statics_AddRef,
    user_statics_Release,
    user_statics_GetIids,
    user_statics_GetRuntimeClassName,
    user_statics_GetTrustLevel,
    user_statics_CreateWatcher,
    user_statics_FindAllAsync,
    user_statics_FindAllAsyncByType,
    user_statics_FindAllAsyncByTypeAndStatus,
    user_statics_GetFromId,
};

static HRESULT WINAPI user_statics2_GetDefault( IUserStatics2 *iface, IUser **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = &user.IUser_iface;
    return S_OK;
}

static const struct IUserStatics2Vtbl user_statics2_vtbl =
{
    user_statics2_QueryInterface,
    user_statics2_AddRef,
    user_statics2_Release,
    user_statics2_GetIids,
    user_statics2_GetRuntimeClassName,
    user_statics2_GetTrustLevel,
    user_statics2_GetDefault,
};

/* ---- UserPicker ---- */

struct picker
{
    IUserPicker IUserPicker_iface;
    LONG ref;
    boolean allow_guests;
    IUser *suggested;
};

static inline struct picker *impl_from_IUserPicker( IUserPicker *iface )
{
    return CONTAINING_RECORD( iface, struct picker, IUserPicker_iface );
}

static HRESULT WINAPI picker_QueryInterface( IUserPicker *iface, REFIID iid, void **out )
{
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_IUserPicker )) return no_interface( iid, out );
    *out = iface;
    IUserPicker_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI picker_AddRef( IUserPicker *iface ) { return InterlockedIncrement( &impl_from_IUserPicker( iface )->ref ); }

static ULONG WINAPI picker_Release( IUserPicker *iface )
{
    struct picker *impl = impl_from_IUserPicker( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->suggested) IUser_Release( impl->suggested );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI picker_GetIids( IUserPicker *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI picker_GetRuntimeClassName( IUserPicker *iface, HSTRING *name ) { return make_string( RuntimeClass_Windows_System_UserPicker, name ); }
static HRESULT WINAPI picker_GetTrustLevel( IUserPicker *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI picker_get_AllowGuestAccounts( IUserPicker *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IUserPicker( iface )->allow_guests;
    return S_OK;
}

static HRESULT WINAPI picker_put_AllowGuestAccounts( IUserPicker *iface, boolean value )
{
    impl_from_IUserPicker( iface )->allow_guests = value;
    return S_OK;
}

static HRESULT WINAPI picker_get_SuggestedSelectedUser( IUserPicker *iface, IUser **value )
{
    struct picker *impl = impl_from_IUserPicker( iface );

    if (!value) return E_POINTER;
    if ((*value = impl->suggested)) IUser_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI picker_put_SuggestedSelectedUser( IUserPicker *iface, IUser *value )
{
    struct picker *impl = impl_from_IUserPicker( iface );

    if (value) IUser_AddRef( value );
    if (impl->suggested) IUser_Release( impl->suggested );
    impl->suggested = value;
    return S_OK;
}

/* IsSupported is false, so callers do not get here; there is no picker UI to show */
static HRESULT WINAPI picker_PickSingleUserAsync( IUserPicker *iface, IAsyncOperation_User **operation )
{
    FIXME( "iface %p, operation %p: no user picker.\n", iface, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IUserPickerVtbl picker_vtbl =
{
    picker_QueryInterface,
    picker_AddRef,
    picker_Release,
    picker_GetIids,
    picker_GetRuntimeClassName,
    picker_GetTrustLevel,
    picker_get_AllowGuestAccounts,
    picker_put_AllowGuestAccounts,
    picker_get_SuggestedSelectedUser,
    picker_put_SuggestedSelectedUser,
    picker_PickSingleUserAsync,
};

static struct
{
    IActivationFactory IActivationFactory_iface;
    IUserPickerStatics IUserPickerStatics_iface;
} picker_factory;

static HRESULT picker_factory_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &picker_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IUserPickerStatics )) *out = &picker_factory.IUserPickerStatics_iface;
    else return no_interface( iid, out );
    return S_OK;
}

STATIC_IINSPECTABLE( picker_activation, IActivationFactory, picker_factory_query, RuntimeClass_Windows_System_UserPicker )
STATIC_IINSPECTABLE( picker_statics, IUserPickerStatics, picker_factory_query, RuntimeClass_Windows_System_UserPicker )

static HRESULT WINAPI picker_activation_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct picker *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUserPicker_iface.lpVtbl = &picker_vtbl;
    impl->ref = 1;
    *instance = (IInspectable *)&impl->IUserPicker_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl picker_activation_vtbl =
{
    picker_activation_QueryInterface,
    picker_activation_AddRef,
    picker_activation_Release,
    picker_activation_GetIids,
    picker_activation_GetRuntimeClassName,
    picker_activation_GetTrustLevel,
    picker_activation_ActivateInstance,
};

static HRESULT WINAPI picker_statics_IsSupported( IUserPickerStatics *iface, boolean *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = FALSE;
    return S_OK;
}

static const struct IUserPickerStaticsVtbl picker_statics_vtbl =
{
    picker_statics_QueryInterface,
    picker_statics_AddRef,
    picker_statics_Release,
    picker_statics_GetIids,
    picker_statics_GetRuntimeClassName,
    picker_statics_GetTrustLevel,
    picker_statics_IsSupported,
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    user.IUser_iface.lpVtbl = &user_vtbl;
    user.IUser2_iface.lpVtbl = &user2_vtbl;
    users.IVectorView_User_iface.lpVtbl = &users_view_vtbl;
    users.IIterable_User_iface.lpVtbl = &users_iterable_vtbl;
    user_factory.IActivationFactory_iface.lpVtbl = &user_activation_vtbl;
    user_factory.IUserStatics_iface.lpVtbl = &user_statics_vtbl;
    user_factory.IUserStatics2_iface.lpVtbl = &user_statics2_vtbl;
    picker_factory.IActivationFactory_iface.lpVtbl = &picker_activation_vtbl;
    picker_factory.IUserPickerStatics_iface.lpVtbl = &picker_statics_vtbl;

    *factory = NULL;
    if (!wcscmp( name, RuntimeClass_Windows_System_User )) *factory = &user_factory.IActivationFactory_iface;
    else if (!wcscmp( name, RuntimeClass_Windows_System_UserPicker )) *factory = &picker_factory.IActivationFactory_iface;
    else return CLASS_E_CLASSNOTAVAILABLE;
    return S_OK;
}

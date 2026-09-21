/* WinRT Windows.Storage.AppDataPaths Implementation
 *
 * Copyright (C) 2026 Bobby Kircher
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
#include "shlobj.h"
#include "knownfolders.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/* AppDataPaths is stateless - every property is a known folder of the calling user - so the
 * statics, the activation factory and the single instance they hand out are one object. */
struct appdatapaths
{
    IActivationFactory IActivationFactory_iface;
    IAppDataPathsStatics IAppDataPathsStatics_iface;
    IAppDataPaths IAppDataPaths_iface;
    LONG ref;
};

static struct appdatapaths appdatapaths;

static HRESULT known_folder_string( const KNOWNFOLDERID *id, HSTRING *value )
{
    WCHAR *path;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = SHGetKnownFolderPath( id, KF_FLAG_DEFAULT, NULL, &path ))) return hr;
    hr = WindowsCreateString( path, wcslen( path ), value );
    CoTaskMemFree( path );
    return hr;
}

static inline struct appdatapaths *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct appdatapaths, IActivationFactory_iface );
}

static HRESULT WINAPI appdatapaths_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct appdatapaths *impl = impl_from_IActivationFactory( iface );

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

    if (IsEqualGUID( iid, &IID_IAppDataPathsStatics ))
    {
        *out = &impl->IAppDataPathsStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAppDataPaths ))
    {
        *out = &impl->IAppDataPaths_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI appdatapaths_factory_AddRef( IActivationFactory *iface )
{
    struct appdatapaths *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI appdatapaths_factory_Release( IActivationFactory *iface )
{
    struct appdatapaths *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI appdatapaths_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI appdatapaths_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI appdatapaths_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI appdatapaths_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* AppDataPaths is not activatable: it is only reachable through its statics. */
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl appdatapaths_factory_vtbl =
{
    appdatapaths_factory_QueryInterface,
    appdatapaths_factory_AddRef,
    appdatapaths_factory_Release,
    /* IInspectable methods */
    appdatapaths_factory_GetIids,
    appdatapaths_factory_GetRuntimeClassName,
    appdatapaths_factory_GetTrustLevel,
    /* IActivationFactory methods */
    appdatapaths_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( appdatapaths_statics, IAppDataPathsStatics, struct appdatapaths, IActivationFactory_iface )

static HRESULT WINAPI appdatapaths_statics_GetForUser( IAppDataPathsStatics *iface, IUser *user, IAppDataPaths **result )
{
    TRACE( "iface %p, user %p, result %p.\n", iface, user, result );

    if (!result) return E_POINTER;
    if (!user) return E_INVALIDARG;
    /* Wine has a single interactive user, so this is the process user's set of folders. */
    *result = &appdatapaths.IAppDataPaths_iface;
    IAppDataPaths_AddRef( *result );
    return S_OK;
}

static HRESULT WINAPI appdatapaths_statics_GetDefault( IAppDataPathsStatics *iface, IAppDataPaths **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    *result = &appdatapaths.IAppDataPaths_iface;
    IAppDataPaths_AddRef( *result );
    return S_OK;
}

static const struct IAppDataPathsStaticsVtbl appdatapaths_statics_vtbl =
{
    appdatapaths_statics_QueryInterface,
    appdatapaths_statics_AddRef,
    appdatapaths_statics_Release,
    /* IInspectable methods */
    appdatapaths_statics_GetIids,
    appdatapaths_statics_GetRuntimeClassName,
    appdatapaths_statics_GetTrustLevel,
    /* IAppDataPathsStatics methods */
    appdatapaths_statics_GetForUser,
    appdatapaths_statics_GetDefault,
};

/* The instance is its own COM identity: the .NET Native projection asks the object it gets back
 * from GetDefault for IUnknown and for its runtime class name, and a factory answering those would
 * hand managed code the wrong type. */
static inline struct appdatapaths *impl_from_IAppDataPaths( IAppDataPaths *iface )
{
    return CONTAINING_RECORD( iface, struct appdatapaths, IAppDataPaths_iface );
}

static HRESULT WINAPI appdatapaths_QueryInterface( IAppDataPaths *iface, REFIID iid, void **out )
{
    struct appdatapaths *impl = impl_from_IAppDataPaths( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAppDataPaths ))
    {
        *out = &impl->IAppDataPaths_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI appdatapaths_AddRef( IAppDataPaths *iface )
{
    struct appdatapaths *impl = impl_from_IAppDataPaths( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI appdatapaths_Release( IAppDataPaths *iface )
{
    struct appdatapaths *impl = impl_from_IAppDataPaths( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI appdatapaths_GetIids( IAppDataPaths *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IAppDataPaths;
    *iid_count = 1;
    return S_OK;
}

static HRESULT WINAPI appdatapaths_GetRuntimeClassName( IAppDataPaths *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Storage_AppDataPaths,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_AppDataPaths) - 1, class_name );
}

static HRESULT WINAPI appdatapaths_GetTrustLevel( IAppDataPaths *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}


#define DEFINE_APPDATAPATHS_GETTER( name, folder )                                                 \
    static HRESULT WINAPI appdatapaths_get_##name( IAppDataPaths *iface, HSTRING *value )          \
    {                                                                                              \
        TRACE( "iface %p, value %p.\n", iface, value );                                            \
        if (!value) return E_POINTER;                                                              \
        return known_folder_string( &folder, value );                                              \
    }

DEFINE_APPDATAPATHS_GETTER( Cookies, FOLDERID_Cookies )
DEFINE_APPDATAPATHS_GETTER( Desktop, FOLDERID_Desktop )
DEFINE_APPDATAPATHS_GETTER( Documents, FOLDERID_Documents )
DEFINE_APPDATAPATHS_GETTER( Favorites, FOLDERID_Favorites )
DEFINE_APPDATAPATHS_GETTER( History, FOLDERID_History )
DEFINE_APPDATAPATHS_GETTER( InternetCache, FOLDERID_InternetCache )
DEFINE_APPDATAPATHS_GETTER( LocalAppData, FOLDERID_LocalAppData )
DEFINE_APPDATAPATHS_GETTER( ProgramData, FOLDERID_ProgramData )
DEFINE_APPDATAPATHS_GETTER( RoamingAppData, FOLDERID_RoamingAppData )

static const struct IAppDataPathsVtbl appdatapaths_vtbl =
{
    appdatapaths_QueryInterface,
    appdatapaths_AddRef,
    appdatapaths_Release,
    /* IInspectable methods */
    appdatapaths_GetIids,
    appdatapaths_GetRuntimeClassName,
    appdatapaths_GetTrustLevel,
    /* IAppDataPaths methods */
    appdatapaths_get_Cookies,
    appdatapaths_get_Desktop,
    appdatapaths_get_Documents,
    appdatapaths_get_Favorites,
    appdatapaths_get_History,
    appdatapaths_get_InternetCache,
    appdatapaths_get_LocalAppData,
    appdatapaths_get_ProgramData,
    appdatapaths_get_RoamingAppData,
};

static struct appdatapaths appdatapaths =
{
    {&appdatapaths_factory_vtbl},
    {&appdatapaths_statics_vtbl},
    {&appdatapaths_vtbl},
    0,
};

IActivationFactory *appdatapaths_factory = &appdatapaths.IActivationFactory_iface;

/*
 * UserDataPaths - the same shape as AppDataPaths, a wider set of the user's known folders.
 */

struct userdatapaths
{
    IActivationFactory IActivationFactory_iface;
    IUserDataPathsStatics IUserDataPathsStatics_iface;
    IUserDataPaths IUserDataPaths_iface;
    LONG ref;
};

static struct userdatapaths userdatapaths;

static inline struct userdatapaths *impl_from_IActivationFactory2( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct userdatapaths, IActivationFactory_iface );
}

static HRESULT WINAPI userdatapaths_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct userdatapaths *impl = impl_from_IActivationFactory2( iface );

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

    if (IsEqualGUID( iid, &IID_IUserDataPathsStatics ))
    {
        *out = &impl->IUserDataPathsStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI userdatapaths_factory_AddRef( IActivationFactory *iface )
{
    struct userdatapaths *impl = impl_from_IActivationFactory2( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI userdatapaths_factory_Release( IActivationFactory *iface )
{
    struct userdatapaths *impl = impl_from_IActivationFactory2( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI userdatapaths_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI userdatapaths_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI userdatapaths_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI userdatapaths_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl userdatapaths_factory_vtbl =
{
    userdatapaths_factory_QueryInterface,
    userdatapaths_factory_AddRef,
    userdatapaths_factory_Release,
    /* IInspectable methods */
    userdatapaths_factory_GetIids,
    userdatapaths_factory_GetRuntimeClassName,
    userdatapaths_factory_GetTrustLevel,
    /* IActivationFactory methods */
    userdatapaths_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( userdatapaths_statics, IUserDataPathsStatics, struct userdatapaths, IActivationFactory_iface )

static HRESULT WINAPI userdatapaths_statics_GetForUser( IUserDataPathsStatics *iface, IUser *user, IUserDataPaths **result )
{
    TRACE( "iface %p, user %p, result %p.\n", iface, user, result );

    if (!result) return E_POINTER;
    if (!user) return E_INVALIDARG;
    *result = &userdatapaths.IUserDataPaths_iface;
    IUserDataPaths_AddRef( *result );
    return S_OK;
}

static HRESULT WINAPI userdatapaths_statics_GetDefault( IUserDataPathsStatics *iface, IUserDataPaths **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    *result = &userdatapaths.IUserDataPaths_iface;
    IUserDataPaths_AddRef( *result );
    return S_OK;
}

static const struct IUserDataPathsStaticsVtbl userdatapaths_statics_vtbl =
{
    userdatapaths_statics_QueryInterface,
    userdatapaths_statics_AddRef,
    userdatapaths_statics_Release,
    /* IInspectable methods */
    userdatapaths_statics_GetIids,
    userdatapaths_statics_GetRuntimeClassName,
    userdatapaths_statics_GetTrustLevel,
    /* IUserDataPathsStatics methods */
    userdatapaths_statics_GetForUser,
    userdatapaths_statics_GetDefault,
};

static inline struct userdatapaths *impl_from_IUserDataPaths( IUserDataPaths *iface )
{
    return CONTAINING_RECORD( iface, struct userdatapaths, IUserDataPaths_iface );
}

static HRESULT WINAPI userdatapaths_QueryInterface( IUserDataPaths *iface, REFIID iid, void **out )
{
    struct userdatapaths *impl = impl_from_IUserDataPaths( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IUserDataPaths ))
    {
        *out = &impl->IUserDataPaths_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI userdatapaths_AddRef( IUserDataPaths *iface )
{
    struct userdatapaths *impl = impl_from_IUserDataPaths( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI userdatapaths_Release( IUserDataPaths *iface )
{
    struct userdatapaths *impl = impl_from_IUserDataPaths( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI userdatapaths_GetIids( IUserDataPaths *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IUserDataPaths;
    *iid_count = 1;
    return S_OK;
}

static HRESULT WINAPI userdatapaths_GetRuntimeClassName( IUserDataPaths *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Storage_UserDataPaths,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_UserDataPaths) - 1, class_name );
}

static HRESULT WINAPI userdatapaths_GetTrustLevel( IUserDataPaths *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

#define DEFINE_USERDATAPATHS_GETTER( name )                                                        \
    static HRESULT WINAPI userdatapaths_get_##name( IUserDataPaths *iface, HSTRING *value )        \
    {                                                                                              \
        TRACE( "iface %p, value %p.\n", iface, value );                                            \
        if (!value) return E_POINTER;                                                              \
        return known_folder_string( &FOLDERID_##name, value );                                     \
    }

DEFINE_USERDATAPATHS_GETTER( CameraRoll )
DEFINE_USERDATAPATHS_GETTER( Cookies )
DEFINE_USERDATAPATHS_GETTER( Desktop )
DEFINE_USERDATAPATHS_GETTER( Documents )
DEFINE_USERDATAPATHS_GETTER( Downloads )
DEFINE_USERDATAPATHS_GETTER( Favorites )
DEFINE_USERDATAPATHS_GETTER( History )
DEFINE_USERDATAPATHS_GETTER( InternetCache )
DEFINE_USERDATAPATHS_GETTER( LocalAppData )
DEFINE_USERDATAPATHS_GETTER( LocalAppDataLow )
DEFINE_USERDATAPATHS_GETTER( Music )
DEFINE_USERDATAPATHS_GETTER( Pictures )
DEFINE_USERDATAPATHS_GETTER( Profile )
DEFINE_USERDATAPATHS_GETTER( Recent )
DEFINE_USERDATAPATHS_GETTER( RoamingAppData )
DEFINE_USERDATAPATHS_GETTER( SavedPictures )
DEFINE_USERDATAPATHS_GETTER( Screenshots )
DEFINE_USERDATAPATHS_GETTER( Templates )
DEFINE_USERDATAPATHS_GETTER( Videos )

static const struct IUserDataPathsVtbl userdatapaths_vtbl =
{
    userdatapaths_QueryInterface,
    userdatapaths_AddRef,
    userdatapaths_Release,
    /* IInspectable methods */
    userdatapaths_GetIids,
    userdatapaths_GetRuntimeClassName,
    userdatapaths_GetTrustLevel,
    /* IUserDataPaths methods */
    userdatapaths_get_CameraRoll,
    userdatapaths_get_Cookies,
    userdatapaths_get_Desktop,
    userdatapaths_get_Documents,
    userdatapaths_get_Downloads,
    userdatapaths_get_Favorites,
    userdatapaths_get_History,
    userdatapaths_get_InternetCache,
    userdatapaths_get_LocalAppData,
    userdatapaths_get_LocalAppDataLow,
    userdatapaths_get_Music,
    userdatapaths_get_Pictures,
    userdatapaths_get_Profile,
    userdatapaths_get_Recent,
    userdatapaths_get_RoamingAppData,
    userdatapaths_get_SavedPictures,
    userdatapaths_get_Screenshots,
    userdatapaths_get_Templates,
    userdatapaths_get_Videos,
};

static struct userdatapaths userdatapaths =
{
    {&userdatapaths_factory_vtbl},
    {&userdatapaths_statics_vtbl},
    {&userdatapaths_vtbl},
    0,
};

IActivationFactory *userdatapaths_factory = &userdatapaths.IActivationFactory_iface;

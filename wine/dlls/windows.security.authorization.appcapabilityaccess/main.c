/*
 * Windows.Security.Authorization.AppCapabilityAccess.AppCapability
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
 * Windows answers from the CapabilityAccessManager service through CapabilityAccessManagerClient.dll. A prefix has no
 * such service, every CheckAccess came back denied, and Media Player painted "can't access your music library" instead
 * of the library. A prefix has one user and no per-app capability database: a capability the running package's
 * manifest declares is allowed, anything else is not declared. Wine's KnownFolders already map the libraries to the
 * user's own folders, so "allowed" is also true. With no package manifest to check against (a test program), a caller
 * that asks for a capability on purpose is allowed it.
 */

#include <stdarg.h>
#include <stdlib.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_System
#define WIDL_using_Windows_Security_Authorization_AppCapabilityAccess
#include "windows.foundation.h"
#include "windows.security.authorization.appcapabilityaccess.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appcapability);

/* ---- the capabilities the package declares ---- */

static WCHAR **declared;
static unsigned int declared_count;
static INIT_ONCE declared_once = INIT_ONCE_STATIC_INIT;

/* <Capability Name="x"/>, <uap:Capability .../>, <DeviceCapability .../> and their kin all end in "Capability" and
 * carry Name="x" */
static BOOL CALLBACK load_declared( INIT_ONCE *once, void *param, void **context )
{
    WCHAR path[MAX_PATH];
    LARGE_INTEGER size;
    DWORD got = 0;
    HANDLE file;
    char *text;
    const char *p;

    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", path, ARRAY_SIZE(path) - 20 )) return TRUE;
    wcscat( path, L"\\AppxManifest.xml" );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return TRUE;
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > (4 << 20) || !(text = calloc( 1, size.QuadPart + 1 )))
    {
        CloseHandle( file );
        return TRUE;
    }
    ReadFile( file, text, size.QuadPart, &got, NULL );
    CloseHandle( file );

    for (p = text; (p = strstr( p, "Capability" )); p += 10)
    {
        const char *attr = strstr( p, "Name=\"" ), *close = strchr( p, '>' ), *end;
        WCHAR **list;
        int n;

        if (!attr || (close && attr > close)) continue;
        attr += 6;
        if (!(end = strchr( attr, '"' ))) continue;
        if (!(list = realloc( declared, (declared_count + 1) * sizeof(*list) ))) break;
        declared = list;
        n = MultiByteToWideChar( CP_UTF8, 0, attr, end - attr, NULL, 0 );
        if (!(declared[declared_count] = calloc( n + 1, sizeof(WCHAR) ))) break;
        MultiByteToWideChar( CP_UTF8, 0, attr, end - attr, declared[declared_count], n );
        TRACE( "the manifest declares %s\n", debugstr_w(declared[declared_count]) );
        declared_count++;
    }
    free( text );
    return TRUE;
}

static AppCapabilityAccessStatus status_for( const WCHAR *capability )
{
    unsigned int i;

    InitOnceExecuteOnce( &declared_once, load_declared, NULL, NULL );
    if (!capability || !*capability) return AppCapabilityAccessStatus_NotDeclaredByApp;
    for (i = 0; i < declared_count; i++)
        if (!wcsicmp( declared[i], capability )) return AppCapabilityAccessStatus_Allowed;
    return declared_count ? AppCapabilityAccessStatus_NotDeclaredByApp : AppCapabilityAccessStatus_Allowed;
}

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT get_class_name( HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Security_Authorization_AppCapabilityAccess_AppCapability,
                                wcslen( RuntimeClass_Windows_Security_Authorization_AppCapabilityAccess_AppCapability ), name );
}

/* ---- AppCapability ---- */

struct capability
{
    IAppCapability IAppCapability_iface;
    IAppCapability2 IAppCapability2_iface;
    LONG ref;
    HSTRING name;
    HSTRING message;
};

static inline struct capability *impl_from_IAppCapability( IAppCapability *iface )
{
    return CONTAINING_RECORD( iface, struct capability, IAppCapability_iface );
}

static inline struct capability *impl_from_IAppCapability2( IAppCapability2 *iface )
{
    return CONTAINING_RECORD( iface, struct capability, IAppCapability2_iface );
}

static HRESULT WINAPI capability_QueryInterface( IAppCapability *iface, REFIID iid, void **out )
{
    struct capability *impl = impl_from_IAppCapability( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppCapability ))
        *out = &impl->IAppCapability_iface;
    else if (IsEqualGUID( iid, &IID_IAppCapability2 )) *out = &impl->IAppCapability2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI capability_AddRef( IAppCapability *iface )
{
    return InterlockedIncrement( &impl_from_IAppCapability( iface )->ref );
}

static ULONG WINAPI capability_Release( IAppCapability *iface )
{
    struct capability *impl = impl_from_IAppCapability( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->name );
        WindowsDeleteString( impl->message );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI capability_GetIids( IAppCapability *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI capability_GetRuntimeClassName( IAppCapability *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI capability_GetTrustLevel( IAppCapability *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI capability_get_CapabilityName( IAppCapability *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IAppCapability( iface )->name, value );
}

/* nothing that asks for capabilities reads it, and handing back an invented user would be worse than none */
static HRESULT WINAPI capability_get_User( IAppCapability *iface, IUser **value )
{
    FIXME( "iface %p, value %p: no user object.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

/* on Windows this only ever prompts, and CheckAccess already answers without one */
static HRESULT WINAPI capability_RequestAccessAsync( IAppCapability *iface, IAsyncOperation_AppCapabilityAccessStatus **operation )
{
    FIXME( "iface %p, operation %p: not implemented.\n", iface, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI capability_CheckAccess( IAppCapability *iface, AppCapabilityAccessStatus *result )
{
    struct capability *impl = impl_from_IAppCapability( iface );

    if (!result) return E_POINTER;
    *result = status_for( WindowsGetStringRawBuffer( impl->name, NULL ) );
    TRACE( "%s -> %d\n", debugstr_hstring( impl->name ), *result );
    return S_OK;
}

static HRESULT WINAPI capability_add_AccessChanged( IAppCapability *iface,
        ITypedEventHandler_AppCapability_AppCapabilityAccessChangedEventArgs *handler, EventRegistrationToken *token )
{
    static LONG64 next_token;

    TRACE( "iface %p, handler %p, token %p: access never changes here.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI capability_remove_AccessChanged( IAppCapability *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct IAppCapabilityVtbl capability_vtbl =
{
    capability_QueryInterface,
    capability_AddRef,
    capability_Release,
    capability_GetIids,
    capability_GetRuntimeClassName,
    capability_GetTrustLevel,
    capability_get_CapabilityName,
    capability_get_User,
    capability_RequestAccessAsync,
    capability_CheckAccess,
    capability_add_AccessChanged,
    capability_remove_AccessChanged,
};

static HRESULT WINAPI capability2_QueryInterface( IAppCapability2 *iface, REFIID iid, void **out )
{
    return capability_QueryInterface( &impl_from_IAppCapability2( iface )->IAppCapability_iface, iid, out );
}
static ULONG WINAPI capability2_AddRef( IAppCapability2 *iface ) { return capability_AddRef( &impl_from_IAppCapability2( iface )->IAppCapability_iface ); }
static ULONG WINAPI capability2_Release( IAppCapability2 *iface ) { return capability_Release( &impl_from_IAppCapability2( iface )->IAppCapability_iface ); }
static HRESULT WINAPI capability2_GetIids( IAppCapability2 *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI capability2_GetRuntimeClassName( IAppCapability2 *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI capability2_GetTrustLevel( IAppCapability2 *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI capability2_get_DisplayMessage( IAppCapability2 *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IAppCapability2( iface )->message, value );
}

static HRESULT WINAPI capability2_put_DisplayMessage( IAppCapability2 *iface, HSTRING value )
{
    struct capability *impl = impl_from_IAppCapability2( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->message );
    impl->message = copy;
    return S_OK;
}

static const struct IAppCapability2Vtbl capability2_vtbl =
{
    capability2_QueryInterface,
    capability2_AddRef,
    capability2_Release,
    capability2_GetIids,
    capability2_GetRuntimeClassName,
    capability2_GetTrustLevel,
    capability2_get_DisplayMessage,
    capability2_put_DisplayMessage,
};

static HRESULT create_capability( HSTRING name, IAppCapability **result )
{
    struct capability *impl;
    HRESULT hr;

    if (!result) return E_POINTER;
    *result = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppCapability_iface.lpVtbl = &capability_vtbl;
    impl->IAppCapability2_iface.lpVtbl = &capability2_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( name, &impl->name )))
    {
        free( impl );
        return hr;
    }
    *result = &impl->IAppCapability_iface;
    return S_OK;
}

/* ---- the statics ---- */

static struct
{
    IActivationFactory IActivationFactory_iface;
    IAppCapabilityStatics IAppCapabilityStatics_iface;
} statics;

static HRESULT statics_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &statics.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IAppCapabilityStatics )) *out = &statics.IAppCapabilityStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out ) { return statics_query( iid, out ); }
static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* no default constructor on Windows either */
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

static HRESULT WINAPI app_statics_QueryInterface( IAppCapabilityStatics *iface, REFIID iid, void **out ) { return statics_query( iid, out ); }
static ULONG WINAPI app_statics_AddRef( IAppCapabilityStatics *iface ) { return 2; }
static ULONG WINAPI app_statics_Release( IAppCapabilityStatics *iface ) { return 1; }
static HRESULT WINAPI app_statics_GetIids( IAppCapabilityStatics *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI app_statics_GetRuntimeClassName( IAppCapabilityStatics *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI app_statics_GetTrustLevel( IAppCapabilityStatics *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI app_statics_RequestAccessForCapabilitiesAsync( IAppCapabilityStatics *iface, IIterable_HSTRING *names,
        IAsyncOperation_IMapView_HSTRING_AppCapabilityAccessStatus **operation )
{
    FIXME( "iface %p, names %p, operation %p: not implemented.\n", iface, names, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI app_statics_RequestAccessForCapabilitiesForUserAsync( IAppCapabilityStatics *iface, IUser *user,
        IIterable_HSTRING *names, IAsyncOperation_IMapView_HSTRING_AppCapabilityAccessStatus **operation )
{
    FIXME( "iface %p, user %p, names %p, operation %p: not implemented.\n", iface, user, names, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI app_statics_Create( IAppCapabilityStatics *iface, HSTRING name, IAppCapability **result )
{
    TRACE( "iface %p, name %s, result %p.\n", iface, debugstr_hstring( name ), result );
    return create_capability( name, result );
}

static HRESULT WINAPI app_statics_CreateWithProcessIdForUser( IAppCapabilityStatics *iface, IUser *user, HSTRING name,
                                                              UINT32 pid, IAppCapability **result )
{
    TRACE( "iface %p, user %p, name %s, pid %u, result %p.\n", iface, user, debugstr_hstring( name ), pid, result );
    return create_capability( name, result );
}

static const struct IAppCapabilityStaticsVtbl app_statics_vtbl =
{
    app_statics_QueryInterface,
    app_statics_AddRef,
    app_statics_Release,
    app_statics_GetIids,
    app_statics_GetRuntimeClassName,
    app_statics_GetTrustLevel,
    app_statics_RequestAccessForCapabilitiesAsync,
    app_statics_RequestAccessForCapabilitiesForUserAsync,
    app_statics_Create,
    app_statics_CreateWithProcessIdForUser,
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    statics.IActivationFactory_iface.lpVtbl = &factory_vtbl;
    statics.IAppCapabilityStatics_iface.lpVtbl = &app_statics_vtbl;

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), RuntimeClass_Windows_Security_Authorization_AppCapabilityAccess_AppCapability ))
        return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &statics.IActivationFactory_iface;
    return S_OK;
}

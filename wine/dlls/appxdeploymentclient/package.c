/* WinRT Windows.Management.Deployment.PackageManager Implementation
 *
 * Copyright (C) 2023 Mohamad Al-Jaf
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
#include "appmodel.h"
#include "roapi.h"

LONG WINAPI GetCurrentPackageFullName( UINT32 *length, WCHAR *name );
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

struct package_manager
{
    IPackageManager IPackageManager_iface;
    IPackageManager2 IPackageManager2_iface;
    IPackageManager4 IPackageManager4_iface;
    LONG ref;
};

static inline struct package_manager *impl_from_IPackageManager( IPackageManager *iface )
{
    return CONTAINING_RECORD( iface, struct package_manager, IPackageManager_iface );
}

static HRESULT WINAPI package_manager_QueryInterface( IPackageManager *iface, REFIID iid, void **out )
{
    struct package_manager *impl = impl_from_IPackageManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IPackageManager ))
    {
        *out = &impl->IPackageManager_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IPackageManager2 ))
    {
        *out = &impl->IPackageManager2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IPackageManager4 ))
    {
        *out = &impl->IPackageManager4_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI package_manager_AddRef( IPackageManager *iface )
{
    struct package_manager *impl = impl_from_IPackageManager( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI package_manager_Release( IPackageManager *iface )
{
    struct package_manager *impl = impl_from_IPackageManager( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI package_manager_GetIids( IPackageManager *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_GetRuntimeClassName( IPackageManager *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_GetTrustLevel( IPackageManager *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_AddPackageAsync( IPackageManager *iface, IUriRuntimeClass *uri,
    IIterable_Uri *dependencies, DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, uri %p, dependencies %p, options %d, operation %p stub!\n", iface, uri, dependencies, options, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_UpdatePackageAsync( IPackageManager *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, uri %p, dependencies %p, options %d, operation %p stub!\n", iface, uri, dependencies, options, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_RemovePackageAsync( IPackageManager *iface, HSTRING name,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring(name), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_StagePackageAsync( IPackageManager *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, uri %p, dependencies %p, operation %p stub!\n", iface, uri, dependencies, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_RegisterPackageAsync( IPackageManager *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, uri %p, dependencies %p, options %d, operation %p stub!\n", iface, uri, dependencies, options, operation );
    return E_NOTIMPL;
}

/* What a Find* query is filtering on; NULL fields match anything. */
struct package_filter
{
    HSTRING name;
    HSTRING publisher;
    HSTRING family_name;
    PackageTypes types;
};

static BOOL hstring_matches( HSTRING want, HSTRING have )
{
    INT32 order = 1;

    if (!want) return TRUE;
    WindowsCompareStringOrdinal( want, have, &order );
    return !order;
}

/* Answers every Find* variant. The running package is the only one this host has installed, so
 * the result is that package when it passes the filter and an empty collection otherwise. There
 * is a single user, so any security id means "this user". */
static HRESULT find_packages( const struct package_filter *filter, IIterable_Package **packages )
{
    static const WCHAR classW[] = L"Windows.ApplicationModel.Package";
    HSTRING str, name = NULL, publisher = NULL, family = NULL;
    IPackageStatics *statics;
    IPackage *package = NULL;
    IPackageId *id;
    BOOL match;
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;

    if (!(filter->types & PackageTypes_Main)) return package_list_create( NULL, packages );

    if (FAILED(hr = WindowsCreateString( classW, ARRAY_SIZE(classW) - 1, &str ))) return hr;
    hr = RoGetActivationFactory( str, &IID_IPackageStatics, (void **)&statics );
    WindowsDeleteString( str );
    if (SUCCEEDED(hr))
    {
        if (FAILED(IPackageStatics_get_Current( statics, &package ))) package = NULL;
        IPackageStatics_Release( statics );
    }
    if (!package) return package_list_create( NULL, packages );

    if (FAILED(hr = IPackage_get_Id( package, &id )))
    {
        IPackage_Release( package );
        return hr;
    }
    IPackageId_get_Name( id, &name );
    IPackageId_get_Publisher( id, &publisher );
    IPackageId_get_FamilyName( id, &family );
    IPackageId_Release( id );

    match = hstring_matches( filter->name, name ) && hstring_matches( filter->publisher, publisher ) &&
            hstring_matches( filter->family_name, family );
    TRACE( "name %s publisher %s family %s against current %s: %s\n", debugstr_hstring( filter->name ),
           debugstr_hstring( filter->publisher ), debugstr_hstring( filter->family_name ),
           debugstr_hstring( family ), match ? "match" : "no match" );
    WindowsDeleteString( name );
    WindowsDeleteString( publisher );
    WindowsDeleteString( family );

    if (!match && filter->family_name)
    {
        /* a registered stand-in for a platform package (standin.c) */
        IPackage *standin;
        if (standin_package_create( WindowsGetStringRawBuffer( filter->family_name, NULL ), &standin ) == S_OK)
        {
            IPackage_Release( package );
            hr = package_list_create( standin, packages );
            IPackage_Release( standin );
            return hr;
        }
    }

    hr = package_list_create( match ? package : NULL, packages );
    IPackage_Release( package );
    return hr;
}

#define ALL_TYPES (PackageTypes)~0u

static HRESULT WINAPI package_manager_FindPackages( IPackageManager *iface, IIterable_Package **packages )
{
    TRACE( "iface %p, packages %p.\n", iface, packages );
    return find_packages( &(struct package_filter){.types = ALL_TYPES}, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByUserSecurityId( IPackageManager *iface, HSTRING sid, IIterable_Package **packages )
{
    TRACE( "iface %p, sid %s, packages %p.\n", iface, debugstr_hstring(sid), packages );
    return find_packages( &(struct package_filter){.types = ALL_TYPES}, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByNamePublisher( IPackageManager *iface, HSTRING name, HSTRING publisher, IIterable_Package **packages )
{
    TRACE( "iface %p, name %s, publisher %s, packages %p.\n", iface, debugstr_hstring(name), debugstr_hstring(publisher), packages );

    if (!name || !publisher) return E_INVALIDARG;
    return find_packages( &(struct package_filter){.name = name, .publisher = publisher, .types = ALL_TYPES}, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByUserSecurityIdNamePublisher( IPackageManager *iface, HSTRING sid,
    HSTRING name, HSTRING publisher, IIterable_Package **packages )
{
    TRACE( "iface %p, sid %s, name %s, publisher %s, packages %p.\n", iface, debugstr_hstring(sid), debugstr_hstring(name), debugstr_hstring(publisher), packages );
    return find_packages( &(struct package_filter){.name = name, .publisher = publisher, .types = ALL_TYPES}, packages );
}

static HRESULT WINAPI package_manager_FindUsers( IPackageManager *iface, HSTRING name, IIterable_PackageUserInformation **users )
{
    FIXME( "iface %p, name %s, users %p stub!\n", iface, debugstr_hstring(name), users );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_SetPackageState( IPackageManager *iface, HSTRING name, PackageState state )
{
    FIXME("iface %p, name %s, state %d stub!\n", iface, debugstr_hstring(name), state);
    return E_NOTIMPL;
}

/* The only package this host knows about is the one the process is running as, and the
 * native windows.applicationmodel.dll already answers Package.Current for it. Hand that back
 * when asked for it by full name: AppInfo.Package in the native DLL goes through
 * PackageManager::FindPackageByUserSecurityIdPackageFullName, and E_NOTIMPL there made
 * Solitaire throw out of the step that follows its bootflow, so it never left its splash. */
static HRESULT find_current_package( HSTRING name, IPackage **package )
{
    static const WCHAR classW[] = L"Windows.ApplicationModel.Package";
    WCHAR current[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    UINT32 len = ARRAY_SIZE(current);
    IPackageStatics *statics;
    HSTRING str;
    HRESULT hr;

    if (!package) return E_POINTER;
    *package = NULL;
    if (GetCurrentPackageFullName( &len, current ) != ERROR_SUCCESS) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (wcsicmp( current, WindowsGetStringRawBuffer( name, NULL ) )) return S_OK; /* not found: NULL package */

    if (FAILED(hr = WindowsCreateString( classW, ARRAY_SIZE(classW) - 1, &str ))) return hr;
    hr = RoGetActivationFactory( str, &IID_IPackageStatics, (void **)&statics );
    WindowsDeleteString( str );
    if (FAILED(hr)) return hr;
    hr = IPackageStatics_get_Current( statics, package );
    IPackageStatics_Release( statics );
    TRACE( "current package %s -> %p, hr %#lx\n", debugstr_w(current), *package, hr );
    return hr;
}

static HRESULT WINAPI package_manager_FindPackageByPackageFullName( IPackageManager *iface, HSTRING name, IPackage **package )
{
    TRACE( "iface %p, name %s, package %p.\n", iface, debugstr_hstring(name), package );
    return find_current_package( name, package );
}

static HRESULT WINAPI package_manager_CleanupPackageForUserAsync( IPackageManager *iface, HSTRING name, HSTRING sid,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, name %s, sid %s, operation %p stub!\n", iface, debugstr_hstring(name), debugstr_hstring(sid), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_FindPackagesByPackageFamilyName( IPackageManager *iface, HSTRING family_name,
    IIterable_Package **packages )
{
    TRACE( "iface %p, family_name %s, packages %p.\n", iface, debugstr_hstring(family_name), packages );
    return find_packages( &(struct package_filter){.family_name = family_name, .types = ALL_TYPES}, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByUserSecurityIdPackageFamilyName( IPackageManager *iface, HSTRING sid,
    HSTRING family_name, IIterable_Package **packages )
{
    TRACE( "iface %p, sid %s, family_name %s, packages %p.\n", iface, debugstr_hstring(sid), debugstr_hstring(family_name), packages );
    return find_packages( &(struct package_filter){.family_name = family_name, .types = ALL_TYPES}, packages );
}

static HRESULT WINAPI package_manager_FindPackageByUserSecurityIdPackageFullName( IPackageManager *iface, HSTRING sid, HSTRING name, IPackage **package )
{
    TRACE( "iface %p, sid %s, name %s, package %p.\n", iface, debugstr_hstring(sid), debugstr_hstring(name), package );
    return find_current_package( name, package );
}

static const struct IPackageManagerVtbl package_manager_vtbl =
{
    package_manager_QueryInterface,
    package_manager_AddRef,
    package_manager_Release,
    /* IInspectable methods */
    package_manager_GetIids,
    package_manager_GetRuntimeClassName,
    package_manager_GetTrustLevel,
    /* IPackageManager methods */
    package_manager_AddPackageAsync,
    package_manager_UpdatePackageAsync,
    package_manager_RemovePackageAsync,
    package_manager_StagePackageAsync,
    package_manager_RegisterPackageAsync,
    package_manager_FindPackages,
    package_manager_FindPackagesByUserSecurityId,
    package_manager_FindPackagesByNamePublisher,
    package_manager_FindPackagesByUserSecurityIdNamePublisher,
    package_manager_FindUsers,
    package_manager_SetPackageState,
    package_manager_FindPackageByPackageFullName,
    package_manager_CleanupPackageForUserAsync,
    package_manager_FindPackagesByPackageFamilyName,
    package_manager_FindPackagesByUserSecurityIdPackageFamilyName,
    package_manager_FindPackageByUserSecurityIdPackageFullName
};

DEFINE_IINSPECTABLE( package_manager2, IPackageManager2, struct package_manager, IPackageManager_iface );

static HRESULT WINAPI package_manager2_RemovePackageWithOptionsAsync( IPackageManager2 *iface, HSTRING name, RemovalOptions options,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, name %s, options %d, operation %p stub!\n", iface, debugstr_hstring(name), options, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager2_StagePackageWithOptionsAsync( IPackageManager2 *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, uri %p, dependencies %p, options %d, operation %p stub!\n", iface, uri, dependencies, options, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager2_RegisterPackageByFullNameAsync( IPackageManager2 *iface, HSTRING name, IIterable_HSTRING *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, name %s, dependencies %p, options %d, operation %p stub!\n", iface, debugstr_hstring(name), dependencies, options, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager2_FindPackagesWithPackageTypes( IPackageManager2 *iface, PackageTypes types, IIterable_Package **packages )
{
    TRACE( "iface %p, types %d, packages %p.\n", iface, types, packages );
    return find_packages( &(struct package_filter){.types = types}, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByUserSecurityIdWithPackageTypes( IPackageManager2 *iface, HSTRING sid,
    PackageTypes types, IIterable_Package **packages )
{
    TRACE( "iface %p, sid %s, types %d, packages %p.\n", iface, debugstr_hstring(sid), types, packages );
    return find_packages( &(struct package_filter){.types = types}, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByNamePublisherWithPackageTypes( IPackageManager2 *iface, HSTRING name, HSTRING publisher,
    PackageTypes types, IIterable_Package **packages )
{
    TRACE( "iface %p, name %s, publisher %s, types %d, packages %p.\n", iface, debugstr_hstring(name), debugstr_hstring(publisher), types, packages );
    return find_packages( &(struct package_filter){.name = name, .publisher = publisher, .types = types}, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByUserSecurityIdNamePublisherWithPackageTypes( IPackageManager2 *iface, HSTRING sid, HSTRING name,
    HSTRING publisher, PackageTypes types, IIterable_Package **packages )
{
    TRACE( "iface %p, sid %s, name %s, publisher %s, types %d, packages %p.\n", iface, debugstr_hstring(sid), debugstr_hstring(name), debugstr_hstring(publisher), types, packages );
    return find_packages( &(struct package_filter){.name = name, .publisher = publisher, .types = types}, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByPackageFamilyNameWithPackageTypes( IPackageManager2 *iface, HSTRING family_name, PackageTypes types,
   IIterable_Package **packages )
{
    TRACE( "iface %p, family_name %s, types %d, packages %p.\n", iface, debugstr_hstring(family_name), types, packages );
    return find_packages( &(struct package_filter){.family_name = family_name, .types = types}, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByUserSecurityIdPackageFamilyNameWithPackageTypes( IPackageManager2 *iface, HSTRING sid, HSTRING family_name,
    PackageTypes types, IIterable_Package **packages )
{
    TRACE( "iface %p, sid %s, family_name %s, types %d, packages %p.\n", iface, debugstr_hstring(sid), debugstr_hstring(family_name), types, packages );
    return find_packages( &(struct package_filter){.family_name = family_name, .types = types}, packages );
}

static HRESULT WINAPI package_manager2_StageUserDataAsync( IPackageManager2 *iface, HSTRING name,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring(name), operation );
    return E_NOTIMPL;
}

static const struct IPackageManager2Vtbl package_manager2_vtbl =
{
    package_manager2_QueryInterface,
    package_manager2_AddRef,
    package_manager2_Release,
    /* IInspectable methods */
    package_manager2_GetIids,
    package_manager2_GetRuntimeClassName,
    package_manager2_GetTrustLevel,
    /* IPackageManager2 methods */
    package_manager2_RemovePackageWithOptionsAsync,
    package_manager2_StagePackageWithOptionsAsync,
    package_manager2_RegisterPackageByFullNameAsync,
    package_manager2_FindPackagesWithPackageTypes,
    package_manager2_FindPackagesByUserSecurityIdWithPackageTypes,
    package_manager2_FindPackagesByNamePublisherWithPackageTypes,
    package_manager2_FindPackagesByUserSecurityIdNamePublisherWithPackageTypes,
    package_manager2_FindPackagesByPackageFamilyNameWithPackageTypes,
    package_manager2_FindPackagesByUserSecurityIdPackageFamilyNameWithPackageTypes,
    package_manager2_StageUserDataAsync,
};

struct package_manager_statics
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct package_manager_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct package_manager_statics, IActivationFactory_iface );
}

DEFINE_IINSPECTABLE( package_manager4, IPackageManager4, struct package_manager, IPackageManager_iface );

/* The volumes: the system volume alone (volume.c). The Xbox app asks for them when it looks for
 * where to install a dependency or a game, and an empty list made that fail. */
static HRESULT get_package_volumes( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    static const struct vector_iids iids =
    {
        .vector = &IID_IVector_PackageVolume,
        .view = &IID_IVectorView_PackageVolume,
        .iterable = &IID_IIterable_PackageVolume,
        .iterator = &IID_IIterator_PackageVolume,
    };
    IVector_PackageVolume *vector;
    IVectorView_PackageVolume *view;
    IPackageVolume *volume;
    HRESULT hr;

    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;
    if (SUCCEEDED(hr = package_volume_create( &volume )))
    {
        hr = IVector_PackageVolume_Append( vector, volume );
        IPackageVolume_Release( volume );
    }
    if (SUCCEEDED(hr)) hr = IVector_PackageVolume_GetView( vector, &view );
    IVector_PackageVolume_Release( vector );
    if (FAILED(hr)) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT WINAPI package_manager4_GetPackageVolumesAsync( IPackageManager4 *iface,
        IAsyncOperation_IVectorView_PackageVolume **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return async_operation_inspectable_create( &IID_IAsyncOperation_IVectorView_PackageVolume, (IUnknown *)iface,
                                               NULL, get_package_volumes, (IAsyncOperation_IInspectable **)operation );
}

static const struct IPackageManager4Vtbl package_manager4_vtbl =
{
    package_manager4_QueryInterface,
    package_manager4_AddRef,
    package_manager4_Release,
    /* IInspectable methods */
    package_manager4_GetIids,
    package_manager4_GetRuntimeClassName,
    package_manager4_GetTrustLevel,
    /* IPackageManager4 methods */
    package_manager4_GetPackageVolumesAsync,
};

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct package_manager_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct package_manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct package_manager_statics *impl = impl_from_IActivationFactory( iface );
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
    struct package_manager *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        *instance = NULL;
        return E_OUTOFMEMORY;
    }

    impl->IPackageManager_iface.lpVtbl = &package_manager_vtbl;
    impl->IPackageManager2_iface.lpVtbl = &package_manager2_vtbl;
    impl->IPackageManager4_iface.lpVtbl = &package_manager4_vtbl;
    impl->ref = 1;

    *instance = (IInspectable *)&impl->IPackageManager_iface;
    return S_OK;
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

static struct package_manager_statics package_manager_statics =
{
    {&factory_vtbl},
    1,
};

IActivationFactory *package_manager_factory = &package_manager_statics.IActivationFactory_iface;

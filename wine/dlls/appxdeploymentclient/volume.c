/* WinRT Windows.Management.Deployment.PackageVolume
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

/*
 * The one package volume here: the system drive. Packages are staged in plain directories rather
 * than in a store the deployment service manages, but the system volume is where an install would
 * go, and apps look for it before installing a dependency (the Xbox app's Gaming Services:
 * "Failed to find the system volume for install" when the volume list was empty).
 */
struct package_volume
{
    IPackageVolume IPackageVolume_iface;
    IPackageVolume2 IPackageVolume2_iface;
    LONG ref;
};

static inline struct package_volume *impl_from_IPackageVolume( IPackageVolume *iface )
{
    return CONTAINING_RECORD( iface, struct package_volume, IPackageVolume_iface );
}

static HRESULT WINAPI volume_QueryInterface( IPackageVolume *iface, REFIID iid, void **out )
{
    struct package_volume *impl = impl_from_IPackageVolume( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPackageVolume ))
    {
        IInspectable_AddRef( (*out = &impl->IPackageVolume_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IPackageVolume2 ))
    {
        IInspectable_AddRef( (*out = &impl->IPackageVolume2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI volume_AddRef( IPackageVolume *iface )
{
    return InterlockedIncrement( &impl_from_IPackageVolume( iface )->ref );
}

static ULONG WINAPI volume_Release( IPackageVolume *iface )
{
    struct package_volume *impl = impl_from_IPackageVolume( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI volume_GetIids( IPackageVolume *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI volume_GetRuntimeClassName( IPackageVolume *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Management_Deployment_PackageVolume,
                                wcslen( RuntimeClass_Windows_Management_Deployment_PackageVolume ), class_name );
}

static HRESULT WINAPI volume_GetTrustLevel( IPackageVolume *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI volume_get_IsOffline( IPackageVolume *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI volume_get_IsSystemVolume( IPackageVolume *iface, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT system_drive_string( const WCHAR *suffix, HSTRING *value )
{
    WCHAR path[MAX_PATH];
    UINT len = GetSystemWindowsDirectoryW( path, ARRAY_SIZE(path) );

    if (len < 3) return E_FAIL;
    path[3] = 0;   /* "C:\" */
    if (suffix) wcscat( path, suffix );
    return WindowsCreateString( path, wcslen( path ), value );
}

static HRESULT WINAPI volume_get_MountPoint( IPackageVolume *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return system_drive_string( NULL, value );
}

static HRESULT WINAPI volume_get_Name( IPackageVolume *iface, HSTRING *value )
{
    WCHAR root[MAX_PATH], name[MAX_PATH];

    TRACE( "iface %p, value %p.\n", iface, value );

    GetSystemWindowsDirectoryW( root, ARRAY_SIZE(root) );
    root[3] = 0;
    if (!GetVolumeNameForVolumeMountPointW( root, name, ARRAY_SIZE(name) )) wcscpy( name, root );
    return WindowsCreateString( name, wcslen( name ), value );
}

static HRESULT WINAPI volume_get_PackageStorePath( IPackageVolume *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return system_drive_string( L"Program Files\\WindowsApps", value );
}

static HRESULT WINAPI volume_get_SupportsHardLinks( IPackageVolume *iface, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

#define FIND_STUB( name, ... ) \
    static HRESULT WINAPI volume_##name( IPackageVolume *iface, __VA_ARGS__, IVector_Package **packages ) \
    { \
        FIXME( "iface %p stub!\n", iface ); \
        return E_NOTIMPL; \
    }

static HRESULT WINAPI volume_FindPackages( IPackageVolume *iface, IVector_Package **packages )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}
FIND_STUB( FindPackagesByNamePublisher, HSTRING name, HSTRING publisher )
FIND_STUB( FindPackagesByPackageFamilyName, HSTRING family )
FIND_STUB( FindPackagesWithPackageTypes, PackageTypes types )
FIND_STUB( FindPackagesByNamePublisherWithPackageTypes, HSTRING name, HSTRING publisher, PackageTypes types )
FIND_STUB( FindPackagesByPackageFamilyNameWithPackageTypes, HSTRING family, PackageTypes types )
FIND_STUB( FindPackage, HSTRING full_name )
FIND_STUB( FindPackagesForUser, HSTRING sid )
FIND_STUB( FindPackagesForUserByNamePublisher, HSTRING sid, HSTRING name, HSTRING publisher )
FIND_STUB( FindPackagesForUserByPackageFamilyName, HSTRING sid, HSTRING family )
FIND_STUB( FindPackagesForUserWithPackageTypes, HSTRING sid, PackageTypes types )
FIND_STUB( FindPackagesForUserByNamePublisherWithPackageTypes, HSTRING sid, HSTRING name, HSTRING publisher, PackageTypes types )
FIND_STUB( FindPackagesForUserByPackageFamilyNameWithPackageTypes, HSTRING sid, HSTRING family, PackageTypes types )
FIND_STUB( FindPackageForUser, HSTRING sid, HSTRING full_name )
#undef FIND_STUB

static const struct IPackageVolumeVtbl volume_vtbl =
{
    volume_QueryInterface,
    volume_AddRef,
    volume_Release,
    /* IInspectable methods */
    volume_GetIids,
    volume_GetRuntimeClassName,
    volume_GetTrustLevel,
    /* IPackageVolume methods */
    volume_get_IsOffline,
    volume_get_IsSystemVolume,
    volume_get_MountPoint,
    volume_get_Name,
    volume_get_PackageStorePath,
    volume_get_SupportsHardLinks,
    volume_FindPackages,
    volume_FindPackagesByNamePublisher,
    volume_FindPackagesByPackageFamilyName,
    volume_FindPackagesWithPackageTypes,
    volume_FindPackagesByNamePublisherWithPackageTypes,
    volume_FindPackagesByPackageFamilyNameWithPackageTypes,
    volume_FindPackage,
    volume_FindPackagesForUser,
    volume_FindPackagesForUserByNamePublisher,
    volume_FindPackagesForUserByPackageFamilyName,
    volume_FindPackagesForUserWithPackageTypes,
    volume_FindPackagesForUserByNamePublisherWithPackageTypes,
    volume_FindPackagesForUserByPackageFamilyNameWithPackageTypes,
    volume_FindPackageForUser,
};

DEFINE_IINSPECTABLE( volume2, IPackageVolume2, struct package_volume, IPackageVolume_iface );

static HRESULT WINAPI volume2_get_IsFullTrustPackageSupported( IPackageVolume2 *iface, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI volume2_get_IsAppxInstallSupported( IPackageVolume2 *iface, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI volume2_GetAvailableSpaceAsync( IPackageVolume2 *iface, IAsyncOperation_UINT64 **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static const struct IPackageVolume2Vtbl volume2_vtbl =
{
    volume2_QueryInterface,
    volume2_AddRef,
    volume2_Release,
    /* IInspectable methods */
    volume2_GetIids,
    volume2_GetRuntimeClassName,
    volume2_GetTrustLevel,
    /* IPackageVolume2 methods */
    volume2_get_IsFullTrustPackageSupported,
    volume2_get_IsAppxInstallSupported,
    volume2_GetAvailableSpaceAsync,
};

HRESULT package_volume_create( IPackageVolume **out )
{
    struct package_volume *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPackageVolume_iface.lpVtbl = &volume_vtbl;
    impl->IPackageVolume2_iface.lpVtbl = &volume2_vtbl;
    impl->ref = 1;
    *out = &impl->IPackageVolume_iface;
    return S_OK;
}

/* Stand-in packages: identities PackageManager reports as installed without a package on disk
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * Some packages are platform components an app requires before it will start: the Xbox app
 * installs Gaming Services when PackageManager cannot find Microsoft.GamingServices_8wekyb3d8bbwe.
 * Gaming Services itself cannot run here (NT services with kernel drivers); wine-uwp answers for
 * its client API instead (shims/gameplatform). Its package identity is registered here, under
 *   HKLM\Software\Wine\WineUWP\StandInPackages\<family name>
 *     Name, Publisher, PublisherId, Version ("a.b.c.d"), Architecture (DWORD), FullName
 * and PackageManager reports it as installed, so the app does not try to install it.
 */

#define WIDL_using_Windows_System
#define WIDL_using_Windows_Storage
#define WIDL_using_Windows_ApplicationModel_Core
#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

struct standin
{
    IPackage IPackage_iface;
    IPackageId IPackageId_iface;
    IPackage3 IPackage3_iface;
    IPackageStatus IPackageStatus_iface;
    LONG ref;
    WCHAR name[128], publisher[256], publisher_id[32], full_name[256], family[256];
    PackageVersion version;
    ProcessorArchitecture arch;
};

static inline struct standin *impl_from_IPackage( IPackage *iface )
{
    return CONTAINING_RECORD( iface, struct standin, IPackage_iface );
}

static HRESULT WINAPI standin_QueryInterface( IPackage *iface, REFIID iid, void **out )
{
    struct standin *impl = impl_from_IPackage( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPackage ))
    {
        IInspectable_AddRef( (*out = &impl->IPackage_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IPackage3 ))
    {
        IInspectable_AddRef( (*out = &impl->IPackage3_iface) );
        return S_OK;
    }
    FIXME( "%s: %s not implemented, returning E_NOINTERFACE.\n", debugstr_w(impl->family), debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI standin_AddRef( IPackage *iface )
{
    return InterlockedIncrement( &impl_from_IPackage( iface )->ref );
}

static ULONG WINAPI standin_Release( IPackage *iface )
{
    struct standin *impl = impl_from_IPackage( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI standin_GetIids( IPackage *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }

static HRESULT WINAPI standin_GetRuntimeClassName( IPackage *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.Package", 32, name );
}

static HRESULT WINAPI standin_GetTrustLevel( IPackage *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI standin_get_Id( IPackage *iface, IPackageId **value )
{
    struct standin *impl = impl_from_IPackage( iface );
    IPackage_AddRef( iface );
    *value = &impl->IPackageId_iface;
    return S_OK;
}

static HRESULT WINAPI standin_get_InstalledLocation( IPackage *iface, IStorageFolder **value )
{
    FIXME( "%s: no installed location\n", debugstr_w(impl_from_IPackage( iface )->family) );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI standin_get_IsFramework( IPackage *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI standin_get_Dependencies( IPackage *iface, IVectorView_Package **value )
{
    static const struct vector_iids iids =
    {
        .vector = &IID_IVector_Package,
        .view = &IID_IVectorView_Package,
        .iterable = &IID_IIterable_Package,
        .iterator = &IID_IIterator_Package,
    };
    IVector_Package *vector;
    HRESULT hr;

    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;
    hr = IVector_Package_GetView( vector, value );
    IVector_Package_Release( vector );
    return hr;
}

static const struct IPackageVtbl standin_vtbl =
{
    standin_QueryInterface,
    standin_AddRef,
    standin_Release,
    standin_GetIids,
    standin_GetRuntimeClassName,
    standin_GetTrustLevel,
    standin_get_Id,
    standin_get_InstalledLocation,
    standin_get_IsFramework,
    standin_get_Dependencies,
};

static inline struct standin *impl_from_IPackageId( IPackageId *iface )
{
    return CONTAINING_RECORD( iface, struct standin, IPackageId_iface );
}

static HRESULT WINAPI id_QueryInterface( IPackageId *iface, REFIID iid, void **out )
{
    struct standin *impl = impl_from_IPackageId( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPackageId ))
    {
        IPackageId_AddRef( (*out = &impl->IPackageId_iface) );
        return S_OK;
    }
    FIXME( "%s: PackageId %s not implemented, returning E_NOINTERFACE.\n", debugstr_w(impl->family), debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI id_AddRef( IPackageId *iface ) { return standin_AddRef( &impl_from_IPackageId( iface )->IPackage_iface ); }
static ULONG WINAPI id_Release( IPackageId *iface ) { return standin_Release( &impl_from_IPackageId( iface )->IPackage_iface ); }
static HRESULT WINAPI id_GetIids( IPackageId *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }

static HRESULT WINAPI id_GetRuntimeClassName( IPackageId *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.PackageId", 34, name );
}

static HRESULT WINAPI id_GetTrustLevel( IPackageId *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT string( const WCHAR *s, HSTRING *value ) { return WindowsCreateString( s, wcslen( s ), value ); }

static HRESULT WINAPI id_get_Name( IPackageId *iface, HSTRING *value ) { return string( impl_from_IPackageId( iface )->name, value ); }
static HRESULT WINAPI id_get_Version( IPackageId *iface, PackageVersion *value )
{
    *value = impl_from_IPackageId( iface )->version;
    return S_OK;
}
static HRESULT WINAPI id_get_Architecture( IPackageId *iface, ProcessorArchitecture *value )
{
    *value = impl_from_IPackageId( iface )->arch;
    return S_OK;
}
static HRESULT WINAPI id_get_ResourceId( IPackageId *iface, HSTRING *value ) { return string( L"", value ); }
static HRESULT WINAPI id_get_Publisher( IPackageId *iface, HSTRING *value ) { return string( impl_from_IPackageId( iface )->publisher, value ); }
static HRESULT WINAPI id_get_PublisherId( IPackageId *iface, HSTRING *value ) { return string( impl_from_IPackageId( iface )->publisher_id, value ); }
static HRESULT WINAPI id_get_FullName( IPackageId *iface, HSTRING *value ) { return string( impl_from_IPackageId( iface )->full_name, value ); }
static HRESULT WINAPI id_get_FamilyName( IPackageId *iface, HSTRING *value ) { return string( impl_from_IPackageId( iface )->family, value ); }

static const struct IPackageIdVtbl id_vtbl =
{
    id_QueryInterface,
    id_AddRef,
    id_Release,
    id_GetIids,
    id_GetRuntimeClassName,
    id_GetTrustLevel,
    id_get_Name,
    id_get_Version,
    id_get_Architecture,
    id_get_ResourceId,
    id_get_Publisher,
    id_get_PublisherId,
    id_get_FullName,
    id_get_FamilyName,
};

/* IPackage3: an installed package in good order */
DEFINE_IINSPECTABLE( standin3, IPackage3, struct standin, IPackage_iface );

static HRESULT WINAPI standin3_get_Status( IPackage3 *iface, IPackageStatus **value )
{
    struct standin *impl = impl_from_IPackage3( iface );
    IPackage_AddRef( &impl->IPackage_iface );
    *value = &impl->IPackageStatus_iface;
    return S_OK;
}

static HRESULT WINAPI standin3_get_InstalledDate( IPackage3 *iface, DateTime *value )
{
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    value->UniversalTime = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI standin3_GetAppListEntriesAsync( IPackage3 *iface, IAsyncOperation_IVectorView_AppListEntry **operation )
{
    FIXME( "%s: stub!\n", debugstr_w(impl_from_IPackage3( iface )->family) );
    return E_NOTIMPL;
}

static const struct IPackage3Vtbl standin3_vtbl =
{
    standin3_QueryInterface, standin3_AddRef, standin3_Release,
    standin3_GetIids, standin3_GetRuntimeClassName, standin3_GetTrustLevel,
    standin3_get_Status, standin3_get_InstalledDate, standin3_GetAppListEntriesAsync,
};

/* PackageStatus: nothing wrong */
static inline struct standin *impl_from_IPackageStatus( IPackageStatus *iface )
{
    return CONTAINING_RECORD( iface, struct standin, IPackageStatus_iface );
}

static HRESULT WINAPI status_QueryInterface( IPackageStatus *iface, REFIID iid, void **out )
{
    struct standin *impl = impl_from_IPackageStatus( iface );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPackageStatus ))
    {
        IInspectable_AddRef( (*out = &impl->IPackageStatus_iface) );
        return S_OK;
    }
    FIXME( "PackageStatus %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI status_AddRef( IPackageStatus *iface ) { return standin_AddRef( &impl_from_IPackageStatus( iface )->IPackage_iface ); }
static ULONG WINAPI status_Release( IPackageStatus *iface ) { return standin_Release( &impl_from_IPackageStatus( iface )->IPackage_iface ); }
static HRESULT WINAPI status_GetIids( IPackageStatus *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI status_GetRuntimeClassName( IPackageStatus *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.PackageStatus", 38, name );
}
static HRESULT WINAPI status_GetTrustLevel( IPackageStatus *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI status_true( IPackageStatus *iface, boolean *value ) { *value = TRUE; return S_OK; }
static HRESULT WINAPI status_false( IPackageStatus *iface, boolean *value ) { *value = FALSE; return S_OK; }

static const struct IPackageStatusVtbl status_vtbl =
{
    status_QueryInterface, status_AddRef, status_Release,
    status_GetIids, status_GetRuntimeClassName, status_GetTrustLevel,
    status_true,   /* VerifyIsOK */
    status_false, status_false, status_false, status_false, status_false, status_false,
    status_false, status_false, status_false, status_false, status_false,
};

static void reg_string( HKEY key, const WCHAR *value, WCHAR *buf, DWORD len )
{
    len *= sizeof(WCHAR);
    if (RegGetValueW( key, NULL, value, RRF_RT_REG_SZ, NULL, buf, &len )) buf[0] = 0;
}

/* the stand-in package for this family name, if one is registered */
HRESULT standin_package_create( const WCHAR *family, IPackage **out )
{
    WCHAR path[512], version[64];
    struct standin *impl;
    DWORD arch = 9, len = sizeof(arch);   /* ProcessorArchitecture_X64 */
    unsigned int v[4] = {0};
    HKEY key;

    *out = NULL;
    swprintf( path, ARRAY_SIZE(path), L"Software\\Wine\\WineUWP\\StandInPackages\\%s", family );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key )) return S_FALSE;
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        RegCloseKey( key );
        return E_OUTOFMEMORY;
    }
    impl->IPackage_iface.lpVtbl = &standin_vtbl;
    impl->IPackageId_iface.lpVtbl = &id_vtbl;
    impl->IPackage3_iface.lpVtbl = &standin3_vtbl;
    impl->IPackageStatus_iface.lpVtbl = &status_vtbl;
    impl->ref = 1;
    lstrcpynW( impl->family, family, ARRAY_SIZE(impl->family) );
    reg_string( key, L"Name", impl->name, ARRAY_SIZE(impl->name) );
    reg_string( key, L"Publisher", impl->publisher, ARRAY_SIZE(impl->publisher) );
    reg_string( key, L"PublisherId", impl->publisher_id, ARRAY_SIZE(impl->publisher_id) );
    reg_string( key, L"FullName", impl->full_name, ARRAY_SIZE(impl->full_name) );
    reg_string( key, L"Version", version, ARRAY_SIZE(version) );
    RegGetValueW( key, NULL, L"Architecture", RRF_RT_REG_DWORD, NULL, &arch, &len );
    RegCloseKey( key );
    swscanf( version, L"%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3] );
    impl->version.Major = v[0];
    impl->version.Minor = v[1];
    impl->version.Build = v[2];
    impl->version.Revision = v[3];
    impl->arch = arch;
    TRACE( "stand-in %s: %s\n", debugstr_w(family), debugstr_w(impl->full_name) );
    *out = &impl->IPackage_iface;
    return S_OK;
}

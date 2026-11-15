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
 * Packages the Microsoft Store installed are registered the same way, with a Location, under
 *   HKLM\Software\Wine\WineUWP\InstalledPackages\<family name>
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
    IPackage2 IPackage2_iface;
    const void **package4_vtbl;     /* IPackage4, see package4_vtbl */
    LONG ref;
    WCHAR name[128], publisher[256], publisher_id[32], full_name[256], family[256];
    WCHAR display_name[256], publisher_display_name[256];
    DWORD signature_kind;           /* PackageSignatureKind */
    PackageVersion version;
    ProcessorArchitecture arch;
};

static const GUID IID_IPackage4_local = { 0x65aed1ae, 0xb95b, 0x450c, { 0x88, 0x2b, 0x62, 0x55, 0x18, 0x7f, 0x39, 0x7e } };

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
    /* the Microsoft Store reads both before it counts a package as one of its installs */
    if (IsEqualGUID( iid, &IID_IPackage2 ))
    {
        IInspectable_AddRef( (*out = &impl->IPackage2_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IPackage4_local ))
    {
        IInspectable_AddRef( (*out = &impl->package4_vtbl) );
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

static HRESULT WINAPI standin_GetIids( IPackage *iface, ULONG *count, IID **iids )
{
    TRACE( "%p\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI standin_GetRuntimeClassName( IPackage *iface, HSTRING *name )
{
    TRACE( "%p\n", iface );
    return WindowsCreateString( L"Windows.ApplicationModel.Package", 32, name );
}

static HRESULT WINAPI standin_GetTrustLevel( IPackage *iface, TrustLevel *level )
{
    TRACE( "%p\n", iface );
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI standin_get_Id( IPackage *iface, IPackageId **value )
{
    TRACE( "%p\n", iface );
    struct standin *impl = impl_from_IPackage( iface );
    IPackage_AddRef( iface );
    *value = &impl->IPackageId_iface;
    return S_OK;
}

static HRESULT WINAPI standin_get_InstalledLocation( IPackage *iface, IStorageFolder **value )
{
    TRACE( "%p\n", iface );
    FIXME( "%s: no installed location\n", debugstr_w(impl_from_IPackage( iface )->family) );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI standin_get_IsFramework( IPackage *iface, boolean *value )
{
    TRACE( "%p\n", iface );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI standin_get_Dependencies( IPackage *iface, IVectorView_Package **value )
{
    TRACE( "%p\n", iface );
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
static HRESULT WINAPI id_GetIids( IPackageId *iface, ULONG *count, IID **iids )
{
    TRACE( "%p\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI id_GetRuntimeClassName( IPackageId *iface, HSTRING *name )
{
    TRACE( "%p\n", iface );
    return WindowsCreateString( L"Windows.ApplicationModel.PackageId", 34, name );
}

static HRESULT WINAPI id_GetTrustLevel( IPackageId *iface, TrustLevel *level )
{
    TRACE( "%p\n", iface );
    *level = BaseTrust;
    return S_OK;
}

static HRESULT string( const WCHAR *s, HSTRING *value ) { return WindowsCreateString( s, wcslen( s ), value ); }

static HRESULT WINAPI id_get_Name( IPackageId *iface, HSTRING *value )
{
    TRACE( "%p\n", iface );
    return string( impl_from_IPackageId( iface )->name, value );
}
static HRESULT WINAPI id_get_Version( IPackageId *iface, PackageVersion *value )
{
    TRACE( "%p\n", iface );
    *value = impl_from_IPackageId( iface )->version;
    return S_OK;
}
static HRESULT WINAPI id_get_Architecture( IPackageId *iface, ProcessorArchitecture *value )
{
    TRACE( "%p\n", iface );
    *value = impl_from_IPackageId( iface )->arch;
    return S_OK;
}
static HRESULT WINAPI id_get_ResourceId( IPackageId *iface, HSTRING *value )
{
    TRACE( "%p\n", iface );
    return string( L"", value );
}
static HRESULT WINAPI id_get_Publisher( IPackageId *iface, HSTRING *value )
{
    TRACE( "%p\n", iface );
    return string( impl_from_IPackageId( iface )->publisher, value );
}
static HRESULT WINAPI id_get_PublisherId( IPackageId *iface, HSTRING *value )
{
    TRACE( "%p\n", iface );
    return string( impl_from_IPackageId( iface )->publisher_id, value );
}
static HRESULT WINAPI id_get_FullName( IPackageId *iface, HSTRING *value )
{
    TRACE( "%p\n", iface );
    return string( impl_from_IPackageId( iface )->full_name, value );
}
static HRESULT WINAPI id_get_FamilyName( IPackageId *iface, HSTRING *value )
{
    TRACE( "%p\n", iface );
    return string( impl_from_IPackageId( iface )->family, value );
}

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
    TRACE( "%p\n", iface );
    struct standin *impl = impl_from_IPackage3( iface );
    IPackage_AddRef( &impl->IPackage_iface );
    *value = &impl->IPackageStatus_iface;
    return S_OK;
}

static HRESULT WINAPI standin3_get_InstalledDate( IPackage3 *iface, DateTime *value )
{
    TRACE( "%p\n", iface );
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    value->UniversalTime = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI standin3_GetAppListEntriesAsync( IPackage3 *iface, IAsyncOperation_IVectorView_AppListEntry **operation )
{
    TRACE( "%p\n", iface );
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
static HRESULT WINAPI status_GetIids( IPackageStatus *iface, ULONG *count, IID **iids )
{
    TRACE( "%p\n", iface );
    return E_NOTIMPL;
}
static HRESULT WINAPI status_GetRuntimeClassName( IPackageStatus *iface, HSTRING *name )
{
    TRACE( "%p\n", iface );
    return WindowsCreateString( L"Windows.ApplicationModel.PackageStatus", 38, name );
}
static HRESULT WINAPI status_GetTrustLevel( IPackageStatus *iface, TrustLevel *level )
{
    TRACE( "%p\n", iface );
    *level = BaseTrust; return S_OK;
}
static HRESULT WINAPI status_true( IPackageStatus *iface, boolean *value )
{
    TRACE( "%p\n", iface );
    *value = TRUE; return S_OK;
}
static HRESULT WINAPI status_false( IPackageStatus *iface, boolean *value )
{
    TRACE( "%p\n", iface );
    *value = FALSE; return S_OK;
}

static const struct IPackageStatusVtbl status_vtbl =
{
    status_QueryInterface, status_AddRef, status_Release,
    status_GetIids, status_GetRuntimeClassName, status_GetTrustLevel,
    status_true,   /* VerifyIsOK */
    status_false, status_false, status_false, status_false, status_false, status_false,
    status_false, status_false, status_false, status_false, status_false,
};

static HRESULT string( const WCHAR *s, HSTRING *value );

/* IPackage2 */
static inline struct standin *impl_from_IPackage2( IPackage2 *iface ) { return CONTAINING_RECORD( iface, struct standin, IPackage2_iface ); }
static HRESULT WINAPI package2_QueryInterface( IPackage2 *iface, REFIID iid, void **out ) { return standin_QueryInterface( &impl_from_IPackage2( iface )->IPackage_iface, iid, out ); }
static ULONG WINAPI package2_AddRef( IPackage2 *iface ) { return standin_AddRef( &impl_from_IPackage2( iface )->IPackage_iface ); }
static ULONG WINAPI package2_Release( IPackage2 *iface ) { return standin_Release( &impl_from_IPackage2( iface )->IPackage_iface ); }
static HRESULT WINAPI package2_GetIids( IPackage2 *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI package2_GetRuntimeClassName( IPackage2 *iface, HSTRING *name ) { return string( L"Windows.ApplicationModel.Package", name ); }
static HRESULT WINAPI package2_GetTrustLevel( IPackage2 *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI package2_get_DisplayName( IPackage2 *iface, HSTRING *value )
{
    struct standin *impl = impl_from_IPackage2( iface );
    TRACE( "%p\n", iface );
    return string( impl->display_name[0] ? impl->display_name : impl->name, value );
}
static HRESULT WINAPI package2_get_PublisherDisplayName( IPackage2 *iface, HSTRING *value )
{
    struct standin *impl = impl_from_IPackage2( iface );
    TRACE( "%p\n", iface );
    return string( impl->publisher_display_name[0] ? impl->publisher_display_name : impl->publisher, value );
}
static HRESULT WINAPI package2_get_Description( IPackage2 *iface, HSTRING *value ) { return string( L"", value ); }
static HRESULT WINAPI package2_get_Logo( IPackage2 *iface, IUriRuntimeClass **value ) { *value = NULL; return S_OK; }
static HRESULT WINAPI package2_false( IPackage2 *iface, boolean *value ) { *value = FALSE; return S_OK; }

static const struct IPackage2Vtbl package2_vtbl =
{
    package2_QueryInterface, package2_AddRef, package2_Release,
    package2_GetIids, package2_GetRuntimeClassName, package2_GetTrustLevel,
    package2_get_DisplayName, package2_get_PublisherDisplayName, package2_get_Description, package2_get_Logo,
    package2_false, package2_false, package2_false,
};

/* IPackage4 {65aed1ae}, not in our IDL: 6 get_SignatureKind, 7 get_IsOptional, 8 VerifyContentIntegrityAsync */
static struct standin *impl_from_package4( void *iface ) { return CONTAINING_RECORD( iface, struct standin, package4_vtbl ); }
static HRESULT WINAPI package4_QueryInterface( void *iface, REFIID iid, void **out ) { return standin_QueryInterface( &impl_from_package4( iface )->IPackage_iface, iid, out ); }
static ULONG WINAPI package4_AddRef( void *iface ) { return standin_AddRef( &impl_from_package4( iface )->IPackage_iface ); }
static ULONG WINAPI package4_Release( void *iface ) { return standin_Release( &impl_from_package4( iface )->IPackage_iface ); }
static HRESULT WINAPI package4_GetIids( void *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI package4_GetRuntimeClassName( void *iface, HSTRING *name ) { return string( L"Windows.ApplicationModel.Package", name ); }
static HRESULT WINAPI package4_GetTrustLevel( void *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI package4_get_SignatureKind( void *iface, INT32 *value )
{
    TRACE( "%p -> %lu\n", iface, impl_from_package4( iface )->signature_kind );
    *value = impl_from_package4( iface )->signature_kind;
    return S_OK;
}
static HRESULT WINAPI package4_get_IsOptional( void *iface, boolean *value ) { *value = FALSE; return S_OK; }
static HRESULT WINAPI package4_VerifyContentIntegrityAsync( void *iface, void **operation )
{
    FIXME( "%p: not verified\n", iface );
    *operation = NULL;
    return E_NOTIMPL;
}
static const void *package4_vtbl[] =
{
    package4_QueryInterface, package4_AddRef, package4_Release,
    package4_GetIids, package4_GetRuntimeClassName, package4_GetTrustLevel,
    package4_get_SignatureKind, package4_get_IsOptional, package4_VerifyContentIntegrityAsync,
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
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key ))
    {
        /* a package the Store's install control installed (windows.applicationmodel.store.preview.installcontrol
         * register_installed), with the same values */
        swprintf( path, ARRAY_SIZE(path), L"Software\\Wine\\WineUWP\\InstalledPackages\\%s", family );
        if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key )) return S_FALSE;
    }
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        RegCloseKey( key );
        return E_OUTOFMEMORY;
    }
    impl->IPackage_iface.lpVtbl = &standin_vtbl;
    impl->IPackageId_iface.lpVtbl = &id_vtbl;
    impl->IPackage3_iface.lpVtbl = &standin3_vtbl;
    impl->IPackageStatus_iface.lpVtbl = &status_vtbl;
    impl->IPackage2_iface.lpVtbl = &package2_vtbl;
    impl->package4_vtbl = package4_vtbl;
    impl->signature_kind = 3;   /* PackageSignatureKind_Store */
    impl->ref = 1;
    lstrcpynW( impl->family, family, ARRAY_SIZE(impl->family) );
    reg_string( key, L"Name", impl->name, ARRAY_SIZE(impl->name) );
    reg_string( key, L"Publisher", impl->publisher, ARRAY_SIZE(impl->publisher) );
    reg_string( key, L"PublisherId", impl->publisher_id, ARRAY_SIZE(impl->publisher_id) );
    reg_string( key, L"FullName", impl->full_name, ARRAY_SIZE(impl->full_name) );
    reg_string( key, L"Version", version, ARRAY_SIZE(version) );
    RegGetValueW( key, NULL, L"Architecture", RRF_RT_REG_DWORD, NULL, &arch, &len );
    reg_string( key, L"DisplayName", impl->display_name, ARRAY_SIZE(impl->display_name) );
    reg_string( key, L"PublisherDisplayName", impl->publisher_display_name, ARRAY_SIZE(impl->publisher_display_name) );
    len = sizeof(impl->signature_kind);
    RegGetValueW( key, NULL, L"SignatureKind", RRF_RT_REG_DWORD, NULL, &impl->signature_kind, &len );
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

/* every package registered under InstalledPackages, for enumerations without a family */
HRESULT installed_packages( IPackage ***packages, UINT32 *count )
{
    WCHAR family[256];
    DWORD index, len;
    IPackage **array = NULL, **grown, *package;
    UINT32 n = 0;
    HKEY key;

    *packages = NULL;
    *count = 0;
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\WineUWP\\InstalledPackages", 0, KEY_READ, &key ))
        return S_OK;
    for (index = 0; len = ARRAY_SIZE(family), !RegEnumKeyExW( key, index, family, &len, NULL, NULL, NULL, NULL ); index++)
    {
        if (standin_package_create( family, &package ) != S_OK) continue;
        if (!(grown = realloc( array, (n + 1) * sizeof(*array) )))
        {
            IPackage_Release( package );
            break;
        }
        array = grown;
        array[n++] = package;
    }
    RegCloseKey( key );
    *packages = array;
    *count = n;
    return S_OK;
}

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
#define WIDL_using_Windows_Storage_Streams
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
    WCHAR location[MAX_PATH];       /* the stage directory of a package the Store installed, else empty */
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

static HRESULT app_entry_create( struct standin *package, const WCHAR *app_id, IInspectable **out );

/* the first <Application Id="..."> in a stage's manifest */
static BOOL manifest_app_id( const WCHAR *location, WCHAR *id, DWORD len )
{
    WCHAR path[MAX_PATH + 32];
    char *text = NULL, *p, *end;
    DWORD size, read;
    HANDLE file;
    BOOL ret = FALSE;

    swprintf( path, ARRAY_SIZE(path), L"%s\\AppxManifest.xml", location );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if ((size = GetFileSize( file, NULL )) != INVALID_FILE_SIZE && (text = malloc( size + 1 )) &&
        ReadFile( file, text, size, &read, NULL ))
    {
        text[read] = 0;
        if ((p = strstr( text, "<Application " )) && (p = strstr( p, " Id=\"" )) && (end = strchr( p += 5, '"' )))
        {
            char *app_end = strstr( end, "</Application>" ), *none = strstr( end, "AppListEntry=\"none\"" );

            id[MultiByteToWideChar( CP_UTF8, 0, p, min( end - p, (int)len - 1 ), id, len - 1 )] = 0;
            /* uap:VisualElements AppListEntry="none": not in the app list, as the Windows App Runtime's own
             * packages are not (the Store showed them in Library with Open) */
            ret = !none || (app_end && none > app_end);
        }
    }
    free( text );
    CloseHandle( file );
    return ret;
}

static const GUID IID_IVector_AppListEntry_local = { 0xb063023c, 0xb4e2, 0x50f8, { 0x92, 0xe2, 0xdb, 0x9f, 0x0e, 0x88, 0x23, 0x66 } };
static const GUID IID_IIterable_AppListEntry_local = { 0x86f4d4ef, 0xd8fd, 0x5fb5, { 0x80, 0x7c, 0x72, 0xda, 0x8f, 0xc9, 0xe5, 0x44 } };
static const GUID IID_IIterator_AppListEntry_local = { 0xb93e2028, 0x50bc, 0x599e, { 0xb3, 0xd9, 0x42, 0x7b, 0x61, 0xd2, 0x6c, 0x01 } };

/* The app list entries of a package the Store installed: its first application, launched through the stage's
 * wineuwp-launch.exe. The Microsoft Store shows "Open" for an installed package only when it has one. A stand-in
 * for a platform package has none. */
static HRESULT get_app_list_entries( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    static const struct vector_iids iids =
    {
        .vector = &IID_IVector_AppListEntry_local,
        .view = &IID_IVectorView_AppListEntry,
        .iterable = &IID_IIterable_AppListEntry_local,
        .iterator = &IID_IIterator_AppListEntry_local,
    };
    struct standin *impl = impl_from_IPackage3( (IPackage3 *)invoker );
    IVector_IInspectable *vector;
    IVectorView_IInspectable *view;
    IInspectable *entry;
    WCHAR app_id[128];
    HRESULT hr;

    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;
    if (impl->location[0] && manifest_app_id( impl->location, app_id, ARRAY_SIZE(app_id) ) &&
        SUCCEEDED(hr = app_entry_create( impl, app_id, &entry )))
    {
        hr = IVector_IInspectable_Append( vector, entry );
        IInspectable_Release( entry );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( vector, &view );
    IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT WINAPI standin3_GetAppListEntriesAsync( IPackage3 *iface, IAsyncOperation_IVectorView_AppListEntry **operation )
{
    TRACE( "%p\n", iface );
    return async_operation_inspectable_create( &IID_IAsyncOperation_IVectorView_AppListEntry, (IUnknown *)iface, NULL,
                                               get_app_list_entries, (IAsyncOperation_IInspectable **)operation );
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
    reg_string( key, L"Location", impl->location, ARRAY_SIZE(impl->location) );
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

/* IAsyncOperation<boolean>, completed when it is made: LaunchAsync's answer. put_Completed calls the handler at once,
 * so both a .get() and a co_await see it. */
struct bool_op
{
    IAsyncOperation_boolean IAsyncOperation_boolean_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    boolean result;
};

static inline struct bool_op *impl_from_bool_op( IAsyncOperation_boolean *iface ) { return CONTAINING_RECORD( iface, struct bool_op, IAsyncOperation_boolean_iface ); }
static inline struct bool_op *impl_from_bool_info( IAsyncInfo *iface ) { return CONTAINING_RECORD( iface, struct bool_op, IAsyncInfo_iface ); }

static HRESULT WINAPI bool_op_QueryInterface( IAsyncOperation_boolean *iface, REFIID iid, void **out )
{
    struct bool_op *impl = impl_from_bool_op( iface );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_boolean ))
        *out = &impl->IAsyncOperation_boolean_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo )) *out = &impl->IAsyncInfo_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &impl->ref );
    return S_OK;
}
static ULONG WINAPI bool_op_AddRef( IAsyncOperation_boolean *iface ) { return InterlockedIncrement( &impl_from_bool_op( iface )->ref ); }
static ULONG WINAPI bool_op_Release( IAsyncOperation_boolean *iface )
{
    struct bool_op *impl = impl_from_bool_op( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}
static HRESULT WINAPI bool_op_GetIids( IAsyncOperation_boolean *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI bool_op_GetRuntimeClassName( IAsyncOperation_boolean *iface, HSTRING *name )
{
    return string( L"Windows.Foundation.IAsyncOperation`1<Boolean>", name );
}
static HRESULT WINAPI bool_op_GetTrustLevel( IAsyncOperation_boolean *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI bool_op_put_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean *handler )
{
    if (!handler) return E_POINTER;
    return IAsyncOperationCompletedHandler_boolean_Invoke( handler, iface, Completed );
}
static HRESULT WINAPI bool_op_get_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean **handler )
{
    *handler = NULL;
    return S_OK;
}
static HRESULT WINAPI bool_op_GetResults( IAsyncOperation_boolean *iface, boolean *results )
{
    *results = impl_from_bool_op( iface )->result;
    return S_OK;
}
static const struct IAsyncOperation_booleanVtbl bool_op_vtbl =
{
    bool_op_QueryInterface, bool_op_AddRef, bool_op_Release,
    bool_op_GetIids, bool_op_GetRuntimeClassName, bool_op_GetTrustLevel,
    bool_op_put_Completed, bool_op_get_Completed, bool_op_GetResults,
};

static HRESULT WINAPI bool_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out ) { return bool_op_QueryInterface( &impl_from_bool_info( iface )->IAsyncOperation_boolean_iface, iid, out ); }
static ULONG WINAPI bool_info_AddRef( IAsyncInfo *iface ) { return bool_op_AddRef( &impl_from_bool_info( iface )->IAsyncOperation_boolean_iface ); }
static ULONG WINAPI bool_info_Release( IAsyncInfo *iface ) { return bool_op_Release( &impl_from_bool_info( iface )->IAsyncOperation_boolean_iface ); }
static HRESULT WINAPI bool_info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI bool_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name ) { return string( L"Windows.Foundation.IAsyncOperation`1<Boolean>", name ); }
static HRESULT WINAPI bool_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI bool_info_get_Id( IAsyncInfo *iface, UINT32 *id ) { *id = 1; return S_OK; }
static HRESULT WINAPI bool_info_get_Status( IAsyncInfo *iface, AsyncStatus *status ) { *status = Completed; return S_OK; }
static HRESULT WINAPI bool_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code ) { *code = S_OK; return S_OK; }
static HRESULT WINAPI bool_info_Cancel( IAsyncInfo *iface ) { return S_OK; }
static HRESULT WINAPI bool_info_Close( IAsyncInfo *iface ) { return S_OK; }
static const struct IAsyncInfoVtbl bool_info_vtbl =
{
    bool_info_QueryInterface, bool_info_AddRef, bool_info_Release,
    bool_info_GetIids, bool_info_GetRuntimeClassName, bool_info_GetTrustLevel,
    bool_info_get_Id, bool_info_get_Status, bool_info_get_ErrorCode, bool_info_Cancel, bool_info_Close,
};

static HRESULT bool_op_create( boolean result, IAsyncOperation_boolean **out )
{
    struct bool_op *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_boolean_iface.lpVtbl = &bool_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &bool_info_vtbl;
    impl->ref = 1;
    impl->result = result;
    *out = &impl->IAsyncOperation_boolean_iface;
    return S_OK;
}

/* An app the Store installed starts the way the Xbox app starts a game: its stage's wineuwp-launch.exe, with the
 * environment run.sh gave the Store left out (its identity, DLL overrides, framework path, test switches); the
 * launcher sets the app's own from identity.env and launch.env. */
static WCHAR *clean_environment( void )
{
    static const WCHAR *drop[] = { L"WINE_UWP_", L"UWP_", L"WINEDLLOVERRIDES=", L"WINEPATH=", L"WINE_D3D_CONFIG=",
                                   L"WINE_SKIP_NATIVE_THREAD_CALLS=" };
    WCHAR *env = GetEnvironmentStringsW(), *block, *p, *out;
    size_t len = 0;
    unsigned int i;
    BOOL skip;

    for (p = env; p && *p; p += wcslen( p ) + 1) len += wcslen( p ) + 1;
    if (!(block = out = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        if (env) FreeEnvironmentStringsW( env );
        return NULL;
    }
    for (p = env; p && *p; p += wcslen( p ) + 1)
    {
        for (i = 0, skip = FALSE; i < ARRAY_SIZE(drop); i++) if (!wcsnicmp( p, drop[i], wcslen( drop[i] ) )) skip = TRUE;
        if (skip) continue;
        wcscpy( out, p );
        out += wcslen( p ) + 1;
    }
    *out = 0;
    if (env) FreeEnvironmentStringsW( env );
    return block;
}

static BOOL launch_stage( const WCHAR *location )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR exe[MAX_PATH + 32], cmd[MAX_PATH + 40], *env;
    BOOL ret;

    swprintf( exe, ARRAY_SIZE(exe), L"%s\\wineuwp-launch.exe", location );
    swprintf( cmd, ARRAY_SIZE(cmd), L"\"%s\"", exe );
    env = clean_environment();
    ret = CreateProcessW( exe, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env, location, &si, &pi );
    free( env );
    if (!ret)
    {
        WARN( "starting %s failed, error %lu\n", debugstr_w(exe), GetLastError() );
        return FALSE;
    }
    MESSAGE( "wine: started %s\n", debugstr_w(exe) );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return TRUE;
}

/* AppListEntry, with its AppDisplayInfo */
struct app_entry
{
    IAppListEntry IAppListEntry_iface;
    IAppDisplayInfo IAppDisplayInfo_iface;
    const void **entry2_vtbl;   /* IAppListEntry2 {d0a618ad}: 6 get_AppUserModelId */
    const void **entry3_vtbl;   /* IAppListEntry3 {6099f28d}: 6 LaunchForUserAsync */
    LONG ref;
    WCHAR display_name[256], location[MAX_PATH], aumid[512];
};
static const GUID IID_IAppListEntry2_local = { 0xd0a618ad, 0xbf35, 0x42ac, { 0xac, 0x06, 0x86, 0xee, 0xeb, 0x41, 0xd0, 0x4b } };
static const GUID IID_IAppListEntry3_local = { 0x6099f28d, 0xfc32, 0x470a, { 0xbc, 0x69, 0x4b, 0x06, 0x1a, 0x76, 0xef, 0x2e } };
static const void *entry2_vtbl[], *entry3_vtbl[];

static inline struct app_entry *impl_from_IAppListEntry( IAppListEntry *iface ) { return CONTAINING_RECORD( iface, struct app_entry, IAppListEntry_iface ); }
static inline struct app_entry *impl_from_IAppDisplayInfo( IAppDisplayInfo *iface ) { return CONTAINING_RECORD( iface, struct app_entry, IAppDisplayInfo_iface ); }

static HRESULT WINAPI entry_QueryInterface( IAppListEntry *iface, REFIID iid, void **out )
{
    struct app_entry *impl = impl_from_IAppListEntry( iface );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAppListEntry ))
    {
        IInspectable_AddRef( (*out = &impl->IAppListEntry_iface) );
        return S_OK;
    }
    /* the Microsoft Store reads the AppUserModelId before "Open" launches */
    if (IsEqualGUID( iid, &IID_IAppListEntry2_local ))
    {
        IInspectable_AddRef( (*out = &impl->entry2_vtbl) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAppListEntry3_local ))
    {
        IInspectable_AddRef( (*out = &impl->entry3_vtbl) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI entry_AddRef( IAppListEntry *iface ) { return InterlockedIncrement( &impl_from_IAppListEntry( iface )->ref ); }
static ULONG WINAPI entry_Release( IAppListEntry *iface )
{
    struct app_entry *impl = impl_from_IAppListEntry( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}
static HRESULT WINAPI entry_GetIids( IAppListEntry *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI entry_GetRuntimeClassName( IAppListEntry *iface, HSTRING *name ) { return string( L"Windows.ApplicationModel.Core.AppListEntry", name ); }
static HRESULT WINAPI entry_GetTrustLevel( IAppListEntry *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI entry_get_DisplayInfo( IAppListEntry *iface, IAppDisplayInfo **value )
{
    struct app_entry *impl = impl_from_IAppListEntry( iface );
    IAppListEntry_AddRef( iface );
    *value = &impl->IAppDisplayInfo_iface;
    return S_OK;
}
static HRESULT WINAPI entry_LaunchAsync( IAppListEntry *iface, IAsyncOperation_boolean **operation )
{
    struct app_entry *impl = impl_from_IAppListEntry( iface );
    TRACE( "%p, launching %s\n", iface, debugstr_w(impl->location) );
    return bool_op_create( launch_stage( impl->location ), operation );
}
static const struct IAppListEntryVtbl entry_vtbl =
{
    entry_QueryInterface, entry_AddRef, entry_Release,
    entry_GetIids, entry_GetRuntimeClassName, entry_GetTrustLevel,
    entry_get_DisplayInfo, entry_LaunchAsync,
};

static HRESULT WINAPI display_QueryInterface( IAppDisplayInfo *iface, REFIID iid, void **out )
{
    struct app_entry *impl = impl_from_IAppDisplayInfo( iface );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAppDisplayInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IAppDisplayInfo_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI display_AddRef( IAppDisplayInfo *iface ) { return entry_AddRef( &impl_from_IAppDisplayInfo( iface )->IAppListEntry_iface ); }
static ULONG WINAPI display_Release( IAppDisplayInfo *iface ) { return entry_Release( &impl_from_IAppDisplayInfo( iface )->IAppListEntry_iface ); }
static HRESULT WINAPI display_GetIids( IAppDisplayInfo *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI display_GetRuntimeClassName( IAppDisplayInfo *iface, HSTRING *name ) { return string( L"Windows.ApplicationModel.AppDisplayInfo", name ); }
static HRESULT WINAPI display_GetTrustLevel( IAppDisplayInfo *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI display_get_DisplayName( IAppDisplayInfo *iface, HSTRING *value ) { return string( impl_from_IAppDisplayInfo( iface )->display_name, value ); }
static HRESULT WINAPI display_get_Description( IAppDisplayInfo *iface, HSTRING *value ) { return string( L"", value ); }
static HRESULT WINAPI display_GetLogo( IAppDisplayInfo *iface, Size size, IRandomAccessStreamReference **value )
{
    FIXME( "%p: no logo\n", iface );
    *value = NULL;
    return E_NOTIMPL;
}
static const struct IAppDisplayInfoVtbl display_vtbl =
{
    display_QueryInterface, display_AddRef, display_Release,
    display_GetIids, display_GetRuntimeClassName, display_GetTrustLevel,
    display_get_DisplayName, display_get_Description, display_GetLogo,
};

#define ENTRY_VIEW( n ) \
    static struct app_entry *impl_from_entry##n( void *iface ) { return CONTAINING_RECORD( iface, struct app_entry, entry##n##_vtbl ); } \
    static HRESULT WINAPI entry##n##_QueryInterface( void *iface, REFIID iid, void **out ) { return entry_QueryInterface( &impl_from_entry##n( iface )->IAppListEntry_iface, iid, out ); } \
    static ULONG WINAPI entry##n##_AddRef( void *iface ) { return entry_AddRef( &impl_from_entry##n( iface )->IAppListEntry_iface ); } \
    static ULONG WINAPI entry##n##_Release( void *iface ) { return entry_Release( &impl_from_entry##n( iface )->IAppListEntry_iface ); } \
    static HRESULT WINAPI entry##n##_GetIids( void *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; } \
    static HRESULT WINAPI entry##n##_GetRuntimeClassName( void *iface, HSTRING *name ) { return string( L"Windows.ApplicationModel.Core.AppListEntry", name ); } \
    static HRESULT WINAPI entry##n##_GetTrustLevel( void *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
ENTRY_VIEW( 2 )
ENTRY_VIEW( 3 )
static HRESULT WINAPI entry2_get_AppUserModelId( void *iface, HSTRING *value )
{
    TRACE( "%p -> %s\n", iface, debugstr_w(impl_from_entry2( iface )->aumid) );
    return string( impl_from_entry2( iface )->aumid, value );
}
static HRESULT WINAPI entry3_LaunchForUserAsync( void *iface, IInspectable *user, IAsyncOperation_boolean **operation )
{
    return entry_LaunchAsync( &impl_from_entry3( iface )->IAppListEntry_iface, operation );
}
static const void *entry2_vtbl[] =
{
    entry2_QueryInterface, entry2_AddRef, entry2_Release, entry2_GetIids, entry2_GetRuntimeClassName, entry2_GetTrustLevel,
    entry2_get_AppUserModelId,
};
static const void *entry3_vtbl[] =
{
    entry3_QueryInterface, entry3_AddRef, entry3_Release, entry3_GetIids, entry3_GetRuntimeClassName, entry3_GetTrustLevel,
    entry3_LaunchForUserAsync,
};

static HRESULT app_entry_create( struct standin *package, const WCHAR *app_id, IInspectable **out )
{
    struct app_entry *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppListEntry_iface.lpVtbl = &entry_vtbl;
    impl->IAppDisplayInfo_iface.lpVtbl = &display_vtbl;
    impl->entry2_vtbl = entry2_vtbl;
    impl->entry3_vtbl = entry3_vtbl;
    impl->ref = 1;
    swprintf( impl->aumid, ARRAY_SIZE(impl->aumid), L"%s!%s", package->family, app_id );
    lstrcpynW( impl->display_name, package->display_name[0] ? package->display_name : package->name, ARRAY_SIZE(impl->display_name) );
    lstrcpynW( impl->location, package->location, ARRAY_SIZE(impl->location) );
    TRACE( "%s!%s at %s\n", debugstr_w(package->family), debugstr_w(app_id), debugstr_w(impl->location) );
    *out = (IInspectable *)&impl->IAppListEntry_iface;
    return S_OK;
}

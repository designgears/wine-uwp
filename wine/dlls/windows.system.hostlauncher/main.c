/* WinRT Windows.System.Launcher on the host
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
 * Windows' Launcher hands URIs, files and folders to the shell broker, which a prefix does not have:
 * Shapr3D's "Log In" fail-fasted inside it. Here they go to ShellExecute, which winebrowser forwards
 * to the host's handlers (xdg-open), and each operation completes with whether a handler took it.
 *
 * A URI whose scheme an installed Xbox-app game declares (uap:Protocol in its manifest) starts that
 * game instead: games live in $WINE_UWP_GAMES_DATA/stage, marked by a .wine-uwp-xbox file.
 *
 * WINE_UWP_LAUNCHER=log logs a launch and reports success without launching, and leaves the target
 * in C:\users\Public\wineuwp-launched.uri for browser-callback.sh. Test runs need it: xdg-open
 * reaches the user's running browser over D-Bus, whatever display the test is on.
 *
 * The module is not named windows.system.launcher: a builtin of that name would also take over the
 * other classes Windows registers in Windows.System.Launcher.dll (LauncherOptions and others).
 */

#include "initguid.h"
#include "private.h"
#include "shellapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(launcher);

/* only forward-declared, so widl gives them no short names */
#define ILauncherOptions __x_ABI_CWindows_CSystem_CILauncherOptions
#define IFolderLauncherOptions __x_ABI_CWindows_CSystem_CIFolderLauncherOptions

/* What one operation launches or asks about; the parameter of its async callback. */
struct request
{
    IUnknown IUnknown_iface;
    LONG ref;
    WCHAR *target;
    WCHAR *family;
};

static inline struct request *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct request, IUnknown_iface );
}

static HRESULT WINAPI request_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI request_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_IUnknown( iface )->ref );
}

static ULONG WINAPI request_Release( IUnknown *iface )
{
    struct request *impl = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        free( impl->target );
        free( impl->family );
        free( impl );
    }
    return ref;
}

static const IUnknownVtbl request_vtbl =
{
    request_QueryInterface,
    request_AddRef,
    request_Release,
};

static HRESULT request_create( HSTRING target, HSTRING family, IUnknown **out )
{
    struct request *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUnknown_iface.lpVtbl = &request_vtbl;
    impl->ref = 1;
    impl->target = wcsdup( target ? WindowsGetStringRawBuffer( target, NULL ) : L"" );
    if (family && WindowsGetStringLen( family )) impl->family = wcsdup( WindowsGetStringRawBuffer( family, NULL ) );
    if (!impl->target || (family && WindowsGetStringLen( family ) && !impl->family))
    {
        request_Release( &impl->IUnknown_iface );
        return E_OUTOFMEMORY;
    }
    *out = &impl->IUnknown_iface;
    return S_OK;
}

static HRESULT uri_string( IUriRuntimeClass *uri, HSTRING *str )
{
    if (!uri) return E_INVALIDARG;
    return IUriRuntimeClass_get_AbsoluteUri( uri, str );
}

static HRESULT item_path( IUnknown *item, HSTRING *str )
{
    IStorageItem *storage_item;
    HRESULT hr;

    if (!item) return E_INVALIDARG;
    if (FAILED(hr = IUnknown_QueryInterface( item, &IID_IStorageItem, (void **)&storage_item ))) return hr;
    hr = IStorageItem_get_Path( storage_item, str );
    IStorageItem_Release( storage_item );
    return hr;
}

/* Installed games */

static char *read_small_file( const WCHAR *path )
{
    HANDLE file;
    DWORD size, read = 0;
    char *data;

    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = min( GetFileSize( file, NULL ), 4 << 20 );
    if ((data = malloc( size + 1 )))
    {
        ReadFile( file, data, size, &read, NULL );
        data[read] = 0;
    }
    CloseHandle( file );
    return data;
}

static BOOL contains_nocase( const char *hay, const char *needle )
{
    size_t len = strlen( needle );

    if (!len) return FALSE;
    for (; *hay; hay++) if (!_strnicmp( hay, needle, len )) return TRUE;
    return FALSE;
}

/* Whether a game's marker names the package family, or its manifest the scheme. */
static BOOL game_matches( const WCHAR *dir, const WCHAR *family, const WCHAR *scheme )
{
    WCHAR path[MAX_PATH];
    char needle[512], value[256], *marker, *manifest;
    BOOL ret = FALSE;

    swprintf( path, ARRAY_SIZE(path), L"%s\\.wine-uwp-xbox", dir );
    if (!(marker = read_small_file( path ))) return FALSE;
    if (family && WideCharToMultiByte( CP_UTF8, 0, family, -1, value, sizeof(value), NULL, NULL ))
    {
        snprintf( needle, sizeof(needle), "family=%s\n", value );
        ret = contains_nocase( marker, needle );
    }
    free( marker );
    if (ret || !scheme || !WideCharToMultiByte( CP_UTF8, 0, scheme, -1, value, sizeof(value), NULL, NULL ))
        return ret;

    swprintf( path, ARRAY_SIZE(path), L"%s\\AppxManifest.xml", dir );
    if (!(manifest = read_small_file( path ))) return FALSE;
    snprintf( needle, sizeof(needle), "Protocol Name=\"%s\"", value );
    ret = contains_nocase( manifest, needle );
    free( manifest );
    return ret;
}

/* The stage directory of the installed game that is the package family or handles the scheme. */
static BOOL find_game( const WCHAR *family, const WCHAR *scheme, WCHAR *dir, DWORD size )
{
    WCHAR data[MAX_PATH], stage[MAX_PATH], pattern[MAX_PATH], *p;
    WIN32_FIND_DATAW fd;
    BOOL found = FALSE;
    HANDLE find;

    if (!GetEnvironmentVariableW( L"WINE_UWP_GAMES_DATA", data, ARRAY_SIZE(data) )) return FALSE;
    swprintf( stage, ARRAY_SIZE(stage), L"Z:%s/stage", data );
    for (p = stage; *p; p++) if (*p == '/') *p = '\\';
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", stage );

    if ((find = FindFirstFileW( pattern, &fd )) == INVALID_HANDLE_VALUE) return FALSE;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        swprintf( dir, size, L"%s\\%s", stage, fd.cFileName );
        found = game_matches( dir, family, scheme );
    } while (!found && FindNextFileW( find, &fd ));
    FindClose( find );
    return found;
}

/* A game gets the environment a launcher would give it: what run.sh exported for the app starting
 * it (package identity, DLL overrides, framework path, test switches) is left out, and the game's
 * wineuwp-launch.exe sets its own from its identity.env and launch.env. */
static WCHAR *clean_environment(void)
{
    static const WCHAR *drop[] = { L"WINE_UWP_", L"UWP_", L"WINEDLLOVERRIDES=", L"WINEPATH=", L"WINE_D3D_CONFIG=",
                                   L"WINE_SKIP_NATIVE_THREAD_CALLS=" };
    WCHAR *env = GetEnvironmentStringsW(), *block, *p, *out;
    size_t len = 0, i;

    for (p = env; *p; p += wcslen( p ) + 1) len += wcslen( p ) + 1;
    if (!(block = out = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        FreeEnvironmentStringsW( env );
        return NULL;
    }
    for (p = env; *p; p += wcslen( p ) + 1)
    {
        for (i = 0; i < ARRAY_SIZE(drop); i++) if (!wcsnicmp( p, drop[i], wcslen( drop[i] ) )) break;
        if (i < ARRAY_SIZE(drop)) continue;
        wcscpy( out, p );
        out += wcslen( p ) + 1;
    }
    *out = 0;
    FreeEnvironmentStringsW( env );
    return block;
}

static BOOL start_game( const WCHAR *dir, const WCHAR *uri )
{
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 2], *env;
    BOOL ret;

    swprintf( exe, ARRAY_SIZE(exe), L"%s\\wineuwp-launch.exe", dir );
    swprintf( cmd, ARRAY_SIZE(cmd), L"\"%s\"", exe );
    env = clean_environment();
    ret = CreateProcessW( exe, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env, dir, &si, &pi );
    free( env );
    if (!ret)
    {
        ERR( "%s: starting %s failed, error %lu.\n", debugstr_w( uri ), debugstr_w( exe ), GetLastError() );
        return FALSE;
    }
    MESSAGE( "wine: launcher: %s started %s\n", debugstr_w( uri ), debugstr_w( exe ) );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return TRUE;
}

/* The scheme of a URI, or an empty string. */
static void uri_scheme( const WCHAR *uri, WCHAR *scheme, size_t size )
{
    const WCHAR *colon = wcschr( uri, ':' );
    size_t len = colon ? colon - uri : 0;

    if (len >= size) len = 0;
    memcpy( scheme, uri, len * sizeof(WCHAR) );
    scheme[len] = 0;
}

static BOOL is_web_scheme( const WCHAR *scheme )
{
    return !wcsicmp( scheme, L"http" ) || !wcsicmp( scheme, L"https" );
}

/* Launching */

static void log_launch( const WCHAR *target )
{
    HANDLE file;
    DWORD written;
    char *utf8;
    int len;

    file = CreateFileW( L"C:\\users\\Public\\wineuwp-launched.uri", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return;
    len = WideCharToMultiByte( CP_UTF8, 0, target, -1, NULL, 0, NULL, NULL );
    if ((utf8 = malloc( len )))
    {
        WideCharToMultiByte( CP_UTF8, 0, target, -1, utf8, len, NULL, NULL );
        WriteFile( file, utf8, len - 1, &written, NULL );
        free( utf8 );
    }
    CloseHandle( file );
}

/* Hands a URI or path to the host, and says whether a handler took it. Launches are rare and worth
 * seeing, so they are always reported. */
static BOOL launch( const WCHAR *target, BOOL is_uri )
{
    WCHAR scheme[64], dir[MAX_PATH], mode[8];
    BOOL ok;

    if (!*target) return FALSE;
    if (is_uri)
    {
        uri_scheme( target, scheme, ARRAY_SIZE(scheme) );
        if (*scheme && !is_web_scheme( scheme ) && find_game( NULL, scheme, dir, ARRAY_SIZE(dir) ))
            return start_game( dir, target );
    }
    if (GetEnvironmentVariableW( L"WINE_UWP_LAUNCHER", mode, ARRAY_SIZE(mode) ) && !wcscmp( mode, L"log" ))
    {
        log_launch( target );
        MESSAGE( "wine: launcher: %s (dry run, WINE_UWP_LAUNCHER=log)\n", debugstr_w( target ) );
        return TRUE;
    }
    ok = (INT_PTR)ShellExecuteW( NULL, L"open", target, NULL, NULL, SW_SHOWNORMAL ) > 32;
    MESSAGE( "wine: launcher: %s -> %s\n", debugstr_w( target ), ok ? "launched" : "no handler" );
    return ok;
}

static HRESULT launch_uri_bool( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    if (!called_async) return STATUS_PENDING;
    result->vt = VT_BOOL;
    result->boolVal = launch( impl_from_IUnknown( param )->target, TRUE ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT launch_item_bool( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    if (!called_async) return STATUS_PENDING;
    result->vt = VT_BOOL;
    result->boolVal = launch( impl_from_IUnknown( param )->target, FALSE ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT launch_uri_status( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    if (!called_async) return STATUS_PENDING;
    result->vt = VT_I4;
    result->lVal = launch( impl_from_IUnknown( param )->target, TRUE ) ? LaunchUriStatus_Success
                                                                       : LaunchUriStatus_ProtocolUnavailable;
    return S_OK;
}

/* Web and mail links go to the host; another scheme is supported if an installed game declares it. */
static HRESULT query_uri( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct request *request = impl_from_IUnknown( param );
    WCHAR scheme[64], dir[MAX_PATH];

    uri_scheme( request->target, scheme, ARRAY_SIZE(scheme) );
    result->vt = VT_I4;
    if (request->family)
        result->lVal = find_game( request->family, NULL, dir, ARRAY_SIZE(dir) ) ? LaunchQuerySupportStatus_Available
                                                                                : LaunchQuerySupportStatus_AppNotInstalled;
    else if (is_web_scheme( scheme ) || !wcsicmp( scheme, L"mailto" ))
        result->lVal = LaunchQuerySupportStatus_Available;
    else
        result->lVal = find_game( NULL, scheme, dir, ARRAY_SIZE(dir) ) ? LaunchQuerySupportStatus_Available
                                                                       : LaunchQuerySupportStatus_NotSupported;
    TRACE( "%s, family %s -> %ld.\n", debugstr_w( request->target ), debugstr_w( request->family ), result->lVal );
    return S_OK;
}

static HRESULT start_bool( HSTRING target, async_operation_callback callback, IAsyncOperation_boolean **operation )
{
    IUnknown *request;
    HRESULT hr;

    *operation = NULL;
    if (FAILED(hr = request_create( target, NULL, &request ))) return hr;
    hr = async_operation_boolean_create( NULL, request, callback, operation );
    IUnknown_Release( request );
    return hr;
}

static HRESULT launch_uri_async( IUriRuntimeClass *uri, IAsyncOperation_boolean **operation )
{
    HSTRING str;
    HRESULT hr;

    if (FAILED(hr = uri_string( uri, &str ))) return hr;
    hr = start_bool( str, launch_uri_bool, operation );
    WindowsDeleteString( str );
    return hr;
}

static HRESULT launch_item_async( IUnknown *item, IAsyncOperation_boolean **operation )
{
    HSTRING str;
    HRESULT hr;

    if (FAILED(hr = item_path( item, &str ))) return hr;
    hr = start_bool( str, launch_item_bool, operation );
    WindowsDeleteString( str );
    return hr;
}

static HRESULT launch_uri_for_user_async( IUriRuntimeClass *uri, IAsyncOperation_LaunchUriStatus **operation )
{
    IUnknown *request;
    HSTRING str;
    HRESULT hr;

    *operation = NULL;
    if (FAILED(hr = uri_string( uri, &str ))) return hr;
    hr = request_create( str, NULL, &request );
    WindowsDeleteString( str );
    if (FAILED(hr)) return hr;
    hr = async_operation_uri_status_create( NULL, request, launch_uri_status, operation );
    IUnknown_Release( request );
    return hr;
}

static HRESULT query_uri_async( IUriRuntimeClass *uri, HSTRING family, IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    IUnknown *request;
    HSTRING str;
    HRESULT hr;

    *operation = NULL;
    if (FAILED(hr = uri_string( uri, &str ))) return hr;
    hr = request_create( str, family, &request );
    WindowsDeleteString( str );
    if (FAILED(hr)) return hr;
    hr = async_operation_query_status_create( NULL, request, query_uri, operation );
    IUnknown_Release( request );
    return hr;
}

/* The statics */

static struct
{
    IActivationFactory IActivationFactory_iface;
    ILauncherStatics ILauncherStatics_iface;
    ILauncherStatics2 ILauncherStatics2_iface;
    ILauncherStatics3 ILauncherStatics3_iface;
    ILauncherStatics4 ILauncherStatics4_iface;
    ILauncherStatics5 ILauncherStatics5_iface;
} impl;

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILauncherStatics )) *out = &impl.ILauncherStatics_iface;
    else if (IsEqualGUID( iid, &IID_ILauncherStatics2 )) *out = &impl.ILauncherStatics2_iface;
    else if (IsEqualGUID( iid, &IID_ILauncherStatics3 )) *out = &impl.ILauncherStatics3_iface;
    else if (IsEqualGUID( iid, &IID_ILauncherStatics4 )) *out = &impl.ILauncherStatics4_iface;
    else if (IsEqualGUID( iid, &IID_ILauncherStatics5 )) *out = &impl.ILauncherStatics5_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_Launcher, wcslen( RuntimeClass_Windows_System_Launcher ),
                                class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p: static class.\n", iface, instance );
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

#define DEFINE_STATICS_INSPECTABLE( pfx, iface_type )                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    { return factory_QueryInterface( &impl.IActivationFactory_iface, iid, out ); }                 \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface ) { return 2; }                            \
    static ULONG WINAPI pfx##_Release( iface_type *iface ) { return 1; }                           \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    { return factory_GetIids( &impl.IActivationFactory_iface, iid_count, iids ); }                 \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    { return factory_GetRuntimeClassName( &impl.IActivationFactory_iface, class_name ); }          \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    { return factory_GetTrustLevel( &impl.IActivationFactory_iface, trust_level ); }

DEFINE_STATICS_INSPECTABLE( statics, ILauncherStatics )

static HRESULT WINAPI statics_LaunchFileAsync( ILauncherStatics *iface, IStorageFile *file,
                                               IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );
    return launch_item_async( (IUnknown *)file, operation );
}

static HRESULT WINAPI statics_LaunchFileWithOptionsAsync( ILauncherStatics *iface, IStorageFile *file,
                                                          ILauncherOptions *options, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, file %p, options %p, operation %p.\n", iface, file, options, operation );
    return launch_item_async( (IUnknown *)file, operation );
}

static HRESULT WINAPI statics_LaunchUriAsync( ILauncherStatics *iface, IUriRuntimeClass *uri,
                                              IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, operation %p.\n", iface, uri, operation );
    return launch_uri_async( uri, operation );
}

static HRESULT WINAPI statics_LaunchUriWithOptionsAsync( ILauncherStatics *iface, IUriRuntimeClass *uri,
                                                         ILauncherOptions *options, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, options %p, operation %p.\n", iface, uri, options, operation );
    return launch_uri_async( uri, operation );
}

static const struct ILauncherStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* ILauncherStatics methods */
    statics_LaunchFileAsync,
    statics_LaunchFileWithOptionsAsync,
    statics_LaunchUriAsync,
    statics_LaunchUriWithOptionsAsync,
};

DEFINE_STATICS_INSPECTABLE( statics2, ILauncherStatics2 )

/* Launching for results needs the target app to answer through the broker. */
static HRESULT WINAPI statics2_LaunchUriForResultsAsync( ILauncherStatics2 *iface, IUriRuntimeClass *uri,
                                                         ILauncherOptions *options, IInspectable **operation )
{
    FIXME( "iface %p, uri %p, options %p, operation %p stub!\n", iface, uri, options, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_LaunchUriForResultsWithDataAsync( ILauncherStatics2 *iface, IUriRuntimeClass *uri,
                                                                 ILauncherOptions *options, IInspectable *data,
                                                                 IInspectable **operation )
{
    FIXME( "iface %p, uri %p, options %p, data %p, operation %p stub!\n", iface, uri, options, data, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_LaunchUriWithDataAsync( ILauncherStatics2 *iface, IUriRuntimeClass *uri,
                                                       ILauncherOptions *options, IInspectable *data,
                                                       IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, options %p, data %p, operation %p.\n", iface, uri, options, data, operation );
    return launch_uri_async( uri, operation );
}

static HRESULT WINAPI statics2_QueryUriSupportAsync( ILauncherStatics2 *iface, IUriRuntimeClass *uri,
                                                     LaunchQuerySupportType type,
                                                     IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    TRACE( "iface %p, uri %p, type %d, operation %p.\n", iface, uri, type, operation );
    return query_uri_async( uri, NULL, operation );
}

static HRESULT WINAPI statics2_QueryUriSupportWithPackageFamilyNameAsync( ILauncherStatics2 *iface, IUriRuntimeClass *uri,
                                                                          LaunchQuerySupportType type, HSTRING family,
                                                                          IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    TRACE( "iface %p, uri %p, type %d, family %s, operation %p.\n", iface, uri, type, debugstr_hstring( family ), operation );
    return query_uri_async( uri, family, operation );
}

static HRESULT WINAPI statics2_QueryFileSupportAsync( ILauncherStatics2 *iface, IStorageFile *file,
                                                      IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    FIXME( "iface %p, file %p, operation %p stub!\n", iface, file, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_QueryFileSupportWithPackageFamilyNameAsync( ILauncherStatics2 *iface, IStorageFile *file,
                                                                           HSTRING family,
                                                                           IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    FIXME( "iface %p, file %p, family %s, operation %p stub!\n", iface, file, debugstr_hstring( family ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_FindUriSchemeHandlersAsync( ILauncherStatics2 *iface, HSTRING scheme, IInspectable **operation )
{
    FIXME( "iface %p, scheme %s, operation %p stub!\n", iface, debugstr_hstring( scheme ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_FindUriSchemeHandlersWithLaunchUriTypeAsync( ILauncherStatics2 *iface, HSTRING scheme,
                                                                            LaunchQuerySupportType type, IInspectable **operation )
{
    FIXME( "iface %p, scheme %s, type %d, operation %p stub!\n", iface, debugstr_hstring( scheme ), type, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_FindFileHandlersAsync( ILauncherStatics2 *iface, HSTRING extension, IInspectable **operation )
{
    FIXME( "iface %p, extension %s, operation %p stub!\n", iface, debugstr_hstring( extension ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct ILauncherStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* ILauncherStatics2 methods */
    statics2_LaunchUriForResultsAsync,
    statics2_LaunchUriForResultsWithDataAsync,
    statics2_LaunchUriWithDataAsync,
    statics2_QueryUriSupportAsync,
    statics2_QueryUriSupportWithPackageFamilyNameAsync,
    statics2_QueryFileSupportAsync,
    statics2_QueryFileSupportWithPackageFamilyNameAsync,
    statics2_FindUriSchemeHandlersAsync,
    statics2_FindUriSchemeHandlersWithLaunchUriTypeAsync,
    statics2_FindFileHandlersAsync,
};

DEFINE_STATICS_INSPECTABLE( statics3, ILauncherStatics3 )

static HRESULT WINAPI statics3_LaunchFolderAsync( ILauncherStatics3 *iface, IStorageFolder *folder,
                                                  IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, folder %p, operation %p.\n", iface, folder, operation );
    return launch_item_async( (IUnknown *)folder, operation );
}

static HRESULT WINAPI statics3_LaunchFolderWithOptionsAsync( ILauncherStatics3 *iface, IStorageFolder *folder,
                                                             IFolderLauncherOptions *options, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, folder %p, options %p, operation %p.\n", iface, folder, options, operation );
    return launch_item_async( (IUnknown *)folder, operation );
}

static const struct ILauncherStatics3Vtbl statics3_vtbl =
{
    statics3_QueryInterface,
    statics3_AddRef,
    statics3_Release,
    /* IInspectable methods */
    statics3_GetIids,
    statics3_GetRuntimeClassName,
    statics3_GetTrustLevel,
    /* ILauncherStatics3 methods */
    statics3_LaunchFolderAsync,
    statics3_LaunchFolderWithOptionsAsync,
};

DEFINE_STATICS_INSPECTABLE( statics4, ILauncherStatics4 )

/* There are no app URI handlers (https links claimed by apps), so ask about the URI itself. */
static HRESULT WINAPI statics4_QueryAppUriSupportAsync( ILauncherStatics4 *iface, IUriRuntimeClass *uri,
                                                        IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    TRACE( "iface %p, uri %p, operation %p.\n", iface, uri, operation );
    return query_uri_async( uri, NULL, operation );
}

static HRESULT WINAPI statics4_QueryAppUriSupportWithPackageFamilyNameAsync( ILauncherStatics4 *iface, IUriRuntimeClass *uri,
                                                                             HSTRING family,
                                                                             IAsyncOperation_LaunchQuerySupportStatus **operation )
{
    TRACE( "iface %p, uri %p, family %s, operation %p.\n", iface, uri, debugstr_hstring( family ), operation );
    return query_uri_async( uri, family, operation );
}

static HRESULT WINAPI statics4_FindAppUriHandlersAsync( ILauncherStatics4 *iface, IUriRuntimeClass *uri, IInspectable **operation )
{
    FIXME( "iface %p, uri %p, operation %p stub!\n", iface, uri, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

/* A prefix has one user. */
static HRESULT WINAPI statics4_LaunchUriForUserAsync( ILauncherStatics4 *iface, IUser *user, IUriRuntimeClass *uri,
                                                      IAsyncOperation_LaunchUriStatus **operation )
{
    TRACE( "iface %p, user %p, uri %p, operation %p.\n", iface, user, uri, operation );
    return launch_uri_for_user_async( uri, operation );
}

static HRESULT WINAPI statics4_LaunchUriWithOptionsForUserAsync( ILauncherStatics4 *iface, IUser *user, IUriRuntimeClass *uri,
                                                                 ILauncherOptions *options,
                                                                 IAsyncOperation_LaunchUriStatus **operation )
{
    TRACE( "iface %p, user %p, uri %p, options %p, operation %p.\n", iface, user, uri, options, operation );
    return launch_uri_for_user_async( uri, operation );
}

static HRESULT WINAPI statics4_LaunchUriWithDataForUserAsync( ILauncherStatics4 *iface, IUser *user, IUriRuntimeClass *uri,
                                                              ILauncherOptions *options, IInspectable *data,
                                                              IAsyncOperation_LaunchUriStatus **operation )
{
    TRACE( "iface %p, user %p, uri %p, options %p, data %p, operation %p.\n", iface, user, uri, options, data, operation );
    return launch_uri_for_user_async( uri, operation );
}

static HRESULT WINAPI statics4_LaunchUriForResultsForUserAsync( ILauncherStatics4 *iface, IUser *user, IUriRuntimeClass *uri,
                                                                ILauncherOptions *options, IInspectable **operation )
{
    FIXME( "iface %p, user %p, uri %p, options %p, operation %p stub!\n", iface, user, uri, options, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_LaunchUriForResultsWithDataForUserAsync( ILauncherStatics4 *iface, IUser *user,
                                                                        IUriRuntimeClass *uri, ILauncherOptions *options,
                                                                        IInspectable *data, IInspectable **operation )
{
    FIXME( "iface %p, user %p, uri %p, options %p, data %p, operation %p stub!\n", iface, user, uri, options, data, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct ILauncherStatics4Vtbl statics4_vtbl =
{
    statics4_QueryInterface,
    statics4_AddRef,
    statics4_Release,
    /* IInspectable methods */
    statics4_GetIids,
    statics4_GetRuntimeClassName,
    statics4_GetTrustLevel,
    /* ILauncherStatics4 methods */
    statics4_QueryAppUriSupportAsync,
    statics4_QueryAppUriSupportWithPackageFamilyNameAsync,
    statics4_FindAppUriHandlersAsync,
    statics4_LaunchUriForUserAsync,
    statics4_LaunchUriWithOptionsForUserAsync,
    statics4_LaunchUriWithDataForUserAsync,
    statics4_LaunchUriForResultsForUserAsync,
    statics4_LaunchUriForResultsWithDataForUserAsync,
};

DEFINE_STATICS_INSPECTABLE( statics5, ILauncherStatics5 )

static HRESULT WINAPI statics5_LaunchFolderPathAsync( ILauncherStatics5 *iface, HSTRING path,
                                                      IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, path %s, operation %p.\n", iface, debugstr_hstring( path ), operation );
    return start_bool( path, launch_item_bool, operation );
}

static HRESULT WINAPI statics5_LaunchFolderPathWithOptionsAsync( ILauncherStatics5 *iface, HSTRING path,
                                                                 IFolderLauncherOptions *options,
                                                                 IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, path %s, options %p, operation %p.\n", iface, debugstr_hstring( path ), options, operation );
    return start_bool( path, launch_item_bool, operation );
}

static HRESULT WINAPI statics5_LaunchFolderPathForUserAsync( ILauncherStatics5 *iface, IUser *user, HSTRING path,
                                                             IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, user %p, path %s, operation %p.\n", iface, user, debugstr_hstring( path ), operation );
    return start_bool( path, launch_item_bool, operation );
}

static HRESULT WINAPI statics5_LaunchFolderPathWithOptionsForUserAsync( ILauncherStatics5 *iface, IUser *user, HSTRING path,
                                                                        IFolderLauncherOptions *options,
                                                                        IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, user %p, path %s, options %p, operation %p.\n", iface, user, debugstr_hstring( path ), options, operation );
    return start_bool( path, launch_item_bool, operation );
}

static const struct ILauncherStatics5Vtbl statics5_vtbl =
{
    statics5_QueryInterface,
    statics5_AddRef,
    statics5_Release,
    /* IInspectable methods */
    statics5_GetIids,
    statics5_GetRuntimeClassName,
    statics5_GetTrustLevel,
    /* ILauncherStatics5 methods */
    statics5_LaunchFolderPathAsync,
    statics5_LaunchFolderPathWithOptionsAsync,
    statics5_LaunchFolderPathForUserAsync,
    statics5_LaunchFolderPathWithOptionsForUserAsync,
};

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), RuntimeClass_Windows_System_Launcher ))
        return CLASS_E_CLASSNOTAVAILABLE;

    impl.IActivationFactory_iface.lpVtbl = &factory_vtbl;
    impl.ILauncherStatics_iface.lpVtbl = &statics_vtbl;
    impl.ILauncherStatics2_iface.lpVtbl = &statics2_vtbl;
    impl.ILauncherStatics3_iface.lpVtbl = &statics3_vtbl;
    impl.ILauncherStatics4_iface.lpVtbl = &statics4_vtbl;
    impl.ILauncherStatics5_iface.lpVtbl = &statics5_vtbl;
    *factory = &impl.IActivationFactory_iface;
    return S_OK;
}

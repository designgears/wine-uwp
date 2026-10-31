/*
 * wineuwp-install: the command line of appxinstall
 *
 *   wineuwp-install PACKAGE DATADIR --windows TREE [--stage NAME] [--pause-file FILE]
 *                   [--tags 'a;b'] [--languages 'en-US;en'] [--keys FILE|-] [--keys-once FILE]
 *   wineuwp-install PACKAGE [DATADIR] --info [--windows TREE] [--icon OUT]
 *   wineuwp-install APPINSTALLER --latest
 *   wineuwp-install STOREID|FAMILY [DATADIR --windows TREE [--stage NAME]] --store
 *
 * --store installs the Store's newest package of a product (its Store id) or package family, downloaded from the
 * Store's delivery service; an encrypted one (EAppx/EMsix) is opened with the signed-in account's licence, after a
 * browser sign-in if nobody is signed in. Without DATADIR it only reports what it would install.
 * PACKAGE is a file, an http(s) URL, or an installed package's directory (Program Files\WindowsApps\<full name> of a
 * Windows installation, as stage-app.sh staged them); paths may be Unix paths. Progress goes to stdout as lines:
 * "PID <pid>", "STAGE <what>", "PROGRESS <done> <total>", other messages as they are, and "INSTALLED <stage>" last on
 * success.
 * Exit status 0 on success, 1 on failure (the reason on stderr), 143 when cancelled.
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "wincon.h"
#include "wine/appxinstall.h"

static volatile LONG paused, cancelled;
static char store_keys[8192];   /* --store: the licence's content keys, wiped after the install */
static const WCHAR *pause_file;

/* writes a line as UTF-8 to stdout, which a host process reads */
static void out( const char *format, ... )
{
    char line[4096];
    va_list args;
    DWORD written;
    int n;

    va_start( args, format );
    n = vsnprintf( line, sizeof(line) - 1, format, args );
    va_end( args );
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), line, n, &written, NULL );
}

static char *utf8( const WCHAR *s )
{
    int n = WideCharToMultiByte( CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL );
    char *r = malloc( n );
    if (r) WideCharToMultiByte( CP_UTF8, 0, s, -1, r, n, NULL, NULL );
    return r;
}

static void fail( const char *format, ... )
{
    char line[1024];
    va_list args;
    DWORD written;

    va_start( args, format );
    vsnprintf( line, sizeof(line) - 2, format, args );
    va_end( args );
    strcat( line, "\n" );
    WriteFile( GetStdHandle( STD_ERROR_HANDLE ), line, strlen( line ), &written, NULL );
}

static void CALLBACK progress( void *context, enum appx_event event, UINT64 done, UINT64 total, const WCHAR *text )
{
    char *t = utf8( text ? text : L"" );

    switch (event)
    {
    case APPX_EVENT_STAGE: out( "STAGE %s", t ); break;
    case APPX_EVENT_PROGRESS: out( "PROGRESS %I64u %I64u", done, total ); break;
    case APPX_EVENT_MESSAGE: out( "%s", t ); break;
    case APPX_EVENT_INSTALLED: out( "INSTALLED %s", t ); break;
    }
    free( t );
}

/* the Xbox app's Pause: the Gaming Services stand-in leaves the pause file while it is paused */
static DWORD WINAPI watch_pause( void *arg )
{
    for (;;)
    {
        InterlockedExchange( &paused, GetFileAttributesW( pause_file ) != INVALID_FILE_ATTRIBUTES );
        Sleep( 200 );
    }
    return 0;
}

static BOOL WINAPI on_signal( DWORD type )
{
    InterlockedExchange( &cancelled, 1 );
    return TRUE;
}

/* a Unix path as a Windows one; anything else as it is */
static WCHAR *path_arg( const WCHAR *arg )
{
    WCHAR *dos, *ret;
    char *unix;

    if (arg[0] != '/') return wcsdup( arg );
    if (!(unix = utf8( arg ))) return NULL;
    dos = wine_get_dos_file_name( unix );
    free( unix );
    if (!dos) return NULL;
    ret = wcsdup( dos );
    HeapFree( GetProcessHeap(), 0, dos );
    return ret;
}

/* content keys from a file or stdin ('-'): "kid:key;" pairs on one line, or "kid key" lines; as "kid:key;..." */
static char *read_keys( const WCHAR *source, BOOL once )
{
    HANDLE file;
    char *data, *ret, *p, *line, *next;
    DWORD size, got, total = 0;

    if (!wcscmp( source, L"-" )) file = GetStdHandle( STD_INPUT_HANDLE );
    else file = CreateFileW( source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = 1 << 20;
    if (!(data = malloc( size + 1 ))) return NULL;
    while (total < size && ReadFile( file, data + total, size - total, &got, NULL ) && got) total += got;
    data[total] = 0;
    if (wcscmp( source, L"-" )) CloseHandle( file );
    /* --keys-once: the file is in $XDG_RUNTIME_DIR, the user's alone, and is gone as soon as it is read */
    if (once) DeleteFileW( source );
    if (!(ret = malloc( total * 2 + 2 ))) { free( data ); return NULL; }
    *ret = 0;
    for (line = data; line && *line; line = next)
    {
        char kid[129], key[129];
        if ((next = strpbrk( line, "\r\n" ))) *next++ = 0;
        for (p = line; *p; p++) if (*p == ' ' || *p == '\t') *p = ':';
        if (strchr( line, ';' ) || sscanf( line, "%128[0-9a-fA-F-]:%128[0-9a-fA-F]", kid, key ) != 2)
        {
            if (*line) strcat( ret, line );
            if (*line && line[strlen( line ) - 1] != ';') strcat( ret, ";" );
        }
        else
        {
            strcat( ret, kid );
            strcat( ret, ":" );
            strcat( ret, key );
            strcat( ret, ";" );
        }
    }
    free( data );
    return ret;
}

/* ASCII case-insensitive strstr: the catalogue spells package names in its own case */
static const char *find_nocase( const char *hay, const char *needle )
{
    size_t n = strlen( needle );
    for (; *hay; hay++) if (!_strnicmp( hay, needle, n )) return hay;
    return NULL;
}

/* the string value of "key":"..." at or after p, before stop (if any), into out */
static BOOL json_string_after( const char *p, const char *stop, const char *key, char *out, size_t size )
{
    char pattern[64];
    const char *v, *end;

    snprintf( pattern, sizeof(pattern), "\"%s\":\"", key );
    if (!(v = strstr( p, pattern )) || (stop && v > stop)) return FALSE;
    v += strlen( pattern );
    if (!(end = strchr( v, '"' )) || end - v >= size) return FALSE;
    memcpy( out, v, end - v );
    out[end - v] = 0;
    return TRUE;
}

/* the last "key":"..." before p */
static BOOL json_string_before( const char *start, const char *p, const char *key, char *out, size_t size )
{
    char pattern[64];
    const char *v, *last = NULL;

    snprintf( pattern, sizeof(pattern), "\"%s\":\"", key );
    for (v = start; (v = strstr( v, pattern )) && v < p; v++) last = v;
    return last && json_string_after( last, p, key, out, size );
}

/* winexboxauth.dll: the signed-in account's Store licences */
typedef HRESULT (WINAPI *content_keys_fn)( const char *, const char *, char *, UINT32 );
#define XA_E_MSA 0x80047802   /* winexboxauth: the licensing service (or login.live.com) refused */

static content_keys_fn store_sign_in(void)
{
    typedef HRESULT (WINAPI *signin_fn)( void );
    HMODULE auth = LoadLibraryW( L"winexboxauth.dll" );
    content_keys_fn get_keys = auth ? (void *)GetProcAddress( auth, "XboxAuthContentKeys" ) : NULL;
    signin_fn sign_in = auth ? (void *)GetProcAddress( auth, "XboxAuthSignIn" ) : NULL;
    HRESULT hr;

    if (!get_keys)
    {
        fail( "--store: no winexboxauth.dll for the licence" );
        return NULL;
    }
    if (sign_in)
    {
        out( "STAGE Signing in" );
        if (FAILED(hr = sign_in()))
        {
            switch ((ULONG)hr)
            {
            case 0x80047805: fail( "The sign-in code expired before the sign-in was finished. Run the command again." ); break;
            case 0x80047804: fail( "Could not reach Microsoft's sign-in service. Check the internet connection." ); break;
            case 0x80047802: fail( "Microsoft's sign-in refused the request (error %#lx).", hr ); break;
            case 0x80047803: fail( "Signed in, but Xbox Live refused the account (error %#lx). An Xbox profile may need "
                                   "creating first at https://www.xbox.com.", hr ); break;
            default: fail( "Sign-in failed (error %#lx).", hr ); break;
            }
            return NULL;
        }
    }
    return get_keys;
}

/* one package the catalogue lists for the product */
struct catalogue_package
{
    char full_name[256], format[32], content_id[64];
    ULONGLONG version;
};

static ULONGLONG full_name_version( const char *full_name )
{
    unsigned int v[4] = {0};
    const char *p = strchr( full_name, '_' );
    if (p) sscanf( p + 1, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3] );
    return ((ULONGLONG)v[0] << 48) | ((ULONGLONG)v[1] << 32) | ((ULONGLONG)v[2] << 16) | v[3];
}

/* The catalogue's x64/neutral packages of the family (it lists every version it still serves, each with its own
 * format and content id), newest first. */
static unsigned int catalogue_packages( const char *body, const char *family, struct catalogue_package *list,
                                        unsigned int max )
{
    static const char key[] = "\"PackageFullName\":\"";
    const char *name_end = strchr( family, '_' ), *p, *next;
    unsigned int count = 0, i;

    for (p = strstr( body, key ); p && count < max; p = next)
    {
        struct catalogue_package c = {{0}};
        size_t name_len = name_end ? name_end - family : 0;

        next = strstr( p + 1, key );
        if (!json_string_after( p, NULL, "PackageFullName", c.full_name, sizeof(c.full_name) )) continue;
        if (!name_len || _strnicmp( c.full_name, family, name_len ) || c.full_name[name_len] != '_') continue;
        if (!find_nocase( c.full_name, "_x64_" ) && !find_nocase( c.full_name, "_neutral_" )) continue;
        for (i = 0; i < count; i++) if (!_stricmp( list[i].full_name, c.full_name )) break;
        if (i < count) continue;
        json_string_before( body, p, "PackageFormat", c.format, sizeof(c.format) );
        json_string_after( p, next, "ContentId", c.content_id, sizeof(c.content_id) );
        c.version = full_name_version( c.full_name );
        for (i = count; i > 0 && list[i - 1].version < c.version; i--) list[i] = list[i - 1];
        list[i] = c;
        count++;
    }
    return count;
}

static BOOL is_encrypted( const char *format )
{
    return !_strnicmp( format, "EAppx", 5 ) || !_strnicmp( format, "EMsix", 5 );
}

/* The Store's newest package of a product (Store id, e.g. 9PCDNBHR11MR) or package family, through the catalogue
 * and the delivery service: what an install from the Store would fetch. *url receives its download address, and
 * keys (KEYS_SIZE bytes) its content keys if it is encrypted. With url NULL it only reports what it found.
 *
 * The licensing service no longer issues keys for some packages (fault 3043 DeviceNameNotHexBinary, which it also
 * gives Windows' own device tickets) while it still does for older content of the same product, so when the
 * newest package's licence is refused this goes down the catalogue's older versions. */
static int store_package( const WCHAR *id, WCHAR **url_ret, char *keys, UINT32 keys_size )
{
    struct appx_store_package package;
    struct catalogue_package list[32], *chosen = NULL;
    char *body, *url, category[64], family_a[256];
    unsigned int count, i;
    WCHAR path[512], family[256];
    content_keys_fn get_keys = NULL;
    DWORD status;
    HRESULT hr;

    if (wcschr( id, '_' ))
        swprintf( path, ARRAY_SIZE(path), L"/v7.0/products/lookup?alternateId=PackageFamilyName&value=%s"
                  L"&market=US&languages=en-US&fieldsTemplate=details", id );
    else
        swprintf( path, ARRAY_SIZE(path), L"/v7.0/products/%s?market=US&languages=en-US&fieldsTemplate=details", id );
    if (FAILED(hr = appx_store_get( L"displaycatalog.mp.microsoft.com", path, &body, &status )))
    {
        fail( "--store: catalogue error %#lx", hr );
        return 1;
    }
    if (!json_string_after( body, NULL, "WuCategoryId", category, sizeof(category) ) ||
        !json_string_after( body, NULL, "PackageFamilyName", family_a, sizeof(family_a) ))
    {
        appx_free( body );
        fail( "--store: the catalogue has no app package for this product (HTTP %lu)", status );
        return 1;
    }
    count = catalogue_packages( body, family_a, list, ARRAY_SIZE(list) );
    appx_free( body );
    MultiByteToWideChar( CP_UTF8, 0, family_a, -1, family, ARRAY_SIZE(family) );
    out( "family %s, category %s", family_a, category );
    if (FAILED(hr = appx_store_find_package( category, family, &package )))
    {
        fail( "--store: no package, error %#lx", hr );
        return 1;
    }
    out( "package %s", package.moniker );

    /* the delivery service's newest first, then the catalogue's older ones */
    for (i = 0; i < count; i++)
        if (!_stricmp( list[i].full_name, package.moniker ))
        {
            struct catalogue_package newest = list[i];
            memmove( list + 1, list, i * sizeof(*list) );
            list[0] = newest;
            break;
        }
    if (i == count)
    {
        fail( "--store: the catalogue does not list %s", package.moniker );
        return 1;
    }

    for (i = 0; i < count; i++)
    {
        struct catalogue_package *c = list + i;

        if (i && c->version >= list[0].version) continue;
        out( "%s: format %s%s%s", c->full_name, c->format[0] ? c->format : "unknown",
             c->content_id[0] ? ", content id " : "", c->content_id );
        if (!_strnicmp( c->format, "Xvc", 3 ) || !_strnicmp( c->format, "MsixVc", 6 ))
        {
            fail( "--store: a GDK (.msixvc) title; install it from the Xbox app" );
            return 1;
        }
        if (!is_encrypted( c->format ) || !url_ret)
        {
            chosen = c;
            break;
        }
        if (!c->content_id[0]) continue;
        if (!get_keys && !(get_keys = store_sign_in())) return 1;
        out( "STAGE Getting the licence" );
        hr = get_keys( c->content_id, "US", keys, keys_size );
        if (SUCCEEDED(hr))
        {
            chosen = c;
            break;
        }
        SecureZeroMemory( keys, keys_size );
        if ((ULONG)hr != XA_E_MSA)
        {
            fail( "--store: licence error %#lx", hr );
            return 1;
        }
        out( "the licensing service refused %s; trying an older version", c->full_name );
    }
    if (!chosen)
    {
        if (wcschr( id, '_' ))
            fail( "The Store gave no licence for any version of this package. Does the signed-in account own it?" );
        else
            fail( "The Store gave no licence for any version of this package. Does the signed-in account own it? "
                  "(A free app needs \"Get\" once: https://apps.microsoft.com/detail/%ls)", id );
        return 1;
    }
    if (chosen != list && FAILED(hr = appx_store_find_package_moniker( category, family, chosen->full_name, &package )))
    {
        SecureZeroMemory( keys, keys_size );
        fail( "--store: the delivery service no longer has %s, error %#lx", chosen->full_name, hr );
        return 1;
    }
    if (chosen != list) out( "package %s", package.moniker );

    if (FAILED(hr = appx_store_package_url( &package, &url )))
    {
        if (keys) SecureZeroMemory( keys, keys_size );
        fail( "--store: no download address, error %#lx", hr );
        return 1;
    }
    if (!url_ret)
    {
        out( "url %.48s...", url );
        appx_free( url );
        return 0;
    }
    *url_ret = malloc( (strlen( url ) + 1) * sizeof(WCHAR) );
    if (*url_ret) MultiByteToWideChar( CP_UTF8, 0, url, -1, *url_ret, strlen( url ) + 1 );
    appx_free( url );
    return *url_ret ? 0 : 1;
}

static void usage(void)
{
    fail( "usage: wineuwp-install PACKAGE DATADIR --windows TREE [--stage NAME] [--pause-file FILE] "
          "[--tags T] [--languages L] [--keys FILE|-] [--keys-once FILE]\n"
          "       wineuwp-install PACKAGE [DATADIR] --info [--windows TREE] [--icon OUT]\n"
          "       wineuwp-install APPINSTALLER --latest\n"
          "       wineuwp-install STOREID|FAMILY [DATADIR --windows TREE [--stage NAME]] --store" );
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    struct appx_install_params params = { 0 };
    const WCHAR *positional[2] = { NULL }, *keys = NULL, *icon = NULL;
    WCHAR *package = NULL, *data_dir = NULL, *windows = NULL, *launch_defaults = NULL, *d3d = NULL, *env;
    BOOL info = FALSE, latest = FALSE, store = FALSE, keys_once = FALSE;
    unsigned int npos = 0;
    HRESULT hr;
    int i;

    for (i = 1; i < argc; i++)
    {
        const WCHAR *a = argv[i];
        BOOL has_value = i + 1 < argc;

        if (!wcscmp( a, L"--info" )) info = TRUE;
        else if (!wcscmp( a, L"--latest" )) latest = TRUE;
        else if (!wcscmp( a, L"--store" )) store = TRUE;
        else if (!wcscmp( a, L"--windows" ) && has_value) windows = path_arg( argv[++i] );
        else if (!wcscmp( a, L"--stage" ) && has_value) params.stage = argv[++i];
        else if (!wcscmp( a, L"--pause-file" ) && has_value) pause_file = path_arg( argv[++i] );
        else if (!wcscmp( a, L"--tags" ) && has_value) params.tags = argv[++i];
        else if (!wcscmp( a, L"--languages" ) && has_value) params.languages = argv[++i];
        else if (!wcscmp( a, L"--icon" ) && has_value) icon = path_arg( argv[++i] );
        else if (!wcscmp( a, L"--keys" ) && has_value) keys = argv[++i];
        else if (!wcscmp( a, L"--keys-once" ) && has_value) { keys = path_arg( argv[++i] ); keys_once = TRUE; }
        else if (a[0] == '-' && a[1] == '-') { usage(); return 2; }
        else if (npos < 2) positional[npos++] = a;
        else { usage(); return 2; }
    }
    if (!npos) { usage(); return 2; }
    package = positional[0][0] == '/' ? path_arg( positional[0] ) : wcsdup( positional[0] );
    if (npos > 1) data_dir = path_arg( positional[1] );

    if (latest)
    {
        WCHAR *version;
        char *v;
        if (FAILED(hr = appx_latest( package, &version ))) { fail( "%s: error %#lx", "--latest", hr ); return 1; }
        v = utf8( version );
        out( "%s", v );
        free( v );
        appx_free( version );
        return 0;
    }
    if (store)
    {
        WCHAR *url = NULL;
        int ret;

        if (!data_dir) return store_package( positional[0], NULL, NULL, 0 );
        if (!windows) { fail( "DATADIR and --windows TREE are needed to install" ); return 2; }
        ret = store_package( positional[0], &url, store_keys, sizeof(store_keys) );
        if (ret) { SecureZeroMemory( store_keys, sizeof(store_keys) ); return ret; }
        free( package );
        package = url;
        if (store_keys[0]) params.content_keys = store_keys;
    }
    else if (keys) params.content_keys = read_keys( keys, keys_once );
    else if ((env = _wgetenv( L"WINE_UWP_CONTENT_KEYS" )))
    {
        params.content_keys = utf8( env );
        SetEnvironmentVariableW( L"WINE_UWP_CONTENT_KEYS", NULL );
    }
    if (info)
    {
        char *json;
        DWORD written;
        if (FAILED(hr = appx_info( package, windows, data_dir, icon, params.content_keys, &json ))) { fail( "--info: error %#lx", hr ); return 1; }
        WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), json, strlen( json ), &written, NULL );
        appx_free( json );
        return 0;
    }
    if (!data_dir || !windows) { fail( "DATADIR and --windows TREE are needed to install" ); return 2; }

    /* the repository's launch defaults, where run.sh's environment says they are */
    if ((env = _wgetenv( L"WINE_UWP_ROOT" )))
    {
        WCHAR buf[MAX_PATH];
        swprintf( buf, ARRAY_SIZE(buf), L"%s/launch-defaults.env", env );
        launch_defaults = path_arg( buf );
    }
    /* packaging/install-d3d-runtime.sh's output: DXVK's dxgi/d3d11 and vkd3d-proton's d3d12/d3d12core */
    if ((env = _wgetenv( L"WINE_UWP_D3D_RUNTIME" ))) d3d = path_arg( env );
    else if ((env = _wgetenv( L"UWP_BIN" )) || (env = _wgetenv( L"WINE_UWP_ROOT" )))
    {
        WCHAR buf[MAX_PATH];
        swprintf( buf, ARRAY_SIZE(buf), _wgetenv( L"UWP_BIN" ) ? L"%s/ext/d3d-runtime/x86_64-windows"
                                                                : L"%s/uwp-bin/ext/d3d-runtime/x86_64-windows", env );
        d3d = path_arg( buf );
    }

    params.package = package;
    params.d3d_runtime = d3d;
    params.data_dir = data_dir;
    params.windows_tree = windows;
    params.launch_defaults = launch_defaults;
    params.pause = &paused;
    params.cancel = &cancelled;
    params.callback = progress;
    if (pause_file) CloseHandle( CreateThread( NULL, 0, watch_pause, NULL, 0, NULL ) );
    SetConsoleCtrlHandler( on_signal, TRUE );

    out( "PID %lu", GetCurrentProcessId() );
    hr = appx_install( &params );
    SecureZeroMemory( store_keys, sizeof(store_keys) );
    if (hr == HRESULT_FROM_WIN32( ERROR_CANCELLED ) || cancelled) return 143;
    if (FAILED(hr))
    {
        fail( "install failed: error %#lx", hr );
        return 1;
    }
    return 0;
}

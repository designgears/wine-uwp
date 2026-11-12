/*
 * wineuwp-install: the command line of appxinstall
 *
 *   wineuwp-install PACKAGE DATADIR --windows TREE [--stage NAME] [--pause-file FILE]
 *                   [--tags 'a;b'] [--languages 'en-US;en'] [--keys FILE|-] [--keys-once FILE]
 *   wineuwp-install PACKAGE [DATADIR] --info [--windows TREE] [--icon OUT]
 *   wineuwp-install APPINSTALLER --latest
 *   wineuwp-install STOREID|FAMILY [DATADIR --windows TREE [--stage NAME]] --store [--allow-older]
 *
 * --store installs the Store's newest package of a product (its Store id) or package family, downloaded from the
 * Store's delivery service; an encrypted one (EAppx/EMsix) is opened with the signed-in account's licence, after a
 * browser sign-in if nobody is signed in. Without DATADIR it only reports what it would install. When the licensing
 * service refuses the newest package's licence, --allow-older installs the newest older version the catalogue still
 * lists that it does license.
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
#include "wine/storecatalogue.h"

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

/* A Store id is 12 letters and digits (9PCDNBHR11MR); a package family name is Name_PublisherId, the name 3 to 50
 * letters, digits, dots and dashes, the publisher id 13 base-32 characters. Anything else is refused before it goes
 * into a catalogue URL. A Store id is upper-cased, as the catalogue spells it. */
static BOOL valid_store_id( WCHAR *id, BOOL *family )
{
    const WCHAR *underscore = wcschr( id, '_' ), *p;
    size_t len = wcslen( id );

    if ((*family = underscore != NULL))
    {
        if (underscore - id < 3 || underscore - id > 50 || wcslen( underscore + 1 ) != 13) return FALSE;
        for (p = id; p < underscore; p++)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
                return FALSE;
        for (p = underscore + 1; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return FALSE;
        return TRUE;
    }
    if (len != 12) return FALSE;
    for (p = id; *p; p++)
    {
        if (*p >= 'a' && *p <= 'z') *(WCHAR *)p -= 'a' - 'A';
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return FALSE;
    }
    return TRUE;
}

/* one package the catalogue lists for the product */
struct store_candidate
{
    const char *full_name, *format, *content_id;   /* in the parsed catalogue */
    ULONGLONG version;
};

static ULONGLONG full_name_version( const char *full_name )
{
    unsigned int v[4] = {0};
    const char *p = strchr( full_name, '_' );
    if (p) sscanf( p + 1, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3] );
    return ((ULONGLONG)v[0] << 48) | ((ULONGLONG)v[1] << 32) | ((ULONGLONG)v[2] << 16) | v[3];
}

/* ASCII case-insensitive strstr: the catalogue spells package names in its own case */
static const char *find_nocase( const char *hay, const char *needle )
{
    size_t n = strlen( needle );
    for (; *hay; hay++) if (!_strnicmp( hay, needle, n )) return hay;
    return NULL;
}

/* The catalogue's x64/neutral packages of the family (it lists every version it still serves, each with its own
 * format and content id), newest first; free the array with free(). */
static struct store_candidate *store_candidates( const struct json *product, const char *family, unsigned int *count )
{
    const char *name_end = strchr( family, '_' );
    size_t name_len = name_end ? name_end - family : 0;
    struct catalogue_package *pkgs;
    struct store_candidate *list, c;
    unsigned int n, i, j, total = 0;

    *count = 0;
    if (!name_len || !(pkgs = catalogue_packages( product, &n ))) return NULL;
    if (!(list = calloc( n, sizeof(*list) )))
    {
        free( pkgs );
        return NULL;
    }
    for (i = 0; i < n; i++)
    {
        c.full_name = json_str( json_get( pkgs[i].json, "PackageFullName" ) );
        if (_strnicmp( c.full_name, family, name_len ) || c.full_name[name_len] != '_') continue;
        if (!find_nocase( c.full_name, "_x64_" ) && !find_nocase( c.full_name, "_neutral_" )) continue;
        for (j = 0; j < total; j++) if (!_stricmp( list[j].full_name, c.full_name )) break;
        if (j < total) continue;
        c.format = json_str( json_get( pkgs[i].json, "PackageFormat" ) );
        c.content_id = json_str( json_get( pkgs[i].json, "ContentId" ) );
        c.version = full_name_version( c.full_name );
        for (j = total; j > 0 && list[j - 1].version < c.version; j--) list[j] = list[j - 1];
        list[j] = c;
        total++;
    }
    free( pkgs );
    *count = total;
    return list;
}

/* What the licensing service said when it refused (winexboxauth keeps it), as a line of the output. */
static void refusal_reason( const char *full_name )
{
    typedef void (WINAPI *error_fn)( char *, UINT32 );
    HMODULE auth = GetModuleHandleW( L"winexboxauth.dll" );
    error_fn get_error = auth ? (void *)GetProcAddress( auth, "XboxAuthLicenceError" ) : NULL;
    char why[512] = "";

    if (get_error) get_error( why, sizeof(why) );
    if (why[0]) out( "licensing service, %s: %s", full_name, why );
}

/* Does the signed-in account own the Store product? S_OK yes, S_FALSE no, an error if nobody can tell. */
static HRESULT owns_product( const char *product_id )
{
    typedef HRESULT (WINAPI *owns_fn)( const char * );
    HMODULE auth = GetModuleHandleW( L"winexboxauth.dll" );
    owns_fn owns = auth ? (void *)GetProcAddress( auth, "XboxAuthOwnsProduct" ) : NULL;

    if (!owns || !*product_id) return E_NOTIMPL;
    return owns( product_id );
}

/* The signed-in account doesn't own it: get it with that account, or sign in with another one. */
static void not_owned( const char *product_id )
{
    static char *(CDECL *unix_name)( const WCHAR * );
    WCHAR store[MAX_PATH];
    char *path = NULL;

    if (!unix_name) unix_name = (void *)GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "wine_get_unix_file_name" );
    if (ExpandEnvironmentStringsW( L"%LOCALAPPDATA%\\wineuwp\\xboxauth.bin", store, ARRAY_SIZE(store) ) && unix_name)
        path = unix_name( store );
    fail( "The signed-in account doesn't own this product. \"Get\" it once with that account "
          "(https://apps.microsoft.com/detail/%s; free apps too), or, to sign in with the account that owns it, delete "
          "%s and run this again.", product_id, path ? path : "%LOCALAPPDATA%\\wineuwp\\xboxauth.bin in the prefix" );
    HeapFree( GetProcessHeap(), 0, path );
}

/* The Store's newest package of a product (a Store id) or package family, through the catalogue and the delivery
 * service: what an install from the Store would fetch. *url_ret receives its download address, and keys (keys_size
 * bytes) its content keys if it is encrypted. With url_ret NULL it only reports what it found.
 *
 * The licensing service refuses keys for some packages (HTTP 400, fault 3043 DeviceNameNotHexBinary) while it still
 * issues them for older content of the same product. That is a version the Store doesn't offer any more, so going
 * down the catalogue's older versions only happens when asked to (allow_older, --allow-older). */
static int store_package( const WCHAR *arg, BOOL allow_older, WCHAR **url_ret, char *keys, UINT32 keys_size )
{
    struct appx_store_package package;
    struct store_candidate *list = NULL, *chosen = NULL, newest;
    const struct json *product;
    struct json *root;
    char *body, *url, category[64], family_a[256], product_id[64];
    unsigned int count, i;
    WCHAR path[512], family[256], id[64];
    content_keys_fn get_keys = NULL;
    BOOL by_family, listed;
    DWORD status;
    HRESULT hr;
    int ret = 1;

    if (wcslen( arg ) >= ARRAY_SIZE(id) || !valid_store_id( lstrcpyW( id, arg ), &by_family ))
    {
        fail( "--store: %ls is neither a Store id (12 letters and digits, e.g. 9WZDNCRFHVN5) nor a package family name "
              "(Name_PublisherId)", arg );
        return 2;
    }
    if (by_family)
        swprintf( path, ARRAY_SIZE(path), L"/v7.0/products/lookup?alternateId=PackageFamilyName&value=%s"
                  L"&market=US&languages=en-US&fieldsTemplate=details", id );
    else
        swprintf( path, ARRAY_SIZE(path), L"/v7.0/products/%s?market=US&languages=en-US&fieldsTemplate=details", id );
    if (FAILED(hr = appx_store_get( L"displaycatalog.mp.microsoft.com", path, &body, &status )))
    {
        fail( "--store: catalogue error %#lx", hr );
        return 1;
    }
    root = json_parse( body, strlen( body ) );
    appx_free( body );
    product = catalogue_product( root );
    lstrcpynA( family_a, json_str( json_get( json_get( product, "Properties" ), "PackageFamilyName" ) ), sizeof(family_a) );
    lstrcpynA( product_id, json_str( json_get( product, "ProductId" ) ), sizeof(product_id) );
    if (!product_id[0] && !by_family) WideCharToMultiByte( CP_UTF8, 0, id, -1, product_id, sizeof(product_id), NULL, NULL );
    list = store_candidates( product, family_a, &count );
    lstrcpynA( category, catalogue_wu_category( product, ~0u ), sizeof(category) );
    if (!category[0] || !family_a[0])
    {
        fail( "--store: the catalogue has no app package for this product (HTTP %lu)", status );
        goto done;
    }
    MultiByteToWideChar( CP_UTF8, 0, family_a, -1, family, ARRAY_SIZE(family) );
    out( "family %s, category %s", family_a, category );
    if (FAILED(hr = appx_store_find_package( category, family, &package )))
    {
        fail( "--store: no package, error %#lx", hr );
        goto done;
    }
    out( "package %s", package.moniker );

    /* the delivery service's newest first, then the catalogue's older ones */
    for (i = 0; i < count; i++) if (!_stricmp( list[i].full_name, package.moniker )) break;
    if ((listed = i < count))
    {
        newest = list[i];
        memmove( list + 1, list, i * sizeof(*list) );
        list[0] = newest;
    }
    else
    {
        /* the catalogue doesn't describe it: its format is unknown, so it is installed as is. An encrypted one fails
         * in the installer, which says so. */
        out( "the catalogue does not list %s; installing it without a licence", package.moniker );
        newest.full_name = package.moniker;
        newest.format = newest.content_id = "";
        newest.version = full_name_version( package.moniker );
        chosen = &newest;
    }

    for (i = 0; listed && i < count; i++)
    {
        struct store_candidate *c = list + i;

        if (i && c->version >= list[0].version) continue;
        out( "%s: format %s%s%s", c->full_name, c->format[0] ? c->format : "unknown",
             c->content_id[0] ? ", content id " : "", c->content_id );
        if (catalogue_format_xvc( c->format ))
        {
            fail( "--store: a GDK (.msixvc) title; install it from the Xbox app" );
            goto done;
        }
        if (!catalogue_format_encrypted( c->format ) || !url_ret)
        {
            chosen = c;
            break;
        }
        if (!c->content_id[0]) continue;
        if (!get_keys && !(get_keys = store_sign_in())) goto done;
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
            goto done;
        }
        refusal_reason( c->full_name );
        if (!allow_older)
        {
            if (owns_product( product_id ) == S_FALSE) not_owned( product_id );
            else
                fail( "The Store's licensing service refused the licence for %s. Older versions of this product may "
                      "still be licensed: --allow-older (wine-uwp install-store ID --allow-older) installs the newest "
                      "one that is, which the Store no longer offers.", c->full_name );
            goto done;
        }
        out( "the licensing service refused %s; trying an older version (--allow-older)", c->full_name );
    }
    if (!chosen)
    {
        hr = owns_product( product_id );
        if (hr == S_FALSE) not_owned( product_id );
        else if (hr == S_OK)
            fail( "The Store gave no licence for any version of this package, although the signed-in account owns it. "
                  "The Store refuses new devices once an account has 10 that download Store apps and games, and every "
                  "wine-uwp setup (each prefix, Flatpak or reinstall) is a new device, shown as \"PC\". Remove the ones "
                  "you no longer use at https://account.microsoft.com/devices/content and run this again." );
        else
            fail( "The Store gave no licence for any version of this package. Either the signed-in account doesn't own "
                  "it (a free app needs \"Get\" once: https://apps.microsoft.com/detail/%s), or it has reached the "
                  "Store's limit of 10 download devices (remove old ones at https://account.microsoft.com/devices/content).",
                  *product_id ? product_id : "STOREID" );
        goto done;
    }
    if (chosen != list && chosen != &newest &&
        FAILED(hr = appx_store_find_package_moniker( category, family, chosen->full_name, &package )))
    {
        SecureZeroMemory( keys, keys_size );
        fail( "--store: the delivery service no longer has %s, error %#lx", chosen->full_name, hr );
        goto done;
    }
    if (chosen != list && chosen != &newest) out( "package %s", package.moniker );

    if (FAILED(hr = appx_store_package_url( &package, &url )))
    {
        if (keys) SecureZeroMemory( keys, keys_size );
        fail( "--store: no download address, error %#lx", hr );
        goto done;
    }
    if (!url_ret)
    {
        out( "url %.48s...", url );
        appx_free( url );
        ret = 0;
        goto done;
    }
    *url_ret = malloc( (strlen( url ) + 1) * sizeof(WCHAR) );
    if (*url_ret) MultiByteToWideChar( CP_UTF8, 0, url, -1, *url_ret, strlen( url ) + 1 );
    appx_free( url );
    ret = *url_ret ? 0 : 1;

done:
    free( list );
    json_free( root );
    return ret;
}

static void usage(void)
{
    fail( "usage: wineuwp-install PACKAGE DATADIR --windows TREE [--stage NAME] [--pause-file FILE] "
          "[--tags T] [--languages L] [--keys FILE|-] [--keys-once FILE]\n"
          "       wineuwp-install PACKAGE [DATADIR] --info [--windows TREE] [--icon OUT]\n"
          "       wineuwp-install APPINSTALLER --latest\n"
          "       wineuwp-install STOREID|FAMILY [DATADIR --windows TREE [--stage NAME]] --store [--allow-older]" );
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    struct appx_install_params params = { 0 };
    const WCHAR *positional[2] = { NULL }, *keys = NULL, *icon = NULL;
    WCHAR *package = NULL, *data_dir = NULL, *windows = NULL, *launch_defaults = NULL, *d3d = NULL, *env;
    BOOL info = FALSE, latest = FALSE, store = FALSE, keys_once = FALSE, allow_older = FALSE;
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
        else if (!wcscmp( a, L"--allow-older" )) allow_older = TRUE;
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

        if (!data_dir) return store_package( positional[0], allow_older, NULL, NULL, 0 );
        if (!windows) { fail( "DATADIR and --windows TREE are needed to install" ); return 2; }
        ret = store_package( positional[0], allow_older, &url, store_keys, sizeof(store_keys) );
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

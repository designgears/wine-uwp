/*
 * wineuwp-install: the command line of appxinstall
 *
 *   wineuwp-install PACKAGE DATADIR --windows TREE [--stage NAME] [--pause-file FILE]
 *                   [--tags 'a;b'] [--languages 'en-US;en'] [--keys FILE|-] [--keys-once FILE]
 *   wineuwp-install PACKAGE [DATADIR] --info [--windows TREE] [--icon OUT]
 *   wineuwp-install APPINSTALLER --latest
 *
 * PACKAGE is a file or an http(s) URL; paths may be Unix paths. Progress goes to stdout as lines: "PID <pid>",
 * "STAGE <what>", "PROGRESS <done> <total>", other messages as they are, and "INSTALLED <stage>" last on success.
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

static void usage(void)
{
    fail( "usage: wineuwp-install PACKAGE DATADIR --windows TREE [--stage NAME] [--pause-file FILE] "
          "[--tags T] [--languages L] [--keys FILE|-] [--keys-once FILE]\n"
          "       wineuwp-install PACKAGE [DATADIR] --info [--windows TREE] [--icon OUT]\n"
          "       wineuwp-install APPINSTALLER --latest" );
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    struct appx_install_params params = { 0 };
    const WCHAR *positional[2] = { NULL }, *keys = NULL, *icon = NULL;
    WCHAR *package = NULL, *data_dir = NULL, *windows = NULL, *launch_defaults = NULL, *d3d = NULL, *env;
    BOOL info = FALSE, latest = FALSE, keys_once = FALSE;
    unsigned int npos = 0;
    HRESULT hr;
    int i;

    for (i = 1; i < argc; i++)
    {
        const WCHAR *a = argv[i];
        BOOL has_value = i + 1 < argc;

        if (!wcscmp( a, L"--info" )) info = TRUE;
        else if (!wcscmp( a, L"--latest" )) latest = TRUE;
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
    if (keys) params.content_keys = read_keys( keys, keys_once );
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
    if (hr == HRESULT_FROM_WIN32( ERROR_CANCELLED ) || cancelled) return 143;
    if (FAILED(hr))
    {
        fail( "install failed: error %#lx", hr );
        return 1;
    }
    return 0;
}

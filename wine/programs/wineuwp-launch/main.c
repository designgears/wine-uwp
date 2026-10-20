/*
 * wineuwp-launch: start an installed UWP app with a plain `wine wineuwp-launch.exe`
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
 * The way any Wine launcher (a desktop entry, Lutris) runs a Windows program. run.sh does the same from the host side,
 * but a launcher that only knows "run this exe in this prefix" cannot run a shell script first, so the steps that make
 * a package start happen here, from the stage directory this program sits in (the installer copies it there):
 *
 *   1. the package identity: identity.env, "export NAME=VALUE" lines, becomes the environment. Wine reads
 *      WINE_UWP_PACKAGE_FULL_NAME and friends from it, and a child's Unix environment follows its Windows block.
 *   2. launch.env, same format: the rest of what run.sh exports (DLL overrides, the framework search path, renderer).
 *      WINE_UWP_PACKAGE_PATH is this directory. Values are literal: nothing is expanded.
 *   3. the per-package AppContainer storage key Windows.Globalization writes to (without it the first launch dies
 *      with 0x80070032).
 *   4. CoreMessagingRegistrar started and RUNNING: CoreMessaging.dll waits for it before the first CoreDispatcher.
 *   5. the app, from this directory, with this process's command line tail; its exit code is ours.
 *
 * Failures are shown in a message box: a launcher started from a menu has no console.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winsvc.h"
#include "winuser.h"

static WCHAR dir[MAX_PATH];

static void fail( const WCHAR *format, ... )
{
    WCHAR text[1024];
    va_list args;

    va_start( args, format );
    vswprintf( text, ARRAY_SIZE(text), format, args );
    va_end( args );
    MessageBoxW( NULL, text, L"wine-uwp", MB_OK | MB_ICONERROR );
}

static WCHAR *widen( const char *s, int len )
{
    int n = MultiByteToWideChar( CP_UTF8, 0, s, len, NULL, 0 );
    WCHAR *w = malloc( (n + 1) * sizeof(WCHAR) );
    if (w)
    {
        MultiByteToWideChar( CP_UTF8, 0, s, len, w, n );
        w[n] = 0;
    }
    return w;
}

/* "export NAME=VALUE" / "NAME=VALUE" lines; single or double quotes around VALUE are removed; # starts a comment */
static BOOL load_env_file( const WCHAR *name, BOOL required )
{
    WCHAR path[MAX_PATH];
    char *data, *line, *next;
    LARGE_INTEGER size;
    DWORD got = 0;
    HANDLE file;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", dir, name );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        if (required) fail( L"Cannot read %s\nReinstall the app with the wine-uwp setup.", path );
        return !required;
    }
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > (1 << 20) || !(data = malloc( size.QuadPart + 1 )))
    {
        CloseHandle( file );
        return !required;
    }
    ReadFile( file, data, size.QuadPart, &got, NULL );
    CloseHandle( file );
    data[got] = 0;

    for (line = data; line && *line; line = next)
    {
        char *eq, *end, *value;
        WCHAR *wname, *wvalue;

        if ((next = strchr( line, '\n' ))) *next++ = 0;
        end = line + strlen( line );
        while (end > line && (end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        line += strspn( line, " \t" );
        if (!*line || *line == '#') continue;
        if (!strncmp( line, "export ", 7 )) line += 7;
        if (!(eq = strchr( line, '=' )) || eq == line) continue;
        *eq = 0;
        value = eq + 1;
        end = value + strlen( value );
        if (end - value >= 2 && (*value == '\'' || *value == '"') && end[-1] == *value)
        {
            value++;
            *--end = 0;
        }
        wname = widen( line, -1 );
        wvalue = widen( value, -1 );
        if (wname && wvalue) SetEnvironmentVariableW( wname, wvalue );
        free( wname );
        free( wvalue );
    }
    free( data );
    return TRUE;
}

static void ensure_appcontainer_key( const WCHAR *family )
{
    WCHAR key[512];
    HKEY hkey;

    if (!family || !*family) return;
    swprintf( key, ARRAY_SIZE(key), L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\"
              L"AppContainer\\Storage\\%s\\ResourcesConfig", family );
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_READ, NULL, &hkey, NULL )) RegCloseKey( hkey );
}

static void start_coremessaging(void)
{
    SC_HANDLE scm, service;
    SERVICE_STATUS status;
    int i;

    if (!(scm = OpenSCManagerW( NULL, NULL, SC_MANAGER_CONNECT ))) return;
    if ((service = OpenServiceW( scm, L"CoreMessagingRegistrar", SERVICE_START | SERVICE_QUERY_STATUS )))
    {
        StartServiceW( service, 0, NULL );   /* ERROR_SERVICE_ALREADY_RUNNING is fine */
        for (i = 0; i < 150; i++)            /* 15 s, as run.sh waits */
        {
            if (QueryServiceStatus( service, &status ) && status.dwCurrentState == SERVICE_RUNNING) break;
            Sleep( 100 );
        }
        CloseServiceHandle( service );
    }
    CloseServiceHandle( scm );
}

/* the command line after our own executable name, to pass through to the app */
static const WCHAR *args_tail(void)
{
    const WCHAR *p = GetCommandLineW();

    if (*p == '"')
    {
        p = wcschr( p + 1, '"' );
        p = p ? p + 1 : L"";
    }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static WCHAR *env( const WCHAR *name )
{
    DWORD n = GetEnvironmentVariableW( name, NULL, 0 );
    WCHAR *value;

    if (!n || !(value = malloc( n * sizeof(WCHAR) ))) return NULL;
    GetEnvironmentVariableW( name, value, n );
    return value;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR *exe, *family, *path, *command, *p, *slash;
    const WCHAR *tail;
    DWORD code = 0;
    size_t len;

    GetModuleFileNameW( NULL, dir, ARRAY_SIZE(dir) );
    if ((slash = wcsrchr( dir, '\\' ))) *slash = 0;

    if (!load_env_file( L"identity.env", TRUE )) return 1;
    load_env_file( L"launch.env", FALSE );
    SetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", dir );

    if (!(exe = env( L"UWP_EXE" )) || !*exe)
    {
        fail( L"identity.env names no UWP_EXE" );
        return 1;
    }
    for (p = exe; *p; p++) if (*p == '/') *p = '\\';
    family = env( L"WINE_UWP_PACKAGE_FAMILY_NAME" );
    ensure_appcontainer_key( family );
    free( family );
    start_coremessaging();

    tail = args_tail();
    len = wcslen( dir ) + wcslen( exe ) + 2;
    path = malloc( len * sizeof(WCHAR) );
    command = malloc( (len + wcslen( tail ) + 4) * sizeof(WCHAR) );
    if (!path || !command) return 1;
    swprintf( path, len, L"%s\\%s", dir, exe );
    swprintf( command, len + wcslen( tail ) + 4, *tail ? L"\"%s\" %s" : L"\"%s\"", path, tail );

    if (!CreateProcessW( path, command, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, NULL, dir, &si, &pi ))
    {
        fail( L"Cannot start %s (error %lu)", path, GetLastError() );
        return 1;
    }
    CloseHandle( pi.hThread );
    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hProcess );
    return code;
}

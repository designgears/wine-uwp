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
 * The way any Wine launcher (a desktop entry, Lutris) runs a Windows program, and the way run.sh starts a staged app
 * for testing. The steps that make a package start happen here, from the stage directory this program sits in (the
 * installer copies it there) or the one named by --stage:
 *
 *   1. the package identity: identity.env, "export NAME=VALUE" lines, becomes the environment. Wine reads
 *      WINE_UWP_PACKAGE_FULL_NAME and friends from it, and a child's Unix environment follows its Windows block.
 *   2. launch.env, same format: the rest of what an installed app needs (DLL overrides, the framework search path,
 *      renderer). WINE_UWP_PACKAGE_PATH is the stage directory. Values are literal: nothing is expanded.
 *      With --defaults FILE (run.sh) that file is read instead, where a variable already in the environment wins,
 *      and the framework dependencies come from the frameworks directory beside the stage: a test run controls
 *      its environment, as run.sh always let it.
 *   3. the per-package AppContainer storage key Windows.Globalization writes to (without it the first launch dies
 *      with 0x80070032).
 *   4. CoreMessagingRegistrar started and RUNNING: CoreMessaging.dll waits for it before the first CoreDispatcher.
 *   5. the app (UWP_EXE, or --exe), from the stage directory, with the rest of the command line; its exit code is
 *      ours.
 *
 * local.env, same format and optional, is read last on both paths, so what it sets wins: settings that belong to this
 * install rather than to the package (install-app.py keeps it across updates). One is WINE_UWP_INJECT,
 * "DLL!EXPORT[;DLL!EXPORT...]" with DLL relative to the stage: the app starts suspended, each DLL is loaded into it
 * by a remote LoadLibraryW thread, then EXPORT (a thread start routine) runs there, and the app is resumed - what a
 * mod's launcher does on Windows through the package debugger (Phantom Dust Rebirth's pdlaunch.exe with
 * pdnet.dll!PdInjectedInit), which Wine doesn't have. The app is resumed even when a step fails.
 *
 *   wineuwp-launch.exe [--stage DIR] [--defaults FILE] [--graphics DRIVER] [--exe EXE] [args...]
 *
 * --graphics sets HKCU\Software\Wine\Drivers\Graphics, which has no environment override; run.sh pins x11 so a
 * test cannot land on the desktop compositor. Failures are shown in a message box (a launcher started from a menu
 * has no console) and written to stderr.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winsvc.h"
#include "winuser.h"
#include "psapi.h"

static WCHAR dir[MAX_PATH];
static BOOL quiet;   /* started by run.sh: report on stderr, no message box */

static void fail( const WCHAR *format, ... )
{
    WCHAR text[1024];
    va_list args;

    va_start( args, format );
    vswprintf( text, ARRAY_SIZE(text), format, args );
    va_end( args );
    fprintf( stderr, "wineuwp-launch: %ls\n", text );
    if (!quiet) MessageBoxW( NULL, text, L"wine-uwp", MB_OK | MB_ICONERROR );
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

/* "export NAME=VALUE" / "NAME=VALUE" lines; single or double quotes around VALUE are removed; # starts a comment.
 * With keep, a variable already set is left alone. */
static BOOL load_env_path( const WCHAR *path, BOOL required, BOOL keep )
{
    char *data, *line, *next;
    LARGE_INTEGER size;
    DWORD got = 0;
    HANDLE file;

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
        if (wname && wvalue && !(keep && GetEnvironmentVariableW( wname, NULL, 0 ))) SetEnvironmentVariableW( wname, wvalue );
        free( wname );
        free( wvalue );
    }
    free( data );
    return TRUE;
}

static BOOL load_env_file( const WCHAR *name, BOOL required )
{
    WCHAR path[MAX_PATH];

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", dir, name );
    return load_env_path( path, required, FALSE );
}

static WCHAR *env( const WCHAR *name );

/* runs start(arg) in the process and waits for it */
static BOOL run_remote( HANDLE process, LPTHREAD_START_ROUTINE start, void *arg, const WCHAR *what )
{
    HANDLE thread = CreateRemoteThread( process, NULL, 0, start, arg, 0, NULL );
    DWORD wait;

    if (!thread)
    {
        fprintf( stderr, "wineuwp-launch: %ls: CreateRemoteThread failed (%lu)\n", what, GetLastError() );
        return FALSE;
    }
    wait = WaitForSingleObject( thread, 30000 );
    CloseHandle( thread );
    if (wait == WAIT_OBJECT_0) return TRUE;
    fprintf( stderr, "wineuwp-launch: %ls: no answer in 30 s\n", what );
    return FALSE;
}

static HMODULE find_remote_module( HANDLE process, const WCHAR *path )
{
    HMODULE modules[1024];
    WCHAR name[MAX_PATH];
    DWORD needed = 0, i;

    if (!EnumProcessModulesEx( process, modules, sizeof(modules), &needed, LIST_MODULES_64BIT )) return NULL;
    for (i = 0; i < needed / sizeof(HMODULE) && i < ARRAY_SIZE(modules); i++)
        if (GetModuleFileNameExW( process, modules[i], name, ARRAY_SIZE(name) ) && !wcsicmp( name, path ))
            return modules[i];
    return NULL;
}

/* one "DLL!EXPORT" of WINE_UWP_INJECT into the suspended app */
static void inject( HANDLE process, WCHAR *spec )
{
    WCHAR path[MAX_PATH], *export = wcschr( spec, '!' );
    LPTHREAD_START_ROUTINE load = (LPTHREAD_START_ROUTINE)GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "LoadLibraryW" );
    HMODULE remote, local;
    FARPROC init;
    SIZE_T bytes;
    void *remote_path;
    char name[256];

    if (!export || !export[1])
    {
        fprintf( stderr, "wineuwp-launch: WINE_UWP_INJECT entry %ls is not DLL!EXPORT\n", spec );
        return;
    }
    *export++ = 0;
    if (spec[0] == '\\' || (spec[0] && spec[1] == ':')) lstrcpynW( path, spec, ARRAY_SIZE(path) );
    else swprintf( path, ARRAY_SIZE(path), L"%s\\%s", dir, spec );
    WideCharToMultiByte( CP_ACP, 0, export, -1, name, sizeof(name), NULL, NULL );

    /* the export's offset is the same in both copies: find it in a local one loaded as an image only */
    if (!(local = LoadLibraryExW( path, NULL, DONT_RESOLVE_DLL_REFERENCES )) || !(init = GetProcAddress( local, name )))
    {
        fprintf( stderr, "wineuwp-launch: %ls: cannot load it or no export %s (%lu)\n", path, name, GetLastError() );
        if (local) FreeLibrary( local );
        return;
    }
    bytes = (wcslen( path ) + 1) * sizeof(WCHAR);
    if ((remote_path = VirtualAllocEx( process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE )) &&
        WriteProcessMemory( process, remote_path, path, bytes, NULL ) &&
        run_remote( process, load, remote_path, path ))
    {
        if (!(remote = find_remote_module( process, path )))
            fprintf( stderr, "wineuwp-launch: %ls did not load in the app\n", path );
        else if (run_remote( process, (LPTHREAD_START_ROUTINE)((BYTE *)remote + ((BYTE *)init - (BYTE *)local)), NULL, export ))
            fprintf( stderr, "wineuwp-launch: loaded %ls and ran %s\n", path, name );
    }
    if (remote_path) VirtualFreeEx( process, remote_path, 0, MEM_RELEASE );
    FreeLibrary( local );
}

static WCHAR *env( const WCHAR *name )
{
    DWORD n = GetEnvironmentVariableW( name, NULL, 0 );
    WCHAR *value;

    if (!n || !(value = malloc( n * sizeof(WCHAR) ))) return NULL;
    GetEnvironmentVariableW( name, value, n );
    return value;
}

static BOOL file_exists( const WCHAR *name )
{
    WCHAR path[MAX_PATH];

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", dir, name );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

static void append( WCHAR **str, const WCHAR *part )
{
    size_t len = *str ? wcslen( *str ) : 0;
    WCHAR *grown = realloc( *str, (len + wcslen( part ) + 1) * sizeof(WCHAR) );

    if (!grown) return;
    wcscpy( grown + len, part );
    *str = grown;
}

/* The order a host glob lists them in (glibc's collation): letters and digits first, ignoring case and
 * punctuation, so Microsoft.VCLibs.140.00_14.0... comes before Microsoft.VCLibs.140.00.UWPDesktop_14.0.... */
static int __cdecl compare_names( const void *a, const void *b )
{
    const WCHAR *x = *(const WCHAR **)a, *y = *(const WCHAR **)b;

    for (;;)
    {
        while (*x && !iswalnum( *x )) x++;
        while (*y && !iswalnum( *y )) y++;
        if (!*x || !*y || towlower( *x ) != towlower( *y )) break;
        x++;
        y++;
    }
    if (*x || *y) return (int)towlower( *x ) - (int)towlower( *y );
    return wcscmp( *(const WCHAR **)a, *(const WCHAR **)b );
}

/* What the installer writes into launch.env for an installed app, for a stage beside a frameworks directory:
 * WINE_UWP_PACKAGE_DEPENDENCIES ("fullname|family|path;" each, which GetCurrentPackageInfo returns after the main
 * package) unless set, and every framework root appended to WINEPATH, as Windows adds the package graph to the
 * search path. In the order of the host's sorted directory listing, as run.sh's glob gave them. */
static void add_frameworks(void)
{
    WCHAR pattern[MAX_PATH], root[MAX_PATH], item[1024], *deps = NULL, *path, *slash, **names = NULL, **grown;
    WIN32_FIND_DATAW data;
    const WCHAR *name, *publisher;
    BOOL have_deps = GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", NULL, 0 ) > 0;
    unsigned int count = 0, i;
    HANDLE find;

    wcscpy( root, dir );
    if (!(slash = wcsrchr( root, '\\' ))) return;
    wcscpy( slash + 1, L"frameworks" );
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", root );
    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == '.') continue;
        if (!(grown = realloc( names, (count + 1) * sizeof(*names) ))) break;
        names = grown;
        names[count++] = wcsdup( data.cFileName );
    } while (FindNextFileW( find, &data ));
    FindClose( find );
    qsort( names, count, sizeof(*names), compare_names );

    path = env( L"WINEPATH" );
    for (i = 0; i < count; i++)
    {
        name = names[i];
        publisher = wcsstr( name, L"__" );
        swprintf( item, ARRAY_SIZE(item), L"%s|%.*s_%s|%s\\%s;", name, (int)wcscspn( name, L"_" ), name,
                  publisher ? publisher + 2 : L"", root, name );
        append( &deps, item );
        /* WINEPATH is a host list, as run.sh exported it: Z:\a\b is /a/b */
        swprintf( item, ARRAY_SIZE(item), L"%s%s\\%s", path && *path ? L":" : L"", root, name );
        for (slash = item; *slash; slash++) if (*slash == '\\') *slash = '/';
        if ((slash = wcsstr( item, L"Z:/" )) || (slash = wcsstr( item, L"z:/" )))
            memmove( slash, slash + 2, (wcslen( slash + 2 ) + 1) * sizeof(WCHAR) );
        append( &path, item );
        free( names[i] );
    }
    free( names );
    if (deps && !have_deps) SetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", deps );
    if (path) SetEnvironmentVariableW( L"WINEPATH", path );
    free( deps );
    free( path );
}

static void set_graphics_driver( const WCHAR *driver )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\Drivers", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
        return;
    RegSetValueExW( key, L"Graphics", 0, REG_SZ, (const BYTE *)driver, (wcslen( driver ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
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

/* One argument from *p, unquoted, into out; *p moves past it and the blanks after it. */
static BOOL next_arg( const WCHAR **p, WCHAR *out, size_t size )
{
    const WCHAR *s = *p;
    size_t n = 0;
    BOOL quoted = FALSE;

    if (!*s) return FALSE;
    while (*s && (quoted || (*s != ' ' && *s != '\t')))
    {
        if (*s == '"') quoted = !quoted;
        else if (n + 1 < size) out[n++] = *s;
        s++;
    }
    out[n] = 0;
    while (*s == ' ' || *s == '\t') s++;
    *p = s;
    return TRUE;
}

/* A Unix path (/a/b) as the Windows one Wine maps it to (Z:\a\b); a Windows path as it is. */
static void to_windows_path( WCHAR *path, size_t size )
{
    WCHAR *p;

    if (path[0] == '/' && wcslen( path ) + 3 < size)
    {
        memmove( path + 2, path, (wcslen( path ) + 1) * sizeof(WCHAR) );
        path[0] = 'Z';
        path[1] = ':';
    }
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR arg[MAX_PATH], defaults[MAX_PATH] = L"", graphics[64] = L"", exe_arg[MAX_PATH] = L"";
    WCHAR *exe, *family, *path, *command, *p, *slash, *injects, *spec, *ctx;
    const WCHAR *tail = GetCommandLineW(), *rest;
    BOOL staged = FALSE;
    DWORD code = 0;
    size_t len;

    GetModuleFileNameW( NULL, dir, ARRAY_SIZE(dir) );
    if ((slash = wcsrchr( dir, '\\' ))) *slash = 0;

    next_arg( &tail, arg, ARRAY_SIZE(arg) );   /* our own name */
    for (rest = tail; next_arg( &rest, arg, ARRAY_SIZE(arg) ); tail = rest)
    {
        WCHAR *value = NULL;
        size_t size = MAX_PATH;

        if (!wcscmp( arg, L"--stage" )) value = dir;
        else if (!wcscmp( arg, L"--defaults" )) value = defaults;
        else if (!wcscmp( arg, L"--graphics" )) { value = graphics; size = ARRAY_SIZE(graphics); }
        else if (!wcscmp( arg, L"--exe" )) value = exe_arg;
        else break;
        if (!next_arg( &rest, value, size ))
        {
            fail( L"%s needs a value", arg );
            return 1;
        }
        if (value != graphics) to_windows_path( value, size );
        if (value == dir) staged = quiet = TRUE;
    }

    /* a stage of stage-app.sh's may have no identity.env when the defaults are the whole identity */
    if (!load_env_file( L"identity.env", !staged )) return 1;
    if (!*defaults) load_env_file( L"launch.env", FALSE );
    else
    {
        load_env_path( defaults, TRUE, TRUE );
        add_frameworks();
    }
    load_env_file( L"local.env", FALSE );
    SetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", dir );
    /* EOS's Linux bootstrapper issues native Linux syscalls from its high-address helper */
    if (file_exists( L"EasyAntiCheat\\Settings.json" ) && !GetEnvironmentVariableW( L"WINE_HACK_NATIVE_SYSCALLS", NULL, 0 ))
        SetEnvironmentVariableW( L"WINE_HACK_NATIVE_SYSCALLS", L"1" );

    exe = *exe_arg ? wcsdup( exe_arg ) : env( L"UWP_EXE" );
    if (!exe || !*exe)
    {
        fail( L"identity.env names no UWP_EXE" );
        return 1;
    }
    for (p = exe; *p; p++) if (*p == '/') *p = '\\';
    family = env( L"WINE_UWP_PACKAGE_FAMILY_NAME" );
    ensure_appcontainer_key( family );
    free( family );
    if (*graphics) set_graphics_driver( graphics );
    start_coremessaging();

    len = wcslen( dir ) + wcslen( exe ) + 2;
    path = malloc( len * sizeof(WCHAR) );
    command = malloc( (len + wcslen( tail ) + 4) * sizeof(WCHAR) );
    if (!path || !command) return 1;
    if (exe[0] == '\\' || (exe[0] && exe[1] == ':')) wcscpy( path, exe );
    else swprintf( path, len, L"%s\\%s", dir, exe );
    swprintf( command, len + wcslen( tail ) + 4, *tail ? L"\"%s\" %s" : L"\"%s\"", path, tail );

    injects = env( L"WINE_UWP_INJECT" );
    if (injects && !*injects)
    {
        free( injects );
        injects = NULL;
    }
    if (!CreateProcessW( path, command, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT | (injects ? CREATE_SUSPENDED : 0),
                         NULL, dir, &si, &pi ))
    {
        fail( L"Cannot start %s (error %lu)", path, GetLastError() );
        return 1;
    }
    if (injects)
    {
        for (spec = wcstok( injects, L";", &ctx ); spec; spec = wcstok( NULL, L";", &ctx )) inject( pi.hProcess, spec );
        ResumeThread( pi.hThread );
        free( injects );
    }
    CloseHandle( pi.hThread );
    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hProcess );
    return code;
}

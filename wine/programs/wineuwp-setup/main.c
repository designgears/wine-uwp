/*
 * wineuwp-setup: put the Windows pieces a UWP app needs into a prefix
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
 * What install-overrides.sh did, from inside the prefix:
 *
 *   1. the Windows DLLs named in the native override list, the resource-only and native-only modules, Windows' fonts,
 *      the en-US MUI files of every genuine copy and the WinRT metadata, from a Windows tree (an installation's copy or
 *      one harvested from an ISO) into the prefix. Case variants of a name that a prefix update left next to a
 *      genuine copy are removed, and the genuine file goes in under the lowercase name.
 *   2. native DllOverrides for that list (services hosted by svchost do not get WINEDLLOVERRIDES).
 *   3. the .reg files: the language profile, MRT's single module, Windows' in-process COM classes (generated here
 *      from the tree's SOFTWARE hive), activatable classes pinned to the prefix copy of their DLL (from the tree's
 *      wine-uwp-classes.reg), and wineuwp.reg with the tree's path substituted.
 *   4. the classes Wine serves instead of Windows (StateRepository, the pickers, Gaming Services, Xbox sign-in, ...)
 *      pointed at Wine's DLLs, and the Gaming Services stand-in packages and services.
 *   5. Wine modules a prefix update does not refresh (wineuwp, appxinstall, xgameruntime) and the WebView2 runtime
 *      with Wine's host in front of it.
 *
 *   wineuwp-setup.exe --windows DIR --build DIR --data DIR [--webview2 DIR] [--force] [LIST]
 *   wineuwp-setup.exe --verify --windows DIR ...
 *   wineuwp-setup.exe --new-prefix --windows DIR ...
 *
 * --new-prefix does a new prefix's registrations first (what packaging/new-prefix.sh did with wine reg and two Python
 * generators): the CoreMessagingRegistrar service, the x11 graphics driver, and the tree's WinRT classes and COM
 * interfaces generated from its SOFTWARE hive into the tree (wine-uwp-classes.reg, wine-uwp-interfaces.reg) and
 * imported.
 *
 * Directories are host paths. A prefix update that lands after the writes can rewrite the DllPath of a class Wine
 * also registers; --verify checks each class and writes it again, exiting 1 while any was wrong. The caller waits
 * for the prefix to settle (wineserver -w) and runs it until it passes; with --report the last one names them.
 *
 * Nothing is done when the tree, the build, the data files, system32 and the list are as they were on the last run
 * (the stamp in the prefix); --force skips that check.
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
#include "bcrypt.h"

static const WCHAR default_list[] = L"windows.ui.xaml windows.ui twinapi.appcore coremessaging windows.applicationmodel "
    "wintypes threadpoolwinrt msvcp_win mrmcorer bcp47mrm bcp47langs profapi rometadata windows.globalization icu icuuc "
    "icuin winlangdb globinputhost msftedit msxml6 msxml6r wincorlib userdatatypehelperutil fontgroupsoverride "
    "diagnosticdatasettings coreprivacysettingsstore rmclient cryptowinrt";

static const WCHAR *resource_list[] =
{
    L"MrmDeploy.dll", L"Windows.UI.Xaml.Controls.dll", L"Windows.UI.Xaml.Phone.dll", L"Windows.UI.Xaml.InkControls.dll",
    L"Windows.UI.Xaml.Maps.dll", L"Windows.UI.XamlHost.dll", L"Windows.UI.Xaml.Resources.Common.dll",
    L"Windows.UI.Xaml.Resources.19h1.dll", L"Windows.UI.Xaml.Resources.21h1.dll", L"Windows.UI.Xaml.Resources.rs1.dll",
    L"Windows.UI.Xaml.Resources.rs2.dll", L"Windows.UI.Xaml.Resources.rs3.dll", L"Windows.UI.Xaml.Resources.rs4.dll",
    L"Windows.UI.Xaml.Resources.rs5.dll", L"Windows.UI.Xaml.Resources.th.dll", L"Windows.UI.Xaml.Resources.win81.dll",
    L"Windows.UI.Xaml.Resources.win8rtm.dll",
};

/* Windows DLLs Wine has no builtin for: they only have to be there for delay-loads by bare name. */
static const WCHAR *native_only[] = { L"WindowManagementAPI.dll", L"CoreUIComponents.dll" };

static WCHAR *win11;      /* the Windows tree */
static WCHAR *src;        /* its Windows\System32 */
static WCHAR *build;      /* the Wine build (a build tree, or an installed bin/) */
static WCHAR *data;       /* the .reg files */
static WCHAR *webview2;   /* the harvested EdgeWebView directory */
static WCHAR *prefix;     /* the prefix root */
static WCHAR sys32[MAX_PATH];
static BOOL verbose_fail;

/* ---- strings and paths ---- */

static WCHAR * WINAPIV strfw( const WCHAR *fmt, ... )
{
    va_list args;
    WCHAR *ret;
    int len;

    va_start( args, fmt );
    len = _vscwprintf( fmt, args );
    va_end( args );
    if (!(ret = malloc( (len + 1) * sizeof(WCHAR) ))) exit( 2 );
    va_start( args, fmt );
    vswprintf( ret, len + 1, fmt, args );
    va_end( args );
    return ret;
}

static WCHAR *lower( const WCHAR *s )
{
    WCHAR *ret = _wcsdup( s ), *p;
    for (p = ret; *p; p++) *p = towlower( *p );
    return ret;
}

/* Z: and the host path, as the .reg files and the generated registrations have always named the tree */
static WCHAR *dos_path( const char *unix )
{
    int len = MultiByteToWideChar( CP_UNIXCP, 0, unix, -1, NULL, 0 );
    WCHAR *ret = malloc( (len + 2) * sizeof(WCHAR) ), *p;

    ret[0] = 'Z';
    ret[1] = ':';
    MultiByteToWideChar( CP_UNIXCP, 0, unix, -1, ret + 2, len );
    for (p = ret; *p; p++) if (*p == '/') *p = '\\';
    len = wcslen( ret );
    while (len > 3 && ret[len - 1] == '\\') ret[--len] = 0;
    return ret;
}

static BOOL exists( const WCHAR *path )
{
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

static BOOL is_dir( const WCHAR *path )
{
    DWORD attr = GetFileAttributesW( path );
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* a directory's entries, without . and .. */
struct listing
{
    WCHAR **names;
    unsigned count;
};

static void list_dir( const WCHAR *dir, struct listing *l )
{
    WIN32_FIND_DATAW fd;
    WCHAR *pattern = strfw( L"%s\\*", dir );
    HANDLE h = FindFirstFileW( pattern, &fd );

    memset( l, 0, sizeof(*l) );
    free( pattern );
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!wcscmp( fd.cFileName, L"." ) || !wcscmp( fd.cFileName, L".." )) continue;
        l->names = realloc( l->names, (l->count + 1) * sizeof(*l->names) );
        l->names[l->count++] = _wcsdup( fd.cFileName );
    } while (FindNextFileW( h, &fd ));
    FindClose( h );
}

static void free_listing( struct listing *l )
{
    while (l->count) free( l->names[--l->count] );
    free( l->names );
}

/* the first entry of dir whose name is name in any case, as ls | grep -i | head -1 */
static WCHAR *find_any_case( const struct listing *l, const WCHAR *name )
{
    unsigned i;
    for (i = 0; i < l->count; i++) if (!wcsicmp( l->names[i], name )) return l->names[i];
    return NULL;
}

static BOOL read_file( const WCHAR *path, BYTE **data_out, DWORD *size_out )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    LARGE_INTEGER size;
    DWORD done;
    BYTE *buf;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > 0x7fffffff || !(buf = malloc( size.QuadPart + 2 )))
    {
        CloseHandle( file );
        return FALSE;
    }
    if (!ReadFile( file, buf, size.QuadPart, &done, NULL ) || done != size.QuadPart)
    {
        free( buf );
        CloseHandle( file );
        return FALSE;
    }
    CloseHandle( file );
    buf[done] = buf[done + 1] = 0;
    *data_out = buf;
    if (size_out) *size_out = done;
    return TRUE;
}

static BOOL write_file( const WCHAR *path, const void *buf, DWORD size )
{
    HANDLE file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    DWORD done;
    BOOL ok;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile( file, buf, size, &done, NULL ) && done == size;
    CloseHandle( file );
    return ok;
}

/* cmp -s */
static BOOL same_file( const WCHAR *a, const WCHAR *b )
{
    HANDLE fa, fb;
    LARGE_INTEGER sa, sb;
    static BYTE ba[1 << 16], bb[1 << 16];
    DWORD na, nb;
    BOOL same = FALSE;

    fa = CreateFileW( a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (fa == INVALID_HANDLE_VALUE) return FALSE;
    fb = CreateFileW( b, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (fb == INVALID_HANDLE_VALUE)
    {
        CloseHandle( fa );
        return FALSE;
    }
    if (GetFileSizeEx( fa, &sa ) && GetFileSizeEx( fb, &sb ) && sa.QuadPart == sb.QuadPart)
    {
        for (;;)
        {
            if (!ReadFile( fa, ba, sizeof(ba), &na, NULL ) || !ReadFile( fb, bb, sizeof(bb), &nb, NULL ) || na != nb) break;
            if (!na)
            {
                same = TRUE;
                break;
            }
            if (memcmp( ba, bb, na )) break;
        }
    }
    CloseHandle( fa );
    CloseHandle( fb );
    return same;
}

static BOOL copy_file( const WCHAR *from, const WCHAR *to )
{
    if (CopyFileW( from, to, FALSE )) return TRUE;
    fprintf( stderr, "wineuwp-setup: copying %ls to %ls failed, error %lu\n", from, to, GetLastError() );
    return FALSE;
}

/* copy when missing or different */
static BOOL refresh_file( const WCHAR *from, const WCHAR *to )
{
    if (exists( to ) && same_file( from, to )) return FALSE;
    copy_file( from, to );
    return TRUE;
}

static void make_dirs( const WCHAR *path )
{
    WCHAR *p = _wcsdup( path ), *s;

    for (s = p + 3; *s; s++)
    {
        if (*s != '\\') continue;
        *s = 0;
        CreateDirectoryW( p, NULL );
        *s = '\\';
    }
    CreateDirectoryW( p, NULL );
    free( p );
}

/* cp -a of a directory tree */
static void copy_tree( const WCHAR *from, const WCHAR *to )
{
    struct listing l;
    unsigned i;

    CreateDirectoryW( to, NULL );
    list_dir( from, &l );
    for (i = 0; i < l.count; i++)
    {
        WCHAR *a = strfw( L"%s\\%s", from, l.names[i] ), *b = strfw( L"%s\\%s", to, l.names[i] );
        if (is_dir( a )) copy_tree( a, b );
        else copy_file( a, b );
        free( a );
        free( b );
    }
    free_listing( &l );
}

/* a Wine PE module of the build: dlls/NAME/x86_64-windows/NAME.dll in a build tree, lib/wine/x86_64-windows beside an
 * installed bin/ */
static WCHAR *wine_pe( const WCHAR *name )
{
    WCHAR *path = strfw( L"%s\\dlls\\%s\\x86_64-windows\\%s.dll", build, name, name );
    if (exists( path )) return path;
    free( path );
    return strfw( L"%s\\..\\lib\\wine\\x86_64-windows\\%s.dll", build, name );
}

static BOOL is_wine_builtin( const WCHAR *path )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    char head[128];
    DWORD done = 0, i;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile( file, head, sizeof(head), &done, NULL );
    CloseHandle( file );
    for (i = 0; i + 16 <= done; i++) if (!memcmp( head + i, "Wine builtin DLL", 16 )) return TRUE;
    return FALSE;
}

/* ---- registry ---- */

static void set_sz( HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value )
{
    HKEY key;
    if (RegCreateKeyExW( root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)value, (wcslen( value ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

static void set_dword( HKEY root, const WCHAR *path, const WCHAR *name, DWORD value )
{
    HKEY key;
    if (RegCreateKeyExW( root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static WCHAR *get_sz( HKEY root, const WCHAR *path, const WCHAR *name )
{
    WCHAR buf[1024];
    DWORD size = sizeof(buf) - sizeof(WCHAR), type;
    HKEY key;
    LONG ret;

    if (RegOpenKeyExW( root, path, 0, KEY_QUERY_VALUE, &key )) return NULL;
    ret = RegQueryValueExW( key, name, NULL, &type, (BYTE *)buf, &size );
    RegCloseKey( key );
    if (ret || (type != REG_SZ && type != REG_EXPAND_SZ)) return NULL;
    buf[size / sizeof(WCHAR)] = 0;
    return _wcsdup( buf );
}

#define ACTIVATABLE L"Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\"

static void set_class_dll( const WCHAR *cls, const WCHAR *dll )
{
    WCHAR *key = strfw( ACTIVATABLE L"%s", cls ), *path = strfw( L"C:\\windows\\system32\\%s", dll );
    set_sz( HKEY_LOCAL_MACHINE, key, L"DllPath", path );
    free( path );
    free( key );
}

/* DllPath, in-process activation, threading model and trust level */
static void set_class( const WCHAR *cls, const WCHAR *dll, DWORD threading )
{
    WCHAR *key = strfw( ACTIVATABLE L"%s", cls );
    set_class_dll( cls, dll );
    set_dword( HKEY_LOCAL_MACHINE, key, L"ActivationType", 0 );
    set_dword( HKEY_LOCAL_MACHINE, key, L"Threading", threading );
    set_dword( HKEY_LOCAL_MACHINE, key, L"TrustLevel", 0 );
    free( key );
}

/* reg import, through Wine's reg.exe */
static BOOL reg_import( const WCHAR *file )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR *cmd = strfw( L"reg.exe import \"%s\"", file );
    DWORD code = 1;

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = si.hStdError = CreateFileW( L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (CreateProcessW( NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
    {
        WaitForSingleObject( pi.hProcess, INFINITE );
        GetExitCodeProcess( pi.hProcess, &code );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }
    CloseHandle( si.hStdOutput );
    free( cmd );
    return !code;
}

/* imports file unless it is the copy imported last time (marker, beside the prefix) */
static void reg_import_once( const WCHAR *file, const WCHAR *marker )
{
    if (exists( marker ) && same_file( file, marker )) return;
    if (reg_import( file )) copy_file( file, marker );
}

/* ---- the stamp ---- */

static BCRYPT_HASH_HANDLE stamp_hash;

static void stamp_add( const void *p, size_t len )
{
    BCryptHashData( stamp_hash, (UCHAR *)p, len, 0 );
}

static void stamp_file( const WCHAR *path )
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    stamp_add( path, wcslen( path ) * sizeof(WCHAR) );
    if (GetFileAttributesExW( path, GetFileExInfoStandard, &fa ))
    {
        stamp_add( &fa.nFileSizeLow, sizeof(fa.nFileSizeLow) );
        stamp_add( &fa.nFileSizeHigh, sizeof(fa.nFileSizeHigh) );
        stamp_add( &fa.ftLastWriteTime, sizeof(fa.ftLastWriteTime) );
    }
}

static void stamp_tree( const WCHAR *dir )
{
    struct listing l;
    unsigned i;

    stamp_file( dir );
    list_dir( dir, &l );
    for (i = 0; i < l.count; i++)
    {
        WCHAR *p = strfw( L"%s\\%s", dir, l.names[i] );
        if (is_dir( p )) stamp_tree( p );
        else stamp_file( p );
        free( p );
    }
    free_listing( &l );
}

static WCHAR *compute_stamp( const WCHAR *list )
{
    WCHAR *p, hex[33];
    UCHAR digest[16];
    unsigned i;

    BCryptCreateHash( BCRYPT_MD5_ALG_HANDLE, &stamp_hash, NULL, 0, NULL, 0, 0 );
    stamp_tree( src );
    p = strfw( L"%s\\Windows\\Globalization", win11 );
    stamp_tree( p );
    free( p );
    p = strfw( L"%s\\Windows\\Fonts", win11 );
    stamp_tree( p );
    free( p );
    stamp_file( sys32 );
    p = wine_pe( L"ntdll" );
    stamp_file( p );
    free( p );
    p = wine_pe( L"wineuwp" );
    stamp_file( p );
    free( p );
    p = strfw( L"%s\\programs\\wineuwp-setup\\x86_64-windows\\wineuwp-setup.exe", build );
    stamp_file( p );
    free( p );
    p = strfw( L"%s\\globalization.reg", data );
    stamp_file( p );
    free( p );
    stamp_add( list, wcslen( list ) * sizeof(WCHAR) );
    BCryptFinishHash( stamp_hash, digest, sizeof(digest), 0 );
    BCryptDestroyHash( stamp_hash );
    for (i = 0; i < 16; i++) swprintf( hex + 2 * i, 3, L"%02x", digest[i] );
    return _wcsdup( hex );
}

/* ---- the Windows DLLs, fonts, MUI files and metadata ---- */

static void install_native_list( const WCHAR *list )
{
    struct listing srcl, dstl;
    WCHAR *copy = _wcsdup( list ), *ctx, *n;
    unsigned i;

    list_dir( src, &srcl );
    for (n = wcstok( copy, L" ", &ctx ); n; n = wcstok( NULL, L" ", &ctx ))
    {
        WCHAR *want = strfw( L"%s.dll", n ), *f, *low, *from, *to;

        if (!(f = find_any_case( &srcl, want )))
        {
            printf( "missing %ls\n", n );
            free( want );
            continue;
        }
        low = lower( f );
        from = strfw( L"%s\\%s", src, f );
        list_dir( sys32, &dstl );
        for (i = 0; i < dstl.count; i++)
        {
            WCHAR *dup;
            if (wcsicmp( dstl.names[i], want )) continue;
            dup = strfw( L"%s\\%s", sys32, dstl.names[i] );
            if (wcscmp( dstl.names[i], low ) || !same_file( from, dup )) DeleteFileW( dup );
            free( dup );
        }
        free_listing( &dstl );
        to = strfw( L"%s\\%s", sys32, low );
        if (!exists( to )) copy_file( from, to );
        free( to );
        free( from );
        free( low );
        free( want );
    }
    free( copy );
    free_listing( &srcl );
    printf( "installed: %ls\n", list );
}

static unsigned install_named( const WCHAR **names, unsigned count )
{
    unsigned i;

    for (i = 0; i < count; i++)
    {
        WCHAR *from = strfw( L"%s\\%s", src, names[i] ), *low, *to;
        if (!exists( from ))
        {
            printf( "missing %ls\n", names[i] );
            free( from );
            continue;
        }
        low = lower( names[i] );
        to = strfw( L"%s\\%s", sys32, low );
        refresh_file( from, to );
        free( to );
        free( low );
        free( from );
    }
    return count;
}

/* Windows' own fonts: win32u registers every file in C:\windows\Fonts under its bare name, which is also where
 * dwrite's system collection finds it; faces found there are preferred over the host's. */
static void install_fonts(void)
{
    WCHAR *from = strfw( L"%s\\Windows\\Fonts", win11 ), *to = strfw( L"%s\\..\\Fonts", sys32 );
    struct listing l;
    unsigned i, copied = 0;

    if (is_dir( from ))
    {
        make_dirs( to );
        list_dir( from, &l );
        for (i = 0; i < l.count; i++)
        {
            WCHAR *a = strfw( L"%s\\%s", from, l.names[i] ), *b = strfw( L"%s\\%s", to, l.names[i] );
            if (!same_file( a, b ))
            {
                copy_file( a, b );
                copied++;
            }
            free( a );
            free( b );
        }
        printf( "installed: %u Windows fonts (%u copied)\n", l.count, copied );
        free_listing( &l );
    }
    free( from );
    free( to );
}

/* Satellite en-US MUI files of the genuine copies: kernelbase's FindResourceExW falls back to them, and XAML cannot
 * load its key names without windows.ui.xaml.dll.mui. A Wine builtin has its own resources. */
static void install_mui(void)
{
    WCHAR *mui_src = strfw( L"%s\\en-US", src ), *mui_dst = strfw( L"%s\\en-US", sys32 );
    struct listing dlls, srcl, muis;
    unsigned i, n = 0;

    CreateDirectoryW( mui_dst, NULL );
    list_dir( sys32, &dlls );
    list_dir( src, &srcl );
    list_dir( mui_src, &muis );
    for (i = 0; i < dlls.count; i++)
    {
        const WCHAR *b = dlls.names[i], *ext = wcsrchr( b, '.' );
        WCHAR *want, *m, *s, *dll, *from, *genuine, *low, *to;

        if (!ext || wcscmp( ext, L".dll" )) continue;   /* the glob is *.dll */
        want = strfw( L"%s.mui", b );
        m = find_any_case( &muis, want );
        free( want );
        if (!m) continue;
        if (!(s = find_any_case( &srcl, b ))) continue;
        dll = strfw( L"%s\\%s", sys32, b );
        genuine = strfw( L"%s\\%s", src, s );
        if (!same_file( genuine, dll ))
        {
            free( dll );
            free( genuine );
            continue;
        }
        low = lower( b );
        from = strfw( L"%s\\%s", mui_src, m );
        to = strfw( L"%s\\%s.mui", mui_dst, low );
        refresh_file( from, to );
        n++;
        free( to );
        free( from );
        free( low );
        free( genuine );
        free( dll );
    }
    free_listing( &muis );
    free_listing( &srcl );
    free_listing( &dlls );
    free( mui_src );
    free( mui_dst );
    printf( "installed: %u MUI files\n", n );
}

/* Wine ships small placeholder .winmd files; Windows' (same names) must win for type lookups. */
static void install_winmd(void)
{
    WCHAR *from_dir = strfw( L"%s\\WinMetadata", src ), *to_dir = strfw( L"%s\\winmetadata", sys32 );
    struct listing l, dst;
    unsigned i, j;

    CreateDirectoryW( to_dir, NULL );
    list_dir( from_dir, &l );
    for (i = 0; i < l.count; i++)
    {
        const WCHAR *ext = wcsrchr( l.names[i], '.' );
        WCHAR *low, *from, *to;

        if (!ext || wcscmp( ext, L".winmd" )) continue;
        low = lower( l.names[i] );
        from = strfw( L"%s\\%s", from_dir, l.names[i] );
        list_dir( to_dir, &dst );
        for (j = 0; j < dst.count; j++)
        {
            WCHAR *dup;
            if (wcsicmp( dst.names[j], low )) continue;
            dup = strfw( L"%s\\%s", to_dir, dst.names[j] );
            if (wcscmp( dst.names[j], low ) || !same_file( from, dup )) DeleteFileW( dup );
            free( dup );
        }
        free_listing( &dst );
        to = strfw( L"%s\\%s", to_dir, low );
        if (!exists( to )) copy_file( from, to );
        free( to );
        free( from );
        free( low );
    }
    free_listing( &l );
    list_dir( to_dir, &l );
    printf( "installed: %u winmd files\n", l.count );
    free_listing( &l );
    free( from_dir );
    free( to_dir );
}

/* ---- the SOFTWARE hive (regf): enough to enumerate keys and read values ---- */

struct hive
{
    BYTE *d;
    DWORD size;
    DWORD root;
};

static DWORD hive_u32( const struct hive *h, DWORD off ) { return off + 4 <= h->size ? *(DWORD *)(h->d + off) : 0; }
static WORD hive_u16( const struct hive *h, DWORD off ) { return off + 2 <= h->size ? *(WORD *)(h->d + off) : 0; }

static DWORD hive_nk( const struct hive *h, DWORD off )
{
    DWORD p = 0x1000 + off + 4;
    return p + 0x50 <= h->size && !memcmp( h->d + p, "nk", 2 ) ? p : 0;
}

static BOOL hive_open( const WCHAR *path, struct hive *h )
{
    if (!read_file( path, &h->d, &h->size ) || h->size < 0x1000 || memcmp( h->d, "regf", 4 )) return FALSE;
    return !!(h->root = hive_nk( h, hive_u32( h, 0x24 ) ));
}

/* a key's name, as the Python reader decoded it: Latin-1 when compressed, else UTF-16 */
static WCHAR *hive_name( const struct hive *h, DWORD p )
{
    WORD flags = hive_u16( h, p + 2 ), len = hive_u16( h, p + 0x48 );
    const BYTE *raw = h->d + p + 0x4c;
    WCHAR *ret;
    unsigned i;

    if (p + 0x4c + len > h->size) len = 0;
    if (flags & 0x20)
    {
        ret = malloc( (len + 1) * sizeof(WCHAR) );
        for (i = 0; i < len; i++) ret[i] = raw[i];
        ret[len] = 0;
    }
    else
    {
        ret = malloc( (len / 2 + 1) * sizeof(WCHAR) );
        memcpy( ret, raw, len / 2 * sizeof(WCHAR) );
        ret[len / 2] = 0;
    }
    return ret;
}

struct key_list
{
    DWORD *keys;
    unsigned count;
};

static void hive_list( const struct hive *h, DWORD off, struct key_list *out )
{
    DWORD q = 0x1000 + off + 4, i;
    WORD n;

    if (q + 4 > h->size) return;
    n = hive_u16( h, q + 2 );
    if (!memcmp( h->d + q, "lf", 2 ) || !memcmp( h->d + q, "lh", 2 ) || !memcmp( h->d + q, "li", 2 ))
    {
        DWORD stride = h->d[q + 1] == 'i' ? 4 : 8;
        for (i = 0; i < n; i++)
        {
            DWORD k = hive_nk( h, hive_u32( h, q + 4 + stride * i ) );
            if (!k) continue;
            out->keys = realloc( out->keys, (out->count + 1) * sizeof(DWORD) );
            out->keys[out->count++] = k;
        }
    }
    else if (!memcmp( h->d + q, "ri", 2 ))
        for (i = 0; i < n; i++) hive_list( h, hive_u32( h, q + 4 + 4 * i ), out );
}

static void hive_subkeys( const struct hive *h, DWORD p, struct key_list *out )
{
    DWORD count = hive_u32( h, p + 0x14 ), list = hive_u32( h, p + 0x1c );

    memset( out, 0, sizeof(*out) );
    if (count && list != 0xffffffff) hive_list( h, list, out );
}

static DWORD hive_key( const struct hive *h, const WCHAR *path )
{
    WCHAR *copy = _wcsdup( path ), *ctx, *part;
    DWORD p = h->root;

    for (part = wcstok( copy, L"\\", &ctx ); part && p; part = wcstok( NULL, L"\\", &ctx ))
    {
        struct key_list l;
        DWORD next = 0;
        unsigned i;

        hive_subkeys( h, p, &l );
        for (i = 0; i < l.count && !next; i++)
        {
            WCHAR *name = hive_name( h, l.keys[i] );
            if (!wcsicmp( name, part )) next = l.keys[i];
            free( name );
        }
        free( l.keys );
        p = next;
    }
    free( copy );
    return p;
}

/* a string value (REG_SZ/REG_EXPAND_SZ, to the first NUL); name NULL is the default value. The last value of a
 * name wins, as in the Python reader's dict. */
static WCHAR *hive_string( const struct hive *h, DWORD p, const WCHAR *want )
{
    DWORD count = hive_u32( h, p + 0x24 ), list = hive_u32( h, p + 0x28 ), q, i;
    WCHAR *ret = NULL;

    if (!count || list == 0xffffffff) return NULL;
    q = 0x1000 + list + 4;
    for (i = 0; i < count; i++)
    {
        DWORD v = 0x1000 + hive_u32( h, q + 4 * i ) + 4, dlen, doff, type, s;
        WORD nlen, flags;
        WCHAR *name;
        BOOL match;

        if (v + 0x14 > h->size || memcmp( h->d + v, "vk", 2 )) continue;
        nlen = hive_u16( h, v + 2 );
        dlen = hive_u32( h, v + 4 );
        doff = hive_u32( h, v + 8 );
        type = hive_u32( h, v + 12 );
        flags = hive_u16( h, v + 16 );
        if (v + 0x14 + nlen > h->size) continue;
        if (flags & 1)
        {
            unsigned k;
            name = malloc( (nlen + 1) * sizeof(WCHAR) );
            for (k = 0; k < nlen; k++) name[k] = h->d[v + 0x14 + k];
            name[nlen] = 0;
        }
        else
        {
            name = malloc( (nlen / 2 + 1) * sizeof(WCHAR) );
            memcpy( name, h->d + v + 0x14, nlen / 2 * sizeof(WCHAR) );
            name[nlen / 2] = 0;
        }
        match = want ? !wcscmp( name, want ) : !*name;
        free( name );
        if (!match) continue;
        free( ret );
        ret = NULL;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        if (dlen & 0x80000000)
        {
            DWORD len = dlen & 0x7fffffff;
            ret = calloc( 3, sizeof(WCHAR) );
            memcpy( ret, &doff, min( len, 4 ) );
        }
        else
        {
            s = 0x1000 + doff + 4;
            if (s + dlen > h->size) continue;
            ret = calloc( dlen / 2 + 1, sizeof(WCHAR) );
            memcpy( ret, h->d + s, dlen / 2 * sizeof(WCHAR) );
        }
    }
    return ret;
}

/* a REG_DWORD value, def when absent */
static DWORD hive_dword( const struct hive *h, DWORD p, const WCHAR *want, DWORD def )
{
    DWORD count = hive_u32( h, p + 0x24 ), list = hive_u32( h, p + 0x28 ), q, i, ret = def;

    if (!count || list == 0xffffffff) return def;
    q = 0x1000 + list + 4;
    for (i = 0; i < count; i++)
    {
        DWORD v = 0x1000 + hive_u32( h, q + 4 * i ) + 4, dlen, doff, type;
        WORD nlen, flags;
        BOOL match = TRUE;
        unsigned k;

        if (v + 0x14 > h->size || memcmp( h->d + v, "vk", 2 )) continue;
        nlen = hive_u16( h, v + 2 );
        dlen = hive_u32( h, v + 4 );
        doff = hive_u32( h, v + 8 );
        type = hive_u32( h, v + 12 );
        flags = hive_u16( h, v + 16 );
        if (!(flags & 1) || nlen != wcslen( want ) || v + 0x14 + nlen > h->size) continue;
        for (k = 0; k < nlen && match; k++) if (h->d[v + 0x14 + k] != want[k]) match = FALSE;
        if (!match || type != REG_DWORD || (dlen & 0x7fffffff) < 4) continue;
        if (dlen & 0x80000000) ret = doff;
        else ret = hive_u32( h, 0x1000 + doff + 4 );
    }
    return ret;
}

/* ---- .reg text, UTF-16 with a BOM and CRLF between lines (no final one), as the Python generators wrote ---- */

struct text
{
    WCHAR *p;
    size_t n, cap;
};

static void text_add( struct text *t, const WCHAR *s )
{
    size_t len = wcslen( s );
    if (t->n + len + 1 > t->cap)
    {
        t->cap = max( t->cap * 2, t->n + len + 1 + 4096 );
        t->p = realloc( t->p, t->cap * sizeof(WCHAR) );
    }
    memcpy( t->p + t->n, s, len * sizeof(WCHAR) );
    t->n += len;
    t->p[t->n] = 0;
}

static unsigned text_lines;

static void text_line( struct text *t, const WCHAR *s )
{
    if (text_lines++) text_add( t, L"\r\n" );
    text_add( t, s );
}

static BOOL text_write( const struct text *t, const WCHAR *path )
{
    BYTE *buf = malloc( 2 + t->n * sizeof(WCHAR) );
    BOOL ok;

    buf[0] = 0xff;
    buf[1] = 0xfe;
    memcpy( buf + 2, t->p, t->n * sizeof(WCHAR) );
    ok = write_file( path, buf, 2 + t->n * sizeof(WCHAR) );
    free( buf );
    return ok;
}

static WCHAR *reg_escape( const WCHAR *s )
{
    WCHAR *ret = malloc( (2 * wcslen( s ) + 1) * sizeof(WCHAR) ), *d = ret;
    for (; *s; s++)
    {
        if (*s == '\\' || *s == '"') *d++ = '\\';
        *d++ = *s;
    }
    *d = 0;
    return ret;
}

/* ---- Windows' in-process COM classes (was gen-clsid-reg.py) ---- */

/* Classes of DLLs Wine implements itself keep their builtin registration, and so does every CLSID the prefix
 * already registers to a Wine builtin under another DLL name (Wine's Media Foundation File Scheme Handler is mf.dll,
 * Windows' mfcore.dll: importing Windows' entry crashed Halo in mfcore on its first video). */
static const WCHAR *keep_builtin[] =
{
    L"ole32.dll", L"combase.dll", L"oleaut32.dll", L"rpcrt4.dll", L"actxprxy.dll", L"shell32.dll", L"propsys.dll",
    L"shcore.dll", L"windows.storage.dll", L"urlmon.dll", L"user32.dll", L"d2d1.dll", L"d3d11.dll", L"dxgi.dll",
    L"dwrite.dll", L"mf.dll", L"mfplat.dll", L"msxml6.dll", L"iertutil.dll", L"windowscodecs.dll",
    L"uiautomationcore.dll",
};

static BOOL clsid_is_wine_builtin( const WCHAR *clsid )
{
    WCHAR *key = strfw( L"Software\\Classes\\CLSID\\%s\\InprocServer32", clsid ), *path, *base;
    BOOL ret = FALSE;

    path = get_sz( HKEY_LOCAL_MACHINE, key, NULL );
    free( key );
    if (!path) return FALSE;
    if (!wcsnicmp( path, L"c:\\windows\\system32\\", 20 ))
    {
        WCHAR *file;
        base = lower( wcsrchr( path, '\\' ) + 1 );
        file = strfw( L"%s\\%s", sys32, base );
        ret = is_wine_builtin( file );
        free( file );
        free( base );
    }
    free( path );
    return ret;
}

static void generate_clsid_reg( const WCHAR *hive_path, const WCHAR *out )
{
    struct hive h;
    struct key_list classes;
    struct listing present, in_prefix;
    struct text t = {0};
    WCHAR *winpath;
    unsigned i, j, nreg = 0, nskip = 0;
    DWORD clsid_key;

    if (!hive_open( hive_path, &h ) || !(clsid_key = hive_key( &h, L"Classes\\CLSID" )))
    {
        fprintf( stderr, "wineuwp-setup: cannot read %ls\n", hive_path );
        return;
    }
    list_dir( src, &present );
    list_dir( sys32, &in_prefix );
    winpath = _wcsdup( src );
    text_lines = 0;
    text_line( &t, L"Windows Registry Editor Version 5.00" );
    text_line( &t, L"" );
    hive_subkeys( &h, clsid_key, &classes );
    for (i = 0; i < classes.count; i++)
    {
        struct key_list subs;
        WCHAR *dll = NULL, *threading = NULL, *base, *name, *cname, *line, *found;
        DWORD inproc = 0;
        BOOL skip;

        hive_subkeys( &h, classes.keys[i], &subs );
        for (j = 0; j < subs.count && !inproc; j++)
        {
            WCHAR *sname = hive_name( &h, subs.keys[j] );
            if (!wcsicmp( sname, L"inprocserver32" )) inproc = subs.keys[j];
            free( sname );
        }
        free( subs.keys );
        if (!inproc || !(dll = hive_string( &h, inproc, NULL )) || !*dll)
        {
            free( dll );
            continue;
        }
        threading = hive_string( &h, inproc, L"ThreadingModel" );
        {
            WCHAR *slash = dll, *p;
            for (p = dll; *p; p++) if (*p == '\\' || *p == '/') slash = p + 1;
            base = lower( slash );
        }
        cname = hive_name( &h, classes.keys[i] );
        skip = FALSE;
        for (j = 0; j < ARRAY_SIZE(keep_builtin); j++) if (!wcscmp( base, keep_builtin[j] )) skip = TRUE;
        found = find_any_case( &present, base );
        if (!skip && !found) skip = TRUE;
        if (!skip)
        {
            WCHAR *upper = _wcsdup( cname ), *p;
            for (p = upper; *p; p++) *p = towupper( *p );
            skip = clsid_is_wine_builtin( upper );
            free( upper );
        }
        if (skip)
        {
            nskip++;
            free( cname );
            free( base );
            free( threading );
            free( dll );
            continue;
        }
        name = hive_string( &h, classes.keys[i], NULL );
        line = strfw( L"[HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\%s]", cname );
        text_line( &t, line );
        free( line );
        if (name && *name)
        {
            WCHAR *e = reg_escape( name );
            line = strfw( L"@=\"%s\"", e );
            text_line( &t, line );
            free( line );
            free( e );
        }
        line = strfw( L"[HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\%s\\InprocServer32]", cname );
        text_line( &t, line );
        free( line );
        {
            WCHAR *target, *e;
            if (find_any_case( &in_prefix, base )) target = strfw( L"C:\\windows\\system32\\%s", base );
            else target = strfw( L"%s\\%s", winpath, found );
            e = reg_escape( target );
            line = strfw( L"@=\"%s\"", e );
            text_line( &t, line );
            free( line );
            free( e );
            free( target );
        }
        line = strfw( L"\"ThreadingModel\"=\"%s\"", threading ? threading : L"Both" );
        text_line( &t, line );
        free( line );
        text_line( &t, L"" );
        nreg++;
        free( name );
        free( cname );
        free( base );
        free( threading );
        free( dll );
    }
    free( classes.keys );
    text_write( &t, out );
    free( t.p );
    free( winpath );
    free_listing( &in_prefix );
    free_listing( &present );
    free( h.d );
    printf( "registered %u in-process CLSIDs, skipped %u (builtin or DLL not extracted)\n", nreg, nskip );
}

/* the last path component of a DLL path, lowercase */
static WCHAR *dll_base( const WCHAR *dll )
{
    const WCHAR *slash = dll, *p;
    for (p = dll; *p; p++) if (*p == '\\' || *p == '/') slash = p + 1;
    return lower( slash );
}

/* ---- the tree's WinRT classes (was gen-classes-reg.py) ---- */

/* Every in-process class whose DLL the tree has, by the prefix copy's path when the prefix has the DLL (one copy per
 * process), else by the tree's. Server-activated classes are skipped: there is no RuntimeBroker in Wine. */
static void generate_classes_reg( const WCHAR *hive_path, const WCHAR *out )
{
    struct hive h;
    struct key_list classes;
    struct listing present, in_prefix;
    struct text t = {0};
    unsigned i, nreg = 0, nskip_server = 0, nskip_missing = 0;
    DWORD map;

    if (!hive_open( hive_path, &h ) || !(map = hive_key( &h, L"Microsoft\\WindowsRuntime\\ActivatableClassId" )))
    {
        fprintf( stderr, "wineuwp-setup: cannot read %ls\n", hive_path );
        return;
    }
    list_dir( src, &present );
    list_dir( sys32, &in_prefix );
    text_lines = 0;
    text_line( &t, L"Windows Registry Editor Version 5.00" );
    text_line( &t, L"" );
    hive_subkeys( &h, map, &classes );
    for (i = 0; i < classes.count; i++)
    {
        WCHAR *name = hive_name( &h, classes.keys[i] ), *dll = hive_string( &h, classes.keys[i], L"DllPath" );
        WCHAR *base, *found, *target, *e, *line;

        if (!dll || !*dll)
        {
            nskip_server++;
            free( dll );
            free( name );
            continue;
        }
        base = dll_base( dll );
        if (!(found = find_any_case( &present, base )))
        {
            nskip_missing++;
            free( base );
            free( dll );
            free( name );
            continue;
        }
        line = strfw( L"[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\%s]", name );
        text_line( &t, line );
        free( line );
        if (find_any_case( &in_prefix, base )) target = strfw( L"C:\\windows\\system32\\%s", base );
        else target = strfw( L"%s\\%s", src, found );
        e = reg_escape( target );
        line = strfw( L"\"DllPath\"=\"%s\"", e );
        text_line( &t, line );
        free( line );
        line = strfw( L"\"ActivationType\"=dword:%08x", hive_dword( &h, classes.keys[i], L"ActivationType", 0 ) );
        text_line( &t, line );
        free( line );
        line = strfw( L"\"Threading\"=dword:%08x", hive_dword( &h, classes.keys[i], L"Threading", 0 ) );
        text_line( &t, line );
        free( line );
        line = strfw( L"\"TrustLevel\"=dword:%08x", hive_dword( &h, classes.keys[i], L"TrustLevel", 0 ) );
        text_line( &t, line );
        free( line );
        text_line( &t, L"" );
        nreg++;
        free( e );
        free( target );
        free( base );
        free( dll );
        free( name );
    }
    free( classes.keys );
    text_write( &t, out );
    free( t.p );
    free_listing( &in_prefix );
    free_listing( &present );
    free( h.d );
    printf( "registered %u classes, skipped %u server-activated, %u with DLLs not extracted\n", nreg, nskip_server,
            nskip_missing );
}

/* ---- COM interfaces and their proxy/stub DLLs (was gen-interfaces-reg.py) ---- */

/* Every interface with a ProxyStubClsid32, and every proxy CLSID whose DLL the tree has, pointing at the tree's
 * copy, so that Wine's combase marshals through Microsoft's own NDR proxies. */
static void generate_interfaces_reg( const WCHAR *hive_path, const WCHAR *out )
{
    static const WCHAR *keep[] = { L"ole32.dll", L"combase.dll", L"oleaut32.dll", L"rpcrt4.dll", L"actxprxy.dll",
                                   L"shell32.dll", L"propsys.dll", L"shcore.dll", L"windows.storage.dll" };
    struct hive h;
    struct key_list ifaces, classes;
    struct listing present;
    struct text t = {0};
    WCHAR **proxies = NULL;
    unsigned nproxies = 0, i, j, n_if = 0, n_px = 0, n_skip = 0;
    DWORD iface_key, clsid_key;

    if (!hive_open( hive_path, &h ) || !(iface_key = hive_key( &h, L"Classes\\Interface" )) ||
        !(clsid_key = hive_key( &h, L"Classes\\CLSID" )))
    {
        fprintf( stderr, "wineuwp-setup: cannot read %ls\n", hive_path );
        return;
    }
    list_dir( src, &present );
    text_lines = 0;
    text_line( &t, L"Windows Registry Editor Version 5.00" );
    text_line( &t, L"" );
    hive_subkeys( &h, iface_key, &ifaces );
    for (i = 0; i < ifaces.count; i++)
    {
        struct key_list subs;
        WCHAR *ps = NULL, *iid, *name, *line;

        hive_subkeys( &h, ifaces.keys[i], &subs );
        for (j = 0; j < subs.count; j++)
        {
            WCHAR *sname = hive_name( &h, subs.keys[j] );
            BOOL is_ps = !wcsicmp( sname, L"proxystubclsid32" );
            free( sname );
            if (is_ps)
            {
                ps = hive_string( &h, subs.keys[j], NULL );
                break;
            }
        }
        free( subs.keys );
        if (!ps || !*ps)
        {
            free( ps );
            continue;
        }
        iid = hive_name( &h, ifaces.keys[i] );
        name = hive_string( &h, ifaces.keys[i], NULL );
        line = strfw( L"[HKEY_LOCAL_MACHINE\\Software\\Classes\\Interface\\%s]", iid );
        text_line( &t, line );
        free( line );
        if (name && *name)
        {
            WCHAR *e = reg_escape( name );
            line = strfw( L"@=\"%s\"", e );
            text_line( &t, line );
            free( line );
            free( e );
        }
        line = strfw( L"[HKEY_LOCAL_MACHINE\\Software\\Classes\\Interface\\%s\\ProxyStubClsid32]", iid );
        text_line( &t, line );
        free( line );
        line = strfw( L"@=\"%s\"", ps );
        text_line( &t, line );
        free( line );
        text_line( &t, L"" );
        for (j = 0; j < nproxies; j++) if (!wcsicmp( proxies[j], ps )) break;
        if (j == nproxies)
        {
            proxies = realloc( proxies, (nproxies + 1) * sizeof(*proxies) );
            proxies[nproxies++] = _wcsdup( ps );
        }
        n_if++;
        free( name );
        free( iid );
        free( ps );
    }
    free( ifaces.keys );

    hive_subkeys( &h, clsid_key, &classes );
    for (i = 0; i < classes.count; i++)
    {
        WCHAR *cname = hive_name( &h, classes.keys[i] ), *dll = NULL, *threading = NULL, *base, *found, *target, *e, *line;
        struct key_list subs;
        DWORD inproc = 0;
        BOOL keep_it = FALSE;

        for (j = 0; j < nproxies; j++) if (!wcsicmp( proxies[j], cname )) break;
        if (j == nproxies)
        {
            free( cname );
            continue;
        }
        hive_subkeys( &h, classes.keys[i], &subs );
        for (j = 0; j < subs.count && !inproc; j++)
        {
            WCHAR *sname = hive_name( &h, subs.keys[j] );
            if (!wcsicmp( sname, L"inprocserver32" )) inproc = subs.keys[j];
            free( sname );
        }
        free( subs.keys );
        if (!inproc || !(dll = hive_string( &h, inproc, NULL )) || !*dll)
        {
            n_skip++;
            free( dll );
            free( cname );
            continue;
        }
        base = dll_base( dll );
        for (j = 0; j < ARRAY_SIZE(keep); j++) if (!wcscmp( base, keep[j] )) keep_it = TRUE;
        if (keep_it || !(found = find_any_case( &present, base )))
        {
            if (!keep_it) n_skip++;
            free( base );
            free( dll );
            free( cname );
            continue;
        }
        threading = hive_string( &h, inproc, L"ThreadingModel" );
        line = strfw( L"[HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\%s\\InprocServer32]", cname );
        text_line( &t, line );
        free( line );
        if (!wcscmp( base, L"mrmcorer.dll" )) target = _wcsdup( L"C:\\windows\\system32\\mrmcorer.dll" );
        else target = strfw( L"%s\\%s", src, found );
        e = reg_escape( target );
        line = strfw( L"@=\"%s\"", e );
        text_line( &t, line );
        free( line );
        line = strfw( L"\"ThreadingModel\"=\"%s\"", threading ? threading : L"Both" );
        text_line( &t, line );
        free( line );
        text_line( &t, L"" );
        n_px++;
        free( e );
        free( target );
        free( threading );
        free( base );
        free( dll );
        free( cname );
    }
    free( classes.keys );
    text_write( &t, out );
    free( t.p );
    while (nproxies) free( proxies[--nproxies] );
    free( proxies );
    free_listing( &present );
    free( h.d );
    printf( "%u interfaces, %u proxy DLL registrations, %u proxies skipped (DLL not extracted)\n", n_if, n_px, n_skip );
}

/* ---- a new prefix's registrations (was the first half of packaging/new-prefix.sh) ---- */

static void set_expand_sz( HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value )
{
    HKEY key;
    if (RegCreateKeyExW( root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_EXPAND_SZ, (const BYTE *)value, (wcslen( value ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

static void new_prefix_registrations(void)
{
    static const WCHAR svc[] = L"System\\CurrentControlSet\\Services\\CoreMessagingRegistrar";
    static const WCHAR group[] = L"CoreMessagingRegistrar\0";
    WCHAR *hive = strfw( L"%s\\config\\SOFTWARE", src ), *out;
    HKEY key;

    /* Every CoreMessaging endpoint registers with this service: Windows' own CoreMessaging.dll hosted by Wine's
     * svchost. Without it CoreMessaging fail-fasts with ERROR_SERVICE_DOES_NOT_EXIST as the first view starts. */
    set_sz( HKEY_LOCAL_MACHINE, svc, L"DisplayName", L"CoreMessaging" );
    set_dword( HKEY_LOCAL_MACHINE, svc, L"ErrorControl", 1 );
    set_expand_sz( HKEY_LOCAL_MACHINE, svc, L"ImagePath", L"%SystemRoot%\\system32\\svchost.exe -k LocalServiceNoNetwork" );
    set_sz( HKEY_LOCAL_MACHINE, svc, L"ObjectName", L"LocalSystem" );
    set_dword( HKEY_LOCAL_MACHINE, svc, L"Start", 3 );
    set_dword( HKEY_LOCAL_MACHINE, svc, L"Type", 32 );
    if (!RegCreateKeyExW( HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Svchost", 0, NULL, 0,
                          KEY_SET_VALUE, NULL, &key, NULL ))
    {
        RegSetValueExW( key, L"LocalServiceNoNetwork", 0, REG_MULTI_SZ, (const BYTE *)group, sizeof(group) );
        RegCloseKey( key );
    }
    out = strfw( L"%s\\Parameters", svc );
    set_expand_sz( HKEY_LOCAL_MACHINE, out, L"ServiceDll", L"%SystemRoot%\\system32\\CoreMessaging.dll" );
    free( out );
    /* Wine picks its graphics driver from this key only: X11 (Xwayland on a Wayland desktop), as run.sh pins it */
    set_sz( HKEY_CURRENT_USER, L"Software\\Wine\\Drivers", L"Graphics", L"x11" );

    out = strfw( L"%s\\wine-uwp-classes.reg", win11 );
    generate_classes_reg( hive, out );
    reg_import( out );
    free( out );
    out = strfw( L"%s\\wine-uwp-interfaces.reg", win11 );
    generate_interfaces_reg( hive, out );
    reg_import( out );
    free( out );
    free( hive );
}

/* ---- activatable classes pinned to the prefix copy of their DLL (was gen-single-module-reg.py) ---- */

/* A DLL installed into system32 can otherwise be loaded twice in one process: by bare name, and through the absolute
 * tree path the generated class map names. Only byte-identical copies are pinned; for every other name system32 holds
 * Wine's builtin, a different implementation. */
static void generate_single_module_reg( const WCHAR *classes_reg, const WCHAR *out )
{
    struct listing ext;
    WCHAR **same = NULL, **pinned = NULL;
    unsigned nsame = 0, npinned = 0, npin = 0, i;
    struct text t = {0};
    BYTE *raw;
    DWORD size;
    WCHAR *text, *line, *next, *key = NULL;

    list_dir( src, &ext );
    for (i = 0; i < ext.count; i++)
    {
        const WCHAR *dot = wcsrchr( ext.names[i], '.' );
        WCHAR *low, *a, *b;
        if (!dot || wcsicmp( dot, L".dll" )) continue;
        low = lower( ext.names[i] );
        a = strfw( L"%s\\%s", sys32, low );
        b = strfw( L"%s\\%s", src, ext.names[i] );
        if (exists( a ) && same_file( a, b ))
        {
            same = realloc( same, (nsame + 1) * sizeof(*same) );
            same[nsame++] = low;
        }
        else free( low );
        free( a );
        free( b );
    }
    free_listing( &ext );

    if (!read_file( classes_reg, &raw, &size )) return;
    text = (WCHAR *)raw;
    if (size >= 2 && text[0] == 0xfeff) text++;
    text_lines = 0;
    text_line( &t, L"Windows Registry Editor Version 5.00" );
    text_line( &t, L"" );
    for (line = text; line && *line; line = next)
    {
        WCHAR *end = wcspbrk( line, L"\r\n" ), *path, *base, *p, *q;
        size_t len;
        BOOL in_same = FALSE;
        unsigned k;

        if (end)
        {
            next = end + (end[0] == '\r' && end[1] == '\n' ? 2 : 1);
            *end = 0;
        }
        else next = NULL;
        if (line[0] == '[')
        {
            key = line;
            continue;
        }
        len = wcslen( line );
        if (!key || wcsncmp( line, L"\"DllPath\"=\"", 11 ) || len < 13 || line[len - 1] != '"') continue;
        /* the value, unescaped (\\ -> \) */
        path = malloc( len * sizeof(WCHAR) );
        for (p = line + 11, q = path; p < line + len - 1; p++)
        {
            if (p[0] == '\\' && p[1] == '\\') p++;
            *q++ = *p;
        }
        *q = 0;
        base = path;
        for (p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
        base = lower( base );
        for (k = 0; k < nsame; k++) if (!wcscmp( same[k], base )) in_same = TRUE;
        if (in_same && wcsnicmp( path, L"c:\\windows\\system32\\", 20 ))
        {
            WCHAR *v = strfw( L"\"DllPath\"=\"C:\\\\windows\\\\system32\\\\%s\"", base );
            text_line( &t, key );
            text_line( &t, v );
            text_line( &t, L"" );
            free( v );
            npin++;
            for (k = 0; k < npinned; k++) if (!wcscmp( pinned[k], base )) break;
            if (k == npinned)
            {
                pinned = realloc( pinned, (npinned + 1) * sizeof(*pinned) );
                pinned[npinned++] = _wcsdup( base );
            }
        }
        free( base );
        free( path );
    }
    text_write( &t, out );
    free( t.p );
    free( raw );
    printf( "pinned %u classes across %u modules\n", npin, npinned );
    while (nsame) free( same[--nsame] );
    free( same );
    while (npinned) free( pinned[--npinned] );
    free( pinned );
}

/* ---- wineuwp.reg, with the tree's path in place of @WIN11_WINPATH@ ---- */

static void import_wineuwp_reg(void)
{
    WCHAR *file = strfw( L"%s\\wineuwp.reg", data ), *tmp = strfw( L"%s\\.wineuwp.reg.tmp", prefix );
    BYTE *raw;
    DWORD size;
    char *esc, *out, *p, *at;
    const char *mark = "@WIN11_WINPATH@";
    int len;
    size_t n = 0;

    if (!read_file( file, &raw, &size )) goto done;
    /* the tree's DOS path, with each backslash doubled as a .reg string needs */
    len = WideCharToMultiByte( CP_UTF8, 0, win11, -1, NULL, 0, NULL, NULL );
    esc = malloc( 2 * len + 1 );
    {
        char *u = malloc( len ), *d = esc, *s;
        WideCharToMultiByte( CP_UTF8, 0, win11, -1, u, len, NULL, NULL );
        for (s = u; *s; s++)
        {
            if (*s == '\\') *d++ = '\\';
            *d++ = *s;
        }
        *d = 0;
        free( u );
    }
    out = malloc( size * 2 + 4096 );
    for (p = (char *)raw; (at = strstr( p, mark )); p = at + strlen( mark ))
    {
        memcpy( out + n, p, at - p );
        n += at - p;
        memcpy( out + n, esc, strlen( esc ) );
        n += strlen( esc );
    }
    memcpy( out + n, p, strlen( p ) );
    n += strlen( p );
    if (write_file( tmp, out, n )) reg_import( tmp );
    DeleteFileW( tmp );
    free( out );
    free( esc );
    free( raw );
done:
    free( file );
    free( tmp );
}

/* ---- classes Wine serves instead of Windows ---- */

static BOOL in_sys32( const WCHAR *dll )
{
    WCHAR *p = strfw( L"%s\\%s", sys32, dll );
    BOOL ret = exists( p );
    free( p );
    return ret;
}

static void remove_from_sys32( const WCHAR *dll )
{
    WCHAR *p = strfw( L"%s\\%s", sys32, dll );
    DeleteFileW( p );
    free( p );
}

static const WCHAR ms_publisher[] = L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";

static void standin_package( const WCHAR *name, const WCHAR *version )
{
    WCHAR *key = strfw( L"Software\\Wine\\WineUWP\\StandInPackages\\%s_8wekyb3d8bbwe", name );
    WCHAR *full = strfw( L"%s_%s_x64__8wekyb3d8bbwe", name, version );

    set_sz( HKEY_LOCAL_MACHINE, key, L"Name", name );
    set_sz( HKEY_LOCAL_MACHINE, key, L"Publisher", ms_publisher );
    set_sz( HKEY_LOCAL_MACHINE, key, L"PublisherId", L"8wekyb3d8bbwe" );
    set_sz( HKEY_LOCAL_MACHINE, key, L"Version", version );
    set_dword( HKEY_LOCAL_MACHINE, key, L"Architecture", 9 );
    set_sz( HKEY_LOCAL_MACHINE, key, L"FullName", full );
    free( full );
    free( key );
}

static void register_wine_classes(void)
{
    static const WCHAR *staterepo[] = { L"User", L"Application", L"MrtApplication", L"Package", L"PackageFamily",
                                        L"MrtPackage", L"PackageUser", L"TargetDeviceFamily" };
    static const WCHAR *gameplatform[] = { L"PackageRequest", L"Package", L"PackagePreview", L"PackageQueue",
        L"PackageUpdate", L"UploadQueue", L"Storage", L"GameFolder", L"GameProfile", L"GameSave", L"GameStreaming",
        L"GameMetrics", L"Licensing", L"SupportedFeatures", L"PlatformStatus", L"ProcessMonitor", L"Store",
        L"Customization", L"ContentAccess", L"BackgroundConsent" };
    static const struct { const WCHAR *name, *version; } deps[] =
    {
        { L"Microsoft.DesktopAppInstaller", L"1.29.379.0" }, { L"Microsoft.WindowsFeedbackHub", L"1.2401.20253.0" },
        { L"Microsoft.XboxGamingOverlay", L"7.326.8061.0" }, { L"Microsoft.XboxIdentityProvider", L"12.130.16001.0" },
        { L"Microsoft.Xbox.TCUI", L"1.24.10001.0" }, { L"Microsoft.WindowsStore", L"22608.1401.3.0" },
        { L"Microsoft.StorePurchaseApp", L"22607.1401.4.0" },
    };
    static const WCHAR *msxml[][2] =
    {
        { L"{88d96a09-f192-11d4-a65f-0040963251e5}", L"Both" },        /* Free Threaded XML HTTP 6.0 */
        { L"{88d96a0a-f192-11d4-a65f-0040963251e5}", L"Apartment" },   /* XML HTTP 6.0 */
        { L"{88d96a0b-f192-11d4-a65f-0040963251e5}", L"Apartment" },   /* Server XML HTTP 6.0 */
    };
    unsigned i;
    WCHAR *p;

    /* Windows.Internal.StateRepository.*: service-activated in the Windows hive, with no DllPath */
    if (in_sys32( L"windows.staterepositorycore.dll" ))
    {
        remove_from_sys32( L"wineuwp_staterepository.dll" );
        for (i = 0; i < ARRAY_SIZE(staterepo); i++)
        {
            p = strfw( L"Windows.Internal.StateRepository.%s", staterepo[i] );
            set_class( p, L"windows.staterepositorycore.dll", 1 );
            free( p );
        }
    }
    /* Windows' AppCapability asks a service the prefix does not have, so every CheckAccess was denied */
    if (in_sys32( L"windows.security.authorization.appcapabilityaccess.dll" ))
    {
        const WCHAR *cls = L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability";
        set_class_dll( cls, L"windows.security.authorization.appcapabilityaccess.dll" );
        p = strfw( ACTIVATABLE L"%s", cls );
        set_dword( HKEY_LOCAL_MACHINE, p, L"ActivationType", 0 );
        free( p );
    }
    set_class_dll( L"Windows.Storage.StorageLibrary", L"windows.storage.dll" );
    remove_from_sys32( L"wineuwp_storagelibrary.dll" );
    /* out-of-process classes of the UserManager service on Windows */
    if (in_sys32( L"windows.system.user.dll" ))
    {
        set_class( L"Windows.System.User", L"windows.system.user.dll", 0 );
        set_class( L"Windows.System.UserPicker", L"windows.system.user.dll", 0 );
    }
    if (in_sys32( L"windows.system.memorymanager.dll" ))
        set_class_dll( L"Windows.System.MemoryManager", L"windows.system.memorymanager.dll" );
    set_class_dll( L"Windows.Devices.Enumeration.DeviceInformation", L"windows.devices.enumeration.dll" );

    /* msxml6's XMLHTTP classes: libHttpClient (XSAPI, PlayFab) makes every HTTP call through IXMLHTTPRequest2 */
    if (in_sys32( L"msxml6.dll" ))
    {
        for (i = 0; i < ARRAY_SIZE(msxml); i++)
        {
            p = strfw( L"CLSID\\%s\\InprocServer32", msxml[i][0] );
            set_sz( HKEY_CLASSES_ROOT, p, NULL, L"C:\\windows\\system32\\msxml6.dll" );
            set_sz( HKEY_CLASSES_ROOT, p, L"ThreadingModel", msxml[i][1] );
            free( p );
        }
        printf( "registered: msxml6 XMLHTTP classes\n" );
    }

    set_class_dll( L"Windows.Networking.Connectivity.NetworkInformation", L"windows.networking.connectivity.dll" );
    set_class_dll( L"Windows.Media.Audio.SpatialAudioDeviceConfiguration", L"windows.media.devices.dll" );
    set_class_dll( L"Windows.Media.Audio.SpatialAudioFormatSubtype", L"windows.media.devices.dll" );
    if (in_sys32( L"windows.applicationmodel.useractivities.dll" ))
        set_class( L"Windows.ApplicationModel.UserActivities.UserActivityChannel",
                   L"windows.applicationmodel.useractivities.dll", 1 );
    /* only the manager: Windows' other notification classes (ScheduledToastNotification) stay Windows' */
    if (in_sys32( L"windows.ui.notifications.dll" ))
        set_class_dll( L"Windows.UI.Notifications.ToastNotificationManager", L"windows.ui.notifications.dll" );
    if (in_sys32( L"windows.services.store.dll" ))
        set_class( L"Windows.Services.Store.StoreContext", L"windows.services.store.dll", 1 );

    /* Gaming Services: the client classes replace the package's own DLL (OverridesPackage), whose NT services
     * cannot run under Wine */
    if (in_sys32( L"winegameplatform.dll" ))
    {
        remove_from_sys32( L"wineuwp_gameplatform.dll" );
        for (i = 0; i < ARRAY_SIZE(gameplatform); i++)
        {
            WCHAR *key;
            p = strfw( L"Microsoft.GamePlatform.Services.%s", gameplatform[i] );
            set_class( p, L"winegameplatform.dll", 0 );
            key = strfw( ACTIVATABLE L"%s", p );
            set_dword( HKEY_LOCAL_MACHINE, key, L"OverridesPackage", 1 );
            free( key );
            free( p );
        }
        /* its package identity (appxdeploymentclient standin.c), as gamingservices-signed.appxbundle carries it */
        standin_package( L"Microsoft.GamingServices", L"37.114.26001.0" );
        /* the packages the Xbox app lists as its dependencies, identity only */
        for (i = 0; i < ARRAY_SIZE(deps); i++) standin_package( deps[i].name, deps[i].version );
        /* its two NT services: Wine's wineuwp-gamingservice starts, reports RUNNING and stops */
        if (in_sys32( L"wineuwp-gamingservice.exe" ))
        {
            static const WCHAR *svcs[] = { L"GamingServices", L"GamingServicesNet" };
            for (i = 0; i < ARRAY_SIZE(svcs); i++)
            {
                p = strfw( L"System\\CurrentControlSet\\Services\\%s", svcs[i] );
                set_sz( HKEY_LOCAL_MACHINE, p, L"ImagePath", L"C:\\windows\\system32\\wineuwp-gamingservice.exe" );
                set_dword( HKEY_LOCAL_MACHINE, p, L"Type", 16 );
                set_dword( HKEY_LOCAL_MACHINE, p, L"Start", 3 );
                set_dword( HKEY_LOCAL_MACHINE, p, L"ErrorControl", 1 );
                set_sz( HKEY_LOCAL_MACHINE, p, L"ObjectName", L"LocalSystem" );
                set_sz( HKEY_LOCAL_MACHINE, p, L"DisplayName", svcs[i] );
                free( p );
            }
        }
        printf( "installed: GamePlatform (Gaming Services client): Wine's winegameplatform\n" );
    }

    /* Xbox sign-in, and the Microsoft account WebAccount and Store account from the same DLL */
    if (in_sys32( L"winexboxauth.dll" ))
    {
        static const WCHAR *identity[] = { L"XboxAuthManager", L"XboxAuthInitializationArgs" };
        remove_from_sys32( L"wineuwp_xboxauth.dll" );
        for (i = 0; i < ARRAY_SIZE(identity); i++)
        {
            WCHAR *key;
            p = strfw( L"Microsoft.GamePlatform.Services.Identity.%s", identity[i] );
            set_class( p, L"winexboxauth.dll", 0 );
            key = strfw( ACTIVATABLE L"%s", p );
            set_dword( HKEY_LOCAL_MACHINE, key, L"OverridesPackage", 1 );
            free( key );
            free( p );
        }
        set_class( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager", L"winexboxauth.dll", 0 );
        set_class( L"Windows.ApplicationModel.Store.Preview.StoreConfiguration", L"winexboxauth.dll", 0 );
        printf( "installed: Xbox sign-in (XboxAuthManager, WebAuthenticationCoreManager, StoreConfiguration): "
                "Wine's winexboxauth\n" );
    }

    if (in_sys32( L"windows.applicationmodel.store.preview.installcontrol.dll" ))
        remove_from_sys32( L"wineuwp_appinstallmanager.dll" );
    if (in_sys32( L"windows.system.profile.integritypolicy.dll" ))
        set_class_dll( L"Windows.System.Profile.WindowsIntegrityPolicy", L"windows.system.profile.integritypolicy.dll" );
    if (in_sys32( L"windows.ui.shell.taskbarmanager.dll" ))
        set_class_dll( L"Windows.UI.Shell.TaskbarManager", L"windows.ui.shell.taskbarmanager.dll" );
    set_class_dll( L"Windows.Storage.AccessCache.StorageApplicationPermissions", L"windows.storage.dll" );
    remove_from_sys32( L"wineuwp_accesscache.dll" );
}

/* ---- Wine modules a prefix update does not keep in step, and the WebView2 runtime ---- */

static void refresh_wine_modules(void)
{
    WCHAR *from, *to;

    /* wineuwp is rebuilt often, and a stale copy in system32 shadows the build */
    from = wine_pe( L"wineuwp" );
    to = strfw( L"%s\\wineuwp.dll", sys32 );
    if (exists( from ) && exists( to ) && !same_file( from, to ))
    {
        copy_file( from, to );
        printf( "refreshed: stale wineuwp.dll in the prefix\n" );
    }
    free( from );
    free( to );
    from = wine_pe( L"appxinstall" );
    to = strfw( L"%s\\appxinstall.dll", sys32 );
    if (exists( from ) && !same_file( from, to ))
    {
        copy_file( from, to );
        printf( "refreshed: appxinstall.dll in the prefix\n" );
    }
    free( from );
    free( to );

    /* GDK titles LoadLibrary xgameruntime.dll: an old native stand-in and its override go */
    from = wine_pe( L"xgameruntime" );
    if (exists( from ))
    {
        HKEY key;
        to = strfw( L"%s\\xgameruntime.dll", sys32 );
        if (exists( to ) && !is_wine_builtin( to ))
        {
            DeleteFileW( to );
            copy_file( from, to );
        }
        free( to );
        if (!RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\DllOverrides", 0, KEY_SET_VALUE, &key ))
        {
            RegDeleteValueW( key, L"xgameruntime" );
            RegCloseKey( key );
        }
    }
    free( from );
}

/* The WebView2 runtime (Edge's EBWebView, from a Windows ISO), with Wine's webview2host as its
 * EmbeddedBrowserWebView.dll in front of the runtime's own, renamed .real.dll. */
static void install_webview2(void)
{
    WCHAR *app_dir, *host = wine_pe( L"webview2host" ), *ver = NULL, *dst, *vdst, *x64, *real, *orig, *p;
    struct listing l;
    unsigned i;

    if (!webview2 || !exists( host ))
    {
        free( host );
        return;
    }
    app_dir = strfw( L"%s\\Application", webview2 );
    list_dir( app_dir, &l );
    for (i = 0; i < l.count; i++)   /* the last a.b.c.d entry, in ls order */
    {
        unsigned a, b, c, d;
        WCHAR rest;
        if (swscanf( l.names[i], L"%u.%u.%u.%u%c", &a, &b, &c, &d, &rest ) == 4 && iswdigit( l.names[i][0] ))
            if (!ver || wcscmp( l.names[i], ver ) > 0) ver = l.names[i];
    }
    if (!ver) goto done;
    dst = strfw( L"%.2s\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application", sys32 );
    make_dirs( dst );
    vdst = strfw( L"%s\\%s", dst, ver );
    if (!is_dir( vdst ))
    {
        p = strfw( L"%s\\%s", app_dir, ver );
        copy_tree( p, vdst );
        free( p );
    }
    x64 = strfw( L"%s\\EBWebView\\x64", vdst );
    real = strfw( L"%s\\EmbeddedBrowserWebView.real.dll", x64 );
    orig = strfw( L"%s\\EmbeddedBrowserWebView.dll", x64 );
    if (!exists( real )) MoveFileW( orig, real );
    if (!same_file( host, orig )) copy_file( host, orig );
    p = strfw( L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\%s", ver );
    set_sz( HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\ClientState\\"
            "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"EBWebView", p );
    free( p );
    set_sz( HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
            "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"pv", ver );
    set_sz( HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
            "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"name", L"Microsoft Edge WebView2 Runtime" );
    printf( "installed: WebView2 runtime %ls with the wine-uwp host\n", ver );
    free( orig );
    free( real );
    free( x64 );
    free( vdst );
    free( dst );
done:
    free_listing( &l );
    free( app_dir );
    free( host );
}

/* ICU data for the genuine icu.dll: Windows\Globalization\ICU and Time Zone */
static void install_globalization(void)
{
    static const WCHAR *subs[][2] = { { L"ICU", L"icu" }, { L"Time Zone", L"time zone" } };
    WCHAR *glob = strfw( L"%s\\Windows\\Globalization", win11 ), *p;
    unsigned i, j;

    p = strfw( L"%s\\ICU", glob );
    if (!is_dir( p ))
    {
        free( p );
        free( glob );
        return;
    }
    free( p );
    for (i = 0; i < ARRAY_SIZE(subs); i++)
    {
        WCHAR *from = strfw( L"%s\\%s", glob, subs[i][0] ), *to = strfw( L"%s\\..\\globalization\\%s", sys32, subs[i][1] );
        struct listing l;

        make_dirs( to );
        list_dir( from, &l );
        for (j = 0; j < l.count; j++)
        {
            WCHAR *a = strfw( L"%s\\%s", from, l.names[j] ), *b = strfw( L"%s\\%s", to, l.names[j] );
            refresh_file( a, b );
            free( a );
            free( b );
        }
        free_listing( &l );
        free( from );
        free( to );
    }
    printf( "installed: Globalization data\n" );
    free( glob );
}

/* ---- the classes a prefix update may rewrite: written, then checked by --verify ---- */

struct checked_class
{
    WCHAR *cls;
    const WCHAR *dll;
    DWORD threading;
};

static struct checked_class *checked;
static unsigned checked_count;

static void check_class( const WCHAR *cls, const WCHAR *dll, DWORD threading )
{
    checked = realloc( checked, (checked_count + 1) * sizeof(*checked) );
    checked[checked_count].cls = _wcsdup( cls );
    checked[checked_count].dll = dll;
    checked[checked_count].threading = threading;
    checked_count++;
}

static void collect_checked_classes( BOOL write )
{
    static const WCHAR *wg_classes[] =
    {
        L"Windows.Graphics.Imaging.BitmapDecoder", L"Windows.Graphics.Imaging.BitmapEncoder",
        L"Windows.Graphics.Imaging.BitmapPropertySet", L"Windows.Graphics.Imaging.BitmapTransform",
        L"Windows.Graphics.Imaging.BitmapTypedValue", L"Windows.Devices.Display.DisplayMonitor",
        L"Windows.Devices.Display.LogicalDisplayReference",
    };
    static const WCHAR ic[] = L"windows.applicationmodel.store.preview.installcontrol.dll";
    unsigned i;

    if (in_sys32( L"windows.system.user.dll" ))
    {
        check_class( L"Windows.System.UserPicker", L"windows.system.user.dll", 1 );
        check_class( L"Windows.System.User", L"windows.system.user.dll", 1 );
    }
    if (in_sys32( L"windows.system.memorymanager.dll" ))
        check_class( L"Windows.System.MemoryManager", L"windows.system.memorymanager.dll", 1 );
    if (in_sys32( L"windows.devices.enumeration.dll" ))
        check_class( L"Windows.Devices.Enumeration.DeviceInformation", L"windows.devices.enumeration.dll", 1 );
    if (in_sys32( ic ))
    {
        check_class( L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager", ic, 1 );
        check_class( L"Windows.ApplicationModel.Store.Preview.InstallControl.AppUpdateOptions", ic, 1 );
    }
    if (in_sys32( L"windows.ui.shell.taskbarmanager.dll" ))
        check_class( L"Windows.UI.Shell.TaskbarManager", L"windows.ui.shell.taskbarmanager.dll", 1 );
    if (in_sys32( L"windows.services.store.dll" ))
        check_class( L"Windows.Services.Store.StoreContext", L"windows.services.store.dll", 1 );
    if (in_sys32( L"winexboxauth.dll" ))
    {
        check_class( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager", L"winexboxauth.dll", 1 );
        check_class( L"Windows.ApplicationModel.Store.Preview.StoreConfiguration", L"winexboxauth.dll", 1 );
    }
    if (in_sys32( L"windows.media.devices.dll" ))
    {
        check_class( L"Windows.Media.Audio.SpatialAudioDeviceConfiguration", L"windows.media.devices.dll", 1 );
        check_class( L"Windows.Media.Audio.SpatialAudioFormatSubtype", L"windows.media.devices.dll", 1 );
    }
    if (in_sys32( L"windows.networking.connectivity.dll" ))
        check_class( L"Windows.Networking.Connectivity.NetworkInformation", L"windows.networking.connectivity.dll", 1 );
    if (in_sys32( L"windows.applicationmodel.useractivities.dll" ))
        check_class( L"Windows.ApplicationModel.UserActivities.UserActivityChannel",
                     L"windows.applicationmodel.useractivities.dll", 1 );
    if (in_sys32( L"windows.system.profile.integritypolicy.dll" ))
        check_class( L"Windows.System.Profile.WindowsIntegrityPolicy", L"windows.system.profile.integritypolicy.dll", 1 );
    if (in_sys32( L"windows.ui.notifications.dll" ))
        check_class( L"Windows.UI.Notifications.ToastNotificationManager", L"windows.ui.notifications.dll", 1 );

    /* these are written with their threading model, not only checked */
    if (in_sys32( L"windows.storage.pickers.dll" ))
    {
        static const WCHAR *pickers[] = { L"FileOpenPicker", L"FileSavePicker", L"FolderPicker" };
        if (write) remove_from_sys32( L"wineuwp_filepicker.dll" );
        for (i = 0; i < ARRAY_SIZE(pickers); i++)
        {
            WCHAR *p = strfw( L"Windows.Storage.Pickers.%s", pickers[i] );
            if (write) set_class( p, L"windows.storage.pickers.dll", 1 );
            check_class( p, L"windows.storage.pickers.dll", 1 );
            free( p );
        }
    }
    /* Windows registers it in certenroll.dll, which the image lacks: Windows.Web.Http abandons HTTPS without it */
    if (in_sys32( L"windows.security.cryptography.certificates.dll" ))
    {
        const WCHAR *cls = L"Windows.Security.Cryptography.Certificates.Certificate";
        if (write) set_class( cls, L"windows.security.cryptography.certificates.dll", 0 );
        check_class( cls, L"windows.security.cryptography.certificates.dll", 0 );
    }
    /* the genuine Launcher goes through the shell broker and fail-fasts the app */
    if (in_sys32( L"windows.system.hostlauncher.dll" ))
    {
        if (write)
        {
            remove_from_sys32( L"wineuwp_launcher.dll" );
            set_class( L"Windows.System.Launcher", L"windows.system.hostlauncher.dll", 0 );
        }
        check_class( L"Windows.System.Launcher", L"windows.system.hostlauncher.dll", 0 );
    }
    /* Windows.Graphics.dll's imaging and display-monitor classes, which the builtin does not implement, from the
     * genuine DLL under another file name: the builtin must keep its own (DisplayInformation, composition) */
    {
        struct listing l;
        WCHAR *wg;
        list_dir( src, &l );
        if ((wg = find_any_case( &l, L"windows.graphics.dll" )))
        {
            if (write)
            {
                WCHAR *from = strfw( L"%s\\%s", src, wg ), *to = strfw( L"%s\\windows.graphics.native.dll", sys32 );
                refresh_file( from, to );
                free( from );
                free( to );
            }
            for (i = 0; i < ARRAY_SIZE(wg_classes); i++)
            {
                if (write) set_class( wg_classes[i], L"windows.graphics.native.dll", 0 );
                check_class( wg_classes[i], L"windows.graphics.native.dll", 0 );
            }
            if (write) printf( "installed: genuine Windows.Graphics.dll for imaging and display-monitor classes\n" );
        }
        free_listing( &l );
    }
}

/* 0 when every class names its DLL; otherwise each wrong one is written again */
static int verify_classes(void)
{
    unsigned i, bad = 0;

    for (i = 0; i < checked_count; i++)
    {
        WCHAR *key = strfw( ACTIVATABLE L"%s", checked[i].cls ), *path = get_sz( HKEY_LOCAL_MACHINE, key, L"DllPath" );
        WCHAR *want = strfw( L"c:\\windows\\system32\\%s", checked[i].dll );

        if (!path || wcsicmp( path, want ))
        {
            if (verbose_fail) fprintf( stderr, " %ls", checked[i].cls );
            set_class( checked[i].cls, checked[i].dll, checked[i].threading );
            bad++;
        }
        free( want );
        free( path );
        free( key );
    }
    return bad;
}

/* ---- main ---- */

static void usage(void)
{
    fprintf( stderr, "usage: wineuwp-setup --windows DIR --build DIR --data DIR [--webview2 DIR] [--force] [--verify [--report]|--new-prefix] [LIST]\n" );
    exit( 2 );
}

int __cdecl wmain( int argc, WCHAR **argv )
{
    WCHAR *list = NULL, *stamp_path, *stamp = NULL, *p, *q;
    BOOL force = FALSE, verify = FALSE, registrations = FALSE;
    int i;

    for (i = 1; i < argc; i++)
    {
        char *a;
        int len;

        if (!wcscmp( argv[i], L"--force" )) force = TRUE;
        else if (!wcscmp( argv[i], L"--verify" )) verify = TRUE;
        else if (!wcscmp( argv[i], L"--new-prefix" )) registrations = TRUE;
        else if (!wcscmp( argv[i], L"--report" )) verbose_fail = TRUE;
        else if (argv[i][0] == '-' && argv[i][1] == '-' && i + 1 < argc)
        {
            len = WideCharToMultiByte( CP_UNIXCP, 0, argv[i + 1], -1, NULL, 0, NULL, NULL );
            a = malloc( len );
            WideCharToMultiByte( CP_UNIXCP, 0, argv[i + 1], -1, a, len, NULL, NULL );
            if (!wcscmp( argv[i], L"--windows" )) win11 = dos_path( a );
            else if (!wcscmp( argv[i], L"--build" )) build = dos_path( a );
            else if (!wcscmp( argv[i], L"--data" )) data = dos_path( a );
            else if (!wcscmp( argv[i], L"--webview2" )) webview2 = dos_path( a );
            else usage();
            free( a );
            i++;
        }
        else if (argv[i][0] != '-') list = argv[i];
        else usage();
    }
    if (!win11 || !build || !data) usage();
    if (!list || !*list) list = (WCHAR *)default_list;
    src = strfw( L"%s\\Windows\\System32", win11 );
    GetSystemDirectoryW( sys32, ARRAY_SIZE(sys32) );
    if (!(p = _wgetenv( L"WINEPREFIX" ))) usage();
    {
        int len = WideCharToMultiByte( CP_UNIXCP, 0, p, -1, NULL, 0, NULL, NULL );
        char *a = malloc( len );
        WideCharToMultiByte( CP_UNIXCP, 0, p, -1, a, len, NULL, NULL );
        prefix = dos_path( a );
        free( a );
    }

    if (registrations)
    {
        new_prefix_registrations();
        return 0;
    }
    if (verify)
    {
        collect_checked_classes( FALSE );
        if (verbose_fail) fprintf( stderr, "WARNING: shim registration did not stick for:" );
        if (!verify_classes())
        {
            if (verbose_fail) fprintf( stderr, " (none now)\n" );
            printf( "verified: shim ActivatableClassId registrations\n" );
            return 0;
        }
        if (verbose_fail) fprintf( stderr, "\n" );
        return 1;
    }

    stamp_path = strfw( L"%s\\.install-overrides.stamp", prefix );
    stamp = compute_stamp( list );
    if (!force)
    {
        BYTE *old;
        DWORD size;
        if (read_file( stamp_path, &old, &size ))
        {
            BOOL same = size == wcslen( stamp ) * sizeof(WCHAR) && !memcmp( old, stamp, size );
            free( old );
            if (same)
            {
                printf( "overrides unchanged\n" );
                return 0;
            }
        }
    }

    install_native_list( list );
    printf( "installed: %u resource modules\n", install_named( resource_list, ARRAY_SIZE(resource_list) ) );
    printf( "installed: %u native-only modules\n", install_named( native_only, ARRAY_SIZE(native_only) ) );
    install_fonts();
    install_mui();
    install_winmd();

    /* services hosted by svchost do not inherit WINEDLLOVERRIDES */
    q = _wcsdup( list );
    {
        WCHAR *ctx, *n;
        for (n = wcstok( q, L" ", &ctx ); n; n = wcstok( NULL, L" ", &ctx ))
            set_sz( HKEY_CURRENT_USER, L"Software\\Wine\\DllOverrides", n, L"native" );
    }
    free( q );

    /* the per-user language profile (bcp47mrm throws without it), and MRT's single module */
    p = strfw( L"%s\\globalization.reg", data );
    reg_import( p );
    free( p );
    p = strfw( L"%s\\mrt-single-module.reg", data );
    reg_import( p );
    free( p );

    /* Windows' in-process COM classes, and activatable classes pinned to the prefix copy; the markers are per prefix,
     * since one tree can serve several */
    {
        WCHAR *hive = strfw( L"%s\\config\\SOFTWARE", src ), *out = strfw( L"%s\\wine-uwp-clsid.reg", win11 );
        WCHAR *marker = strfw( L"%s\\.wine-uwp-clsid.reg.imported", prefix );
        generate_clsid_reg( hive, out );
        reg_import_once( out, marker );
        free( marker );
        free( out );
        free( hive );
    }
    {
        WCHAR *classes = strfw( L"%s\\wine-uwp-classes.reg", win11 ), *out = strfw( L"%s\\wine-uwp-single-module.reg", win11 );
        WCHAR *marker = strfw( L"%s\\.wine-uwp-single-module.reg.imported", prefix );
        if (exists( classes ))
        {
            generate_single_module_reg( classes, out );
            reg_import_once( out, marker );
        }
        free( marker );
        free( out );
        free( classes );
    }
    /* after the generated registrations, which it overrides in places */
    import_wineuwp_reg();

    register_wine_classes();
    refresh_wine_modules();
    install_webview2();
    install_globalization();
    collect_checked_classes( TRUE );

    write_file( stamp_path, stamp, wcslen( stamp ) * sizeof(WCHAR) );
    /* exit 3: the classes are written and want verifying once the prefix settles */
    return checked_count ? 3 : 0;
}

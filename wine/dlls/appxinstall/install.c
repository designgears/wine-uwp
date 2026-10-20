/*
 * appxinstall: installing a package into DATA_DIR\stage\NAME
 *
 * The new copy is built beside the installed one (NAME.new) and swapped in at the end (the installed one becomes
 * NAME.old until the swap succeeds), so a failed update leaves the old app working. One install of a stage at a
 * time: a lock file under stage\.install-locks, kept after use (deleting one while a worker holds it would let
 * another lock a different file).
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appxinstall_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxinstall);

#define ARCH L"x64"

/* a step of appx_install that failed names itself in the log (warn+appxinstall) */
#define CHECK(expr) do { if (FAILED(hr = (expr))) { WARN( "%s failed: %#lx\n", #expr, hr ); goto done; } } while (0)

void report( const struct appx_install_params *params, enum appx_event event, UINT64 done, UINT64 total,
             const WCHAR *format, ... )
{
    WCHAR text[2048];
    va_list args;

    text[0] = 0;
    if (format)
    {
        va_start( args, format );
        _vsnwprintf( text, ARRAY_SIZE(text) - 1, format, args );
        text[ARRAY_SIZE(text) - 1] = 0;
        va_end( args );
    }
    TRACE( "%d %I64u/%I64u %s\n", event, done, total, debugstr_w(text) );
    if (params && params->callback) params->callback( params->context, event, done, total, text );
}

static BOOL cancelled( const struct appx_install_params *params )
{
    return params && params->cancel && *params->cancel;
}

/* The Xbox app's Pause: waits while params->pause is set. FALSE if the install was cancelled meanwhile. */
BOOL wait_while_paused( const struct appx_install_params *params )
{
    if (!params || !params->pause || !*params->pause) return !cancelled( params );
    report( params, APPX_EVENT_STAGE, 0, 0, L"Paused" );
    while (*params->pause && !cancelled( params )) Sleep( 200 );
    if (cancelled( params )) return FALSE;
    report( params, APPX_EVENT_STAGE, 0, 0, L"Resumed" );
    return TRUE;
}

/* ---- .appinstaller ---- */

struct appinstaller { WCHAR *uri, *package, *version; };

static void appinstaller_free( struct appinstaller *ai )
{
    free( ai->uri );
    free( ai->package );
    free( ai->version );
    memset( ai, 0, sizeof(*ai) );
}

static BOOL is_appinstaller( const WCHAR *path )
{
    const WCHAR *end = wcschr( path, '?' );
    size_t len = end ? end - path : wcslen( path );
    return len >= 13 && !wcsnicmp( path + len - 13, L".appinstaller", 13 );
}

/* an .appinstaller (a file, as a vendor's download page hands one out, or its URL): its own update URI, the main
 * bundle's URI and version; only the main bundle is used, frameworks come from the Windows files */
static HRESULT read_appinstaller( const WCHAR *path, const struct appx_install_params *params, struct appinstaller *ai )
{
    const struct xml_node *main;
    struct xml_node *root;
    struct source *source;
    UINT64 size;
    BYTE *data;
    HRESULT hr;

    if (FAILED(hr = source_open( path, params, &source ))) return hr;
    size = source_size( source );
    if (size > 16u << 20 || !(data = malloc( size + 1 ))) { source_close( source ); return E_OUTOFMEMORY; }
    hr = source_read( source, 0, data, size );
    source_close( source );
    if (SUCCEEDED(hr)) hr = xml_parse( data, size, &root );
    free( data );
    if (FAILED(hr)) return hr;
    if (!(main = xml_find( root, L"MainBundle" ))) main = xml_find( root, L"MainPackage" );
    if (!main || !xml_attr( main, L"Uri" ))
    {
        xml_free( root );
        ERR( "%s names no package to download\n", debugstr_w(path) );
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    }
    ai->package = strdupW( xml_attr( main, L"Uri" ) );
    ai->version = strdupW( xml_attr( main, L"Version" ) );
    if (root->children && xml_attr( root->children, L"Uri" )) ai->uri = strdupW( xml_attr( root->children, L"Uri" ) );
    else if (is_url( path )) ai->uri = strdupW( path );
    xml_free( root );
    return ai->package ? S_OK : E_OUTOFMEMORY;
}

/* ---- frameworks ---- */

static BOOL parse_version( const WCHAR *s, UINT64 *version )
{
    UINT64 v = 0;
    unsigned int parts = 0;

    while (*s && parts < 4)
    {
        WCHAR *end;
        ULONG n = wcstoul( s, &end, 10 );
        if (end == s || n > 0xffff) return FALSE;
        v = (v << 16) | n;
        parts++;
        s = end;
        if (*s == '.') s++;
        else break;
    }
    if (*s) return FALSE;
    while (parts++ < 4) v <<= 16;
    *version = v;
    return TRUE;
}

/* The newest x64 framework package NAME_VERSION_x64__PUBLISHERID with version >= min_version in the harvested
 * WindowsApps or stage\frameworks. With older, the newest of any version: a package built against a newer framework
 * than the Windows ISO has (VCLibs 14.0.33728 for Shapr3D 26.180, the ISO's is 14.0.30704) runs on the older one,
 * since these keep their ABI within a major version. */
static BOOL find_framework( const WCHAR *dirs[2], const struct dependency *dep, BOOL older, WCHAR **found,
                            const WCHAR **parent )
{
    UINT64 min_version = 0, best_version = 0;
    WCHAR pid[14] = { 0 }, *pattern;
    BOOL have_pid = FALSE, any = FALSE;
    size_t name_len = wcslen( dep->name );
    unsigned int i;

    if (*dep->publisher && SUCCEEDED(publisher_id( dep->publisher, pid ))) have_pid = TRUE;
    parse_version( dep->min_version, &min_version );
    *found = NULL;
    for (i = 0; i < 2; i++)
    {
        WIN32_FIND_DATAW fd;
        HANDLE find;

        if (!dirs[i] || !(pattern = path_join( dirs[i], L"*" ))) continue;
        find = FindFirstFileW( pattern, &fd );
        free( pattern );
        if (find == INVALID_HANDLE_VALUE) continue;
        do
        {
            const WCHAR *v = fd.cFileName + name_len + 1, *arch, *id;
            WCHAR version_str[64];
            UINT64 version;
            size_t vlen;

            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcsnicmp( fd.cFileName, dep->name, name_len ) || fd.cFileName[name_len] != '_') continue;
            if (!(arch = wcschr( v, '_' )) || (vlen = arch - v) >= ARRAY_SIZE(version_str)) continue;
            if (wcsnicmp( arch, L"_" ARCH L"__", 6 )) continue;
            id = arch + 6;
            if (wcslen( id ) != 13) continue;
            if (have_pid && wcsicmp( id, pid )) continue;
            memcpy( version_str, v, vlen * sizeof(WCHAR) );
            version_str[vlen] = 0;
            if (!parse_version( version_str, &version )) continue;
            if (!older && version < min_version) continue;
            if (any && version <= best_version) continue;
            free( *found );
            *found = strdupW( fd.cFileName );
            *parent = dirs[i];
            best_version = version;
            any = TRUE;
        } while (FindNextFileW( find, &fd ));
        FindClose( find );
    }
    return any && *found;
}

/* ---- files ---- */

/* removes a file or a directory tree; a symlink or junction is removed, not followed */
static void remove_tree( const WCHAR *path )
{
    DWORD attrs = GetFileAttributesW( path );
    WIN32_FIND_DATAW fd;
    WCHAR *pattern, *child;
    HANDLE find;

    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        SetFileAttributesW( path, FILE_ATTRIBUTE_NORMAL );
        DeleteFileW( path );
        return;
    }
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (pattern = path_join( path, L"*" )))
    {
        if ((find = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!wcscmp( fd.cFileName, L"." ) || !wcscmp( fd.cFileName, L".." )) continue;
                if ((child = path_join( path, fd.cFileName )))
                {
                    remove_tree( child );
                    free( child );
                }
            } while (FindNextFileW( find, &fd ));
            FindClose( find );
        }
        free( pattern );
    }
    RemoveDirectoryW( path );
}

static BOOL copy_tree( const WCHAR *src, const WCHAR *dst )
{
    WIN32_FIND_DATAW fd;
    WCHAR *pattern, *s, *d;
    BOOL ret = TRUE;
    HANDLE find;

    if (!create_directories( dst ) || !(pattern = path_join( src, L"*" ))) return FALSE;
    find = FindFirstFileW( pattern, &fd );
    free( pattern );
    if (find == INVALID_HANDLE_VALUE) return TRUE;
    do
    {
        if (!wcscmp( fd.cFileName, L"." ) || !wcscmp( fd.cFileName, L".." )) continue;
        s = path_join( src, fd.cFileName );
        d = path_join( dst, fd.cFileName );
        if (!s || !d) ret = FALSE;
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ret = copy_tree( s, d );
        else ret = CopyFileW( s, d, FALSE );
        free( s );
        free( d );
    } while (ret && FindNextFileW( find, &fd ));
    FindClose( find );
    return ret;
}

static BOOL copy_into( const WCHAR *src_dir, const WCHAR *name, const WCHAR *dst_dir )
{
    WCHAR *s = path_join( src_dir, name ), *d = path_join( dst_dir, name );
    BOOL ret = s && d && CopyFileW( s, d, FALSE );
    free( s );
    free( d );
    return ret;
}

/* writes text (UTF-8, '\n' line ends) as a whole file */
static HRESULT write_text( const WCHAR *dir, const WCHAR *name, const char *text )
{
    WCHAR *path = path_join( dir, name );
    HANDLE file;
    DWORD written;
    BOOL ok;

    if (!path) return E_OUTOFMEMORY;
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    free( path );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    ok = WriteFile( file, text, strlen( text ), &written, NULL );
    CloseHandle( file );
    return ok ? S_OK : HRESULT_FROM_WIN32( GetLastError() );
}

/* a whole small file as a NUL-terminated string; NULL if it cannot be read */
static char *read_text( const WCHAR *path )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    LARGE_INTEGER size;
    DWORD got;
    char *ret = NULL;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx( file, &size ) && size.QuadPart < (1 << 24) && (ret = malloc( size.QuadPart + 1 )))
    {
        if (ReadFile( file, ret, size.QuadPart, &got, NULL )) ret[got] = 0;
        else { free( ret ); ret = NULL; }
    }
    CloseHandle( file );
    return ret;
}

/* a growable text buffer */
struct text { char *data; size_t len, cap; };

static void text_add( struct text *t, const char *format, ... )
{
    va_list args;
    int n;

    for (;;)
    {
        va_start( args, format );
        n = t->data ? vsnprintf( t->data + t->len, t->cap - t->len, format, args ) : -1;
        va_end( args );
        if (n >= 0 && t->len + n < t->cap) { t->len += n; return; }
        t->cap = max( t->cap * 2, t->len + (n > 0 ? n : 256) + 1 );
        if (!(t->data = realloc( t->data, t->cap ))) { t->len = t->cap = 0; return; }
    }
}

/* a JSON string as Python's json module writes it: ASCII, everything else as \uXXXX (UTF-16 units) */
static void json_string( struct text *t, const WCHAR *s )
{
    if (!s) { text_add( t, "null" ); return; }
    text_add( t, "\"" );
    for (; *s; s++)
    {
        if (*s == '"' || *s == '\\') text_add( t, "\\%c", (char)*s );
        else if (*s == '\n') text_add( t, "\\n" );
        else if (*s == '\r') text_add( t, "\\r" );
        else if (*s == '\t') text_add( t, "\\t" );
        else if (*s == '\b') text_add( t, "\\b" );
        else if (*s == '\f') text_add( t, "\\f" );
        else if (*s < 0x20 || *s > 0x7e) text_add( t, "\\u%04x", *s );
        else text_add( t, "%c", (char)*s );
    }
    text_add( t, "\"" );
}

static BOOL exists( const WCHAR *path );

/* ---- GDK packages ---- */

static void json_list( struct text *t, WCHAR **items, unsigned int count, const char *indent )
{
    unsigned int i;

    if (!count) { text_add( t, "[]" ); return; }
    text_add( t, "[" );
    for (i = 0; i < count; i++)
    {
        text_add( t, "%s\n%s  ", i ? "," : "", indent );
        json_string( t, items[i] );
    }
    text_add( t, "\n%s]", indent );
}

/* a ;-separated attribute as a list, empty items dropped */
static WCHAR **split_attr( const WCHAR *value, unsigned int *count )
{
    WCHAR **list = NULL, **l;
    const WCHAR *p = value;

    *count = 0;
    while (p && *p)
    {
        const WCHAR *end = p + wcscspn( p, L";" );
        if (end > p && (l = realloc( list, (*count + 1) * sizeof(*l) )))
        {
            list = l;
            if ((list[*count] = malloc( (end - p + 1) * sizeof(WCHAR) )))
            {
                memcpy( list[*count], p, (end - p) * sizeof(WCHAR) );
                list[(*count)++][end - p] = 0;
            }
        }
        p = *end ? end + 1 : end;
    }
    return list;
}

static void free_split( WCHAR **list, unsigned int count )
{
    while (count) free( list[--count] );
    free( list );
}

static BOOL region_installed( const struct xvd *xvd, UINT32 id )
{
    unsigned int i;
    for (i = 0; i < xvd_count( xvd ); i++) if (xvd_member_region( xvd, i ) == id) return TRUE;
    return FALSE;
}

static void json_region( struct text *t, BOOL first, UINT32 id, BOOL ready, WCHAR **tags, unsigned int ntags,
                         WCHAR **langs, unsigned int nlangs )
{
    text_add( t, "%s\n    {\n      \"id\": %u,\n      \"ready\": %s,\n      \"tags\": ", first ? "" : ",", id,
              ready ? "true" : "false" );
    json_list( t, tags, ntags, "      " );
    text_add( t, ",\n      \"languages\": " );
    json_list( t, langs, nlangs, "      " );
    text_add( t, "\n    }" );
}

/* .wine-uwp-chunks.json: which GDK chunks were installed, so xgameruntime can answer availability queries */
static HRESULT write_chunk_metadata( const struct package *pkg, const WCHAR *dest, const WCHAR *full,
                                     const struct appx_install_params *params )
{
    struct xvd *xvd = package_xvd( pkg );
    struct xml_node *layout = NULL;
    const struct xml_node *doc = NULL, *node;
    const BYTE *cid;
    unsigned int i, nregions = 0, ready = 0, nfeatures = 0;
    struct text t = { 0 };
    HRESULT hr;

    if (!xvd) return S_OK;
    for (i = 0; i < package_count( pkg ); i++)
    {
        const WCHAR *name = package_name( pkg, i );
        size_t len = wcslen( name );
        BYTE *data;
        ULONG size;

        if (wcsnicmp( name, L"layout_", 7 ) || len < 4 || wcsicmp( name + len - 4, L".xml" )) continue;
        if (SUCCEEDED(package_read( pkg, i, &data, &size )))
        {
            if (FAILED(xml_parse( data, size, &layout ))) layout = NULL;
            free( data );
        }
        break;
    }
    if (layout) for (doc = layout->children; doc && !doc->name; doc = doc->next);

    cid = xvd_content_id( xvd );
    text_add( &t, "{\n  \"version\": 1,\n  \"package\": " );
    json_string( &t, full );
    /* what XPackageGetCurrentProcessPackageIdentifier answers: at most 32 characters, so not the full name; the
     * content id as a GUID's canonical hex */
    text_add( &t, ",\n  \"identifier\": \"%02X%02X%02X%02X%02X%02X%02X%02X", cid[3], cid[2], cid[1], cid[0], cid[5],
              cid[4], cid[7], cid[6] );
    for (i = 8; i < 16; i++) text_add( &t, "%02X", cid[i] );
    text_add( &t, "\",\n  \"features\": [" );
    for (node = doc ? doc->children : NULL; node; node = node->next)
    {
        const struct xml_node *f;
        if (wcscmp( node->name, L"Features" )) continue;
        for (f = node->children; f; f = f->next)
        {
            const WCHAR *id = xml_attr( f, L"Id" ), *name = xml_attr( f, L"DisplayName" ), *hidden = xml_attr( f, L"Hidden" );
            unsigned int ntags;
            WCHAR **tags = split_attr( xml_attr( f, L"Tags" ), &ntags );

            text_add( &t, "%s\n    {\n      \"id\": ", nfeatures++ ? "," : "" );
            json_string( &t, id && *id ? id : L"" );
            text_add( &t, ",\n      \"name\": " );
            json_string( &t, name && *name ? name : L"" );
            text_add( &t, ",\n      \"hidden\": %s,\n      \"tags\": ", hidden && !wcscmp( hidden, L"true" ) ? "true" : "false" );
            json_list( &t, tags, ntags, "      " );
            text_add( &t, "\n    }" );
            free_split( tags, ntags );
        }
        break;
    }
    text_add( &t, nfeatures ? "\n  ],\n  \"regions\": [" : "],\n  \"regions\": [" );
    for (node = doc ? doc->children : NULL; node; node = node->next)
    {
        const WCHAR *id = xml_attr( node, L"Id" );
        unsigned int ntags, nlangs;
        WCHAR **tags, **langs, *end;
        long rid;

        if (wcscmp( node->name, L"Chunk" ) || !id) continue;
        while (iswspace( *id )) id++;
        rid = wcstol( id, &end, 10 );
        while (iswspace( *end )) end++;
        if (end == id || *end) continue;
        tags = split_attr( xml_attr( node, L"Tags" ), &ntags );
        langs = split_attr( xml_attr( node, L"Languages" ), &nlangs );
        if (region_installed( xvd, rid )) ready++;
        json_region( &t, !nregions++, rid, region_installed( xvd, rid ), tags, ntags, langs, nlangs );
        free_split( tags, ntags );
        free_split( langs, nlangs );
    }
    if (!nregions)
    {
        /* the XVC region table when the package has no layout XML, in id order */
        UINT32 last = 0;
        for (;;)
        {
            const struct xvd_region *next = NULL;
            for (i = 0; i < xvd_region_count( xvd ); i++)
            {
                const struct xvd_region *r = xvd_region( xvd, i );
                if (r->id >= 0x40000000 && r->id != 0x40000003) continue;
                if ((nregions && r->id <= last) || (next && r->id >= next->id)) continue;
                next = r;
            }
            if (!next) break;
            last = next->id;
            if (region_installed( xvd, next->id )) ready++;
            json_region( &t, !nregions++, next->id, region_installed( xvd, next->id ), next->tags, next->tag_count,
                         next->languages, next->language_count );
        }
    }
    text_add( &t, nregions ? "\n  ]\n}\n" : "]\n}\n" );
    xml_free( layout );
    if (!t.data) return E_OUTOFMEMORY;
    hr = write_text( dest, L".wine-uwp-chunks.json", t.data );
    free( t.data );
    report( params, APPX_EVENT_MESSAGE, 0, 0, L"GDK chunks: %u/%u ready", ready, nregions );
    return hr;
}

/* whether a file's bytes name a module, case-insensitively: a packed executable (Halo's campaign) imports only its
 * unpacker, so the import table is not enough */
static BOOL file_mentions( const WCHAR *path, const char *name )
{
    HANDLE file, mapping;
    LARGE_INTEGER size;
    const BYTE *view;
    size_t len = strlen( name ), i;
    BOOL found = FALSE;

    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSizeEx( file, &size ) && size.QuadPart >= (LONGLONG)len &&
        (mapping = CreateFileMappingW( file, NULL, PAGE_READONLY, 0, 0, NULL )))
    {
        if ((view = MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, 0 )))
        {
            for (i = 0; i + len <= (size_t)size.QuadPart && !found; i++)
                found = tolower( view[i] ) == name[0] && !_strnicmp( (const char *)view + i, name, len );
            UnmapViewOfFile( view );
        }
        CloseHandle( mapping );
    }
    CloseHandle( file );
    return found;
}

static const WCHAR *gdk_d3d_dlls[] = { L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"d3d12core.dll" };

struct native_dlls
{
    const WCHAR *runtime, *concrt;
    BOOL have_d3d;
    BOOL used[ARRAY_SIZE(gdk_d3d_dlls) + 1];   /* the last: concrt140 */
    const struct appx_install_params *params;
};

/* puts src next to an executable as name unless the directory has one: a link to the shared runtime, or a copy */
static void place_dll( const WCHAR *dir, const WCHAR *src, const WCHAR *name, BOOL link )
{
    WCHAR *target = path_join( dir, name );

    if (target && !exists( target ) && !(link && CreateHardLinkW( target, src, NULL ))) CopyFileW( src, target, TRUE );
    free( target );
}

static void scan_executables( const WCHAR *dir, struct native_dlls *n )
{
    WCHAR *pattern = path_join( dir, L"*" );
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (!pattern) return;
    find = FindFirstFileW( pattern, &fd );
    free( pattern );
    if (find == INVALID_HANDLE_VALUE) return;
    do
    {
        WCHAR *path;
        size_t len = wcslen( fd.cFileName );

        if (!wcscmp( fd.cFileName, L"." ) || !wcscmp( fd.cFileName, L".." )) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!(path = path_join( dir, fd.cFileName ))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (wcsicmp( fd.cFileName, L"easyanticheat" )) scan_executables( path, n );
        }
        else if (len > 4 && !wcsicmp( fd.cFileName + len - 4, L".exe" ) && (fd.nFileSizeLow || fd.nFileSizeHigh))
        {
            unsigned int i;
            if (file_mentions( path, "d3d12.dll" ))
            {
                if (n->have_d3d)
                {
                    for (i = 0; i < ARRAY_SIZE(gdk_d3d_dlls); i++)
                    {
                        WCHAR *src = path_join( n->runtime, gdk_d3d_dlls[i] );
                        if (src) place_dll( dir, src, gdk_d3d_dlls[i], TRUE );
                        free( src );
                        n->used[i] = TRUE;
                    }
                }
                else report( n->params, APPX_EVENT_MESSAGE, 0, 0, L"warning: %s uses Direct3D 12 and there is no %s; "
                             L"run packaging/install-d3d-runtime.sh --download", fd.cFileName,
                             n->runtime ? n->runtime : L"D3D runtime" );
            }
            if (n->concrt && file_mentions( path, "concrt140.dll" ))
            {
                place_dll( dir, n->concrt, L"concrt140.dll", FALSE );
                n->used[ARRAY_SIZE(gdk_d3d_dlls)] = TRUE;
            }
        }
        free( path );
    } while (FindNextFileW( find, &fd ));
    FindClose( find );
}

/* A GDK game's native copies of DLLs Wine also builds, next to each executable that names them; returns the module
 * names, comma-separated and sorted, which launch.env then marks native,builtin for that game only (NULL: none).
 *
 * Wine's d3d12 lacks what these games check for at start (Halo Infinite: Tiled Resources Tier 2), so a game that names
 * d3d12.dll gets vkd3d-proton and the DXVK dxgi it presents through. Wine's concrt140 lacks parts of the Concurrency
 * Runtime (STATUS_WINE_STUB in Halo), so one that names concrt140.dll gets the VCLibs framework's. */
static char *link_gdk_native_dlls( const WCHAR *dest, const WCHAR *fw_root, WCHAR **frameworks, unsigned int count,
                                   const struct appx_install_params *params )
{
    static const char *names[] = { "concrt140", "d3d11", "d3d12", "d3d12core", "dxgi" };   /* sorted */
    static const unsigned int slot[] = { 4, 1, 2, 3, 0 };
    struct native_dlls n = { 0 };
    WCHAR *config = path_join( dest, L"MicrosoftGame.Config" ), *concrt = NULL;
    struct text t = { 0 };
    unsigned int i;

    if (!config || !exists( config )) { free( config ); return NULL; }
    free( config );
    n.params = params;
    n.runtime = params->d3d_runtime;
    n.have_d3d = !!n.runtime;
    for (i = 0; i < ARRAY_SIZE(gdk_d3d_dlls) && n.have_d3d; i++)
    {
        WCHAR *p = path_join( n.runtime, gdk_d3d_dlls[i] );
        n.have_d3d = p && exists( p );
        free( p );
    }
    for (i = 0; i < count && !concrt; i++)
    {
        WCHAR *dir = path_join( fw_root, frameworks[i] ), *p = dir ? path_join( dir, L"concrt140.dll" ) : NULL;
        if (p && exists( p )) { concrt = p; p = NULL; }
        free( p );
        free( dir );
    }
    n.concrt = concrt;
    scan_executables( dest, &n );
    free( concrt );
    for (i = 0; i < ARRAY_SIZE(names); i++)
        if (n.used[slot[i]]) text_add( &t, "%s%s", t.len ? "," : "", names[i] );
    return t.data;
}

/* ---- extraction ---- */

struct extract_context
{
    const struct appx_install_params *params;
    HANDLE file;
    UINT64 done, total;
};

static BOOL write_member( void *context, const BYTE *data, ULONG size )
{
    struct extract_context *ctx = context;
    DWORD written;

    if (!WriteFile( ctx->file, data, size, &written, NULL ) || written != size) return FALSE;
    ctx->done += size;
    /* progress inside a large file too, so a download that is mostly one file does not look stalled */
    report( ctx->params, APPX_EVENT_PROGRESS, ctx->done, ctx->total, NULL );
    return wait_while_paused( ctx->params );
}

static BOOL unsafe_name( const WCHAR *name )
{
    const WCHAR *p = name;

    if (*name == '/' || *name == '\\' || wcschr( name, ':' )) return TRUE;
    while (*p)
    {
        const WCHAR *end = p + wcscspn( p, L"/\\" );
        if (end - p == 2 && p[0] == '.' && p[1] == '.') return TRUE;
        p = *end ? end + 1 : end;
    }
    return FALSE;
}

static HRESULT extract( const struct package *pkg, const WCHAR *dest, const struct appx_install_params *params )
{
    struct extract_context ctx = { params };
    unsigned int i;
    HRESULT hr = S_OK;

    for (i = 0; i < package_count( pkg ); i++)
        if (!package_is_dir( pkg, i )) ctx.total += package_size( pkg, i );
    if (!ctx.total) ctx.total = 1;

    for (i = 0; i < package_count( pkg ) && SUCCEEDED(hr); i++)
    {
        const WCHAR *name = package_name( pkg, i );
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        WCHAR *path, *slash;

        if (package_is_dir( pkg, i )) continue;
        if (unsafe_name( name ))
        {
            ERR( "refusing unsafe path in package: %s\n", debugstr_w(name) );
            return HRESULT_FROM_WIN32( ERROR_BAD_PATHNAME );
        }
        if (!(path = path_join( dest, name ))) return E_OUTOFMEMORY;
        /* already there at its size (a resumed install): nothing to fetch */
        if (GetFileAttributesExW( path, GetFileExInfoStandard, &attrs ) &&
            !(attrs.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            (((UINT64)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow) == package_size( pkg, i ))
        {
            ctx.done += package_size( pkg, i );
            report( params, APPX_EVENT_PROGRESS, ctx.done, ctx.total, NULL );
            free( path );
            continue;
        }
        if ((slash = wcsrchr( path, '\\' )))
        {
            *slash = 0;
            create_directories( path );
            *slash = '\\';
        }
        ctx.file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (ctx.file == INVALID_HANDLE_VALUE) hr = HRESULT_FROM_WIN32( GetLastError() );
        else
        {
            hr = package_extract( pkg, i, write_member, &ctx );
            CloseHandle( ctx.file );
            if (FAILED(hr)) DeleteFileW( path );
        }
        free( path );
        report( params, APPX_EVENT_PROGRESS, ctx.done, ctx.total, NULL );
        if (cancelled( params )) hr = HRESULT_FROM_WIN32( ERROR_CANCELLED );
    }
    return hr;
}

/* the selected logo as .wine-uwp-icon.EXT (png, jpg or jpeg), and as .wine-uwp-icon.png too for the desktop
 * launcher when it is not a PNG, as soon as staging starts */
static void stage_logo( const struct package *pkg, int index, const WCHAR *dest )
{
    const WCHAR *name = package_name( pkg, index ), *dot = wcsrchr( name, '.' );
    WCHAR icon[32], *path, *png;
    struct extract_context ctx = { NULL };

    wcscpy( icon, L".wine-uwp-icon.png" );
    if (dot && (!wcsicmp( dot, L".jpg" ) || !wcsicmp( dot, L".jpeg" )))
        swprintf( icon, ARRAY_SIZE(icon), L".wine-uwp-icon%s", dot );
    _wcslwr( icon );
    if (!(path = path_join( dest, icon ))) return;
    ctx.file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    if (ctx.file != INVALID_HANDLE_VALUE)
    {
        BOOL ok = SUCCEEDED(package_extract( pkg, index, write_member, &ctx ));
        CloseHandle( ctx.file );
        if (!ok) DeleteFileW( path );
        else if (wcscmp( icon, L".wine-uwp-icon.png" ) && (png = path_join( dest, L".wine-uwp-icon.png" )))
        {
            CopyFileW( path, png, FALSE );
            free( png );
        }
    }
    free( path );
}

/* ---- staging ---- */

static HRESULT lock_stage( const WCHAR *stage_root, const WCHAR *stage, HANDLE *lock )
{
    WCHAR *locks, *name, *path;
    OVERLAPPED ov = { 0 };

    if (!(locks = path_join( stage_root, L".install-locks" ))) return E_OUTOFMEMORY;
    create_directories( locks );
    name = malloc( (wcslen( stage ) + 6) * sizeof(WCHAR) );
    if (name) { wcscpy( name, stage ); wcscat( name, L".lock" ); }
    path = name ? path_join( locks, name ) : NULL;
    free( locks );
    free( name );
    if (!path) return E_OUTOFMEMORY;
    *lock = CreateFileW( path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL );
    free( path );
    if (*lock == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if (!LockFileEx( *lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov ))
    {
        CloseHandle( *lock );
        *lock = INVALID_HANDLE_VALUE;
        ERR( "installation already in progress: %s\n", debugstr_w(stage) );
        return HRESULT_FROM_WIN32( ERROR_INSTALL_ALREADY_RUNNING );
    }
    return S_OK;
}

static WCHAR *with_suffix( const WCHAR *path, const WCHAR *suffix )
{
    WCHAR *r = malloc( (wcslen( path ) + wcslen( suffix ) + 1) * sizeof(WCHAR) );
    if (r) { wcscpy( r, path ); wcscat( r, suffix ); }
    return r;
}

static BOOL exists( const WCHAR *path )
{
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

/* publishes dest as final, restoring the installed copy if the rename fails */
static HRESULT publish( const WCHAR *dest, const WCHAR *final )
{
    WCHAR *backup = with_suffix( final, L".old" );
    HRESULT hr = S_OK;

    if (!backup) return E_OUTOFMEMORY;
    remove_tree( backup );
    if (exists( final ) && !MoveFileW( final, backup )) hr = HRESULT_FROM_WIN32( GetLastError() );
    else if (!MoveFileW( dest, final ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        if (exists( backup ) && !exists( final )) MoveFileW( backup, final );
    }
    else remove_tree( backup );
    free( backup );
    return hr;
}

static BOOL valid_stage_name( const WCHAR *stage )
{
    return *stage && wcscmp( stage, L"." ) && wcscmp( stage, L".." ) && !wcspbrk( stage, L"\\/:*?\"<>|" );
}

/* "Z:\..." for a path, or "C:\..." inside a prefix's drive_c, as launch.env wants it */
static char *unix_path( const WCHAR *path )
{
    char *u = wine_get_unix_file_name( path ), *r;
    if (!u) return NULL;
    r = strdup( u );
    HeapFree( GetProcessHeap(), 0, u );
    return r;
}

/* ---- the install ---- */

HRESULT CDECL appx_install( const struct appx_install_params *params )
{
    WCHAR *stage_root = NULL, *fw_root = NULL, *apps_dir = NULL, *sys32 = NULL, *final = NULL, *dest = NULL;
    WCHAR *temp = NULL, *stage = NULL, pid[14], *full = NULL, *family = NULL, **fw_names = NULL;
    const WCHAR **fw_parents = NULL, *dirs[2];
    struct appinstaller ai = { 0 };
    struct manifest *m = NULL;
    struct package *pkg = NULL;
    HANDLE lock = INVALID_HANDLE_VALUE;
    const WCHAR *package = params->package;
    struct text t = { 0 };
    unsigned int i, fw_count = 0;
    int logo;
    HRESULT hr;
    char *s;

    if (!params->package || !params->data_dir || !params->windows_tree) return E_INVALIDARG;
    if (FAILED(hr = xts_self_test())) return hr;

    report( params, APPX_EVENT_STAGE, 0, 0, L"Reading the package" );
    if (!(stage_root = path_join( params->data_dir, L"stage" )) || !(fw_root = path_join( stage_root, L"frameworks" )) ||
        !(apps_dir = path_join( params->windows_tree, L"Program Files\\WindowsApps" )) ||
        !(sys32 = path_join( params->windows_tree, L"Windows\\System32" )) ||
        !(temp = path_join( stage_root, L".install-tmp" )) || !(final = malloc( 64 * sizeof(WCHAR) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    /* scratch space for this install alone (a bundle's package when it is compressed) */
    swprintf( final, 64, L"-%lx-%I64x", GetCurrentProcessId(), GetTickCount64() );
    {
        WCHAR *unique = with_suffix( temp, final );
        free( temp );
        free( final );
        final = NULL;
        if (!(temp = unique)) { hr = E_OUTOFMEMORY; goto done; }
    }
    create_directories( temp );
    if (is_appinstaller( package ))
    {
        CHECK( read_appinstaller( package, params, &ai ) );
        package = ai.package;
    }
    CHECK( package_open( package, params, temp, &pkg ) );
    CHECK( manifest_read( pkg, &m ) );
    if (wcsicmp( m->arch, ARCH ) && wcsicmp( m->arch, L"neutral" ))
    {
        ERR( "%s is built for %s, not " "x64" "\n", debugstr_w(m->name), debugstr_w(m->arch) );
        hr = HRESULT_FROM_WIN32( ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
        goto done;
    }
    CHECK( publisher_id( m->publisher, pid ) );
    full = malloc( (wcslen( m->name ) + wcslen( m->version ) + wcslen( m->arch ) + wcslen( m->resource_id ) + 20) * sizeof(WCHAR) );
    family = malloc( (wcslen( m->name ) + 20) * sizeof(WCHAR) );
    if (!full || !family) { hr = E_OUTOFMEMORY; goto done; }
    swprintf( full, wcslen( m->name ) + wcslen( m->version ) + wcslen( m->arch ) + wcslen( m->resource_id ) + 20,
              L"%s_%s_%s_%s_%s", m->name, m->version, m->arch, m->resource_id, pid );
    swprintf( family, wcslen( m->name ) + 20, L"%s_%s", m->name, pid );
    if (params->stage) stage = strdupW( params->stage );
    else
    {
        const WCHAR *dot = wcsrchr( m->name, '.' );
        if ((stage = strdupW( dot ? dot + 1 : m->name ))) _wcslwr( stage );
    }
    if (!stage) { hr = E_OUTOFMEMORY; goto done; }
    if (!valid_stage_name( stage ))
    {
        ERR( "invalid stage directory name: %s\n", debugstr_w(stage) );
        hr = E_INVALIDARG;
        goto done;
    }
    report( params, APPX_EVENT_MESSAGE, 0, 0, L"package %s", full );

    /* each framework from the Windows files; an older version than the package asks for if that is all there is */
    dirs[0] = apps_dir;
    dirs[1] = fw_root;
    fw_names = calloc( m->dep_count + 1, sizeof(*fw_names) );
    fw_parents = calloc( m->dep_count + 1, sizeof(*fw_parents) );
    if (!fw_names || !fw_parents) { hr = E_OUTOFMEMORY; goto done; }
    for (i = 0; i < m->dep_count; i++)
    {
        if (find_framework( dirs, &m->deps[i], FALSE, &fw_names[fw_count], &fw_parents[fw_count] )) fw_count++;
        else if (find_framework( dirs, &m->deps[i], TRUE, &fw_names[fw_count], &fw_parents[fw_count] ))
        {
            report( params, APPX_EVENT_MESSAGE, 0, 0, L"framework %s: wants %s, using %s", m->deps[i].name,
                    m->deps[i].min_version, fw_names[fw_count] );
            fw_count++;
        }
        else
        {
            ERR( "framework not in the Windows files: %s >= %s\n", debugstr_w(m->deps[i].name),
                 debugstr_w(m->deps[i].min_version) );
            hr = HRESULT_FROM_WIN32( ERROR_DEPENDENCY_NOT_FOUND );
            goto done;
        }
    }

    logo = pick_logo( pkg, m->logo );
    if (!(final = path_join( stage_root, stage )) || !(dest = with_suffix( final, L".new" ))) { hr = E_OUTOFMEMORY; goto done; }
    report( params, APPX_EVENT_STAGE, 0, 0, L"Unpacking %s %s", m->title ? m->title : m->name, m->version );
    CHECK( lock_stage( stage_root, stage, &lock ) );
    {
        WCHAR *backup = with_suffix( final, L".old" );
        if (backup && exists( backup ) && !exists( final )) MoveFileW( backup, final );
        free( backup );
    }
    remove_tree( dest );
    if (!create_directories( dest )) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto done; }
    /* TODO: reuse the installed copy of the same package (install-app.py's seed_from_installed) once Wine has a
     * reflink-aware copy; a plain copy of a 100 GB game costs more than the download it saves */
    if (logo >= 0) stage_logo( pkg, logo, dest );
    else if (m->logo) report( params, APPX_EVENT_MESSAGE, 0, 0, L"warning: manifest logo %s was not included; "
                              L"installing without a launcher icon", m->logo );
    CHECK( extract( pkg, dest, params ) );
    CHECK( write_chunk_metadata( pkg, dest, full, params ) );
    package_close( pkg );
    pkg = NULL;

    report( params, APPX_EVENT_STAGE, 0, 0, L"Installing frameworks" );
    create_directories( fw_root );
    for (i = 0; i < fw_count; i++)
    {
        WCHAR *target = path_join( fw_root, fw_names[i] ), *src = path_join( fw_parents[i], fw_names[i] );
        BOOL ok = target && src && (exists( target ) || copy_tree( src, target ));
        free( target );
        free( src );
        if (!ok) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto done; }
        report( params, APPX_EVENT_MESSAGE, 0, 0, L"framework %s", fw_names[i] );
    }
    /* the DLLs an app loads by bare name, next to it (stage-app.sh does the same) */
    for (i = 0; i < fw_count; i++)
    {
        WCHAR *src = path_join( fw_root, fw_names[i] ), *pattern;
        WIN32_FIND_DATAW fd;
        HANDLE find;

        if (!src) continue;
        if (!wcsnicmp( fw_names[i], L"Microsoft.NET.Native.Runtime.", 29 ) ||
            !wcsnicmp( fw_names[i], L"Microsoft.VCLibs.140.00_", 24 ))
        {
            if ((pattern = path_join( src, L"*.dll" )))
            {
                if ((find = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
                {
                    do copy_into( src, fd.cFileName, dest ); while (FindNextFileW( find, &fd ));
                    FindClose( find );
                }
                free( pattern );
            }
        }
        else if (!wcsnicmp( fw_names[i], L"Microsoft.NET.Native.Framework.", 31 ))
            copy_into( src, L"SharedLibrary.dll", dest );
        free( src );
    }
    copy_into( sys32, L"mrt100.dll", dest );

    /* launch.env: what lets a plain `wine wineuwp-launch.exe` start the app; run.sh computes the same */
    if (params->launch_defaults && (s = read_text( params->launch_defaults )))
    {
        text_add( &t, "%s", s );
        free( s );
    }
    {
        WCHAR *eac = path_join( dest, L"EasyAntiCheat\\Settings.json" );
        /* applied by wineuwp-launch before it creates the game process */
        if (eac && exists( eac )) text_add( &t, "export WINE_HACK_NATIVE_SYSCALLS=1\n" );
        free( eac );
    }
    text_add( &t, "export WINE_UWP_PACKAGE_DEPENDENCIES='" );
    for (i = 0; i < fw_count; i++)
    {
        WCHAR *dir = path_join( fw_root, fw_names[i] ), *under = wcschr( fw_names[i], '_' ), *last = wcsstr( fw_names[i], L"__" );
        char *name = utf8_dup( fw_names[i] ), *win = dir ? utf8_dup( dir ) : NULL;
        char *prefix = name ? strdup( name ) : NULL;
        char *publisher = last ? utf8_dup( last + 2 ) : NULL;
        if (prefix && under) prefix[under - fw_names[i]] = 0;   /* the names are ASCII */
        if (name && win && prefix && publisher) text_add( &t, "%s|%s_%s|%s;", name, prefix, publisher, win );
        free( dir ); free( name ); free( win ); free( prefix ); free( publisher );
    }
    text_add( &t, "'\nexport WINEPATH='" );
    for (i = 0; i < fw_count; i++)
    {
        WCHAR *dir = path_join( fw_root, fw_names[i] );
        char *u = dir ? unix_path( dir ) : NULL;
        if (u) text_add( &t, "%s%s", i ? ":" : "", u );
        free( u );
        free( dir );
    }
    text_add( &t, "'\n" );
    if ((s = link_gdk_native_dlls( dest, fw_root, fw_names, fw_count, params )))
    {
        /* literal, as wineuwp-launch expands nothing: the defaults' list plus this game's own native DLLs */
        char *defaults = params->launch_defaults ? read_text( params->launch_defaults ) : NULL, *base = NULL, *line;
        int base_len = 0;
        for (line = defaults; line; line = strchr( line, '\n' ) ? strchr( line, '\n' ) + 1 : NULL)
        {
            static const char key[] = "export WINEDLLOVERRIDES='";
            char *end;
            if (strncmp( line, key, sizeof(key) - 1 )) continue;
            end = line + sizeof(key) - 1 + strcspn( line + sizeof(key) - 1, "'\n" );
            if (*end == '\'') { base = line + sizeof(key) - 1; base_len = end - base; }
        }
        text_add( &t, "export WINEDLLOVERRIDES='%.*s%s%s=n,b'\n", base_len, base ? base : "", base ? ";" : "", s );
        free( defaults );
        free( s );
    }
    if (!t.data || FAILED(hr = write_text( dest, L"launch.env", t.data ))) { hr = t.data ? hr : E_OUTOFMEMORY; goto done; }
    t.len = 0;
    if (params->launcher)
    {
        WCHAR *target = path_join( dest, L"wineuwp-launch.exe" );
        if (!target || !CopyFileW( params->launcher, target, FALSE ))
            report( params, APPX_EVENT_MESSAGE, 0, 0, L"warning: no %s; the app starts through run.sh only", params->launcher );
        free( target );
    }

    {
        const WCHAR *title_src = m->title;
        WCHAR *title, *p;
        char *u_full = utf8_dup( full ), *u_family = utf8_dup( family ), *u_app = utf8_dup( m->app_id ), *u_exe = utf8_dup( m->exe ), *u_title;
        const WCHAR *dot;

        if (!title_src) title_src = (dot = wcsrchr( m->name, '.' )) ? dot + 1 : m->name;
        if ((title = strdupW( title_src )))
            for (p = title; *p; p++) if (*p == '\'') memmove( p, p + 1, wcslen( p ) * sizeof(WCHAR) ), p--;
        u_title = title ? utf8_dup( title ) : NULL;
        free( title );
        if (u_full && u_family && u_app && u_exe && u_title)
        {
            text_add( &t, "export WINE_UWP_PACKAGE_FULL_NAME=%s\n", u_full );
            text_add( &t, "export WINE_UWP_PACKAGE_FAMILY_NAME=%s\n", u_family );
            text_add( &t, "# no WINE_UWP_PACKAGE_PATH: run.sh computes it from the stage directory\n" );
            text_add( &t, "export WINE_UWP_AUMID=%s!%s\n", u_family, u_app );
            text_add( &t, "export WINE_UWP_WINDOW_TITLE='%s'\n", u_title );
            text_add( &t, "export UWP_EXE='%s'\n", u_exe );
        }
        free( u_full ); free( u_family ); free( u_app ); free( u_exe ); free( u_title );
        if (!t.data || FAILED(hr = write_text( dest, L"identity.env", t.data ))) { hr = t.data ? hr : E_OUTOFMEMORY; goto done; }
        t.len = 0;
    }
    text_add( &t, "%s", "" );
    for (i = 0; i < m->protocol_count; i++)
    {
        char *u = utf8_dup( m->protocols[i] );
        if (u) text_add( &t, "%s\n", u );
        free( u );
    }
    if (!t.data || FAILED(hr = write_text( dest, L".wine-uwp-protocols", t.data ))) { hr = t.data ? hr : E_OUTOFMEMORY; goto done; }
    t.len = 0;
    /* where updates come from: the vendor's .appinstaller URI; an update from a plain package keeps the one the
     * installed copy had */
    {
        char *uri = ai.uri ? utf8_dup( ai.uri ) : NULL;
        WCHAR *old = path_join( final, L".wine-uwp-appinstaller" );
        if (!uri && old && (uri = read_text( old )))
        {
            char *end = uri + strlen( uri );
            while (end > uri && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        }
        free( old );
        if (uri && *uri)
        {
            text_add( &t, "%s\n", uri );
            if (t.data) write_text( dest, L".wine-uwp-appinstaller", t.data );
            t.len = 0;
        }
        free( uri );
    }

    CHECK( publish( dest, final ) );
    {
        const WCHAR *dot = wcsrchr( m->name, '.' );
        report( params, APPX_EVENT_STAGE, 0, 0, L"Installed %s %s", m->title ? m->title : dot ? dot + 1 : m->name, m->version );
    }
    report( params, APPX_EVENT_INSTALLED, 0, 0, L"%s", stage );

done:
    if (FAILED(hr) && dest) remove_tree( dest );   /* never another package's download or backup */
    if (lock != INVALID_HANDLE_VALUE) CloseHandle( lock );
    if (temp) remove_tree( temp );
    package_close( pkg );
    manifest_free( m );
    appinstaller_free( &ai );
    for (i = 0; fw_names && i < fw_count; i++) free( fw_names[i] );
    free( fw_names );
    free( fw_parents );
    free( t.data );
    free( stage_root ); free( fw_root ); free( apps_dir ); free( sys32 ); free( final ); free( dest ); free( temp );
    free( stage ); free( full ); free( family );
    return hr;
}

/* ---- --info ---- */


HRESULT CDECL appx_info( const WCHAR *package, const WCHAR *windows_tree, const WCHAR *data_dir, const WCHAR *icon,
                         const char *content_keys, char **json )
{
    struct appx_install_params params = { 0 };
    WCHAR *apps_dir = NULL, *fw_root = NULL, *temp = NULL, *name;
    struct appinstaller ai = { 0 };
    struct manifest *m = NULL;
    struct package *pkg = NULL;
    const WCHAR *path = package, *dirs[2];
    struct text t = { 0 };
    unsigned int i;
    int logo;
    HRESULT hr;

    if (!package || !json) return E_INVALIDARG;
    params.package = package;
    params.content_keys = content_keys;
    if (windows_tree && !(apps_dir = path_join( windows_tree, L"Program Files\\WindowsApps" ))) return E_OUTOFMEMORY;
    if (data_dir && !(fw_root = path_join( data_dir, L"stage\\frameworks" ))) { hr = E_OUTOFMEMORY; goto done; }
    {
        WCHAR dir[MAX_PATH], unique[64];
        GetTempPathW( ARRAY_SIZE(dir), dir );
        swprintf( unique, ARRAY_SIZE(unique), L"wine-uwp-info-%lx-%I64x", GetCurrentProcessId(), GetTickCount64() );
        if (!(temp = path_join( dir, unique ))) { hr = E_OUTOFMEMORY; goto done; }
        create_directories( temp );
    }
    if (is_appinstaller( path ))
    {
        if (FAILED(hr = read_appinstaller( path, &params, &ai ))) goto done;
        path = ai.package;
    }
    if (FAILED(hr = package_open( path, &params, temp, &pkg ))) goto done;
    if (FAILED(hr = manifest_read( pkg, &m ))) goto done;
    logo = pick_logo( pkg, m->logo );
    if (icon && logo >= 0)
    {
        struct extract_context ctx = { NULL };
        ctx.file = CreateFileW( icon, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (ctx.file == INVALID_HANDLE_VALUE) logo = -1;
        else
        {
            if (FAILED(package_extract( pkg, logo, write_member, &ctx ))) logo = -1;
            CloseHandle( ctx.file );
        }
    }

    text_add( &t, "{\n \"name\": " ); json_string( &t, m->name );
    text_add( &t, ",\n \"title\": " ); json_string( &t, m->title ? m->title : m->name );
    text_add( &t, ",\n \"publisher\": " ); json_string( &t, m->publisher_name );
    text_add( &t, ",\n \"version\": " ); json_string( &t, m->version );
    text_add( &t, ",\n \"architecture\": " ); json_string( &t, m->arch );
    text_add( &t, ",\n \"icon\": " ); json_string( &t, icon && logo >= 0 ? icon : NULL );
    text_add( &t, ",\n \"protocols\": [" );
    for (i = 0; i < m->protocol_count; i++)
    {
        text_add( &t, i ? ",\n  " : "\n  " );
        json_string( &t, m->protocols[i] );
    }
    text_add( &t, m->protocol_count ? "\n ]" : "]" );
    text_add( &t, ",\n \"appinstaller\": " ); json_string( &t, ai.uri );
    text_add( &t, ",\n \"download\": " ); json_string( &t, is_url( path ) ? path : NULL );
    text_add( &t, ",\n \"frameworks\": [" );
    dirs[0] = apps_dir;
    dirs[1] = fw_root;
    for (i = 0; i < m->dep_count; i++)
    {
        const WCHAR *parent;
        BOOL available = FALSE, older = FALSE;

        if (apps_dir)
        {
            if ((available = find_framework( dirs, &m->deps[i], FALSE, &name, &parent ))) free( name );
            if ((older = find_framework( dirs, &m->deps[i], TRUE, &name, &parent ))) free( name );
        }
        text_add( &t, i ? ",\n  {\n   \"name\": " : "\n  {\n   \"name\": " );
        json_string( &t, m->deps[i].name );
        text_add( &t, ",\n   \"min_version\": " );
        json_string( &t, m->deps[i].min_version );
        if (apps_dir) text_add( &t, ",\n   \"available\": %s,\n   \"older\": %s\n  }",
                                available ? "true" : "false", older ? "true" : "false" );
        else text_add( &t, ",\n   \"available\": null,\n   \"older\": null\n  }" );
    }
    text_add( &t, m->dep_count ? "\n ]\n}\n" : "]\n}\n" );
    hr = t.data ? S_OK : E_OUTOFMEMORY;

done:
    package_close( pkg );
    manifest_free( m );
    appinstaller_free( &ai );
    if (temp) remove_tree( temp );
    free( temp );
    free( apps_dir );
    free( fw_root );
    if (FAILED(hr)) free( t.data );
    else *json = t.data;
    return hr;
}

HRESULT CDECL appx_latest( const WCHAR *appinstaller, WCHAR **version )
{
    struct appx_install_params params = { 0 };
    struct appinstaller ai = { 0 };
    HRESULT hr;

    if (!appinstaller || !version) return E_INVALIDARG;
    params.package = appinstaller;
    if (FAILED(hr = read_appinstaller( appinstaller, &params, &ai ))) return hr;
    *version = ai.version;
    ai.version = NULL;
    appinstaller_free( &ai );
    return *version ? S_OK : HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
}

void CDECL appx_free( void *ptr )
{
    free( ptr );
}

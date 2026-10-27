/*
 * Connected storage in the cloud: Xbox Live title storage, for gamesave.c
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
 * https://titlestorage.xboxlive.com/connectedstorage/users/xuid(<xuid>)/scids/<scid>, with the title's XSTS token,
 * a request signature and x-xbl-pfn (protocol notes: xodus-gaming/xodus docs/xbox/titlestorage.md):
 *
 *   GET /                              the container index, 404 when there is none
 *   GET /savedgames/<container>        its atoms: blob name -> atom id
 *   GET /<atom>,binary                 a blob's bytes
 *   PUT /savedgames/<container>?...    the container's atom list
 *   DELETE /savedgames/<container>
 *   PUT|DELETE /lock                   the per-user write lock
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

/*
 * Each container directory of gamesave.c carries:
 *   .sync     what the cloud had when we last agreed: "etag <etag>" then "atom <hex blob name> <atom id>" lines
 *   .changed  hex names of blobs written since (one per line), uploaded to new atoms
 *   .dirty    exists while local changes have not been uploaded
 * On GetForUserAsync the provider syncs: a container whose cloud etag differs is downloaded, unless it has local
 * changes, which win and are uploaded; a container the cloud no longer has is deleted here, unless it was never
 * uploaded. After that, every submit is uploaded in the background and a delete is sent too. A failure leaves .dirty
 * for the next sync. The service's per-user lock is held around each sync and upload.
 */

#define BLOCK_SIZE (4u << 20)

static CRITICAL_SECTION cloud_cs;
static CRITICAL_SECTION_DEBUG cloud_cs_debug =
{
    0, 0, &cloud_cs,
    { &cloud_cs_debug.ProcessLocksList, &cloud_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": cloud_cs") }
};
static CRITICAL_SECTION cloud_cs = { &cloud_cs_debug, -1, 0, 0, 0, 0 };

LONG WINAPI GetCurrentPackageFamilyName( UINT32 *length, WCHAR *name );

static char *package_family( void )
{
    WCHAR name[128];
    UINT32 len = ARRAY_SIZE(name);

    /* the variable is wineuwp-launch's, for processes outside a package (tests) */
    if (GetCurrentPackageFamilyName( &len, name ) &&
        !GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FAMILY_NAME", name, ARRAY_SIZE(name) ))
        return NULL;
    return narrow( name, wcslen( name ) );
}

/* one title storage request; returns the HTTP status, 0 when nothing came back */
DWORD xbl_call( const char *method, const char *url, const char *extra, const void *body, size_t len,
                struct buf *resp )
{
    struct buf h = {0};
    char *header = NULL;
    const char *a, *b;
    WCHAR *wmethod;
    DWORD status;

    resp->n = 0;
    buf_addn( resp, "", 0 );
    if (FAILED(sign_in_silently()) || FAILED(get_token_for_url( url, FALSE, &header, XSTS_WINDOWS_DEVICE ))) return 0;
    hdr_add( &h, "Authorization: %s", header );
    for (a = extra; a && *a; a = *b ? b + 1 : b)
    {
        b = a + strcspn( a, "|" );
        if (b > a)
        {
            buf_addn( &h, a, b - a );
            buf_add( &h, "\r\n" );
        }
    }
    add_signature( &h, method, url, header, body ? body : "", len );
    wmethod = widen( method );
    status = http_request( wmethod, url, buf_str( &h ), body, len, resp );
    free( wmethod );
    str_wipe_free( header );
    buf_free( &h );
    TRACE( "%s %s: HTTP %lu, %u bytes\n", method, debugstr_a(url), status, (unsigned)resp->n );
    return status;
}

/* ---- requests ---- */

/* base + path, with the title's package family name and any extra headers ("A: b|C: d") */
static DWORD ts_call( const char *method, const char *base, const char *path, const char *extra, const void *body,
                      size_t len, struct buf *resp )
{
    char *url = strf( "%s%s", base, path ), *pfn = package_family(), *headers;
    DWORD status;

    /* title storage refuses requests without the title's package (403) */
    headers = strf( "x-xbl-pfn: %s%s%s", S(pfn), extra ? "|" : "", S(extra) );
    status = xbl_call( method, url, headers, body, len, resp );
    free( headers );
    free( pfn );
    free( url );
    return status;
}

static BOOL lock_take( const char *base )
{
    WCHAR wname[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = ARRAY_SIZE(wname);
    struct buf resp = {0};
    char *name, *enc, *path;
    DWORD status;

    if (!GetComputerNameW( wname, &n )) wcscpy( wname, L"Wine" );
    name = narrow( wname, wcslen( wname ) );
    enc = urlencode( S(name) );
    path = strf( "/lock?friendlyName=%s", enc );
    status = ts_call( "PUT", base, path, "x-xbl-lock-ver: 1", NULL, 0, &resp );
    if (status != 200) WARN( "title storage lock: HTTP %lu %s\n", status, debugstr_an( resp.p, min( resp.n, 200 ) ) );
    buf_free( &resp );
    free( path );
    free( enc );
    free( name );
    return status == 200;
}

static void lock_release( const char *base, BOOL uploaded )
{
    struct buf resp = {0};
    ts_call( "DELETE", base, uploaded ? "/lock?newSavesUploaded=true" : "/lock?newSavesUploaded=false", NULL, NULL, 0,
             &resp );
    buf_free( &resp );
}

static char *container_path( const char *container )
{
    char *enc = urlencode( container ), *ret = strf( "/savedgames/%s", enc );
    free( enc );
    return ret;
}

/* ---- the .sync record ---- */

struct sync_record
{
    char *etag;
    unsigned count;
    char **hex, **atom;   /* blob (hex name, as on disk) -> atom id */
};

static void record_free( struct sync_record *r )
{
    unsigned i;
    for (i = 0; i < r->count; i++)
    {
        free( r->hex[i] );
        free( r->atom[i] );
    }
    free( r->hex );
    free( r->atom );
    free( r->etag );
    memset( r, 0, sizeof(*r) );
}

static void record_add( struct sync_record *r, const char *hex, const char *atom )
{
    char **h = realloc( r->hex, (r->count + 1) * sizeof(*h) ), **a;
    if (!h) return;
    r->hex = h;
    if (!(a = realloc( r->atom, (r->count + 1) * sizeof(*a) ))) return;
    r->atom = a;
    r->hex[r->count] = str_dup( hex );
    r->atom[r->count] = str_dup( atom );
    r->count++;
}

static const char *record_atom( const struct sync_record *r, const char *hex )
{
    unsigned i;
    for (i = 0; i < r->count; i++) if (!strcmp( r->hex[i], hex )) return r->atom[i];
    return NULL;
}

static BOOL dot_file( const WCHAR *dir, const WCHAR *name, BYTE **data, DWORD *size )
{
    WCHAR *path = path_join( dir, name );
    BOOL ok = path && read_file( path, data, size );
    free( path );
    return ok;
}

static BOOL has_dot_file( const WCHAR *dir, const WCHAR *name )
{
    WCHAR *path = path_join( dir, name );
    BOOL ok = path && GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
    free( path );
    return ok;
}

static void drop_dot_file( const WCHAR *dir, const WCHAR *name )
{
    WCHAR *path = path_join( dir, name );
    if (path) DeleteFileW( path );
    free( path );
}

static void put_dot_file( const WCHAR *dir, const WCHAR *name, const void *data, size_t len )
{
    WCHAR *path = path_join( dir, name );
    if (path) write_file( path, data, len );
    free( path );
}

/* each line of a dot file, NUL-terminated in place */
static char *next_line( char **p )
{
    char *line = *p, *end;
    if (!line || !*line) return NULL;
    end = line + strcspn( line, "\n" );
    *p = *end ? end + 1 : end;
    *end = 0;
    if (end > line && end[-1] == '\r') end[-1] = 0;
    return line;
}

static BOOL read_text( const WCHAR *dir, const WCHAR *name, char **text )
{
    BYTE *data;
    DWORD size;

    *text = NULL;
    if (!dot_file( dir, name, &data, &size )) return FALSE;
    if (!(*text = realloc( data, size + 1 ))) { free( data ); return FALSE; }
    (*text)[size] = 0;
    return TRUE;
}

static BOOL record_read( const WCHAR *dir, struct sync_record *r )
{
    char *text, *p, *line;

    memset( r, 0, sizeof(*r) );
    if (!read_text( dir, L".sync", &text )) return FALSE;
    for (p = text; (line = next_line( &p ));)
    {
        if (!strncmp( line, "etag ", 5 )) str_set( &r->etag, line + 5 );
        else if (!strncmp( line, "atom ", 5 ))
        {
            char *sp = strchr( line + 5, ' ' );
            if (!sp) continue;
            *sp = 0;
            record_add( r, line + 5, sp + 1 );
        }
    }
    free( text );
    return TRUE;
}

static void record_write( const WCHAR *dir, const struct sync_record *r )
{
    struct buf b = {0};
    unsigned i;

    buf_printf( &b, "etag %s\n", S(r->etag) );
    for (i = 0; i < r->count; i++) buf_printf( &b, "atom %s %s\n", r->hex[i], r->atom[i] );
    put_dot_file( dir, L".sync", b.p, b.n );
    buf_free( &b );
}

/* ---- the index ---- */

/* the cloud's containers, or NULL with *ok FALSE when it could not be read */
static struct json *read_index( const char *base, BOOL *ok )
{
    struct buf resp = {0};
    struct json *j = NULL;
    DWORD status = ts_call( "GET", base, "/", NULL, NULL, 0, &resp );

    *ok = status == 200 || status == 404;
    if (status == 200) j = json_parse( resp.p, resp.n );
    else if (!*ok) WARN( "title storage index: HTTP %lu %s\n", status, debugstr_an( resp.p, min( resp.n, 200 ) ) );
    buf_free( &resp );
    return j;
}

/* the container name of an index entry ("name,savedgame"), or NULL */
static char *entry_name( const struct json *entry )
{
    const char *file = json_str( json_get( entry, "fileName" ) );
    size_t n = strlen( file );
    if (n <= 10 || strcmp( file + n - 10, ",savedgame" )) return NULL;
    return str_dupn( file, n - 10 );
}

static const struct json *index_find( const struct json *index, const char *name )
{
    const struct json *blobs = json_get( index, "blobs" );
    unsigned i;

    for (i = 0; i < json_count( blobs ); i++)
    {
        char *n = entry_name( json_at( blobs, i ) );
        BOOL match = n && !strcmp( n, name );
        free( n );
        if (match) return json_at( blobs, i );
    }
    return NULL;
}

/* ---- download ---- */

static BOOL download_container( const char *base, const WCHAR *dir, const char *name, const struct json *entry )
{
    struct sync_record r = {0};
    struct buf resp = {0};
    char *path = container_path( name ), *display;
    const struct json *atoms;
    struct json *j;
    WCHAR *pattern;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    DWORD status;
    unsigned i;
    BOOL ok = TRUE;

    status = ts_call( "GET", base, path, NULL, NULL, 0, &resp );
    free( path );
    if (status != 200)
    {
        WARN( "download %s: HTTP %lu\n", debugstr_a(name), status );
        buf_free( &resp );
        return FALSE;
    }
    j = json_parse( resp.p, resp.n );
    buf_free( &resp );
    atoms = json_get( j, "atoms" );
    make_dirs( dir );
    for (i = 0; ok && i < json_count( atoms ); i++)
    {
        const struct json *a = json_at( atoms, i );
        const char *blob = json_str( json_get( a, "name" ) ), *atom = json_str( json_get( a, "atom" ) );
        double size = json_get( a, "size" )->number;
        WCHAR *hex = hex_name( blob ), *file = hex ? path_join( dir, hex ) : NULL;
        char *apath = strf( "/%s,binary", atom ), *hexa = hex ? narrow( hex, wcslen( hex ) ) : NULL;

        status = ts_call( "GET", base, apath, NULL, NULL, 0, &resp );
        if (status != 200 || resp.n != (size_t)size || !file || !write_file( file, resp.p, resp.n ))
        {
            WARN( "download %s/%s: HTTP %lu, %Iu of %.0f bytes\n", debugstr_a(name), debugstr_a(blob), status, resp.n, size );
            ok = FALSE;
        }
        else record_add( &r, hexa, atom );
        buf_free( &resp );
        free( apath );
        free( hexa );
        free( file );
        free( hex );
    }
    if (ok && (pattern = path_join( dir, L"*" )))
    {
        /* blobs the cloud does not have */
        if ((h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
        {
            do
            {
                char *hexa;
                WCHAR *file;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || fd.cFileName[0] == '.') continue;
                hexa = narrow( fd.cFileName, wcslen( fd.cFileName ) );
                if (hexa && !record_atom( &r, hexa ) && (file = path_join( dir, fd.cFileName )))
                {
                    DeleteFileW( file );
                    free( file );
                }
                free( hexa );
            } while (FindNextFileW( h, &fd ));
            FindClose( h );
        }
        free( pattern );
    }
    if (ok)
    {
        display = str_dup( json_str( json_get( entry, "displayName" ) ) );
        if (*display) put_dot_file( dir, L".display", display, strlen( display ) );
        free( display );
        r.etag = str_dup( json_str( json_get( entry, "etag" ) ) );
        record_write( dir, &r );
        drop_dot_file( dir, L".changed" );
        drop_dot_file( dir, L".dirty" );
        TRACE( "cloud -> here: %s, %u blobs\n", debugstr_a(name), r.count );
    }
    record_free( &r );
    json_free( j );
    return ok;
}

/* ---- upload ---- */

static char *new_atom_id( void )
{
    GUID g;
    CoCreateGuid( &g );
    return strf( "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                 g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7] );
}

/* POST /atoms/<id> for an upload URI, the bytes to it in blocks, then POST ?commit=true with the block ids */
static BOOL upload_atom( const char *base, const char *atom, const BYTE *data, DWORD size )
{
    struct buf resp = {0}, ids = {0};
    char *path = strf( "/atoms/%s", atom ), *body = strf( "{\"size\":%lu}", size ), *uri = NULL;
    const char *json_hdr = "Content-Type: application/json|Accept: application/json";
    struct json *j;
    DWORD status, done;
    unsigned block;
    BOOL ok = FALSE;

    status = ts_call( "POST", base, path, json_hdr, body, strlen( body ), &resp );
    free( body );
    j = json_parse( resp.p, resp.n );
    if (status == 200) uri = str_dup( json_str( json_get( j, "blobUri" ) ) );
    json_free( j );
    buf_free( &resp );
    if (!uri || !*uri)
    {
        WARN( "upload: no blob URI for atom %s (HTTP %lu)\n", atom, status );
        goto done;
    }
    buf_add( &ids, "[" );
    for (block = 0, done = 0; done < size || !block; block++)
    {
        DWORD n = min( size - done, BLOCK_SIZE );
        char raw[16], *id, *enc, *url;

        snprintf( raw, sizeof(raw), "block-%06u", block );
        id = b64( (const unsigned char *)raw, strlen( raw ), FALSE );
        enc = urlencode( id );
        url = strf( "%s&comp=block&blockid=%s", uri, enc );
        status = http_request( L"PUT", url, "x-ms-version: 2015-04-05\r\n", data + done, n, &resp );
        buf_free( &resp );
        buf_printf( &ids, "%s\"%s\"", block ? "," : "", id );
        free( url );
        free( enc );
        free( id );
        if (status != 201)
        {
            WARN( "upload: block %u of atom %s: HTTP %lu\n", block, atom, status );
            goto done;
        }
        done += n;
    }
    buf_add( &ids, "]" );
    free( path );
    path = strf( "/atoms/%s?commit=true", atom );
    body = strf( "{\"blockIds\":%s,\"size\":%lu}", ids.p, size );
    status = ts_call( "POST", base, path, json_hdr, body, strlen( body ), &resp );
    free( body );
    buf_free( &resp );
    if (!(ok = status / 100 == 2)) WARN( "upload: commit of atom %s: HTTP %lu\n", atom, status );
done:
    buf_free( &ids );
    free( uri );
    free( path );
    return ok;
}

static void iso_now( char out[32] )
{
    SYSTEMTIME t;
    GetSystemTime( &t );
    snprintf( out, 32, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
              t.wMilliseconds );
}

static BOOL upload_container( const char *base, const WCHAR *dir, const char *name )
{
    struct sync_record old = {0}, r = {0};
    struct buf body = {0}, resp = {0};
    char *changed = NULL, *display = NULL, *path, *p, *line, when[32];
    WCHAR *pattern = path_join( dir, L"*" );
    WIN32_FIND_DATAW fd;
    struct json *index;
    BOOL ok = TRUE, index_ok;
    HANDLE h;
    DWORD status;

    record_read( dir, &old );
    read_text( dir, L".changed", &changed );
    read_text( dir, L".display", &display );
    buf_add( &body, "{\"atoms\":[" );
    if (pattern && (h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            char *hexa, *blob, *e, *atom;
            const char *have;
            BOOL fresh;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || fd.cFileName[0] == '.') continue;
            if (wcslen( fd.cFileName ) > 4 && !wcscmp( fd.cFileName + wcslen( fd.cFileName ) - 4, L".new" )) continue;
            hexa = narrow( fd.cFileName, wcslen( fd.cFileName ) );
            blob = unhex_name( fd.cFileName );
            if (!hexa || !blob)
            {
                free( hexa );
                free( blob );
                continue;
            }
            have = record_atom( &old, hexa );
            fresh = !have;
            if (!fresh && changed)
            {
                /* was it written since the last sync? */
                char *copy = str_dup( changed );
                for (p = copy; (line = next_line( &p ));) if (!strcmp( line, hexa )) fresh = TRUE;
                free( copy );
            }
            if (fresh)
            {
                WCHAR *file = path_join( dir, fd.cFileName );
                BYTE *data = NULL;
                DWORD size = 0;

                atom = new_atom_id();
                if (!file || !read_file( file, &data, &size ) || !upload_atom( base, atom, data, size )) ok = FALSE;
                free( data );
                free( file );
            }
            else atom = str_dup( have );
            if (ok)
            {
                e = json_escape( blob );
                buf_printf( &body, "%s{\"atom\":\"%s\",\"name\":\"%s\"}", r.count ? "," : "", atom, e );
                free( e );
                record_add( &r, hexa, atom );
            }
            free( atom );
            free( hexa );
            free( blob );
        } while (ok && FindNextFileW( h, &fd ));
        FindClose( h );
    }
    free( pattern );
    buf_add( &body, "]}" );
    if (ok)
    {
        char *container = container_path( name ), *dn = urlencode( S(display) );
        iso_now( when );
        path = strf( "%s?clientFileTime=%s%s%s", container, when, display && *display ? "&displayName=" : "",
                     display && *display ? dn : "" );
        status = ts_call( "PUT", base, path, "Content-Type: application/json", body.p, body.n, &resp );
        if (!(ok = status / 100 == 2))
            WARN( "upload %s: HTTP %lu %s\n", debugstr_a(name), status, debugstr_an( resp.p, min( resp.n, 200 ) ) );
        buf_free( &resp );
        free( path );
        free( dn );
        free( container );
    }
    if (ok)
    {
        if ((index = read_index( base, &index_ok )))
        {
            const struct json *entry = index_find( index, name );
            if (entry) r.etag = str_dup( json_str( json_get( entry, "etag" ) ) );
            json_free( index );
        }
        record_write( dir, &r );
        drop_dot_file( dir, L".changed" );
        drop_dot_file( dir, L".dirty" );
        TRACE( "here -> cloud: %s, %u blobs\n", debugstr_a(name), r.count );
    }
    buf_free( &body );
    free( changed );
    free( display );
    record_free( &old );
    record_free( &r );
    return ok;
}

static void delete_tree( const WCHAR *dir )
{
    WCHAR *pattern = path_join( dir, L"*" );
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (pattern && (h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *file;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if ((file = path_join( dir, fd.cFileName ))) DeleteFileW( file );
            free( file );
        } while (FindNextFileW( h, &fd ));
        FindClose( h );
    }
    free( pattern );
    RemoveDirectoryW( dir );
}

/* ---- the provider's sync ---- */

char *cloud_base( const char *scid, const char *xuid )
{
    if (!scid || !*scid || !xuid || !*xuid) return NULL;
    return strf( "https://titlestorage.xboxlive.com/connectedstorage/users/xuid(%s)/scids/%s", xuid, scid );
}

void cloud_sync( const char *base, const WCHAR *dir )
{
    WCHAR *pattern;
    WIN32_FIND_DATAW fd;
    struct json *index;
    const struct json *blobs;
    BOOL ok, uploaded = FALSE;
    unsigned i, down = 0, up = 0, gone = 0;
    HANDLE h;

    if (!base) return;
    EnterCriticalSection( &cloud_cs );
    lock_take( base );
    index = read_index( base, &ok );
    if (!ok)
    {
        lock_release( base, FALSE );
        LeaveCriticalSection( &cloud_cs );
        return;
    }
    /* the cloud's containers */
    blobs = json_get( index, "blobs" );
    for (i = 0; i < json_count( blobs ); i++)
    {
        const struct json *entry = json_at( blobs, i );
        char *name = entry_name( entry );
        WCHAR *hex = name ? hex_name( name ) : NULL, *cdir = hex ? path_join( dir, hex ) : NULL;
        struct sync_record r;

        if (cdir)
        {
            record_read( cdir, &r );
            if (!r.etag || strcmp( r.etag, json_str( json_get( entry, "etag" ) ) ))
            {
                if (has_dot_file( cdir, L".dirty" ))
                    TRACE( "%s changed here and in the cloud; keeping this machine's\n", debugstr_a(name) );
                else if (download_container( base, cdir, name, entry )) down++;
            }
            record_free( &r );
        }
        free( cdir );
        free( hex );
        free( name );
    }
    /* ours: new or changed ones go up, synced ones the cloud no longer has are gone */
    if ((pattern = path_join( dir, L"*" )) && (h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *cdir;
            char *name;

            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            if (!(name = unhex_name( fd.cFileName ))) continue;
            if ((cdir = path_join( dir, fd.cFileName )))
            {
                BOOL dirty = has_dot_file( cdir, L".dirty" ), synced = has_dot_file( cdir, L".sync" );
                if (!index_find( index, name ) && synced && !dirty)
                {
                    delete_tree( cdir );
                    gone++;
                }
                else if (dirty || !synced)
                {
                    if (upload_container( base, cdir, name ))
                    {
                        uploaded = TRUE;
                        up++;
                    }
                }
            }
            free( cdir );
            free( name );
        } while (FindNextFileW( h, &fd ));
        FindClose( h );
    }
    free( pattern );
    lock_release( base, uploaded );
    LeaveCriticalSection( &cloud_cs );
    json_free( index );
    TRACE( "title storage synced: %u down, %u up, %u removed\n", down, up, gone );
}

/* ---- after a submit or a delete: in the background ---- */

struct job
{
    char *base;
    WCHAR *dir;         /* the container's, for an upload */
    char *name;
};

static void job_free( struct job *j )
{
    free( j->base );
    free( j->dir );
    free( j->name );
    free( j );
}

static void CALLBACK upload_job( TP_CALLBACK_INSTANCE *instance, void *ctx )
{
    struct job *j = ctx;
    BOOL ok;

    EnterCriticalSection( &cloud_cs );
    /* a later job may have uploaded it already */
    if (has_dot_file( j->dir, L".dirty" ))
    {
        lock_take( j->base );
        ok = upload_container( j->base, j->dir, j->name );
        lock_release( j->base, ok );
        if (!ok) WARN( "%s stays to be uploaded at the next sync\n", debugstr_a(j->name) );
    }
    LeaveCriticalSection( &cloud_cs );
    job_free( j );
}

static void CALLBACK delete_job( TP_CALLBACK_INSTANCE *instance, void *ctx )
{
    struct job *j = ctx;
    struct buf resp = {0};
    char *path = container_path( j->name );
    DWORD status;

    EnterCriticalSection( &cloud_cs );
    lock_take( j->base );
    status = ts_call( "DELETE", j->base, path, NULL, NULL, 0, &resp );
    lock_release( j->base, FALSE );
    LeaveCriticalSection( &cloud_cs );
    TRACE( "cloud delete %s: HTTP %lu\n", debugstr_a(j->name), status );
    buf_free( &resp );
    free( path );
    job_free( j );
}

void cloud_blob_written( const WCHAR *container_dir, const char *blob )
{
    WCHAR *hex = hex_name( blob ), *path = path_join( container_dir, L".changed" );
    char *line = hex ? strf( "%S\n", hex ) : NULL;
    HANDLE f;
    DWORD done;

    if (path && line && (f = CreateFileW( path, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, 0, NULL )) != INVALID_HANDLE_VALUE)
    {
        WriteFile( f, line, strlen( line ), &done, NULL );
        CloseHandle( f );
    }
    put_dot_file( container_dir, L".dirty", "", 0 );
    free( line );
    free( path );
    free( hex );
}

void cloud_container_changed( const char *base, const WCHAR *dir, const char *container )
{
    struct job *j;

    put_dot_file( dir, L".dirty", "", 0 );
    if (!base || !(j = calloc( 1, sizeof(*j) ))) return;
    j->base = str_dup( base );
    j->dir = wcsdup( dir );
    j->name = str_dup( container );
    if (!TrySubmitThreadpoolCallback( upload_job, j, NULL )) job_free( j );
}

void cloud_container_deleted( const char *base, const char *container )
{
    struct job *j;

    if (!base || !(j = calloc( 1, sizeof(*j) ))) return;
    j->base = str_dup( base );
    j->name = str_dup( container );
    if (!TrySubmitThreadpoolCallback( delete_job, j, NULL )) job_free( j );
}

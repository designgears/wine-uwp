/*
 * appxinstall: a package's members, whatever the container
 *
 * A plain .appx/.msix is a zip. A bundle (.appxbundle/.msixbundle) is a zip of such packages, and the one installed
 * is its x64 application package: read in place when stored uncompressed, which is how bundles store them, and
 * extracted to a temporary file otherwise. Store stub packages (AppxMetadata\Stub\...) are skipped.
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

#include <stdlib.h>
#include <string.h>

#include "appxinstall_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxinstall);

struct package
{
    struct source *outer;       /* the file or URL */
    struct source *inner;       /* a bundle's package, in place */
    struct zip *bundle;
    struct zip *zip;            /* the package's members */
    struct xvd *xvd;            /* or a GDK package's */
    struct eappx *eappx;        /* or an encrypted package's */
    WCHAR *temp;                /* a bundle's package extracted here, deleted on close */
    WCHAR **names;              /* member names, percent-decoded, '/'-separated */
    WCHAR *dir;                 /* or an unpacked package: a directory with its AppxManifest.xml */
    UINT64 *sizes;              /* the directory's files' sizes */
    unsigned int count;         /* and their number */
};

/* package zips percent-encode names ("%20" for a space) over UTF-8; [Content_Types].xml is stored literally */
WCHAR *unescape_name( const WCHAR *name )
{
    int len = WideCharToMultiByte( CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL ), i, j, n;
    char *utf8, *out;
    WCHAR *ret = NULL;

    if (!(utf8 = malloc( len )) || !(out = malloc( len ))) { free( utf8 ); return NULL; }
    WideCharToMultiByte( CP_UTF8, 0, name, -1, utf8, len, NULL, NULL );
    for (i = j = 0; utf8[i]; i++)
    {
        int hi, lo;
        if (utf8[i] == '%' && (hi = hex_value( utf8[i + 1] )) >= 0 && (lo = hex_value( utf8[i + 2] )) >= 0)
        {
            out[j++] = (char)(hi * 16 + lo);
            i += 2;
        }
        else out[j++] = utf8[i];
    }
    out[j] = 0;
    n = MultiByteToWideChar( CP_UTF8, 0, out, -1, NULL, 0 );
    if ((ret = malloc( n * sizeof(WCHAR) ))) MultiByteToWideChar( CP_UTF8, 0, out, -1, ret, n );
    free( utf8 );
    free( out );
    return ret;
}

static BOOL write_file( void *context, const BYTE *data, ULONG size )
{
    DWORD written;
    return WriteFile( context, data, size, &written, NULL ) && written == size;
}

/* the x64 application package of a bundle, as a source; S_FALSE if zip is not a bundle */
static HRESULT open_bundle_package( struct package *pkg, const WCHAR *temp_dir )
{
    const struct zip_entry *manifest, *entry = NULL;
    const struct xml_node *node = NULL;
    struct xml_node *root;
    const WCHAR *file;
    UINT64 offset;
    ULONG size;
    BYTE *data;
    HRESULT hr;

    if (!(manifest = zip_find( pkg->bundle, L"AppxMetadata/AppxBundleManifest.xml" ))) return S_FALSE;
    if (FAILED(hr = zip_read( pkg->bundle, manifest, &data, &size ))) return hr;
    hr = xml_parse( data, size, &root );
    free( data );
    if (FAILED(hr)) return hr;
    while ((node = xml_next( root, node, L"Package" )))
    {
        const WCHAR *type = xml_attr( node, L"Type" ), *arch = xml_attr( node, L"Architecture" );
        unsigned int i;

        if (!(file = xml_attr( node, L"FileName" ))) continue;
        if (!wcsnicmp( file, L"AppxMetadata\\", 13 ) || !wcsnicmp( file, L"AppxMetadata/", 13 )) continue;
        if (type && wcsicmp( type, L"application" )) continue;
        if (!arch || wcsicmp( arch, L"x64" )) continue;
        for (i = 0; i < zip_count( pkg->bundle ) && !entry; i++)
        {
            const struct zip_entry *e = zip_entry( pkg->bundle, i );
            WCHAR *decoded = unescape_name( e->name );
            if (decoded && path_equal( decoded, file )) entry = e;
            free( decoded );
        }
        if (entry) break;
    }
    xml_free( root );
    if (!entry)
    {
        ERR( "this bundle has no x64 application package\n" );
        return HRESULT_FROM_WIN32( ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
    }
    TRACE( "bundle package %s, method %u\n", debugstr_w(entry->name), entry->method );
    if (entry->method == 0)
    {
        if (FAILED(hr = zip_data_offset( pkg->bundle, entry, &offset ))) return hr;
        return source_open_range( pkg->outer, offset, entry->size, &pkg->inner );
    }
    else
    {
        HANDLE file_handle;

        if (!(pkg->temp = path_join( temp_dir, L"bundle-package.appx" ))) return E_OUTOFMEMORY;
        file_handle = CreateFileW( pkg->temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (file_handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
        hr = zip_extract( pkg->bundle, entry, write_file, file_handle );
        CloseHandle( file_handle );
        if (FAILED(hr)) return hr;
        return source_open( pkg->temp, NULL, &pkg->inner );
    }
}

static const struct package *sort_pkg;   /* qsort has no context argument */

/* the files under dir, relative and '/'-separated, as package members */
static BOOL list_dir( struct package *pkg, const WCHAR *dir, const WCHAR *rel )
{
    WIN32_FIND_DATAW fd;
    WCHAR *pattern = path_join( dir, L"*" );
    HANDLE find;
    BOOL ok = TRUE;

    if (!pattern) return FALSE;
    find = FindFirstFileW( pattern, &fd );
    free( pattern );
    if (find == INVALID_HANDLE_VALUE) return TRUE;
    do
    {
        WCHAR *name, *sub;
        size_t len;

        if (!wcscmp( fd.cFileName, L"." ) || !wcscmp( fd.cFileName, L".." )) continue;
        len = (rel ? wcslen( rel ) + 1 : 0) + wcslen( fd.cFileName ) + 1;
        if (!(name = malloc( len * sizeof(WCHAR) ))) { ok = FALSE; break; }
        swprintf( name, len, L"%s%s%s", rel ? rel : L"", rel ? L"/" : L"", fd.cFileName );
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            ok = (sub = path_join( dir, fd.cFileName )) && list_dir( pkg, sub, name );
            free( sub );
            free( name );
            if (!ok) break;
            continue;
        }
        if (!(pkg->count % 256))
        {
            WCHAR **names = realloc( pkg->names, (pkg->count + 257) * sizeof(*names) );
            UINT64 *sizes = realloc( pkg->sizes, (pkg->count + 257) * sizeof(*sizes) );
            if (names) pkg->names = names;
            if (sizes) pkg->sizes = sizes;
            if (!names || !sizes) { free( name ); ok = FALSE; break; }
        }
        pkg->sizes[pkg->count] = ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        pkg->names[pkg->count++] = name;
        pkg->names[pkg->count] = NULL;
    } while (FindNextFileW( find, &fd ));
    FindClose( find );
    return ok;
}

static int __cdecl compare_member( const void *a, const void *b )
{
    const struct package *pkg = sort_pkg;
    unsigned int i = *(const unsigned int *)a, j = *(const unsigned int *)b;
    return wcscmp( pkg->names[i], pkg->names[j] );
}

/* in name order, so that what is picked among equals (the logo) does not depend on the directory's order */
static BOOL sort_members( struct package *pkg )
{
    unsigned int *order, i;
    WCHAR **names;
    UINT64 *sizes;

    if (pkg->count < 2) return TRUE;
    order = malloc( pkg->count * sizeof(*order) );
    names = malloc( (pkg->count + 1) * sizeof(*names) );
    sizes = malloc( pkg->count * sizeof(*sizes) );
    if (!order || !names || !sizes) { free( order ); free( names ); free( sizes ); return FALSE; }
    for (i = 0; i < pkg->count; i++) order[i] = i;
    sort_pkg = pkg;
    qsort( order, pkg->count, sizeof(*order), compare_member );
    for (i = 0; i < pkg->count; i++)
    {
        names[i] = pkg->names[order[i]];
        sizes[i] = pkg->sizes[order[i]];
    }
    names[pkg->count] = NULL;
    free( pkg->names );
    free( pkg->sizes );
    pkg->names = names;
    pkg->sizes = sizes;
    free( order );
    return TRUE;
}

/* an installed package's directory, as Windows unpacks it under WindowsApps */
static HRESULT open_dir( const WCHAR *path, struct package *pkg )
{
    WCHAR *manifest = path_join( path, L"AppxManifest.xml" );
    DWORD attrs = manifest ? GetFileAttributesW( manifest ) : INVALID_FILE_ATTRIBUTES;

    free( manifest );
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        ERR( "%s has no AppxManifest.xml\n", debugstr_w(path) );
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    }
    if (!(pkg->dir = strdupW( path ))) return E_OUTOFMEMORY;
    if (!list_dir( pkg, path, NULL ) || !sort_members( pkg )) return E_OUTOFMEMORY;
    TRACE( "%s: %u files\n", debugstr_w(path), pkg->count );
    return S_OK;
}

HRESULT package_open( const WCHAR *path, const struct appx_install_params *params, const WCHAR *temp_dir,
                      struct package **out )
{
    struct package *pkg;
    unsigned int i;
    DWORD attrs;
    HRESULT hr;

    if (!(pkg = calloc( 1, sizeof(*pkg) ))) return E_OUTOFMEMORY;
    if (!is_url( path ) && (attrs = GetFileAttributesW( path )) != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        if (FAILED(hr = open_dir( path, pkg ))) goto fail;
        *out = pkg;
        return S_OK;
    }
    if (FAILED(hr = source_open( path, params, &pkg->outer ))) goto fail;
    if (FAILED(hr = eappx_open( pkg->outer, params, &pkg->eappx ))) goto fail;
    if (hr == S_FALSE && FAILED(hr = xvd_open( pkg->outer, params, &pkg->xvd ))) goto fail;
    if (pkg->eappx || pkg->xvd)
    {
        *out = pkg;
        return S_OK;
    }
    if (FAILED(hr = zip_open( pkg->outer, &pkg->bundle )))
    {
        ERR( "%s is not an app package\n", debugstr_w(path) );
        hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
        goto fail;
    }
    if (FAILED(hr = open_bundle_package( pkg, temp_dir ))) goto fail;
    if (hr == S_OK)
    {
        if (FAILED(hr = zip_open( pkg->inner, &pkg->zip ))) goto fail;
    }
    else
    {
        pkg->zip = pkg->bundle;
        pkg->bundle = NULL;
    }
    if (!(pkg->names = calloc( zip_count( pkg->zip ) + 1, sizeof(*pkg->names) ))) { hr = E_OUTOFMEMORY; goto fail; }
    for (i = 0; i < zip_count( pkg->zip ); i++)
        if (!(pkg->names[i] = unescape_name( zip_entry( pkg->zip, i )->name ))) { hr = E_OUTOFMEMORY; goto fail; }
    *out = pkg;
    return S_OK;

fail:
    package_close( pkg );
    return hr;
}

struct xvd *package_xvd( const struct package *pkg )
{
    return pkg->xvd;
}

unsigned int package_count( const struct package *pkg )
{
    if (pkg->dir) return pkg->count;
    if (pkg->eappx) return eappx_count( pkg->eappx );
    return pkg->xvd ? xvd_count( pkg->xvd ) : zip_count( pkg->zip );
}

const WCHAR *package_name( const struct package *pkg, unsigned int index )
{
    if (pkg->dir) return pkg->names[index];
    if (pkg->eappx) return eappx_name( pkg->eappx, index );
    return pkg->xvd ? xvd_name( pkg->xvd, index ) : pkg->names[index];
}

UINT64 package_size( const struct package *pkg, unsigned int index )
{
    if (pkg->dir) return pkg->sizes[index];
    if (pkg->eappx) return eappx_size( pkg->eappx, index );
    return pkg->xvd ? xvd_size( pkg->xvd, index ) : zip_entry( pkg->zip, index )->size;
}

BOOL package_is_dir( const struct package *pkg, unsigned int index )
{
    const WCHAR *name = package_name( pkg, index );
    return *name && name[wcslen( name ) - 1] == '/';
}

int package_find( const struct package *pkg, const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < package_count( pkg ); i++)
        if (path_equal( package_name( pkg, i ), name )) return i;
    return -1;
}

static HRESULT extract_file( const struct package *pkg, unsigned int index,
                             BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context )
{
    WCHAR *path = path_join( pkg->dir, pkg->names[index] ), *p;
    static const ULONG chunk = 1 << 20;
    HRESULT hr = S_OK;
    HANDLE file;
    BYTE *buf;
    DWORD got;

    if (!path || !(buf = malloc( chunk ))) { free( path ); return E_OUTOFMEMORY; }
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    free( path );
    if (file == INVALID_HANDLE_VALUE) hr = HRESULT_FROM_WIN32( GetLastError() );
    else
    {
        while (SUCCEEDED(hr))
        {
            if (!ReadFile( file, buf, chunk, &got, NULL )) hr = HRESULT_FROM_WIN32( GetLastError() );
            else if (!got) break;
            else if (!write( context, buf, got )) hr = HRESULT_FROM_WIN32( ERROR_WRITE_FAULT );
        }
        CloseHandle( file );
    }
    free( buf );
    return hr;
}

HRESULT package_extract( const struct package *pkg, unsigned int index,
                         BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context )
{
    if (pkg->dir) return extract_file( pkg, index, write, context );
    if (pkg->eappx) return eappx_extract( pkg->eappx, index, write, context );
    if (pkg->xvd) return xvd_extract( pkg->xvd, index, write, context );
    return zip_extract( pkg->zip, zip_entry( pkg->zip, index ), write, context );
}

struct buffer { BYTE *data; ULONG size, capacity; };

static BOOL append( void *context, const BYTE *data, ULONG size )
{
    struct buffer *b = context;
    if (b->size + size > b->capacity) return FALSE;
    memcpy( b->data + b->size, data, size );
    b->size += size;
    return TRUE;
}

HRESULT package_read( const struct package *pkg, unsigned int index, BYTE **data, ULONG *size )
{
    struct buffer b = { 0 };
    HRESULT hr;

    if (!pkg->xvd && !pkg->eappx && !pkg->dir) return zip_read( pkg->zip, zip_entry( pkg->zip, index ), data, size );
    if (package_size( pkg, index ) > 64u << 20) return HRESULT_FROM_WIN32( ERROR_FILE_TOO_LARGE );
    b.capacity = package_size( pkg, index );
    if (!(b.data = malloc( b.capacity + 1 ))) return E_OUTOFMEMORY;
    if (FAILED(hr = package_extract( pkg, index, append, &b )))
    {
        free( b.data );
        return hr;
    }
    b.data[b.size] = 0;
    *data = b.data;
    if (size) *size = b.size;
    return S_OK;
}

void package_close( struct package *pkg )
{
    unsigned int i;

    if (!pkg) return;
    if (pkg->names)
    {
        for (i = 0; pkg->names[i]; i++) free( pkg->names[i] );
        free( pkg->names );
    }
    free( pkg->dir );
    free( pkg->sizes );
    xvd_close( pkg->xvd );
    eappx_close( pkg->eappx );
    if (pkg->zip != pkg->bundle) zip_close( pkg->zip );
    zip_close( pkg->bundle );
    source_close( pkg->inner );
    source_close( pkg->outer );
    if (pkg->temp)
    {
        DeleteFileW( pkg->temp );
        free( pkg->temp );
    }
    free( pkg );
}

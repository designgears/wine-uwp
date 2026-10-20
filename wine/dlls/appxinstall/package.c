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
    WCHAR *temp;                /* a bundle's package extracted here, deleted on close */
    WCHAR **names;              /* member names, percent-decoded, '/'-separated */
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

HRESULT package_open( const WCHAR *path, const struct appx_install_params *params, const WCHAR *temp_dir,
                      struct package **out )
{
    struct package *pkg;
    unsigned int i;
    HRESULT hr;

    if (!(pkg = calloc( 1, sizeof(*pkg) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = source_open( path, params, &pkg->outer ))) goto fail;
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

unsigned int package_count( const struct package *pkg )
{
    return zip_count( pkg->zip );
}

const WCHAR *package_name( const struct package *pkg, unsigned int index )
{
    return pkg->names[index];
}

UINT64 package_size( const struct package *pkg, unsigned int index )
{
    return zip_entry( pkg->zip, index )->size;
}

BOOL package_is_dir( const struct package *pkg, unsigned int index )
{
    const WCHAR *name = pkg->names[index];
    return *name && name[wcslen( name ) - 1] == '/';
}

int package_find( const struct package *pkg, const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < package_count( pkg ); i++)
        if (path_equal( pkg->names[i], name )) return i;
    return -1;
}

HRESULT package_extract( const struct package *pkg, unsigned int index,
                         BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context )
{
    return zip_extract( pkg->zip, zip_entry( pkg->zip, index ), write, context );
}

HRESULT package_read( const struct package *pkg, unsigned int index, BYTE **data, ULONG *size )
{
    return zip_read( pkg->zip, zip_entry( pkg->zip, index ), data, size );
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

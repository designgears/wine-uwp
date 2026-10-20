/*
 * appxinstall: reading .appx/.msix packages and bundles, which are zip files
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
#include <zlib.h>

#include "appxinstall_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxinstall);

#define CHUNK (8u << 20)

/* Wine's zlib has no default allocators */
static void *zalloc( void *opaque, unsigned int items, unsigned int size ) { return calloc( items, size ); }
static void zfree( void *opaque, void *ptr ) { free( ptr ); }

struct zip
{
    struct source *source;
    struct zip_entry *entries;
    unsigned int count;
};

static UINT16 le16( const BYTE *p ) { return p[0] | (p[1] << 8); }
static UINT32 le32( const BYTE *p ) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT64 le64( const BYTE *p ) { return le32( p ) | ((UINT64)le32( p + 4 ) << 32); }

/* the end of central directory record, and its zip64 counterpart when one is there */
static HRESULT find_directory( struct source *source, UINT64 *offset, UINT64 *size, UINT64 *count )
{
    UINT64 total = source_size( source ), start;
    ULONG len, i;
    BYTE *buf;
    HRESULT hr;

    len = (ULONG)min( total, 65557 );
    start = total - len;
    if (!(buf = malloc( len ))) return E_OUTOFMEMORY;
    if (FAILED(hr = source_read( source, start, buf, len ))) goto done;
    hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    for (i = len - 22 + 1; i-- > 0;)
    {
        if (le32( buf + i ) != 0x06054b50) continue;
        *count = le16( buf + i + 10 );
        *size = le32( buf + i + 12 );
        *offset = le32( buf + i + 16 );
        /* zip64: the locator sits just before the record */
        if (i >= 20 && le32( buf + i - 20 ) == 0x07064b50)
        {
            BYTE z64[56];
            if (FAILED(hr = source_read( source, le64( buf + i - 20 + 8 ), z64, sizeof(z64) ))) goto done;
            if (le32( z64 ) != 0x06064b50) { hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT ); goto done; }
            *count = le64( z64 + 32 );
            *size = le64( z64 + 40 );
            *offset = le64( z64 + 48 );
        }
        hr = S_OK;
        break;
    }
done:
    free( buf );
    return hr;
}

HRESULT zip_open( struct source *source, struct zip **out )
{
    UINT64 offset, size, count, pos = 0;
    struct zip *zip;
    BYTE *dir;
    HRESULT hr;

    if (FAILED(hr = find_directory( source, &offset, &size, &count ))) return hr;
    if (size > 512u << 20 || count > size / 46) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!(dir = malloc( size ))) return E_OUTOFMEMORY;
    if (FAILED(hr = source_read( source, offset, dir, size )))
    {
        free( dir );
        return hr;
    }
    if (!(zip = calloc( 1, sizeof(*zip) )) || !(zip->entries = calloc( count ? count : 1, sizeof(*zip->entries) )))
    {
        free( zip );
        free( dir );
        return E_OUTOFMEMORY;
    }
    zip->source = source;
    for (zip->count = 0; zip->count < count; zip->count++)
    {
        struct zip_entry *e = &zip->entries[zip->count];
        UINT16 name_len, extra_len, comment_len, flags;
        const BYTE *p = dir + pos, *x, *end;
        int n;

        if (pos + 46 > size || le32( p ) != 0x02014b50) { hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT ); break; }
        flags = le16( p + 8 );
        e->method = le16( p + 10 );
        e->crc = le32( p + 16 );
        e->compressed = le32( p + 20 );
        e->size = le32( p + 24 );
        name_len = le16( p + 28 );
        extra_len = le16( p + 30 );
        comment_len = le16( p + 32 );
        e->header = le32( p + 42 );
        if (pos + 46 + name_len + extra_len + comment_len > size) { hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT ); break; }
        /* zip64 extra field: the values set to 0xffffffff above, in this order */
        for (x = p + 46 + name_len, end = x + extra_len; x + 4 <= end; x += 4 + le16( x + 2 ))
        {
            const BYTE *v = x + 4, *vend = x + 4 + le16( x + 2 );
            if (le16( x ) != 0x0001 || vend > end) continue;
            if (e->size == 0xffffffff && v + 8 <= vend) { e->size = le64( v ); v += 8; }
            if (e->compressed == 0xffffffff && v + 8 <= vend) { e->compressed = le64( v ); v += 8; }
            if (e->header == 0xffffffff && v + 8 <= vend) { e->header = le64( v ); v += 8; }
        }
        n = MultiByteToWideChar( (flags & 0x800) ? CP_UTF8 : 437, 0, (const char *)p + 46, name_len, NULL, 0 );
        if (!(e->name = calloc( n + 1, sizeof(WCHAR) ))) { hr = E_OUTOFMEMORY; break; }
        MultiByteToWideChar( (flags & 0x800) ? CP_UTF8 : 437, 0, (const char *)p + 46, name_len, e->name, n );
        pos += 46 + name_len + extra_len + comment_len;
    }
    free( dir );
    if (FAILED(hr))
    {
        zip_close( zip );
        return hr;
    }
    *out = zip;
    return S_OK;
}

unsigned int zip_count( const struct zip *zip )
{
    return zip->count;
}

const struct zip_entry *zip_entry( const struct zip *zip, unsigned int index )
{
    return index < zip->count ? &zip->entries[index] : NULL;
}

const struct zip_entry *zip_find( const struct zip *zip, const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < zip->count; i++)
        if (!wcsicmp( zip->entries[i].name, name )) return &zip->entries[i];
    return NULL;
}

/* where an entry's data starts: after its local header, whose name and extra lengths may differ from the
 * central directory's */
HRESULT zip_data_offset( const struct zip *zip, const struct zip_entry *entry, UINT64 *offset )
{
    BYTE local[30];
    HRESULT hr;

    if (FAILED(hr = source_read( zip->source, entry->header, local, sizeof(local) ))) return hr;
    if (le32( local ) != 0x04034b50) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    *offset = entry->header + 30 + le16( local + 26 ) + le16( local + 28 );
    return S_OK;
}

/* Streams an entry's uncompressed bytes to write() in pieces of up to 8 MiB, checking the CRC at the end.
 * write() returns FALSE to stop (a cancel). */
HRESULT zip_extract( const struct zip *zip, const struct zip_entry *entry,
                     BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context )
{
    BYTE *in = NULL, *out = NULL;
    UINT64 offset, left, produced = 0;
    z_stream z = { 0 };
    ULONG crc = 0;
    HRESULT hr;
    int ret;

    if (entry->method != 0 && entry->method != 8)
    {
        ERR( "%s: compression method %u\n", debugstr_w(entry->name), entry->method );
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    }
    if (FAILED(hr = zip_data_offset( zip, entry, &offset ))) return hr;
    if (!(in = malloc( CHUNK ))) return E_OUTOFMEMORY;
    if (entry->method == 8)
    {
        if (!(out = malloc( CHUNK ))) { free( in ); return E_OUTOFMEMORY; }
        z.zalloc = zalloc;
        z.zfree = zfree;
        if ((ret = inflateInit2( &z, -15 )) != Z_OK)
        {
            ERR( "inflateInit2 failed: %d\n", ret );
            free( in );
            free( out );
            return E_FAIL;
        }
    }
    for (left = entry->compressed; left || entry->method == 8;)
    {
        ULONG n = (ULONG)min( left, CHUNK );

        if (n && FAILED(hr = source_read( zip->source, offset, in, n ))) break;
        offset += n;
        left -= n;
        if (entry->method == 0)
        {
            crc = crc32( crc, in, n );
            produced += n;
            if (!write( context, in, n )) { hr = HRESULT_FROM_WIN32( ERROR_CANCELLED ); break; }
            continue;
        }
        z.next_in = in;
        z.avail_in = n;
        do
        {
            z.next_out = out;
            z.avail_out = CHUNK;
            ret = inflate( &z, Z_NO_FLUSH );
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) { hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT ); break; }
            n = CHUNK - z.avail_out;
            crc = crc32( crc, out, n );
            produced += n;
            if (n && !write( context, out, n )) { hr = HRESULT_FROM_WIN32( ERROR_CANCELLED ); break; }
        } while (!z.avail_out && ret != Z_STREAM_END);
        if (FAILED(hr) || ret == Z_STREAM_END) break;
        if (!left) { hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT ); break; }   /* data ended before the stream */
    }
    if (entry->method == 8) inflateEnd( &z );
    free( in );
    free( out );
    if (FAILED(hr)) return hr;
    if (produced != entry->size || crc != entry->crc)
    {
        ERR( "%s: size %I64u/%I64u, crc %08x/%08x\n", debugstr_w(entry->name), produced, entry->size, (unsigned int)crc, entry->crc );
        return HRESULT_FROM_WIN32( ERROR_CRC );
    }
    return S_OK;
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

/* a small entry (a manifest) whole, with a terminating NUL; free *data with free() */
HRESULT zip_read( const struct zip *zip, const struct zip_entry *entry, BYTE **data, ULONG *size )
{
    struct buffer b = { 0 };
    HRESULT hr;

    if (entry->size > 64u << 20) return HRESULT_FROM_WIN32( ERROR_FILE_TOO_LARGE );
    if (!(b.data = malloc( entry->size + 1 ))) return E_OUTOFMEMORY;
    b.capacity = entry->size;
    if (FAILED(hr = zip_extract( zip, entry, append, &b )))
    {
        free( b.data );
        return hr;
    }
    b.data[b.size] = 0;
    *data = b.data;
    if (size) *size = b.size;
    return S_OK;
}

void zip_close( struct zip *zip )
{
    unsigned int i;

    if (!zip) return;
    for (i = 0; i < zip->count; i++) free( zip->entries[i].name );
    free( zip->entries );
    free( zip );
}

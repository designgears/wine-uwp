/*
 * appxinstall: encrypted packages (EAppx / EMsix), the Store's DRM container
 *
 * Layout (little-endian): a header (magic EXPH for a package, EXBH for a bundle; offsets of a footer table, the
 * signature and code integrity blobs, the block map's file id, the key ids, the package full name), the files back to
 * back, and the footer table: 40 bytes per file with its key index (0xffff = not encrypted), compression (1 = raw
 * DEFLATE), file id, offset, and uncompressed and stored lengths. The block map (AppxBlockMap.xml, never encrypted)
 * names the files; its Id attributes are the file ids in hex. A bundle's members are EXPH packages stored whole.
 *
 * An encrypted file is XTS-AES-128 in 512-byte data units, padded on disk to a whole unit, and DEFLATE-compressed
 * after encryption when compressed. The 32-byte content key is the data key then the tweak key; unit i of a file has
 * the tweak (T + i) as a 128-bit little-endian number, T being the four 8-byte quarters of SHA-256("\" + name, then
 * the lower-case family name "<name>_<publisher id>", both UTF-16LE) XORed together.
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

#define HEADER_SIZE 8192
#define FOOTER_SIZE 40
#define UNIT 512
#define CHUNK (4u << 20)
#define MAX_KEYS 64

/* Wine's zlib has no default allocators */
static void *zalloc( void *opaque, unsigned int items, unsigned int size ) { return calloc( items, size ); }
static void zfree( void *opaque, void *ptr ) { free( ptr ); }

struct footer
{
    UINT64 id, offset, size, stored;
    UINT16 key, compression;
};

struct eappx_member
{
    WCHAR *name;            /* '/'-separated */
    WCHAR *stored_name;     /* as the block map has it, for the tweak */
    UINT64 size;
    const struct footer *footer;
};

struct eappx
{
    struct source *source;  /* the package: the file, or the package's range of a bundle */
    struct source *range;   /* owned when source is a range of a bundle */
    BYTE key_ids[MAX_KEYS][16];
    unsigned int key_count;
    WCHAR *family;          /* lower-case */
    struct footer *footers;
    unsigned int footer_count;
    struct eappx_member *members;
    unsigned int member_count;
    struct xts *ciphers[MAX_KEYS];
    const char *keys;
};

static UINT16 le16( const BYTE *p ) { return p[0] | (p[1] << 8); }
static UINT32 le32( const BYTE *p ) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT64 le64( const BYTE *p ) { return le32( p ) | ((UINT64)le32( p + 4 ) << 32); }

static const struct footer *find_footer( const struct footer *footers, unsigned int count, UINT64 id )
{
    unsigned int i;
    for (i = 0; i < count; i++) if (footers[i].id == id) return &footers[i];
    return NULL;
}

static HRESULT read_footers( struct source *source, const BYTE *head, struct footer **out, unsigned int *count )
{
    UINT64 offset = le64( head + 14 ), size = le64( head + 22 );
    struct footer *footers;
    unsigned int i, n;
    BYTE *table;
    HRESULT hr;

    if (size > (64u << 20)) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    n = size / FOOTER_SIZE;
    if (!(table = malloc( size ? size : 1 ))) return E_OUTOFMEMORY;
    if (FAILED(hr = source_read( source, offset, table, size ))) { free( table ); return hr; }
    if (!(footers = calloc( n ? n : 1, sizeof(*footers) ))) { free( table ); return E_OUTOFMEMORY; }
    for (i = 0; i < n; i++)
    {
        const BYTE *f = table + i * FOOTER_SIZE;
        footers[i].key = le16( f + 4 );
        footers[i].compression = le16( f + 6 );
        footers[i].id = le64( f + 8 );
        footers[i].offset = le64( f + 16 );
        footers[i].size = le64( f + 24 );
        footers[i].stored = le64( f + 32 );
    }
    free( table );
    *out = footers;
    *count = n;
    return S_OK;
}

/* an unencrypted file whole (the block map, a bundle manifest), inflated if it is compressed, NUL-terminated */
static HRESULT read_plain( struct source *source, const struct footer *f, BYTE **data, ULONG *size )
{
    BYTE *raw, *out;
    z_stream z = { 0 };
    HRESULT hr;
    int ret;

    if (f->stored > (64u << 20) || f->size > (64u << 20)) return HRESULT_FROM_WIN32( ERROR_FILE_TOO_LARGE );
    if (!(raw = malloc( f->stored ? f->stored : 1 ))) return E_OUTOFMEMORY;
    if (FAILED(hr = source_read( source, f->offset, raw, f->stored ))) { free( raw ); return hr; }
    if (f->compression != 1)
    {
        if (!(out = realloc( raw, f->stored + 1 ))) { free( raw ); return E_OUTOFMEMORY; }
        out[f->stored] = 0;
        *data = out;
        *size = f->stored;
        return S_OK;
    }
    if (!(out = malloc( f->size + 1 ))) { free( raw ); return E_OUTOFMEMORY; }
    z.zalloc = zalloc;
    z.zfree = zfree;
    z.next_in = raw;
    z.avail_in = f->stored;
    z.next_out = out;
    z.avail_out = f->size;
    if (inflateInit2( &z, -15 ) != Z_OK) { free( raw ); free( out ); return E_FAIL; }
    ret = inflate( &z, Z_FINISH );
    inflateEnd( &z );
    free( raw );
    if (ret != Z_STREAM_END) { free( out ); return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT ); }
    out[z.total_out] = 0;
    *data = out;
    *size = z.total_out;
    return S_OK;
}

static HRESULT open_package( struct eappx *e, const struct appx_install_params *params )
{
    BYTE head[HEADER_SIZE];
    const struct xml_node *node = NULL;
    const struct footer *bm;
    struct xml_node *root;
    unsigned int i, p, len;
    WCHAR *full, *first, *last;
    BYTE *data;
    ULONG size;
    HRESULT hr;

    if (FAILED(hr = source_read( e->source, 0, head, min( sizeof(head), source_size( e->source ) ) ))) return hr;
    if (memcmp( head, "EXPH", 4 )) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    e->keys = params ? params->content_keys : NULL;
    e->key_count = le16( head + 86 );
    if (e->key_count > MAX_KEYS || 88 + 32 * e->key_count + 4 > sizeof(head)) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    for (i = 0; i < e->key_count; i++) memcpy( e->key_ids[i], head + 88 + 32 * i, 16 );
    p = 88 + 32 * e->key_count;
    len = le16( head + p + 2 );
    if (p + 4 + len > sizeof(head)) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!(full = calloc( len / 2 + 1, sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    memcpy( full, head + p + 4, len / 2 * sizeof(WCHAR) );
    /* the family name: the first and last parts of the full name */
    first = wcschr( full, '_' );
    last = wcsrchr( full, '_' );
    if (first && last)
    {
        memmove( first + 1, last + 1, (wcslen( last + 1 ) + 1) * sizeof(WCHAR) );
        _wcslwr( full );
        e->family = full;
    }
    else
    {
        free( full );
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    }

    if (FAILED(hr = read_footers( e->source, head, &e->footers, &e->footer_count ))) return hr;
    if (!(bm = find_footer( e->footers, e->footer_count, le64( head + 74 ) ))) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (FAILED(hr = read_plain( e->source, bm, &data, &size ))) return hr;
    hr = xml_parse( data, size, &root );
    free( data );
    if (FAILED(hr)) return hr;
    /* the block map's File elements, which are children of its root element */
    for (node = root->children; node && !node->name; node = node->next);
    for (node = node ? node->children : NULL; node && SUCCEEDED(hr); node = node->next)
    {
        const WCHAR *id = xml_attr( node, L"Id" ), *name = xml_attr( node, L"Name" ), *size_attr = xml_attr( node, L"Size" );
        struct eappx_member *m, *members;
        const struct footer *f;
        WCHAR *s;

        if (wcscmp( node->name, L"File" )) continue;
        if (!id || !name || !size_attr || !(f = find_footer( e->footers, e->footer_count, wcstoull( id, NULL, 16 ) )))
        {
            hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
            break;
        }
        if (!(members = realloc( e->members, (e->member_count + 1) * sizeof(*members) ))) { hr = E_OUTOFMEMORY; break; }
        e->members = members;
        m = &members[e->member_count];
        if (!(m->name = strdupW( name )) || !(m->stored_name = strdupW( name )))
        {
            free( m->name );
            hr = E_OUTOFMEMORY;
            break;
        }
        for (s = m->name; *s; s++) if (*s == '\\') *s = '/';
        m->size = wcstoull( size_attr, NULL, 10 );
        m->footer = f;
        e->member_count++;
    }
    xml_free( root );
    return hr;
}

/* S_FALSE if source is not an encrypted package or bundle; a bundle gives its x64 application package */
HRESULT eappx_open( struct source *source, const struct appx_install_params *params, struct eappx **out )
{
    BYTE head[HEADER_SIZE];
    struct eappx *e;
    HRESULT hr;

    if (source_size( source ) < 88) return S_FALSE;
    if (FAILED(hr = source_read( source, 0, head, min( sizeof(head), source_size( source ) ) ))) return hr;
    if (memcmp( head, "EXPH", 4 ) && memcmp( head, "EXBH", 4 )) return S_FALSE;
    if (!(e = calloc( 1, sizeof(*e) ))) return E_OUTOFMEMORY;
    e->source = source;

    if (!memcmp( head, "EXBH", 4 ))
    {
        const struct footer *bm, *f;
        const struct xml_node *node = NULL;
        struct footer *footers = NULL;
        struct xml_node *root = NULL;
        unsigned int count = 0;
        UINT64 offset = 0, manifest_id = 0;
        BOOL found = FALSE, have_manifest = FALSE;
        BYTE *data;
        ULONG size;

        if (FAILED(hr = read_footers( source, head, &footers, &count ))) goto fail;
        hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
        if ((bm = find_footer( footers, count, le64( head + 74 ) )) && SUCCEEDED(hr = read_plain( source, bm, &data, &size )))
        {
            hr = xml_parse( data, size, &root );
            free( data );
        }
        if (SUCCEEDED(hr))
        {
            for (node = root->children; node && !node->name; node = node->next);
            for (node = node ? node->children : NULL; node; node = node->next)
            {
                const WCHAR *name = xml_attr( node, L"Name" ), *id = xml_attr( node, L"Id" );
                if (wcscmp( node->name, L"File" ) || !name || !id) continue;
                if (wcsicmp( name, L"AppxMetadata\\AppxBundleManifest.xml" )) continue;
                manifest_id = wcstoull( id, NULL, 16 );
                have_manifest = TRUE;
                break;
            }
            xml_free( root );
            root = NULL;
            hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
            if (have_manifest && (f = find_footer( footers, count, manifest_id )) &&
                SUCCEEDED(hr = read_plain( source, f, &data, &size )))
            {
                hr = xml_parse( data, size, &root );
                free( data );
            }
        }
        free( footers );
        if (FAILED(hr)) goto fail;
        node = NULL;
        while ((node = xml_next( root, node, L"Package" )))
        {
            const WCHAR *type = xml_attr( node, L"Type" ), *arch = xml_attr( node, L"Architecture" );
            const WCHAR *off = xml_attr( node, L"Offset" );
            if (type && wcsicmp( type, L"application" )) continue;
            if (!arch || wcsicmp( arch, L"x64" ) || !off) continue;
            offset = wcstoull( off, NULL, 10 );
            found = TRUE;
            break;
        }
        xml_free( root );
        if (!found)
        {
            ERR( "this bundle has no x64 application package\n" );
            hr = HRESULT_FROM_WIN32( ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
            goto fail;
        }
        if (FAILED(hr = source_open_range( source, offset, source_size( source ) - offset, &e->range ))) goto fail;
        e->source = e->range;
    }
    if (FAILED(hr = open_package( e, params ))) goto fail;
    TRACE( "%u files, family %s\n", e->member_count, debugstr_w(e->family) );
    *out = e;
    return S_OK;

fail:
    eappx_close( e );
    return hr;
}

unsigned int eappx_count( const struct eappx *e ) { return e->member_count; }
const WCHAR *eappx_name( const struct eappx *e, unsigned int index ) { return e->members[index].name; }
UINT64 eappx_size( const struct eappx *e, unsigned int index ) { return e->members[index].size; }

static HRESULT get_cipher( struct eappx *e, UINT16 slot, struct xts **xts )
{
    BYTE key[32];
    HRESULT hr;

    if (slot >= e->key_count) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!e->ciphers[slot])
    {
        if (!find_content_key( e->keys, e->key_ids[slot], key ))
        {
            ERR( "this package is encrypted and its content key was not given; it comes with the Store licence\n" );
            return NTE_NO_KEY;
        }
        hr = xts_create( key, &e->ciphers[slot] );
        SecureZeroMemory( key, sizeof(key) );
        if (FAILED(hr)) return hr;
    }
    *xts = e->ciphers[slot];
    return S_OK;
}

/* T for a file: the XOR of SHA-256("\" + name, family)'s four quarters */
static HRESULT file_tweak( const struct eappx *e, const WCHAR *name, UINT64 *tweak )
{
    size_t name_len, family_len = wcslen( e->family );
    BYTE digest[32];
    WCHAR *buf;
    HRESULT hr;

    while (*name == '\\') name++;
    name_len = wcslen( name );
    if (!(buf = malloc( (1 + name_len + family_len) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    buf[0] = '\\';
    memcpy( buf + 1, name, name_len * sizeof(WCHAR) );
    memcpy( buf + 1 + name_len, e->family, family_len * sizeof(WCHAR) );
    hr = sha256( buf, (1 + name_len + family_len) * sizeof(WCHAR), digest );
    free( buf );
    if (FAILED(hr)) return hr;
    *tweak = le64( digest ) ^ le64( digest + 8 ) ^ le64( digest + 16 ) ^ le64( digest + 24 );
    return S_OK;
}

struct decrypt_state
{
    struct xts *xts;
    UINT64 tweak, unit, left;
    BYTE pending[UNIT];
    ULONG pending_size;
    BOOL (*write)( void *context, const BYTE *data, ULONG size );
    void *context;
};

/* hands size bytes to the writer, cut to the file's size */
static BOOL emit( struct decrypt_state *s, const BYTE *data, ULONG size )
{
    ULONG n = (ULONG)min( (UINT64)size, s->left );
    if (!n) return TRUE;
    s->left -= n;
    return s->write( s->context, data, n );
}

static HRESULT decrypt_units( struct decrypt_state *s, BYTE *data, ULONG size )
{
    ULONG off;
    HRESULT hr;

    for (off = 0; off < size; off += UNIT)
    {
        BYTE tweak[16];
        UINT64 lo = s->tweak + s->unit, hi = lo < s->tweak;   /* (T + unit) as 128 bits */
        unsigned int i;

        for (i = 0; i < 8; i++)
        {
            tweak[i] = lo >> (8 * i);
            tweak[8 + i] = hi >> (8 * i);
        }
        if (FAILED(hr = xts_decrypt_unit( s->xts, data + off, UNIT, tweak ))) return hr;
        s->unit++;
    }
    return S_OK;
}

/* feeds stored (or inflated) bytes through decryption, a whole unit at a time */
static HRESULT feed( struct decrypt_state *s, BYTE *data, ULONG size )
{
    HRESULT hr;
    ULONG whole;

    if (!s->xts) return emit( s, data, size ) ? S_OK : HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (s->pending_size)
    {
        ULONG n = min( size, UNIT - s->pending_size );
        memcpy( s->pending + s->pending_size, data, n );
        s->pending_size += n;
        data += n;
        size -= n;
        if (s->pending_size < UNIT) return S_OK;
        if (FAILED(hr = decrypt_units( s, s->pending, UNIT ))) return hr;
        if (!emit( s, s->pending, UNIT )) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        s->pending_size = 0;
    }
    whole = size - size % UNIT;
    if (whole)
    {
        if (FAILED(hr = decrypt_units( s, data, whole ))) return hr;
        if (!emit( s, data, whole )) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    }
    memcpy( s->pending, data + whole, size - whole );
    s->pending_size = size - whole;
    return S_OK;
}

static HRESULT finish( struct decrypt_state *s )
{
    HRESULT hr;

    if (!s->xts || !s->pending_size) return S_OK;
    memset( s->pending + s->pending_size, 0, UNIT - s->pending_size );
    if (FAILED(hr = decrypt_units( s, s->pending, UNIT ))) return hr;
    s->pending_size = 0;
    return emit( s, s->pending, UNIT ) ? S_OK : HRESULT_FROM_WIN32( ERROR_CANCELLED );
}

HRESULT eappx_extract( struct eappx *e, unsigned int index,
                       BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context )
{
    const struct eappx_member *m = &e->members[index];
    const struct footer *f = m->footer;
    struct decrypt_state s = { 0 };
    UINT64 offset = f->offset, left = f->stored;
    BYTE *in = NULL, *out = NULL;
    z_stream z = { 0 };
    BOOL inflating = f->compression == 1;
    int ret = Z_OK;
    HRESULT hr = S_OK;

    s.left = m->size;
    s.write = write;
    s.context = context;
    if (f->key != 0xffff)
    {
        if (FAILED(hr = get_cipher( e, f->key, &s.xts ))) return hr;
        if (FAILED(hr = file_tweak( e, m->stored_name, &s.tweak ))) return hr;
        if (!inflating) left = (left + UNIT - 1) / UNIT * UNIT;   /* padded to a whole unit */
    }
    if (!(in = malloc( CHUNK ))) return E_OUTOFMEMORY;
    if (inflating)
    {
        if (!(out = malloc( CHUNK ))) { free( in ); return E_OUTOFMEMORY; }
        z.zalloc = zalloc;
        z.zfree = zfree;
        if (inflateInit2( &z, -15 ) != Z_OK) { free( in ); free( out ); return E_FAIL; }
    }
    while (left && SUCCEEDED(hr) && ret != Z_STREAM_END)
    {
        ULONG n = (ULONG)min( left, CHUNK );

        if (FAILED(hr = source_read( e->source, offset, in, n ))) break;
        offset += n;
        left -= n;
        if (!inflating)
        {
            hr = feed( &s, in, n );
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
            if (CHUNK - z.avail_out) hr = feed( &s, out, CHUNK - z.avail_out );
        } while (SUCCEEDED(hr) && (z.avail_in || !z.avail_out) && ret != Z_STREAM_END);
    }
    if (SUCCEEDED(hr)) hr = finish( &s );
    if (inflating) inflateEnd( &z );
    free( in );
    free( out );
    if (SUCCEEDED(hr) && s.left)
    {
        ERR( "%s: %I64u bytes short\n", debugstr_w(m->name), s.left );
        hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    }
    return hr;
}

void eappx_close( struct eappx *e )
{
    unsigned int i;

    if (!e) return;
    for (i = 0; i < MAX_KEYS; i++) xts_destroy( e->ciphers[i] );
    for (i = 0; i < e->member_count; i++)
    {
        free( e->members[i].name );
        free( e->members[i].stored_name );
    }
    free( e->members );
    free( e->footers );
    free( e->family );
    source_close( e->range );
    free( e );
}

/*
 * appxinstall: GDK packages (.msixvc), which are XVD containers
 *
 * 4096-byte pages. At 0x200 "msft-xvd", then (little-endian) the drive size at 0x218, the VDUID at 0x220, the
 * lengths of the embedded XVD, user data, XVC info and dynamic header at 0x288..0x297 and the mutable data page count
 * at 0x470. The file is laid out as header (3 pages), embedded XVD, mutable data, hash tree, user data, XVC info,
 * dynamic header, drive, each rounded up to whole pages. The hash tree has levels L3 L2 L1 L0 in that order; L0 has an
 * entry per page from the user data on (170 entries of 24 bytes a page, the last 4 bytes of an entry the page's XTS
 * data unit), each level above one entry per page of the level below.
 *
 * The XVC info holds the content id, a table of 192 key ids and, from 0xDA8, 0x80-byte region headers: id, key index
 * (0xffff = plain), first segment, byte offset and length. The user data has a table of named files; its
 * SegmentMetadata.bin (" PFX") lists the package's files in order, each filling whole pages one after the other from
 * the start of the region whose first segment it is. An encrypted page is XTS-AES-128 with the content key's second
 * half as the data key and its first as the tweak key, tweak = data unit (u32), region id (u32), first 8 bytes of
 * the VDUID.
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

#define PAGE 4096
#define KEY_SLOTS 192
#define PAGES_PER_READ 2048     /* 8 MiB: the HTTP source reads that much straight through, without its cache */
#define L0_WINDOW (8u << 20)

struct xvd_member
{
    WCHAR *name;            /* '/'-separated */
    UINT64 size, page;
    UINT32 region;
};

struct xvd
{
    struct source *source;
    BYTE vduid[8];
    BYTE content_id[16];
    BYTE key_ids[KEY_SLOTS][16];
    UINT64 l0, l0_pages, user;
    struct xvd_region *regions;
    unsigned int region_count;
    struct xvd_member *members;
    unsigned int member_count;
    struct xts *ciphers[KEY_SLOTS];
    const char *keys;
    /* the L0 hash level, a window at a time: fetching it beside each run of data pages made a download from the
     * CDN alternate between two parts of the file */
    BYTE *l0_data;
    UINT64 l0_start, l0_size;
};

static UINT16 le16( const BYTE *p ) { return p[0] | (p[1] << 8); }
static UINT32 le32( const BYTE *p ) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT64 le64( const BYTE *p ) { return le32( p ) | ((UINT64)le32( p + 4 ) << 32); }
static UINT64 pages( UINT64 n ) { return (n + PAGE - 1) / PAGE; }

/* a NUL-terminated (or full) UTF-16LE field */
static WCHAR *utf16_field( const BYTE *p, unsigned int bytes )
{
    unsigned int n = 0;
    WCHAR *r;

    while (n < bytes / 2 && le16( p + 2 * n )) n++;
    if (!(r = malloc( (n + 1) * sizeof(WCHAR) ))) return NULL;
    memcpy( r, p, n * sizeof(WCHAR) );
    r[n] = 0;
    return r;
}

static void add_string( WCHAR ***list, unsigned int *count, const WCHAR *s, unsigned int len )
{
    WCHAR **l = realloc( *list, (*count + 1) * sizeof(*l) ), *v;
    if (!l) return;
    *list = l;
    if (!(v = malloc( (len + 1) * sizeof(WCHAR) ))) return;
    memcpy( v, s, len * sizeof(WCHAR) );
    v[len] = 0;
    l[(*count)++] = v;
}

/* a ;-separated option list, lower-cased; NULL when the option was not given */
static WCHAR **option_list( const WCHAR *value, unsigned int *count )
{
    WCHAR **list = NULL;
    const WCHAR *p = value;

    *count = 0;
    if (!value) return NULL;
    if (!(list = calloc( 1, sizeof(*list) ))) return NULL;
    while (*p)
    {
        const WCHAR *end = p + wcscspn( p, L";" ), *s = p, *e = end;
        while (s < e && iswspace( *s )) s++;
        while (e > s && iswspace( e[-1] )) e--;
        if (e > s)
        {
            unsigned int before = *count;
            add_string( &list, count, s, e - s );
            if (*count > before) _wcslwr( list[before] );
        }
        p = *end ? end + 1 : end;
    }
    return list;
}

static void free_list( WCHAR **list, unsigned int count )
{
    while (count) free( list[--count] );
    free( list );
}

static BOOL in_list( WCHAR **list, unsigned int count, const WCHAR *s, unsigned int len )
{
    unsigned int i;
    for (i = 0; i < count; i++)
        if (wcslen( list[i] ) == len && !wcsnicmp( list[i], s, len )) return TRUE;
    return FALSE;
}

/* whether the chosen features (--tags) and the user's languages need a region; a region with no tags or no
 * languages is always needed */
static BOOL region_wanted( const struct xvd_region *r, const struct appx_install_params *params )
{
    unsigned int i, ntags, nlangs;
    WCHAR **tags = option_list( params ? params->tags : NULL, &ntags );
    WCHAR **langs = option_list( params ? params->languages : NULL, &nlangs );
    BOOL ok = TRUE;

    if (tags && r->tag_count)
    {
        /* a spec "a#b" needs both */
        ok = FALSE;
        for (i = 0; i < r->tag_count && !ok; i++)
        {
            const WCHAR *p = r->tags[i];
            ok = TRUE;
            while (ok)
            {
                const WCHAR *end = p + wcscspn( p, L"#" );
                ok = in_list( tags, ntags, p, end - p );
                if (!*end) break;
                p = end + 1;
            }
        }
    }
    if (ok && langs && r->language_count)
    {
        ok = FALSE;
        for (i = 0; i < r->language_count && !ok; i++)
        {
            const WCHAR *l = r->languages[i];
            unsigned int base = wcscspn( l, L"-" ), j;
            ok = in_list( langs, nlangs, l, wcslen( l ) );
            for (j = 0; j < nlangs && !ok; j++)
                ok = wcscspn( langs[j], L"-" ) == base && !wcsnicmp( langs[j], l, base );
        }
    }
    free_list( tags, ntags );
    free_list( langs, nlangs );
    return ok;
}

static BOOL unsafe_segment( const WCHAR *name )
{
    const WCHAR *p = name;

    if (*name == '/' || *name == '\\') return TRUE;
    while (*p)
    {
        const WCHAR *end = p + wcscspn( p, L"/\\" );
        if (end - p == 2 && p[0] == '.' && p[1] == '.') return TRUE;
        p = *end ? end + 1 : end;
    }
    return FALSE;
}

static HRESULT read_alloc( struct source *source, UINT64 offset, UINT64 size, BYTE **out )
{
    HRESULT hr;

    if (size > (1u << 30)) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!(*out = malloc( size ? size : 1 ))) return E_OUTOFMEMORY;
    if (FAILED(hr = source_read( source, offset, *out, size )))
    {
        free( *out );
        *out = NULL;
    }
    return hr;
}

/* S_FALSE if source is not an XVD */
HRESULT xvd_open( struct source *source, const struct appx_install_params *params, struct xvd **out )
{
    UINT64 drive, levels[4], n, tree, xvc_at, xvc_len;
    UINT32 emb, user, xvc, dyn, version, count, updates, nspecs, table, files, hlen, segs;
    BYTE head[PAGE], *info = NULL, *ud = NULL;
    const BYTE *seg = NULL, *t;
    UINT32 seg_size = 0;
    struct seg_entry { WCHAR *name; UINT64 size; } *entries = NULL;
    struct xvd *xvd;
    unsigned int i, k;
    HRESULT hr;

    if (source_size( source ) < PAGE * 3) return S_FALSE;
    if (FAILED(hr = source_read( source, 0, head, sizeof(head) ))) return hr;
    if (memcmp( head + 0x200, "msft-xvd", 8 )) return S_FALSE;

    if (!(xvd = calloc( 1, sizeof(*xvd) ))) return E_OUTOFMEMORY;
    xvd->source = source;
    xvd->keys = params ? params->content_keys : NULL;
    drive = le64( head + 0x218 );
    memcpy( xvd->vduid, head + 0x220, 8 );
    emb = le32( head + 0x288 );
    user = le32( head + 0x28c );
    xvc = le32( head + 0x290 );
    dyn = le32( head + 0x294 );
    n = pages( user ) + pages( xvc ) + pages( dyn ) + pages( drive );
    levels[0] = max( 1, (n + 169) / 170 );
    for (i = 1; i < 4; i++) levels[i] = ((levels[i - 1] == 1 ? 0 : levels[i - 1]) + 169) / 170;
    tree = 3 + pages( emb ) + head[0x470];     /* one byte: the bytes after it are other fields (257 as a u32) */
    xvd->l0 = tree + levels[1] + levels[2] + levels[3];     /* L3, L2 and L1 come first */
    xvd->l0_pages = levels[0];
    xvd->user = xvd->l0 + levels[0];
    xvc_at = xvd->user + pages( user );
    xvc_len = pages( xvc ) * PAGE;

    hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (xvc_len < 0xDA8) goto fail;
    if (FAILED(hr = read_alloc( source, xvc_at * PAGE, xvc_len, &info ))) goto fail;
    memcpy( xvd->content_id, info, 16 );
    memcpy( xvd->key_ids, info + 16, sizeof(xvd->key_ids) );
    version = le32( info + 0xD10 );
    count = le32( info + 0xD14 );
    hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (version < 1)
    {
        ERR( "this GDK package has no region table (XVC info version 0)\n" );
        goto fail;
    }
    updates = le32( info + 0xD3C );
    nspecs = le32( info + 0xD50 );
    if (0xDA8 + 0x80ull * count + 12ull * updates + 392ull * nspecs > xvc_len) goto fail;
    if (!(xvd->regions = calloc( count ? count : 1, sizeof(*xvd->regions) ))) { hr = E_OUTOFMEMORY; goto fail; }
    for (i = 0; i < count; i++)
    {
        const BYTE *r = info + 0xDA8 + 0x80 * i;
        struct xvd_region *region = &xvd->regions[xvd->region_count++];
        region->id = le32( r );
        region->key = le16( r + 4 );
        region->first_segment = le32( r + 12 );
        region->first_page = le64( r + 0x50 ) / PAGE;
        region->pages = le64( r + 0x58 ) / PAGE;
    }
    /* region specifiers, after 12 bytes per update segment: "Tags" (the install features a region belongs to,
     * "a#b" when it needs both) and "Languages" */
    for (i = 0; i < nspecs; i++)
    {
        const BYTE *s = info + 0xDA8 + 0x80 * count + 12 * updates + 392 * i;
        WCHAR *key = utf16_field( s + 8, 128 ), *value = utf16_field( s + 136, 256 );
        struct xvd_region *region = NULL;

        for (k = 0; k < xvd->region_count; k++) if (xvd->regions[k].id == le32( s )) region = &xvd->regions[k];
        if (region && key && value)
        {
            if (!wcscmp( key, L"Tags" )) add_string( &region->tags, &region->tag_count, value, wcslen( value ) );
            else if (!wcscmp( key, L"Languages" ))
            {
                const WCHAR *p = value;
                while (*p)
                {
                    const WCHAR *end = p + wcscspn( p, L";" );
                    if (end > p) add_string( &region->languages, &region->language_count, p, end - p );
                    p = *end ? end + 1 : end;
                }
            }
        }
        free( key );
        free( value );
    }

    /* the user data's file table, and SegmentMetadata.bin in it */
    if (FAILED(hr = read_alloc( source, xvd->user * PAGE, user, &ud ))) goto fail;
    hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (user < 4 || (table = le32( ud )) > user - 528) goto fail;
    t = ud + table;
    files = le32( t + 524 );
    if (table + 528ull * (files + 1) > user) goto fail;
    for (i = 0; i < files; i++)
    {
        const BYTE *e = t + 528 + 528 * i;
        WCHAR *name = utf16_field( e, 520 );
        UINT32 size = le32( e + 520 ), off = le32( e + 524 );

        if (name && !wcsicmp( name, L"SegmentMetadata.bin" ) && 16ull + off + size <= user)
        {
            seg = ud + 16 + off;
            seg_size = size;
        }
        free( name );
    }
    if (!seg || seg_size < 20 || memcmp( seg, " PFX", 4 ))
    {
        ERR( "this GDK package has no segment table; only packages with one are supported\n" );
        goto fail;
    }
    hlen = le32( seg + 12 );
    segs = le32( seg + 16 );
    if (hlen + 16ull * segs > seg_size) goto fail;
    if (!(entries = calloc( segs ? segs : 1, sizeof(*entries) ))) { hr = E_OUTOFMEMORY; goto fail; }
    for (i = 0; i < segs; i++)
    {
        const BYTE *e = seg + hlen + 16 * i;
        UINT32 off = hlen + 16 * segs + le32( e + 4 ), len = le16( e + 2 );
        WCHAR *p;

        if (off + 2ull * len > seg_size) goto fail;
        if (!(entries[i].name = malloc( (len + 1) * sizeof(WCHAR) ))) { hr = E_OUTOFMEMORY; goto fail; }
        memcpy( entries[i].name, seg + off, len * sizeof(WCHAR) );
        entries[i].name[len] = 0;
        for (p = entries[i].name; *p; p++) if (*p == '\\') *p = '/';
        entries[i].size = le64( e + 8 );
    }

    /* each content region takes its segments in order, page after page from its start; a region the chosen features
     * or languages do not need is left out */
    for (i = 0; i < xvd->region_count; i++)
    {
        const struct xvd_region *r = &xvd->regions[i];
        UINT64 page = r->first_page;

        if (r->id >= 0x40000000 && r->id != 0x40000003) continue;
        if (!region_wanted( r, params )) continue;
        for (k = r->first_segment; k < segs && page < r->first_page + r->pages; k++)
        {
            struct xvd_member *m, *members;

            if (unsafe_segment( entries[k].name ))
            {
                ERR( "refusing unsafe path in package: %s\n", debugstr_w(entries[k].name) );
                hr = HRESULT_FROM_WIN32( ERROR_BAD_PATHNAME );
                goto fail;
            }
            if (!(members = realloc( xvd->members, (xvd->member_count + 1) * sizeof(*members) )))
            {
                hr = E_OUTOFMEMORY;
                goto fail;
            }
            xvd->members = members;
            m = &members[xvd->member_count];
            if (!(m->name = strdupW( entries[k].name ))) { hr = E_OUTOFMEMORY; goto fail; }
            xvd->member_count++;
            m->size = entries[k].size;
            m->region = r->id;
            m->page = page;
            page += max( 1, pages( m->size ) );
        }
    }
    hr = S_OK;

fail:
    if (entries) for (i = 0; i < segs; i++) free( entries[i].name );
    free( entries );
    free( info );
    free( ud );
    if (FAILED(hr))
    {
        xvd_close( xvd );
        return hr;
    }
    TRACE( "%u regions, %u files\n", xvd->region_count, xvd->member_count );
    *out = xvd;
    return S_OK;
}

unsigned int xvd_count( const struct xvd *xvd ) { return xvd->member_count; }
const WCHAR *xvd_name( const struct xvd *xvd, unsigned int index ) { return xvd->members[index].name; }
UINT64 xvd_size( const struct xvd *xvd, unsigned int index ) { return xvd->members[index].size; }
UINT32 xvd_member_region( const struct xvd *xvd, unsigned int index ) { return xvd->members[index].region; }
const BYTE *xvd_content_id( const struct xvd *xvd ) { return xvd->content_id; }
unsigned int xvd_region_count( const struct xvd *xvd ) { return xvd->region_count; }
const struct xvd_region *xvd_region( const struct xvd *xvd, unsigned int index ) { return &xvd->regions[index]; }

static const struct xvd_region *find_region( const struct xvd *xvd, UINT32 id )
{
    unsigned int i;
    for (i = 0; i < xvd->region_count; i++) if (xvd->regions[i].id == id) return &xvd->regions[i];
    return NULL;
}

static HRESULT get_cipher( struct xvd *xvd, UINT16 slot, struct xts **xts )
{
    BYTE key[32], swapped[32];
    HRESULT hr;

    if (!xvd->ciphers[slot])
    {
        if (!find_content_key( xvd->keys, xvd->key_ids[slot], key ))
        {
            const BYTE *k = xvd->key_ids[slot];
            ERR( "this package is encrypted and its content key (%08lx%08lx%08lx%08lx) was not given; it comes with the "
                 "Store licence\n", RtlUlongByteSwap( *(const UINT32 *)k ), RtlUlongByteSwap( *(const UINT32 *)(k + 4) ),
                 RtlUlongByteSwap( *(const UINT32 *)(k + 8) ), RtlUlongByteSwap( *(const UINT32 *)(k + 12) ) );
            return NTE_NO_KEY;
        }
        /* the data key is the second half */
        memcpy( swapped, key + 16, 16 );
        memcpy( swapped + 16, key, 16 );
        hr = xts_create( swapped, &xvd->ciphers[slot] );
        SecureZeroMemory( key, sizeof(key) );
        SecureZeroMemory( swapped, sizeof(swapped) );
        if (FAILED(hr)) return hr;
    }
    *xts = xvd->ciphers[slot];
    return S_OK;
}

/* the 24-byte L0 entry of data page index (counted from the user data) */
static HRESULT l0_entry( struct xvd *xvd, UINT64 index, const BYTE **entry )
{
    UINT64 offset = (index / 170) * PAGE + (index % 170) * 24;
    HRESULT hr;

    if (offset + 24 > xvd->l0_pages * PAGE) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!xvd->l0_data || offset < xvd->l0_start || offset + 24 > xvd->l0_start + xvd->l0_size)
    {
        UINT64 start = offset - offset % PAGE;
        ULONG size = min( L0_WINDOW, xvd->l0_pages * PAGE - start );

        if (!xvd->l0_data && !(xvd->l0_data = malloc( L0_WINDOW ))) return E_OUTOFMEMORY;
        xvd->l0_size = 0;
        if (FAILED(hr = source_read( xvd->source, xvd->l0 * PAGE + start, xvd->l0_data, size ))) return hr;
        xvd->l0_start = start;
        xvd->l0_size = size;
    }
    *entry = xvd->l0_data + (offset - xvd->l0_start);
    return S_OK;
}

static HRESULT read_pages( struct xvd *xvd, const struct xvd_region *region, UINT64 page, ULONG count, BYTE *out )
{
    struct xts *xts;
    ULONG i;
    HRESULT hr;

    if (FAILED(hr = source_read( xvd->source, page * PAGE, out, count * PAGE ))) return hr;
    if (region->key >= KEY_SLOTS) return S_OK;
    if (FAILED(hr = get_cipher( xvd, region->key, &xts ))) return hr;
    for (i = 0; i < count; i++)
    {
        const BYTE *entry;
        BYTE tweak[16];

        if (FAILED(hr = l0_entry( xvd, page + i - xvd->user, &entry ))) return hr;
        memcpy( tweak, entry + 20, 4 );
        tweak[4] = region->id;
        tweak[5] = region->id >> 8;
        tweak[6] = region->id >> 16;
        tweak[7] = region->id >> 24;
        memcpy( tweak + 8, xvd->vduid, 8 );
        if (FAILED(hr = xts_decrypt_unit( xts, out + i * PAGE, PAGE, tweak ))) return hr;
    }
    return S_OK;
}

HRESULT xvd_extract( struct xvd *xvd, unsigned int index,
                     BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context )
{
    const struct xvd_member *m = &xvd->members[index];
    const struct xvd_region *region = find_region( xvd, m->region );
    UINT64 page = m->page, left = m->size, pages_left = max( 1, pages( m->size ) );
    BYTE *buf;
    HRESULT hr = S_OK;

    if (!region) return E_UNEXPECTED;
    if (!(buf = malloc( PAGES_PER_READ * PAGE ))) return E_OUTOFMEMORY;
    while (left)
    {
        ULONG n = min( PAGES_PER_READ, pages_left ), bytes;

        if (FAILED(hr = read_pages( xvd, region, page, n, buf ))) break;
        page += n;
        pages_left -= n;
        bytes = (ULONG)min( left, (UINT64)n * PAGE );
        left -= bytes;
        if (!write( context, buf, bytes )) { hr = HRESULT_FROM_WIN32( ERROR_CANCELLED ); break; }
    }
    free( buf );
    return hr;
}

void xvd_close( struct xvd *xvd )
{
    unsigned int i;

    if (!xvd) return;
    for (i = 0; i < KEY_SLOTS; i++) xts_destroy( xvd->ciphers[i] );
    for (i = 0; i < xvd->member_count; i++) free( xvd->members[i].name );
    for (i = 0; i < xvd->region_count; i++)
    {
        free_list( xvd->regions[i].tags, xvd->regions[i].tag_count );
        free_list( xvd->regions[i].languages, xvd->regions[i].language_count );
    }
    free( xvd->members );
    free( xvd->regions );
    free( xvd->l0_data );
    free( xvd );
}

/*
 * Microsoft.GamePlatform.Services: GDK packages' install features
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
 * A GDK package (.msixvc) read where it lies on the CDN: its install features and recipes, and which of its
 * regions a choice of features and languages needs.
 *
 * Gaming Services answers the install dialog before anything is downloaded, so it reads the package header
 * remotely. What it needs is all near the start of the file:
 *   - the XVD header (page 0) gives the layout: header, embedded XVD, mutable data, hash tree, user data, XVC
 *     info, each a whole number of 4096-byte pages;
 *   - the XVC info holds the region table (0x80 bytes a region from 0xDA8), then 12 bytes per update segment,
 *     then the region specifiers: a region id, then a key and a value in UTF-16 (64 and 128 characters). Key
 *     "Tags" names the features a region belongs to, "a#b" meaning a region needed only when both are chosen
 *     (Halo Infinite's game#hd1 is the base game's high-resolution textures); key "Languages" lists its
 *     languages;
 *   - the user data holds named files: Metadata.json with "InstallFeatures" and "InstallRecipes",
 *     Resources.pri with the features' display names (ms-resource:), and their images.
 * A region with no Tags specifier is always installed, and so is one whose languages are none of the user's
 * only if it has no Languages specifier.
 */

#include "private.h"
#include "winhttp.h"

WINE_DEFAULT_DEBUG_CHANNEL(gameplatform);

#define XVD_PAGE 4096

struct blob { unsigned char *data; size_t size; };

struct gdk_region { UINT32 id; UINT16 key; UINT64 offset, length; struct strlist tags, languages; };
struct gdk_feature { char *id, *name, *image, *tags; BOOL hidden; };
struct gdk_recipe { char *id, *included, *excluded; struct strlist store_ids; };

struct gdk_package
{
    WCHAR store_id[64];
    BOOL ok;
    char *family;
    struct gdk_region *regions;
    unsigned region_count;
    struct gdk_feature *features;
    unsigned feature_count;
    struct gdk_recipe *recipes;
    unsigned recipe_count;
};

static BOOL http_range( const char *url, UINT64 offset, UINT64 length, struct blob *out )
{
    WCHAR host[256], path[4096], extra[4096], range[96], *wurl;
    HINTERNET session, connection = NULL, request = NULL;
    URL_COMPONENTS uc = {sizeof(uc)};
    DWORD got = 0;
    int len;

    out->data = NULL;
    out->size = 0;
    if (!length) return FALSE;
    len = MultiByteToWideChar( CP_UTF8, 0, url, -1, NULL, 0 );
    if (!(wurl = malloc( len * sizeof(WCHAR) ))) return FALSE;
    MultiByteToWideChar( CP_UTF8, 0, url, -1, wurl, len );
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAY_SIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAY_SIZE(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAY_SIZE(extra);
    if (!WinHttpCrackUrl( wurl, 0, 0, &uc ) || uc.dwUrlPathLength + uc.dwExtraInfoLength >= ARRAY_SIZE(path))
    {
        free( wurl );
        return FALSE;
    }
    free( wurl );
    host[uc.dwHostNameLength] = 0;
    memcpy( path + uc.dwUrlPathLength, extra, uc.dwExtraInfoLength * sizeof(WCHAR) );
    path[uc.dwUrlPathLength + uc.dwExtraInfoLength] = 0;
    if (!(out->data = malloc( length ))) return FALSE;

    session = WinHttpOpen( L"Microsoft-Delivery-Optimization/10.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    swprintf( range, ARRAY_SIZE(range), L"Range: bytes=%I64u-%I64u\r\n", offset, offset + length - 1 );
    if (session && (connection = WinHttpConnect( session, host, uc.nPort, 0 )) &&
        (request = WinHttpOpenRequest( connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0 )))
    {
        WinHttpSetTimeouts( request, 20000, 20000, 30000, 30000 );
        if (WinHttpSendRequest( request, range, -1, NULL, 0, 0, 0 ) && WinHttpReceiveResponse( request, NULL ))
            while (out->size < length && WinHttpReadData( request, out->data + out->size, min( length - out->size, 65536 ), &got ) && got)
                out->size += got;
    }
    if (request) WinHttpCloseHandle( request );
    if (connection) WinHttpCloseHandle( connection );
    if (session) WinHttpCloseHandle( session );
    if (out->size != length)
    {
        free( out->data );
        out->data = NULL;
        out->size = 0;
        return FALSE;
    }
    return TRUE;
}

/* little-endian reads; zero past the end */
static UINT8 rd8( const struct blob *b, size_t o ) { return o < b->size ? b->data[o] : 0; }
static UINT16 le16( const struct blob *b, size_t o ) { return rd8( b, o ) | rd8( b, o + 1 ) << 8; }
static UINT32 le32( const struct blob *b, size_t o ) { return le16( b, o ) | (UINT32)le16( b, o + 2 ) << 16; }
static UINT64 le64( const struct blob *b, size_t o ) { return le32( b, o ) | (UINT64)le32( b, o + 4 ) << 32; }

/* a NUL-terminated UTF-16 field of at most chars characters, as UTF-8 */
static char *utf16_field( const struct blob *b, size_t o, size_t chars )
{
    WCHAR *w = malloc( (chars + 1) * sizeof(WCHAR) );
    size_t i;
    char *out;

    if (!w) return NULL;
    for (i = 0; i < chars && o + 2 * i + 1 < b->size; i++) if (!(w[i] = le16( b, o + 2 * i ))) break;
    w[i] = 0;
    out = utf8_dup( w );
    free( w );
    return out;
}

static void upper( char *s )
{
    for (; s && *s; s++) *s = toupper( (unsigned char)*s );
}

static void lower( char *s )
{
    for (; s && *s; s++) *s = tolower( (unsigned char)*s );
}

/* the part of s before sep, compared with the part of t before sep */
static BOOL same_base( const char *s, const char *t )
{
    size_t a = strcspn( s, "-" ), b = strcspn( t, "-" );
    return a == b && !strncmp( s, t, a );
}

/* ---- Resources.pri: one string resource, for the user's languages ---- */

struct pri_section { char name[17]; size_t offset; };

struct pri
{
    const struct blob *d;
    struct pri_section sections[64];
    unsigned count;
};

static size_t pri_find( const struct pri *pri, const char *name )
{
    unsigned i;
    for (i = 0; i < pri->count; i++) if (!strcmp( pri->sections[i].name, name )) return pri->sections[i].offset;
    return (size_t)-1;
}

/* the languages of a qualifier set, upper-cased; empty for the neutral one */
static void pri_qualifier_set_languages( const struct blob *d, size_t dec, unsigned qset, struct strlist *out )
{
    unsigned nd = le16( d, dec ), nq = le16( d, dec + 2 ), nqs = le16( d, dec + 4 ), ndec = le16( d, dec + 6 );
    size_t qsets = dec + 12 + 4 * ndec, quals = qsets + 4 * nqs, distinct = quals + 8 * nq, index = distinct + 12 * nd;
    size_t data = index + 2 * le16( d, dec + 8 );
    unsigned first, n, k, q, dq;
    char *v;

    if (qset >= nqs) return;
    first = le16( d, qsets + 4 * qset );
    n = le16( d, qsets + 4 * qset + 2 );
    for (k = 0; k < n; k++)
    {
        q = le16( d, index + 2 * (first + k) );
        dq = le16( d, quals + 8 * q );
        if (le16( d, distinct + 12 * dq + 2 ) != 0) continue;   /* not a Language qualifier */
        if (!(v = utf16_field( d, data + 2 * le32( d, distinct + 12 * dq + 8 ), 64 ))) continue;
        upper( v );
        strlist_add( out, v );
        free( v );
    }
}

struct pri_schema { const struct blob *d; size_t ents, udata, adata; };

/* an item's full name in the hierarchical schema: scope/scope/name */
static void pri_full_name( const struct pri_schema *s, unsigned i, char *buf, size_t size, int depth )
{
    size_t e = s->ents + 12 * i;
    unsigned parent = le16( s->d, e ), len = rd8( s->d, e + 6 ), flags = rd8( s->d, e + 7 ), off = le16( s->d, e + 8 );
    char *n, up[1024];

    if (flags & 0x20)
    {
        if (!(n = calloc( 1, len + 1 ))) return;
        if (s->adata + off + len <= s->d->size) memcpy( n, s->d->data + s->adata + off, len );
    }
    else if (!(n = utf16_field( s->d, s->udata + 2 * off, len ))) return;
    up[0] = 0;
    if (i && parent != i && depth < 32) pri_full_name( s, parent, up, sizeof(up), depth + 1 );
    if (up[0]) snprintf( buf, size, "%s/%s", up, n );
    else snprintf( buf, size, "%s", n );
    free( n );
}

/* ms-resource:name, ms-resource:///resources/name or ms-resource://package/resources/name */
static char *pri_lookup( const struct blob *d, const char *ref, const struct strlist *langs )
{
    struct pri pri = { d };
    struct pri_schema schema = { d };
    size_t toc, start, hs, dec, map, p, vtypes, i2gtab, groups, infos, cands, index, c, o, base, e;
    unsigned nsec, total, nscopes, nitems, ulen, envlen, hslen, vtsize, i2g, ngroups, niteminfo, largelen;
    unsigned i, g, k, decision, firstcand, ndec, nd, nq, nqs, qfirst, qn, vt, di, ds, type, nstr, nblob, so, sl;
    int item = -1, info = -1, best = -1, best_score = -1;
    char path[1024], name[1024], *q, *v;
    const char *rest;

    if (d->size < 32 || memcmp( d->data, "mrm_pri", 7 )) return NULL;
    toc = le32( d, 16 );
    start = le32( d, 20 );
    nsec = le16( d, 24 );
    for (i = 0; i < nsec && i < ARRAY_SIZE(pri.sections) && toc + 32 * (i + 1) <= d->size; i++)
    {
        struct pri_section *sec = &pri.sections[pri.count++];
        int n = 16;
        memcpy( sec->name, d->data + toc + 32 * i, 16 );
        while (n && (!sec->name[n - 1] || sec->name[n - 1] == ' ')) n--;
        sec->name[n] = 0;
        sec->offset = start + le32( d, toc + 32 * i + 24 ) + 32;
    }
    rest = ref + strlen( "ms-resource:" );
    if (!strncmp( rest, "//", 2 )) rest = strchr( rest + 2, '/' ) ? strchr( rest + 2, '/' ) + 1 : "";
    while (*rest == '/') rest++;
    if (!strchr( rest, '/' )) snprintf( path, sizeof(path), "resources/%s", rest );
    else snprintf( path, sizeof(path), "%s", rest );
    lower( path );

    /* the hierarchical schema: the item with that full name */
    hs = pri_find( &pri, "[mrm_hschemaex]" );
    dec = pri_find( &pri, "[mrm_decn_info]" );
    map = pri_find( &pri, "[mrm_res_map2_]" );
    if (hs == (size_t)-1 || dec == (size_t)-1 || map == (size_t)-1) return NULL;
    p = hs + 8 + 16 + 20 + 2 * le16( d, hs + 2 ) + 2 * le16( d, hs + 4 );
    total = le32( d, p + 6 );
    nscopes = le32( d, p + 10 );
    nitems = le32( d, p + 14 );
    ulen = le32( d, p + 18 );
    schema.ents = p + 30;
    schema.udata = schema.ents + 12 * (size_t)total + 8 * (size_t)nscopes + 2 * (size_t)nitems;
    schema.adata = schema.udata + 2 * (size_t)ulen;
    for (i = 0; i < total && item < 0; i++)
    {
        e = schema.ents + 12 * i;
        if (e + 12 > d->size) break;
        if (rd8( d, e + 7 ) & 0x10) continue;   /* a scope */
        name[0] = 0;
        pri_full_name( &schema, i, name, sizeof(name), 0 );
        lower( name );
        if (!strcmp( name, path )) item = le16( d, e + 10 );
    }
    if (item < 0) return NULL;

    /* the resource map: the item's candidates, and the decision whose qualifier sets they follow */
    envlen = le16( d, map );
    hslen = le16( d, map + 6 );
    vtsize = le16( d, map + 10 );
    i2g = le16( d, map + 12 );
    ngroups = le16( d, map + 14 );
    niteminfo = le32( d, map + 16 );
    largelen = le32( d, map + 28 );
    p = map + 32 + envlen + hslen;
    vtypes = p;
    p += 8 * vtsize;
    i2gtab = p;
    p += 4 * i2g;
    groups = p;
    p += 4 * ngroups;
    infos = p;
    p += 4 * (size_t)niteminfo + largelen;
    cands = p;
    for (g = 0; g < i2g && info < 0; g++)
    {
        unsigned first = le16( d, i2gtab + 4 * g ), grp = le16( d, i2gtab + 4 * g + 2 ), size, firstinfo;
        if (grp >= ngroups) continue;
        size = le16( d, groups + 4 * grp );
        firstinfo = le16( d, groups + 4 * grp + 2 );
        if ((unsigned)item >= first && (unsigned)item < first + size) info = firstinfo + item - first;
    }
    if (info < 0 || (unsigned)info >= niteminfo) return NULL;
    decision = le16( d, infos + 4 * info );
    firstcand = le16( d, infos + 4 * info + 2 );
    ndec = le16( d, dec + 6 );
    nd = le16( d, dec );
    nq = le16( d, dec + 2 );
    nqs = le16( d, dec + 4 );
    if (decision >= ndec) return NULL;
    qfirst = le16( d, dec + 12 + 4 * decision );
    qn = le16( d, dec + 12 + 4 * decision + 2 );
    index = dec + 12 + 4 * ndec + 4 * nqs + 8 * nq + 12 * nd;

    /* best candidate: the user's language exactly, then its base language, then the neutral one */
    for (k = 0; k < qn; k++)
    {
        struct strlist cl = {0};
        unsigned qs = le16( d, index + 2 * (qfirst + k) ), li, l;
        int score;

        pri_qualifier_set_languages( d, dec, qs, &cl );
        score = cl.count ? 0 : 1;
        for (li = 0; li < langs->count; li++)
        {
            const char *want = langs->items[li];
            for (l = 0; l < cl.count; l++)
            {
                int s = 0;
                if (!strcmp( cl.items[l], want )) s = 100;
                else if (same_base( cl.items[l], want )) s = 50;
                if (s) score = max( score, s - (int)li );
            }
        }
        strlist_free( &cl );
        if (score > best_score)
        {
            best_score = score;
            best = k;
        }
    }
    if (best < 0) return NULL;
    c = cands;
    for (k = 0; k < firstcand + best; k++)
    {
        if (rd8( d, c ) != 1) return NULL;   /* only data-item candidates are understood */
        c += 8;
    }
    if (rd8( d, c ) != 1) return NULL;
    vt = rd8( d, c + 1 );
    di = le16( d, c + 4 );
    ds = le16( d, c + 6 );
    type = vt < vtsize ? le32( d, vtypes + 8 * vt + 4 ) : 4;
    if (ds >= pri.count) return NULL;
    o = pri.sections[ds].offset;
    nstr = le16( d, o + 4 );
    nblob = le16( d, o + 6 );
    if (di >= nstr) return NULL;
    base = o + 12 + 4 * nstr + 8 * nblob;
    so = le16( d, o + 12 + 4 * di );
    sl = le16( d, o + 12 + 4 * di + 2 );
    if (base + so + sl > d->size) return NULL;
    if (type == 0)      /* String: UTF-16 */
    {
        struct blob s = { d->data + base + so, sl };
        v = utf16_field( &s, 0, sl / 2 );
    }
    else if ((v = malloc( sl + 1 )))    /* AsciiString, Utf8String */
    {
        memcpy( v, d->data + base + so, sl );
        v[sl] = 0;
    }
    (void)q;
    return v;
}

static void user_languages( struct strlist *out )
{
    ULONG count = 0, len = 0;
    WCHAR *buf;
    const WCHAR *p;
    char *s;

    if (GetUserPreferredUILanguages( MUI_LANGUAGE_NAME, &count, NULL, &len ) && len && (buf = malloc( len * sizeof(WCHAR) )))
    {
        if (GetUserPreferredUILanguages( MUI_LANGUAGE_NAME, &count, buf, &len ))
            for (p = buf; *p; p += wcslen( p ) + 1)
            {
                if (!(s = utf8_dup( p ))) continue;
                upper( s );
                strlist_add( out, s );
                free( s );
            }
        free( buf );
    }
    if (!out->count) strlist_add( out, "EN-US" );
}

/* ---- the package, read once per product ---- */

struct gdk_file { char *name; size_t offset, size; };

static const struct gdk_file *find_file( const struct gdk_file *files, unsigned count, const char *name )
{
    unsigned i;
    for (i = 0; i < count; i++) if (!strcmp( files[i].name, name )) return &files[i];
    return NULL;
}

static char *dup_str( const char *s )
{
    return strdup( s ? s : "" );
}

static BOOL gdk_read( const char *url, struct gdk_package *pkg )
{
    struct blob h = {0}, info = {0}, ud = {0}, meta_blob = {0}, pri_blob = {0};
    UINT64 drive, n, level, tree, user_at, xvc_at;
    UINT32 emb, user, xvc, dyn, nregions, nupdates, nspecs, i, count;
    const struct json *list, *f;
    struct gdk_file *files = NULL;
    unsigned file_count = 0, j, k;
    struct strlist langs = {0};
    struct json *meta = NULL;
    WCHAR image_dir[MAX_PATH] = {0}, base[MAX_PATH];
    const struct gdk_file *file;
    size_t specs, table, s;
    BOOL ret = FALSE;
    int i2;

#define PAGES(x) (((UINT64)(x) + XVD_PAGE - 1) / XVD_PAGE)
    if (!http_range( url, 0, XVD_PAGE, &h ) || memcmp( h.data + 0x200, "msft-xvd", 8 ))
    {
        WARN( "gdk: no XVD header at %s\n", debugstr_a( url ) );
        goto done;
    }
    drive = le64( &h, 0x218 );
    emb = le32( &h, 0x288 );
    user = le32( &h, 0x28c );
    xvc = le32( &h, 0x290 );
    dyn = le32( &h, 0x294 );
    n = PAGES(user) + PAGES(xvc) + PAGES(dyn) + PAGES(drive);
    level = max( 1, (n + 169) / 170 );
    tree = level;
    for (i2 = 0; i2 < 3; i2++)
    {
        level = level == 1 ? 0 : (level + 169) / 170;
        tree += level;
    }
    user_at = 3 + PAGES(emb) + rd8( &h, 0x470 ) + tree;
    xvc_at = user_at + PAGES(user);

    if (!http_range( url, xvc_at * XVD_PAGE, PAGES(xvc) * XVD_PAGE, &info ) || info.size < 0xDA8 || le32( &info, 0xD10 ) < 1)
    {
        WARN( "gdk: no region table\n" );
        goto done;
    }
    nregions = le32( &info, 0xD14 );
    nupdates = le32( &info, 0xD3C );
    nspecs = le32( &info, 0xD50 );
    specs = 0xDA8 + 0x80 * (size_t)nregions + 12 * (size_t)nupdates;
    if (specs + 392 * (size_t)nspecs > info.size)
    {
        WARN( "gdk: region specifiers past the XVC info\n" );
        goto done;
    }
    if (!(pkg->regions = calloc( max( nregions, 1 ), sizeof(*pkg->regions) ))) goto done;
    for (i = 0; i < nregions; i++)
    {
        size_t r = 0xDA8 + 0x80 * (size_t)i;
        struct gdk_region *g = &pkg->regions[pkg->region_count++];
        g->id = le32( &info, r );
        g->key = le16( &info, r + 4 );
        g->offset = le64( &info, r + 0x50 );
        g->length = le64( &info, r + 0x58 );
    }
    for (i = 0; i < nspecs; i++)
    {
        UINT32 id;
        char *key, *value;

        s = specs + 392 * (size_t)i;
        id = le32( &info, s );
        key = utf16_field( &info, s + 8, 64 );
        value = utf16_field( &info, s + 136, 128 );
        for (j = 0; key && value && j < pkg->region_count; j++)
        {
            if (pkg->regions[j].id != id) continue;
            if (!strcmp( key, "Tags" )) strlist_add( &pkg->regions[j].tags, value );
            else if (!strcmp( key, "Languages" )) strlist_split( &pkg->regions[j].languages, value, ';' );
        }
        free( key );
        free( value );
    }

    /* the user data's files */
    if (!http_range( url, user_at * XVD_PAGE, user, &ud ) || ud.size < 16) goto done;
    table = le32( &ud, 0 );
    if (table + 528 <= ud.size)
    {
        pkg->family = utf16_field( &ud, table + 4, 260 );
        count = le32( &ud, table + 524 );
        if (!(files = calloc( max( count, 1 ), sizeof(*files) ))) goto done;
        for (i = 0; i < count && table + 528 * (size_t)(i + 2) <= ud.size; i++)
        {
            size_t e = table + 528 * (size_t)(i + 1);
            UINT32 size = le32( &ud, e + 520 ), off = le32( &ud, e + 524 );
            if (16 + (size_t)off + size > ud.size) continue;
            files[file_count].name = utf16_field( &ud, e, 260 );
            files[file_count].offset = 16 + (size_t)off;
            files[file_count++].size = size;
        }
    }
    if (!pkg->family) pkg->family = dup_str( "" );

    /* Metadata.json: the features and recipes; names from Resources.pri, images kept beside our other state */
    if ((file = find_file( files, file_count, "Metadata.json" )))
        meta = json_parse( (char *)ud.data + file->offset, file->size );
    if ((file = find_file( files, file_count, "Resources.pri" )))
    {
        pri_blob.data = ud.data + file->offset;
        pri_blob.size = file->size;
    }
    user_languages( &langs );
    if (GetEnvironmentVariableW( L"LOCALAPPDATA", base, MAX_PATH ) && wcslen( base ) + 80 < MAX_PATH)
    {
        WCHAR family[260];
        wide_copy( family, ARRAY_SIZE(family), pkg->family );
        swprintf( image_dir, ARRAY_SIZE(image_dir), L"%s\\wineuwp", base );
        CreateDirectoryW( image_dir, NULL );
        wcscat( image_dir, L"\\gdk" );
        CreateDirectoryW( image_dir, NULL );
        if (wcslen( image_dir ) + wcslen( family ) + 2 < MAX_PATH)
        {
            wcscat( image_dir, L"\\" );
            wcscat( image_dir, family );
            CreateDirectoryW( image_dir, NULL );
        }
        else image_dir[0] = 0;
    }
    list = json_get( meta, "InstallFeatures" );
    if (!(pkg->features = calloc( max( json_count( list ), 1 ), sizeof(*pkg->features) ))) goto done;
    for (k = 0; k < json_count( list ); k++)
    {
        struct gdk_feature *g = &pkg->features[pkg->feature_count++];
        const char *image;
        char *named;

        f = json_at( list, k );
        g->id = dup_str( json_str( json_get( f, "Id" ) ) );
        g->name = dup_str( json_str( json_get( f, "DisplayName" ) ) );
        g->tags = dup_str( json_str( json_get( f, "Tags" ) ) );
        g->hidden = json_get( f, "Hidden" )->type == JSON_BOOL && json_get( f, "Hidden" )->boolean;
        if (g->name && !strncmp( g->name, "ms-resource:", 12 ) && pri_blob.size &&
            (named = pri_lookup( &pri_blob, g->name, &langs )))
        {
            if (*named)
            {
                free( g->name );
                g->name = named;
            }
            else free( named );
        }
        image = json_str( json_get( f, "Image" ) );
        if (*image && image_dir[0] && !strpbrk( image, "\\/:" ) && (file = find_file( files, file_count, image )))
        {
            WCHAR path[MAX_PATH], wimage[MAX_PATH];
            HANDLE out;
            DWORD written;

            wide_copy( wimage, ARRAY_SIZE(wimage), image );
            swprintf( path, ARRAY_SIZE(path), L"%s\\%s", image_dir, wimage );
            out = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
            if (out != INVALID_HANDLE_VALUE)
            {
                WriteFile( out, ud.data + file->offset, file->size, &written, NULL );
                CloseHandle( out );
                g->image = utf8_dup( path );
            }
        }
        if (!g->image) g->image = dup_str( "" );
    }
    list = json_get( meta, "InstallRecipes" );
    if (!(pkg->recipes = calloc( max( json_count( list ), 1 ), sizeof(*pkg->recipes) ))) goto done;
    for (k = 0; k < json_count( list ); k++)
    {
        struct gdk_recipe *g = &pkg->recipes[pkg->recipe_count++];
        const struct json *ids;

        f = json_at( list, k );
        g->id = dup_str( json_str( json_get( f, "Id" ) ) );
        g->included = dup_str( json_str( json_get( f, "IncludedFeatures" ) ) );
        g->excluded = dup_str( json_str( json_get( f, "ExcludedFeatures" ) ) );
        ids = json_get( f, "StoreIds" );
        for (j = 0; j < json_count( ids ); j++) strlist_add( &g->store_ids, json_str( json_at( ids, j ) ) );
    }
    pkg->ok = TRUE;
    ret = TRUE;
    TRACE( "gdk: %s: %u regions, %u specifiers, %u features, %u recipes\n", debugstr_a( pkg->family ), pkg->region_count,
           nspecs, pkg->feature_count, pkg->recipe_count );
#undef PAGES

done:
    for (j = 0; j < file_count; j++) free( files[j].name );
    free( files );
    json_free( meta );
    strlist_free( &langs );
    free( h.data );
    free( info.data );
    free( ud.data );
    (void)meta_blob;
    return ret;
}

static SRWLOCK packages_lock = SRWLOCK_INIT;
static struct gdk_package **packages;      /* read once, kept for the process */
static unsigned packages_count;

static const struct gdk_package *gdk_package( const WCHAR *store_id )
{
    struct gdk_package *found = NULL, *pkg, **grown;
    struct product d;
    unsigned i;
    char *url;

    AcquireSRWLockShared( &packages_lock );
    for (i = 0; i < packages_count && !found; i++) if (!wcscmp( packages[i]->store_id, store_id )) found = packages[i];
    ReleaseSRWLockShared( &packages_lock );
    if (found) return found;

    if (!(pkg = calloc( 1, sizeof(*pkg) )))
    {
        static const struct gdk_package none;
        return &none;
    }
    lstrcpynW( pkg->store_id, store_id, ARRAY_SIZE(pkg->store_id) );
    query_product( store_id, &d );
    if (d.found && !d.uwp && SUCCEEDED(msixvc_url( &d.content_id, &url )))
    {
        gdk_read( url, pkg );
        free( url );
    }
    AcquireSRWLockExclusive( &packages_lock );
    if ((grown = realloc( packages, (packages_count + 1) * sizeof(*packages) )))
    {
        packages = grown;
        packages[packages_count++] = pkg;
    }
    ReleaseSRWLockExclusive( &packages_lock );
    return pkg;
}

/* ---- entitlement: owned outright, or in the subscription's catalogue ---- */

static BOOL entitled_to( const char *store_id )
{
    typedef HRESULT (WINAPI *owns_fn)( const char * );
    HMODULE xboxauth = LoadLibraryW( L"winexboxauth.dll" );
    owns_fn owns = xboxauth ? (void *)GetProcAddress( xboxauth, "XboxAuthOwnsProduct" ) : NULL;
    char needle[256];
    size_t len = strlen( store_id );

    if (owns && owns( store_id ) == S_OK) return TRUE;
    /* a length-prefixed string in the protobuf: the product is part of the subscription */
    if (len + 1 > sizeof(needle)) return FALSE;
    needle[0] = (char)len;
    memcpy( needle + 1, store_id, len );
    return content_access_has( needle, len + 1 );
}

/* ---- the choice: features -> tags -> regions ---- */

/* the features of the default recipe (and every hidden one), or of chosen when the app configured some */
static void gdk_selected_features( const struct gdk_package *pkg, const struct strlist *chosen, struct strlist *sel )
{
    struct strlist parts = {0};
    unsigned i, j;

    for (i = 0; i < pkg->feature_count; i++) if (pkg->features[i].hidden) strlist_add_unique( sel, pkg->features[i].id );
    if (chosen) for (i = 0; i < chosen->count; i++) strlist_add_unique( sel, chosen->items[i] );
    else
        for (i = 0; i < pkg->recipe_count; i++)
        {
            if (strcmp( pkg->recipes[i].id, "default" )) continue;
            strlist_split( &parts, pkg->recipes[i].included, ';' );
            for (j = 0; j < parts.count; j++) strlist_add_unique( sel, parts.items[j] );
            strlist_free( &parts );
        }
}

static void gdk_tags( const struct gdk_package *pkg, const struct strlist *features, struct strlist *tags )
{
    struct strlist parts = {0};
    unsigned i, j;

    for (i = 0; i < pkg->feature_count; i++)
    {
        if (!strlist_has( features, pkg->features[i].id )) continue;
        strlist_split( &parts, pkg->features[i].tags, ';' );
        for (j = 0; j < parts.count; j++) strlist_add_unique( tags, parts.items[j] );
        strlist_free( &parts );
    }
}

static BOOL gdk_region_wanted( const struct gdk_region *r, const struct strlist *tags, const struct strlist *langs )
{
    unsigned i, j;

    if (r->tags.count)
    {
        BOOL any = FALSE;
        for (i = 0; i < r->tags.count; i++)
        {
            struct strlist parts = {0};
            BOOL all = TRUE;
            strlist_split( &parts, r->tags.items[i], '#' );
            for (j = 0; j < parts.count; j++) all &= strlist_has( tags, parts.items[j] );
            strlist_free( &parts );
            any |= all;
        }
        if (!any) return FALSE;
    }
    if (r->languages.count)
    {
        BOOL match = FALSE;
        for (i = 0; i < r->languages.count; i++)
        {
            char *u = dup_str( r->languages.items[i] );
            upper( u );
            for (j = 0; u && j < langs->count; j++)
            {
                const char *want = langs->items[j];
                size_t wl = strcspn( want, "-" );
                match |= !strcmp( u, want ) || (strlen( u ) == wl && !strncmp( u, want, wl )) || same_base( u, want );
            }
            free( u );
        }
        if (!match) return FALSE;
    }
    return TRUE;
}

static UINT64 gdk_install_size( const struct gdk_package *pkg, const struct strlist *features )
{
    struct strlist tags = {0}, langs = {0};
    UINT64 size = 0;
    unsigned i;

    gdk_tags( pkg, features, &tags );
    user_languages( &langs );
    for (i = 0; i < pkg->region_count; i++)
    {
        const struct gdk_region *r = &pkg->regions[i];
        if ((r->id < 0x40000000 || r->id == 0x40000003) && gdk_region_wanted( r, &tags, &langs )) size += r->length;
    }
    strlist_free( &tags );
    strlist_free( &langs );
    return size;
}

/* ---- PackageFeature, PackageRecipe, and views of them ---- */

static const GUID IID_IPackageFeature = {0xa7a6472c,0x7eaf,0x58e9,{0xb2,0x1e,0x85,0xa3,0x07,0xa2,0x98,0x9e}};
static const GUID IID_IPackageRecipe = {0xafd64aa0,0x060f,0x51e8,{0x93,0xab,0x9b,0xcb,0x20,0x04,0x8c,0x86}};
static const GUID IID_IVectorView_PackageFeature = {0xe8024431,0xb448,0x5d50,{0xb8,0xc3,0x71,0x1d,0x18,0xe7,0xd8,0x66}};
static const GUID IID_IIterable_PackageFeature = {0x4d32f49a,0xd606,0x51c9,{0xbf,0x36,0x2d,0x76,0xbb,0x63,0xb3,0xaf}};
static const GUID IID_IIterator_PackageFeature = {0xebd85e37,0x7bd3,0x51fd,{0x8f,0xb0,0xcd,0xb4,0x73,0x36,0xf9,0x4d}};
static const GUID IID_IVectorView_PackageRecipe = {0x35e6aa5d,0x24eb,0x5f80,{0xa6,0xb1,0x75,0xf6,0x02,0xf3,0xfe,0xc8}};
static const GUID IID_IIterable_PackageRecipe = {0x6856e35e,0xdfaf,0x5d2a,{0xbb,0x25,0x94,0xe4,0x34,0x76,0x25,0xa1}};
static const GUID IID_IIterator_PackageRecipe = {0x10a66354,0xdf59,0x5c40,{0xb6,0x90,0xe7,0x45,0x02,0xf7,0xd1,0xc5}};

static HRESULT hstr_utf8( const char *s, HSTRING *out )
{
    int len = MultiByteToWideChar( CP_UTF8, 0, s ? s : "", -1, NULL, 0 );
    WCHAR *w = malloc( len * sizeof(WCHAR) );
    HRESULT hr;

    if (!w) return E_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, 0, s ? s : "", -1, w, len );
    hr = hstr( w, out );
    free( w );
    return hr;
}

/* the feature and recipe objects point into the package, which is kept for the process */
struct feature_obj
{
    struct object obj;
    const struct gdk_feature *f;
    const struct gdk_recipe *r;
    char *store_ids;
    BOOL entitled, selected;
};

static void feature_obj_destroy( struct object *obj )
{
    free( ((struct feature_obj *)obj)->store_ids );
}

static HRESULT WINAPI feat_get_Id( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->f->id, v ); }
static HRESULT WINAPI feat_get_DisplayName( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->f->name, v ); }
static HRESULT WINAPI feat_get_Image( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->f->image, v ); }
static HRESULT WINAPI feat_get_Tags( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->f->tags, v ); }
GETTER(struct feature_obj, feat, Entitled, boolean, This->entitled)
GETTER(struct feature_obj, feat, Selected, boolean, This->selected)

static const void *feature_vtbl[] =
{
    OBJ_METHODS, feat_get_Id, feat_get_DisplayName, feat_get_Image, feat_get_Tags, feat_get_Entitled, feat_get_Selected
};

static HRESULT WINAPI rcp_get_Id( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->r->id, v ); }
static HRESULT WINAPI rcp_get_IncludedFeatures( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->r->included, v ); }
static HRESULT WINAPI rcp_get_ExcludedFeatures( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->r->excluded, v ); }
static HRESULT WINAPI rcp_get_StoreIds( struct feature_obj *This, HSTRING *v ) { return hstr_utf8( This->store_ids, v ); }

static const void *recipe_vtbl[] =
{
    OBJ_METHODS, rcp_get_Id, rcp_get_IncludedFeatures, rcp_get_ExcludedFeatures, rcp_get_StoreIds, feat_get_Selected
};

/* the product a PackageInstance stands for: its own source when that is a Store id, or the install's */
static void instance_store_id( struct factory *f, WCHAR *store_id, size_t size )
{
    struct installed_game g;

    if (package_game( f, &g ) || queued_game( f, &g ))
    {
        lstrcpynW( store_id, g.store_id, size );
        installed_game_clear( &g );
    }
    else lstrcpynW( store_id, f->source, size );
}

/* PackageInstance.QueryFeatures: the package's install features. The install dialog of a game that has them asks
 * before it offers Install; "not implemented" closed it (error 50253) and Install did nothing. */
HRESULT WINAPI pi_QueryFeatures( struct factory *f, IInspectable **out )
{
    const struct gdk_package *pkg;
    struct strlist sel = {0}, inc = {0};
    struct feature_obj *feat;
    struct obj_view *view;
    WCHAR store_id[64];
    unsigned i, j, k;
    BOOL entitled;

    if (!out) return E_POINTER;
    instance_store_id( f, store_id, ARRAY_SIZE(store_id) );
    pkg = gdk_package( store_id );
    if (!(view = obj_view_create( &IID_IVectorView_PackageFeature, &IID_IIterable_PackageFeature, &IID_IIterator_PackageFeature,
                                  L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageFeature>" )))
        return E_OUTOFMEMORY;
    gdk_selected_features( pkg, NULL, &sel );
    for (i = 0; i < pkg->feature_count; i++)
    {
        const struct gdk_feature *ft = &pkg->features[i];
        if (ft->hidden) continue;
        /* entitled unless a recipe that brings it in names products this account has none of */
        entitled = TRUE;
        for (j = 0; j < pkg->recipe_count; j++)
        {
            const struct gdk_recipe *r = &pkg->recipes[j];
            BOOL included;
            if (!r->store_ids.count) continue;
            strlist_split( &inc, r->included, ';' );
            included = strlist_has( &inc, ft->id );
            strlist_free( &inc );
            if (!included) continue;
            entitled = FALSE;
            for (k = 0; k < r->store_ids.count; k++) entitled |= entitled_to( r->store_ids.items[k] );
        }
        if (!(feat = object_create( sizeof(*feat), feature_vtbl, &IID_IPackageFeature,
                                    L"Microsoft.GamePlatform.Services.PackageFeature", feature_obj_destroy )))
            continue;
        feat->f = ft;
        feat->entitled = entitled;
        feat->selected = strlist_has( &sel, ft->id );
        obj_view_add( view, (IInspectable *)feat );
        TRACE( "PackageInstance.QueryFeatures(%s): %s \"%s\", tags %s, %s, %s\n", debugstr_w( store_id ), ft->id, ft->name,
               ft->tags, entitled ? "entitled" : "not entitled", feat->selected ? "selected" : "not selected" );
    }
    strlist_free( &sel );
    *out = (IInspectable *)view;
    return S_OK;
}

HRESULT WINAPI pi_QueryRecipes( struct factory *f, IInspectable **out )
{
    const struct gdk_package *pkg;
    struct feature_obj *rcp;
    struct obj_view *view;
    WCHAR store_id[64];
    unsigned i;

    if (!out) return E_POINTER;
    instance_store_id( f, store_id, ARRAY_SIZE(store_id) );
    pkg = gdk_package( store_id );
    if (!(view = obj_view_create( &IID_IVectorView_PackageRecipe, &IID_IIterable_PackageRecipe, &IID_IIterator_PackageRecipe,
                                  L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageRecipe>" )))
        return E_OUTOFMEMORY;
    for (i = 0; i < pkg->recipe_count; i++)
    {
        if (!(rcp = object_create( sizeof(*rcp), recipe_vtbl, &IID_IPackageRecipe,
                                   L"Microsoft.GamePlatform.Services.PackageRecipe", feature_obj_destroy )))
            continue;
        rcp->r = &pkg->recipes[i];
        rcp->store_ids = strlist_join( &pkg->recipes[i].store_ids, ';' );
        rcp->selected = !strcmp( pkg->recipes[i].id, "default" );
        obj_view_add( view, (IInspectable *)rcp );
    }
    TRACE( "PackageInstance.QueryRecipes -> %u\n", pkg->recipe_count );
    *out = (IInspectable *)view;
    return S_OK;
}

/* ---- what a request installs ---- */

UINT64 gdk_request_size( const WCHAR *store_id, const struct strlist *features, UINT64 fallback )
{
    const struct gdk_package *pkg = gdk_package( store_id );
    struct strlist sel = {0};
    UINT64 size;

    if (!pkg->ok || !pkg->feature_count) return fallback;
    gdk_selected_features( pkg, features, &sel );
    size = gdk_install_size( pkg, &sel );
    strlist_free( &sel );
    return size;
}

static WCHAR *wide_dup( const char *s )
{
    int len = MultiByteToWideChar( CP_UTF8, 0, s ? s : "", -1, NULL, 0 );
    WCHAR *w = malloc( len * sizeof(WCHAR) );
    if (w) MultiByteToWideChar( CP_UTF8, 0, s ? s : "", -1, w, len );
    return w;
}

/* the install-feature tags and languages to install; FALSE (and NULLs) to install every region */
BOOL gdk_installer_choice( const WCHAR *store_id, const struct strlist *features, WCHAR **tags, WCHAR **languages )
{
    const struct gdk_package *pkg = gdk_package( store_id );
    struct strlist sel = {0}, t = {0}, l = {0};
    char *tj, *lj;

    *tags = *languages = NULL;
    if (!pkg->ok || !pkg->feature_count) return FALSE;
    gdk_selected_features( pkg, features, &sel );
    gdk_tags( pkg, &sel, &t );
    strlist_sort( &t );
    user_languages( &l );
    tj = strlist_join( &t, ';' );
    lj = strlist_join( &l, ';' );
    TRACE( "install: features -> tags %s, languages %s\n", debugstr_a( tj ), debugstr_a( lj ) );
    *tags = wide_dup( tj );
    *languages = wide_dup( lj );
    free( tj );
    free( lj );
    strlist_free( &sel );
    strlist_free( &t );
    strlist_free( &l );
    return TRUE;
}

HRESULT WINAPI request_QuerySize( struct package_request *r, IInspectable **v )
{
    struct product d;
    UINT64 size;

    if (!v) return E_POINTER;
    query_product( r->store_id, &d );
    size = d.uwp ? d.size : gdk_request_size( r->store_id, r->features_chosen ? &r->features : NULL, d.size );
    TRACE( "PackageRequest.QuerySize(%s) -> %I64u\n", debugstr_w( r->store_id ), size );
    return (*v = request_size_create( size, FALSE )) ? S_OK : E_OUTOFMEMORY;
}

/* ---- PackageSpecifier vectors, PackageFeatureSize, PackagePreview ---- */

static const GUID IID_IPackageSpecifier = {0xb9f1224d,0x53d2,0x5c45,{0x8a,0x00,0x2b,0xdf,0x75,0xea,0xd8,0xf7}};
static const GUID IID_IVector_PackageSpecifier = {0x194d8e83,0x8d1f,0x5ad2,{0x97,0xc6,0xcf,0xb8,0xcc,0xfd,0x9c,0x9c}};
static const GUID IID_IVectorView_PackageSpecifier = {0x3b289596,0x3c6e,0x5aa2,{0xa4,0x12,0xdc,0xfa,0x01,0x3e,0x5c,0x5e}};
static const GUID IID_IIterable_PackageSpecifier = {0x915a098a,0x14e9,0x5ae3,{0x9a,0xa3,0x88,0x39,0x2a,0x15,0x23,0xa1}};
static const GUID IID_IIterator_PackageSpecifier = {0xc9f7da29,0x7588,0x52fd,{0xa1,0xec,0x50,0x73,0x6b,0xb9,0x33,0xa7}};
static const GUID IID_IPackageFeatureSize = {0x49efa3f5,0x5fac,0x5707,{0x9a,0xe5,0x9f,0xd8,0x13,0x7e,0xf4,0xbf}};
static const GUID IID_IVectorView_PackageFeatureSize = {0x1e81dbf1,0x9d0c,0x5821,{0xb0,0x5f,0x32,0xae,0xf7,0xf6,0xd4,0xad}};
static const GUID IID_IIterable_PackageFeatureSize = {0x08dba954,0x6261,0x5882,{0xb6,0xf7,0xf1,0xff,0xd4,0x6e,0xf3,0xe6}};
static const GUID IID_IIterator_PackageFeatureSize = {0xb0a92423,0xbd15,0x5a71,{0x93,0x05,0xeb,0xdc,0xc2,0x2d,0x7b,0xfa}};
static const GUID IID_IPackagePreview = {0xf99e1a4a,0x5416,0x5636,{0xb6,0x2f,0x98,0x17,0x3e,0x97,0xa8,0x1d}};
static const GUID IID_IPackagePreviewSize = {0xdfbca13d,0xe4ac,0x5eca,{0xa7,0xec,0x5b,0xe8,0x92,0x60,0x8c,0x8c}};

enum { SPEC_CONTENT_TYPE, SPEC_DEVICE, SPEC_LANGUAGE, SPEC_TAG, SPEC_RECIPE, SPEC_FEATURE };

typedef HRESULT (WINAPI *spec_type_fn)( void *, INT32 * );
typedef HRESULT (WINAPI *spec_value_fn)( void *, HSTRING * );

/* IPackageSpecifier: get_Type is slot 6, get_Value slot 8 */
static BOOL read_specifier( IInspectable *item, INT32 *type, HSTRING *value )
{
    void **spec = NULL;

    *type = -1;
    *value = NULL;
    if (!item || FAILED(IInspectable_QueryInterface( item, &IID_IPackageSpecifier, (void **)&spec )) || !spec) return FALSE;
    ((spec_type_fn)(*(void ***)spec)[6])( spec, type );
    ((spec_value_fn)(*(void ***)spec)[8])( spec, value );
    IUnknown_Release( (IUnknown *)spec );
    return TRUE;
}

/* IVector<PackageSpecifier>: what the app adds to a request or a preview (features, languages, recipes...) */
struct spec_vector
{
    struct object obj;
    IInspectable **items;
    UINT32 count;
};

static void spec_vector_destroy( struct object *obj )
{
    struct spec_vector *v = (struct spec_vector *)obj;
    UINT32 i;
    for (i = 0; i < v->count; i++) IInspectable_Release( v->items[i] );
    free( v->items );
}

static struct obj_view *spec_snapshot( struct spec_vector *v )
{
    struct obj_view *view = obj_view_create( &IID_IVectorView_PackageSpecifier, &IID_IIterable_PackageSpecifier,
                                             &IID_IIterator_PackageSpecifier,
                                             L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageSpecifier>" );
    UINT32 i;

    if (!view) return NULL;
    for (i = 0; i < v->count; i++)
    {
        IInspectable_AddRef( v->items[i] );
        obj_view_add( view, v->items[i] );
    }
    return view;
}

static void log_item( const char *what, IInspectable *item )
{
    HSTRING value;
    INT32 type;

    read_specifier( item, &type, &value );
    TRACE( "PackageSpecifier %s: type %d, value %s\n", what, type, value ? debugstr_hstring( value ) : "?" );
    WindowsDeleteString( value );
}

static HRESULT WINAPI sv_GetAt( struct spec_vector *v, UINT32 i, IInspectable **out )
{
    if (!out) return E_POINTER;
    if (i >= v->count)
    {
        *out = NULL;
        return E_BOUNDS;
    }
    *out = v->items[i];
    IInspectable_AddRef( *out );
    return S_OK;
}

static HRESULT WINAPI sv_get_Size( struct spec_vector *v, UINT32 *out )
{
    if (!out) return E_POINTER;
    *out = v->count;
    return S_OK;
}

static HRESULT WINAPI sv_GetView( struct spec_vector *v, IInspectable **out )
{
    if (!out) return E_POINTER;
    return (*out = (IInspectable *)spec_snapshot( v )) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI sv_IndexOf( struct spec_vector *v, IInspectable *x, UINT32 *idx, boolean *found )
{
    UINT32 i;
    if (!idx || !found) return E_POINTER;
    *idx = 0;
    *found = FALSE;
    for (i = 0; i < v->count; i++)
        if (v->items[i] == x)
        {
            *idx = i;
            *found = TRUE;
            break;
        }
    return S_OK;
}

static HRESULT WINAPI sv_SetAt( struct spec_vector *v, UINT32 i, IInspectable *x )
{
    if (i >= v->count || !x) return E_BOUNDS;
    log_item( "set", x );
    IInspectable_AddRef( x );
    IInspectable_Release( v->items[i] );
    v->items[i] = x;
    return S_OK;
}

static HRESULT WINAPI sv_InsertAt( struct spec_vector *v, UINT32 i, IInspectable *x )
{
    IInspectable **items;

    if (i > v->count || !x) return E_BOUNDS;
    if (!(items = realloc( v->items, (v->count + 1) * sizeof(*items) ))) return E_OUTOFMEMORY;
    log_item( "inserted", x );
    v->items = items;
    memmove( items + i + 1, items + i, (v->count - i) * sizeof(*items) );
    items[i] = x;
    IInspectable_AddRef( items[i] );
    v->count++;
    return S_OK;
}

static HRESULT WINAPI sv_RemoveAt( struct spec_vector *v, UINT32 i )
{
    if (i >= v->count) return E_BOUNDS;
    IInspectable_Release( v->items[i] );
    memmove( v->items + i, v->items + i + 1, (v->count - i - 1) * sizeof(*v->items) );
    v->count--;
    return S_OK;
}

static HRESULT WINAPI sv_Append( struct spec_vector *v, IInspectable *x ) { return sv_InsertAt( v, v->count, x ); }
static HRESULT WINAPI sv_RemoveAtEnd( struct spec_vector *v ) { return v->count ? sv_RemoveAt( v, v->count - 1 ) : E_BOUNDS; }

static HRESULT WINAPI sv_Clear( struct spec_vector *v )
{
    while (v->count) IInspectable_Release( v->items[--v->count] );
    return S_OK;
}

static HRESULT WINAPI sv_GetMany( struct spec_vector *v, UINT32 start, UINT32 n, IInspectable **out, UINT32 *got )
{
    UINT32 i;
    if (!got) return E_POINTER;
    *got = 0;
    for (i = start; i < v->count && *got < n; i++)
    {
        out[*got] = v->items[i];
        IInspectable_AddRef( out[*got] );
        ++*got;
    }
    return S_OK;
}

static HRESULT WINAPI sv_ReplaceAll( struct spec_vector *v, UINT32 n, IInspectable **x )
{
    UINT32 i;
    sv_Clear( v );
    for (i = 0; i < n; i++) if (x[i]) sv_Append( v, x[i] );
    return S_OK;
}

static HRESULT WINAPI sv_First( const void ***iface, IInspectable **out )
{
    struct obj_view *view;

    if (!out) return E_POINTER;
    if (!(view = spec_snapshot( (struct spec_vector *)OBJ2(iface) ))) return E_OUTOFMEMORY;
    *out = obj_iterator_create( view );
    IInspectable_Release( (IInspectable *)view );
    return *out ? S_OK : E_OUTOFMEMORY;
}

static const void *spec_vector_vtbl[] =
{
    OBJ_METHODS, sv_GetAt, sv_get_Size, sv_GetView, sv_IndexOf, sv_SetAt, sv_InsertAt, sv_RemoveAt, sv_Append,
    sv_RemoveAtEnd, sv_Clear, sv_GetMany, sv_ReplaceAll
};
static const void *spec_iterable_vtbl[] = { OBJ2_METHODS, sv_First };

struct spec_vector *spec_vector_create( void )
{
    struct spec_vector *v = object_create( sizeof(*v), spec_vector_vtbl, &IID_IVector_PackageSpecifier,
        L"Windows.Foundation.Collections.IVector`1<Microsoft.GamePlatform.Services.PackageSpecifier>", spec_vector_destroy );
    if (!v) return NULL;
    v->obj.vtbl2 = spec_iterable_vtbl;
    v->obj.iid2 = IID_IIterable_PackageSpecifier;
    return v;
}

/* the values of one type */
static void spec_values( struct spec_vector *v, INT32 want, struct strlist *out )
{
    HSTRING value;
    INT32 type;
    UINT32 i;
    char *s;

    for (i = 0; i < v->count; i++)
    {
        if (!read_specifier( v->items[i], &type, &value )) continue;
        if (type == want && value && (s = utf8_dup( WindowsGetStringRawBuffer( value, NULL ) )))
        {
            strlist_add( out, s );
            free( s );
        }
        WindowsDeleteString( value );
    }
}

/* The features a request or preview stands for: Feature specifiers, else the features of its Recipe specifiers,
 * else what ConfigureFeatures named, else (NULL) the default recipe. */
static const struct strlist *choose_features( const struct gdk_package *pkg, struct spec_vector *specs, BOOL configured,
                                              const struct strlist *configured_list, struct strlist *storage )
{
    struct strlist recipes = {0};
    unsigned i, j;

    strlist_free( storage );
    if (specs)
    {
        spec_values( specs, SPEC_FEATURE, storage );
        spec_values( specs, SPEC_RECIPE, &recipes );
        for (i = 0; i < recipes.count; i++)
            for (j = 0; j < pkg->recipe_count; j++)
                if (!strcmp( pkg->recipes[j].id, recipes.items[i] )) strlist_split( storage, pkg->recipes[j].included, ';' );
        strlist_free( &recipes );
        if (storage->count) return storage;
    }
    if (configured)
    {
        strlist_copy( storage, configured_list );
        return storage;
    }
    return NULL;
}

struct size_obj
{
    struct object obj;
    char *id;
    UINT64 size;
    BOOL shared;
};

static void size_obj_destroy( struct object *obj )
{
    free( ((struct size_obj *)obj)->id );
}

static HRESULT WINAPI fs_get_Id( struct size_obj *This, HSTRING *v ) { return hstr_utf8( This->id, v ); }
GETTER(struct size_obj, fs, TotalSize, UINT64, This->size)
GETTER(struct size_obj, fs, DownloadSize, UINT64, This->size)
GETTER(struct size_obj, fs, Shared, boolean, This->shared)
GETTER(struct size_obj, fs, StorageSpaceChange, INT64, (INT64)This->size)

static const void *feature_size_vtbl[] = { OBJ_METHODS, fs_get_Id, fs_get_TotalSize, fs_get_DownloadSize, fs_get_Shared };
static const void *preview_size_vtbl[] = { OBJ_METHODS, fs_get_TotalSize, fs_get_DownloadSize, fs_get_StorageSpaceChange };

/* What a feature adds on its own: the regions the choice with it needs that the choice without it does not. A
 * region another feature also needs (game#hd1 for HD1 next to Game) is shared. */
static UINT64 gdk_feature_size( const struct gdk_package *pkg, const char *feature, BOOL *shared )
{
    struct strlist with = {0}, without = {0}, tw = {0}, two = {0}, langs = {0};
    UINT64 size = 0;
    unsigned i, j;

    gdk_selected_features( pkg, NULL, &with );
    strlist_add_unique( &with, feature );
    for (i = 0; i < with.count; i++) if (strcmp( with.items[i], feature )) strlist_add( &without, with.items[i] );
    gdk_tags( pkg, &with, &tw );
    gdk_tags( pkg, &without, &two );
    user_languages( &langs );
    *shared = FALSE;
    for (i = 0; i < pkg->region_count; i++)
    {
        const struct gdk_region *r = &pkg->regions[i];
        if (r->id >= 0x40000000 || !r->tags.count) continue;
        if (gdk_region_wanted( r, &tw, &langs ) && !gdk_region_wanted( r, &two, &langs ))
        {
            size += r->length;
            for (j = 0; j < r->tags.count; j++) *shared |= !!strchr( r->tags.items[j], '#' );
        }
    }
    strlist_free( &with );
    strlist_free( &without );
    strlist_free( &tw );
    strlist_free( &two );
    strlist_free( &langs );
    return size;
}

/* PackagePreview: a package before it is installed - its size for a choice of features, and each feature's */
struct preview
{
    struct object obj;
    WCHAR store_id[256];
    struct spec_vector *specs;
    struct strlist features;
    BOOL configured;
};

static void preview_destroy( struct object *obj )
{
    struct preview *p = (struct preview *)obj;
    obj_Release( &p->specs->obj );
    strlist_free( &p->features );
}

static HRESULT WINAPI pv_get_Specifiers( struct preview *p, IInspectable **v )
{
    if (!v) return E_POINTER;
    obj_AddRef( &p->specs->obj );
    *v = (IInspectable *)p->specs;
    return S_OK;
}

static HRESULT WINAPI pv_QuerySize( struct preview *p, IInspectable **v )
{
    const struct gdk_package *pkg = gdk_package( p->store_id );
    struct strlist storage = {0}, sel = {0};
    struct size_obj *s;
    struct product d;
    UINT64 size;

    if (!v) return E_POINTER;
    query_product( p->store_id, &d );
    if (pkg->ok && pkg->feature_count)
    {
        gdk_selected_features( pkg, choose_features( pkg, p->specs, p->configured, &p->features, &storage ), &sel );
        size = gdk_install_size( pkg, &sel );
    }
    else size = d.size;
    strlist_free( &storage );
    strlist_free( &sel );
    TRACE( "PackagePreview(%s).QuerySize -> %I64u\n", debugstr_w( p->store_id ), size );
    if (!(s = object_create( sizeof(*s), preview_size_vtbl, &IID_IPackagePreviewSize,
                             L"Microsoft.GamePlatform.Services.PackagePreviewSize", size_obj_destroy )))
        return E_OUTOFMEMORY;
    s->size = size;
    *v = (IInspectable *)s;
    return S_OK;
}

static HRESULT WINAPI pv_QueryFeatureSizes( struct preview *p, IInspectable **v )
{
    const struct gdk_package *pkg = gdk_package( p->store_id );
    struct obj_view *view;
    struct size_obj *s;
    unsigned i;

    if (!v) return E_POINTER;
    if (!(view = obj_view_create( &IID_IVectorView_PackageFeatureSize, &IID_IIterable_PackageFeatureSize, &IID_IIterator_PackageFeatureSize,
                                  L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageFeatureSize>" )))
        return E_OUTOFMEMORY;
    for (i = 0; i < pkg->feature_count; i++)
    {
        if (pkg->features[i].hidden) continue;
        if (!(s = object_create( sizeof(*s), feature_size_vtbl, &IID_IPackageFeatureSize,
                                 L"Microsoft.GamePlatform.Services.PackageFeatureSize", size_obj_destroy )))
            continue;
        s->id = dup_str( pkg->features[i].id );
        s->size = gdk_feature_size( pkg, pkg->features[i].id, &s->shared );
        TRACE( "PackagePreview(%s).QueryFeatureSizes: %s %I64u%s\n", debugstr_w( p->store_id ), s->id, s->size,
               s->shared ? " (shared)" : "" );
        obj_view_add( view, (IInspectable *)s );
    }
    *v = (IInspectable *)view;
    return S_OK;
}

static HRESULT WINAPI pv_ConfigureFeatures( struct preview *p, IInspectable *iterable )
{
    read_features( iterable, &p->features );
    p->configured = TRUE;
    TRACE( "PackagePreview(%s).ConfigureFeatures: %u features\n", debugstr_w( p->store_id ), p->features.count );
    return S_OK;
}

static const void *preview_vtbl[] = { OBJ_METHODS, pv_get_Specifiers, pv_QuerySize, pv_QueryFeatureSizes, pv_ConfigureFeatures };

/* PackagePreview.Create(source, sourceType) */
HRESULT WINAPI pv_Create( struct factory *f, HSTRING source, INT32 type, IInspectable **out )
{
    struct installed_game g;
    struct preview *p;
    GUID id;

    if (!out) return E_POINTER;
    if (!(p = object_create( sizeof(*p), preview_vtbl, &IID_IPackagePreview,
                             L"Microsoft.GamePlatform.Services.PackagePreview", preview_destroy )))
        return E_OUTOFMEMORY;
    if (!(p->specs = spec_vector_create()))
    {
        free( p );
        return E_OUTOFMEMORY;
    }
    lstrcpynW( p->store_id, WindowsGetStringRawBuffer( source, NULL ), ARRAY_SIZE(p->store_id) );
    if (type != 1)
    {
        if (find_installed( p->store_id, &g ))
        {
            lstrcpynW( p->store_id, g.store_id, ARRAY_SIZE(p->store_id) );
            installed_game_clear( &g );
        }
        else
        {
            /* an install in the queue, also a failed one: the Xbox app previews its features by installation id
             * after a failure, and a preview that knew no product reported no size or features and the app's
             * catalogue then crashed reading them (Fortnite, Command_ProcessDevicesChanged) */
            BOOL is_guid = SUCCEEDED(CLSIDFromString( p->store_id, &id ));
            queued_store_id( p->store_id, is_guid ? &id : NULL, p->store_id, ARRAY_SIZE(p->store_id) );
        }
    }
    TRACE( "PackagePreview.Create(%s, type %d)\n", debugstr_w( p->store_id ), type );
    *out = (IInspectable *)p;
    return S_OK;
}

HRESULT WINAPI request_get_Specifiers( struct package_request *r, IInspectable **v )
{
    if (!v) return E_POINTER;
    if (!r->specs && !(r->specs = spec_vector_create())) return E_OUTOFMEMORY;
    obj_AddRef( &r->specs->obj );
    *v = (IInspectable *)r->specs;
    return S_OK;
}

/* Feature and Recipe specifiers on the request win over ConfigureFeatures */
void request_features_from_specifiers( struct package_request *r, struct strlist *features, BOOL *chosen )
{
    struct strlist storage = {0};
    const struct gdk_package *pkg;

    if (!r->specs) return;
    pkg = gdk_package( r->store_id );
    if (choose_features( pkg, r->specs, FALSE, features, &storage ))
    {
        strlist_free( features );
        *features = storage;
        *chosen = TRUE;
    }
    else strlist_free( &storage );
}

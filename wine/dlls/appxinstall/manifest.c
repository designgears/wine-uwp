/*
 * appxinstall: a package's identity from its AppxManifest.xml
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


/* The 13-character publisher id in a package family name: the first 8 bytes of the SHA-256 of the UTF-16LE
 * publisher string, as 13 5-bit groups (the last one padded) in Crockford's base32 alphabet. */
HRESULT publisher_id( const WCHAR *publisher, WCHAR id[14] )
{
    static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    BYTE digest[32];
    UINT64 bits = 0;
    unsigned int i;
    HRESULT hr;

    if (FAILED(hr = sha256( publisher, wcslen( publisher ) * sizeof(WCHAR), digest ))) return hr;
    for (i = 0; i < 8; i++) bits = (bits << 8) | digest[i];
    /* 64 bits shifted left by one: 65 bits as 13 groups of 5 */
    for (i = 0; i < 13; i++)
    {
        unsigned int shift = 60 - 5 * i, v;
        /* (bits << 1) >> shift, without the 65th bit falling off */
        v = shift ? (unsigned int)((bits >> (shift - 1)) & 31) : (unsigned int)((bits << 1) & 31);
        id[i] = alphabet[v];
    }
    id[13] = 0;
    return S_OK;
}

static WCHAR *dup_attr( const struct xml_node *node, const WCHAR *name, const WCHAR *fallback )
{
    const WCHAR *v = xml_attr( node, name );
    return strdupW( v ? v : fallback );
}

/* the first text of an element named name that is not an ms-resource: reference */
static WCHAR *plain_text( const struct xml_node *root, const WCHAR *name )
{
    const struct xml_node *node = NULL;

    while ((node = xml_next( root, node, name )))
        if (node->text && *node->text && wcsncmp( node->text, L"ms-resource:", 12 )) return strdupW( node->text );
    return NULL;
}

static int __cdecl compare_strings( const void *a, const void *b )
{
    return wcscmp( *(const WCHAR * const *)a, *(const WCHAR * const *)b );
}

HRESULT manifest_read( const struct package *pkg, struct manifest **out )
{
    const struct xml_node *ident, *app, *visual, *node = NULL;
    struct xml_node *root;
    struct manifest *m;
    int index;
    ULONG size;
    BYTE *data;
    HRESULT hr;
    WCHAR *p;

    if ((index = package_find( pkg, L"AppxManifest.xml" )) < 0) return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    if (FAILED(hr = package_read( pkg, index, &data, &size ))) return hr;
    hr = xml_parse( data, size, &root );
    free( data );
    if (FAILED(hr)) return hr;
    if (!(ident = xml_find( root, L"Identity" )) || !(app = xml_find( root, L"Application" )))
    {
        xml_free( root );
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    }
    if (!(m = calloc( 1, sizeof(*m) ))) { xml_free( root ); return E_OUTOFMEMORY; }
    m->name = dup_attr( ident, L"Name", L"" );
    m->publisher = dup_attr( ident, L"Publisher", L"" );
    m->version = dup_attr( ident, L"Version", L"0.0.0.0" );
    m->arch = dup_attr( ident, L"ProcessorArchitecture", L"neutral" );
    m->resource_id = dup_attr( ident, L"ResourceId", L"" );
    m->app_id = dup_attr( app, L"Id", L"App" );
    m->exe = dup_attr( app, L"Executable", L"" );
    for (p = m->exe; p && *p; p++) if (*p == '\\') *p = '/';
    m->title = plain_text( root, L"DisplayName" );
    m->publisher_name = plain_text( root, L"PublisherDisplayName" );
    if ((visual = xml_find( app, L"VisualElements" )))
    {
        const WCHAR *logo = xml_attr( visual, L"Square44x44Logo" );
        if (!logo) logo = xml_attr( visual, L"Square150x150Logo" );
        if ((m->logo = strdupW( logo ))) for (p = m->logo; *p; p++) if (*p == '\\') *p = '/';
    }
    while ((node = xml_next( root, node, L"PackageDependency" )))
    {
        struct dependency *deps = realloc( m->deps, (m->dep_count + 1) * sizeof(*deps) );
        if (!deps) { hr = E_OUTOFMEMORY; break; }
        m->deps = deps;
        deps[m->dep_count].name = dup_attr( node, L"Name", L"" );
        deps[m->dep_count].min_version = dup_attr( node, L"MinVersion", L"0.0.0.0" );
        deps[m->dep_count].publisher = dup_attr( node, L"Publisher", L"" );
        m->dep_count++;
    }
    /* URI schemes the app owns (a browser sign-in comes back through one), lowercased, sorted, unique */
    node = NULL;
    while (SUCCEEDED(hr) && (node = xml_next( root, node, L"Protocol" )))
    {
        const WCHAR *name = xml_attr( node, L"Name" );
        WCHAR **protocols, *lower;
        unsigned int i;

        if (!name || !*name) continue;
        if (!(lower = strdupW( name ))) { hr = E_OUTOFMEMORY; break; }
        _wcslwr( lower );
        for (i = 0; i < m->protocol_count; i++) if (!wcscmp( m->protocols[i], lower )) break;
        if (i < m->protocol_count) { free( lower ); continue; }
        if (!(protocols = realloc( m->protocols, (m->protocol_count + 1) * sizeof(*protocols) )))
        {
            free( lower );
            hr = E_OUTOFMEMORY;
            break;
        }
        m->protocols = protocols;
        protocols[m->protocol_count++] = lower;
    }
    if (m->protocol_count) qsort( m->protocols, m->protocol_count, sizeof(*m->protocols), compare_strings );
    xml_free( root );
    if (FAILED(hr) || !m->name || !m->publisher || !m->version || !m->arch || !m->resource_id || !m->app_id || !m->exe)
    {
        manifest_free( m );
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    *out = m;
    return S_OK;
}

void manifest_free( struct manifest *m )
{
    unsigned int i;

    if (!m) return;
    for (i = 0; i < m->dep_count; i++)
    {
        free( m->deps[i].name );
        free( m->deps[i].min_version );
        free( m->deps[i].publisher );
    }
    for (i = 0; i < m->protocol_count; i++) free( m->protocols[i] );
    free( m->deps );
    free( m->protocols );
    free( m->name );
    free( m->publisher );
    free( m->version );
    free( m->arch );
    free( m->resource_id );
    free( m->app_id );
    free( m->exe );
    free( m->title );
    free( m->publisher_name );
    free( m->logo );
    free( m );
}

/* the largest targetsize-N or scale-N in a qualifier string */
static unsigned int qualifier_size( const WCHAR *q )
{
    unsigned int best = 0;
    const WCHAR *p;

    for (p = q; *p; p++)
    {
        const WCHAR *digits = NULL;
        if (!wcsncmp( p, L"targetsize-", 11 )) digits = p + 11;
        else if (!wcsncmp( p, L"scale-", 6 )) digits = p + 6;
        if (digits && iswdigit( *digits )) best = max( best, wcstoul( digits, NULL, 10 ) );
    }
    return best;
}

/* The best member for a manifest logo path: the plain unplated 256 px variant if there is one, then the largest
 * target size or scale. MRT stores qualified names (Logo.targetsize-256.png) next to the unqualified path. */
int pick_logo( const struct package *pkg, const WCHAR *logo )
{
    WCHAR *base, *ext;
    int best = -1, best_normal = 0, best_unplated = 0, best_plain = 0;
    unsigned int i, best_size = 0;
    size_t blen, elen;

    if (!logo || !(base = strdupW( logo ))) return -1;
    _wcslwr( base );
    for (ext = base + wcslen( base ); ext > base && *ext != '.' && *ext != '/'; ext--);
    if (*ext != '.') ext = base + wcslen( base );
    elen = wcslen( ext );
    blen = ext - base;
    for (i = 0; i < package_count( pkg ); i++)
    {
        WCHAR *low = strdupW( package_name( pkg, i ) ), *quals;
        size_t len;
        unsigned int size;
        int normal, unplated, plain;

        if (!low) continue;
        _wcslwr( low );
        len = wcslen( low );
        if (len < blen + elen || wcsncmp( low, base, blen ) || wcscmp( low + len - elen, ext ))
        {
            free( low );
            continue;
        }
        low[len - elen] = 0;
        quals = low + blen;
        while (*quals == '.') quals++;
        size = qualifier_size( quals );
        unplated = size >= 256 && wcsstr( quals, L"unplated" ) && !wcsstr( quals, L"lightunplated" );
        plain = !*quals;
        /* high-contrast variants only when there is nothing else: the Python's max((unplated, size, plain)) took the
         * first of equals in zip order, which for Calculator was the white-on-black contrast icon */
        normal = !wcsstr( quals, L"contrast-" );
        if (best < 0 || normal > best_normal || (normal == best_normal && (unplated > best_unplated ||
            (unplated == best_unplated && (size > best_size || (size == best_size && plain > best_plain))))))
        {
            best = i;
            best_normal = normal;
            best_unplated = unplated;
            best_size = size;
            best_plain = plain;
        }
        free( low );
    }
    free( base );
    return best;
}

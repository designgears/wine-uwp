/*
 * appxinstall: small helpers
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

int hex_value( char c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* package paths: case-insensitive, '/' and '\' the same */
BOOL path_equal( const WCHAR *a, const WCHAR *b )
{
    for (;; a++, b++)
    {
        WCHAR ca = *a == '\\' ? '/' : towlower( *a ), cb = *b == '\\' ? '/' : towlower( *b );
        if (ca != cb) return FALSE;
        if (!ca) return TRUE;
    }
}

/* dir\name, with name's '/' as '\'; free with free() */
WCHAR *path_join( const WCHAR *dir, const WCHAR *name )
{
    size_t dlen = wcslen( dir ), nlen = wcslen( name );
    WCHAR *ret, *p;

    if (!(ret = malloc( (dlen + nlen + 2) * sizeof(WCHAR) ))) return NULL;
    memcpy( ret, dir, dlen * sizeof(WCHAR) );
    p = ret + dlen;
    if (dlen && dir[dlen - 1] != '\\' && dir[dlen - 1] != '/') *p++ = '\\';
    memcpy( p, name, (nlen + 1) * sizeof(WCHAR) );
    for (; *p; p++) if (*p == '/') *p = '\\';
    return ret;
}

WCHAR *strdupW( const WCHAR *s )
{
    WCHAR *r;
    if (!s) return NULL;
    if ((r = malloc( (wcslen( s ) + 1) * sizeof(WCHAR) ))) wcscpy( r, s );
    return r;
}

/* creates dir and its parents */
BOOL create_directories( const WCHAR *dir )
{
    WCHAR *copy, *p;
    BOOL ret = TRUE;

    if (!(copy = strdupW( dir ))) return FALSE;
    for (p = copy + 1; *p; p++)
    {
        if (*p != '\\' || p[-1] == ':') continue;
        *p = 0;
        CreateDirectoryW( copy, NULL );
        *p = '\\';
    }
    if (!CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) ret = FALSE;
    free( copy );
    return ret;
}

/* a UTF-8 copy; free with free() */
char *utf8_dup( const WCHAR *s )
{
    int n = WideCharToMultiByte( CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL );
    char *r = malloc( n );
    if (r) WideCharToMultiByte( CP_UTF8, 0, s, -1, r, n, NULL, NULL );
    return r;
}

/* the content key named by key id kid, from "kid:key;..." (hex) */
BOOL find_content_key( const char *keys, const BYTE kid[16], BYTE key[32] )
{
    const char *p = keys;

    while (p && *p)
    {
        const char *colon = strchr( p, ':' ), *end = strchr( p, ';' );
        BYTE id[16];
        unsigned int i, n;

        if (!end) end = p + strlen( p );
        if (colon && colon < end)
        {
            for (i = n = 0; p + i + 1 < colon && n < 16; i += 2)
            {
                int hi = hex_value( p[i] ), lo = hex_value( p[i + 1] );
                if (hi < 0 || lo < 0) break;
                id[n++] = hi * 16 + lo;
            }
            if (n == 16 && p + i == colon && !memcmp( id, kid, 16 ) && end - colon - 1 == 64)
            {
                for (i = 0; i < 32; i++)
                {
                    int hi = hex_value( colon[1 + 2 * i] ), lo = hex_value( colon[2 + 2 * i] );
                    if (hi < 0 || lo < 0) return FALSE;
                    key[i] = hi * 16 + lo;
                }
                return TRUE;
            }
        }
        p = *end ? end + 1 : end;
    }
    return FALSE;
}

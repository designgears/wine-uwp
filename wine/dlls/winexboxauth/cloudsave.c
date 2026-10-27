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
    if (FAILED(sign_in_silently()) || FAILED(get_token_for_url( url, FALSE, &header, TRUE ))) return 0;
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

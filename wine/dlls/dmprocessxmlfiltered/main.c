/*
 * Device management (MDM) configuration through OMA-DM SyncML
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
 * The Microsoft Store's WinStore.App.dll imports DMProcessConfigXMLFiltered statically to read device-management
 * policies, such as whether only the private store is allowed; without this DLL it does not load (c0000135).
 * Windows' DLL drags in the whole enrolment stack (dmcmnutils, dsreg, attestation, TPM provisioning). A machine with
 * no management enrolment has no such policies, so every Get is answered with SyncML status 404 (not found), which
 * is what Windows reports for a policy that was never set.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "oleauto.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmprocessxml);

/* the text between open and close at or after from, as a new string, and where close is */
static WCHAR *between( const WCHAR *from, const WCHAR *open, const WCHAR *close, const WCHAR **end )
{
    const WCHAR *a = wcsstr( from, open ), *b;
    WCHAR *r;

    if (!a) return NULL;
    a += wcslen( open );
    if (!(b = wcsstr( a, close ))) return NULL;
    if (end) *end = b;
    if ((r = malloc( (b - a + 1) * sizeof(WCHAR) )))
    {
        memcpy( r, a, (b - a) * sizeof(WCHAR) );
        r[b - a] = 0;
    }
    return r;
}

/***********************************************************************
 *          DMProcessConfigXMLFiltered (dmprocessxmlfiltered.@)
 */
HRESULT WINAPI DMProcessConfigXMLFiltered( const WCHAR *in, const WCHAR **allowed, DWORD allowed_count, BSTR *out )
{
    static const WCHAR status_fmt[] = L"<Status><CmdID>%u</CmdID><MsgRef>%s</MsgRef><CmdRef>%s</CmdRef>"
                                       L"<Cmd>Get</Cmd><Data>404</Data></Status>";
    const WCHAR *pos, *get, *end;
    WCHAR *msgid, *reply, *p;
    unsigned int count = 0, len;
    size_t size;

    TRACE( "%s %p %lu %p\n", debugstr_w(in), allowed, allowed_count, out );
    if (!in || !out) return E_INVALIDARG;
    *out = NULL;
    if (!(msgid = between( in, L"<MsgID>", L"</MsgID>", NULL )) && !(msgid = wcsdup( L"1" ))) return E_OUTOFMEMORY;
    for (pos = in; (get = wcsstr( pos, L"<Get>" )); pos = get + 5) count++;
    /* each Status at most the format, two ids and the command reference, which is shorter than the input */
    size = 64 + count * (ARRAY_SIZE(status_fmt) + 24 + wcslen( msgid )) + wcslen( in );
    if (!(reply = malloc( size * sizeof(WCHAR) ))) { free( msgid ); return E_OUTOFMEMORY; }
    p = reply + swprintf( reply, size, L"<SyncML xmlns=\"SYNCML:SYNCML1.2\"><SyncBody>" );
    count = 0;
    for (pos = in; (get = wcsstr( pos, L"<Get>" )); pos = get + 5)
    {
        WCHAR *cmd = between( get, L"<CmdID>", L"</CmdID>", &end );
        len = swprintf( p, size - (p - reply), status_fmt, ++count, msgid, cmd ? cmd : L"" );
        free( cmd );
        p += len;
    }
    swprintf( p, size - (p - reply), L"<Final/></SyncBody></SyncML>" );
    TRACE( "reply %s\n", debugstr_w(reply) );
    *out = SysAllocString( reply );
    free( reply );
    free( msgid );
    return *out ? S_OK : E_OUTOFMEMORY;
}

/***********************************************************************
 *          DMProcessConfigXML (dmprocessxmlfiltered.@)
 */
HRESULT WINAPI DMProcessConfigXML( const WCHAR *in, BSTR *out )
{
    return DMProcessConfigXMLFiltered( in, NULL, 0, out );
}

/*
 * Windows Firewall API: network isolation
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
 * Windows.Web.Http's SendRequestOperation::CreateInternalRequest calls NetworkIsolationDiagnoseConnectFailure(server)
 * for every request, before a connection exists, and fails the request on a non-zero result. It reaches it through
 * api-ms-win-net-isolation-l1-1-1, which used to be unresolved here: the delay-load stub raised and took the app down
 * with its first HttpClient request (Shapr3D). There is no AppContainer isolation to diagnose: report success.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(firewallapi);

DWORD WINAPI NetworkIsolationDiagnoseConnectFailure( const WCHAR *server )
{
    TRACE( "%s\n", debugstr_w(server) );
    return ERROR_SUCCESS;
}

/*
 * GamingServices / GamingServicesNet: stand-in NT services
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
 * Gaming Services is a platform of two auto-start services (GamingServices, LocalSystem, and GamingServicesNet,
 * LocalService) plus the kernel drivers xvdd.sys and gameflt.sys, none of which can run under Wine. Its client API is
 * answered by wine-uwp's gameplatform stand-in. The Xbox app also checks the services themselves: its full-trust
 * server starts "GamingServices" through the service control manager and fails with ERROR_SERVICE_DOES_NOT_EXIST
 * without it. So the services exist, start and stop, and do nothing else. One executable serves both names.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winsvc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(gamingservice);

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status;
static HANDLE stop_event;

static void set_state( DWORD state )
{
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus( status_handle, &status );
}

static DWORD WINAPI handler( DWORD control, DWORD type, void *data, void *context )
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        set_state( SERVICE_STOP_PENDING );
        SetEvent( stop_event );
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main( DWORD argc, WCHAR **argv )
{
    const WCHAR *name = argc ? argv[0] : L"GamingServices";

    if (!(status_handle = RegisterServiceCtrlHandlerExW( name, handler, NULL ))) return;
    stop_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    set_state( SERVICE_RUNNING );
    TRACE( "%s running\n", debugstr_w(name) );
    WaitForSingleObject( stop_event, INFINITE );
    set_state( SERVICE_STOPPED );
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    SERVICE_TABLE_ENTRYW table[] = { { (WCHAR *)L"", service_main }, { NULL, NULL } };

    return StartServiceCtrlDispatcherW( table ) ? 0 : 1;
}

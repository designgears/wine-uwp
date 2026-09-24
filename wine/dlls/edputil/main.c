/*
 * Enterprise Data Protection (Windows Information Protection) utilities
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
 * Windows ships these in edputil.dll behind ext-ms-win-edputil-policy-l1-1-0. Nothing documents
 * them. Windows' own callers (windows.applicationmodel.datatransfer's Clipboard, the Launcher,
 * the share host, twinui.appcore) ask EdpGetIsManaged() first, a no-argument BOOL, and skip every
 * other EDP call when it is FALSE. This machine has no enterprise policy, so that is the honest
 * answer. The rest answer "no context" / "no enterprise" in case a caller does not check first.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(edputil);

BOOL WINAPI EdpGetIsManaged(void)
{
    TRACE( "\n" );
    return FALSE;
}

BOOL WINAPI EdpIsUIPolicyEvaluationEnabledForThread(void)
{
    TRACE( "\n" );
    return FALSE;
}

void WINAPI EdpFreeContext( void *context )
{
    TRACE( "%p\n", context );
}

void WINAPI EdpUtilFreeEnterpriseContext( void *context )
{
    TRACE( "%p\n", context );
}

static HRESULT no_context( void **context )
{
    if (context) *context = NULL;
    return S_OK;
}

HRESULT WINAPI EdpGetContextForWindow( HWND hwnd, void **context )
{
    TRACE( "%p %p\n", hwnd, context );
    return no_context( context );
}

HRESULT WINAPI EdpGetContextForProcess( DWORD pid, void **context )
{
    TRACE( "%lu %p\n", pid, context );
    return no_context( context );
}

HRESULT WINAPI EdpGetContextForPackageFullName( const WCHAR *name, void **context )
{
    TRACE( "%s %p\n", debugstr_w(name), context );
    return no_context( context );
}

HRESULT WINAPI EdpGetContextForBinaryPath( const WCHAR *path, void **context )
{
    TRACE( "%s %p\n", debugstr_w(path), context );
    return no_context( context );
}

BOOL WINAPI EdpIsContextExemptOrEnlightenedAllowed( void *context )
{
    TRACE( "%p\n", context );
    return TRUE;
}

HRESULT WINAPI EdpCheckAccessForContext( void *context, void *arg1, void *arg2 )
{
    FIXME( "%p %p %p: no enterprise policy\n", context, arg1, arg2 );
    return S_OK;
}

HRESULT WINAPI EdpRequestAccessForContext( void *context, void *arg1, void *arg2 )
{
    FIXME( "%p %p %p: no enterprise policy\n", context, arg1, arg2 );
    return S_OK;
}

static HRESULT no_string( WCHAR **str )
{
    if (str) *str = NULL;
    return S_OK;
}

HRESULT WINAPI EdpGetEnterpriseIdForClipboard( WCHAR **id )
{
    TRACE( "%p\n", id );
    return no_string( id );
}

HRESULT WINAPI EdpGetSourceAppIdForClipboard( WCHAR **id )
{
    TRACE( "%p\n", id );
    return no_string( id );
}

HRESULT WINAPI EdpGetClipboardAccessDeniedData( void *arg, void **data )
{
    TRACE( "%p %p\n", arg, data );
    return no_context( data );
}

HRESULT WINAPI EdpGetAppLockerUniqueAppIdentifier( void *arg, WCHAR **id )
{
    TRACE( "%p %p\n", arg, id );
    return no_string( id );
}

HRESULT WINAPI EdpUtilGetEnterpriseContextForView( void *view, void **context )
{
    TRACE( "%p %p\n", view, context );
    return no_context( context );
}

BOOL WINAPI EdpShouldShowEnterpriseIndicator( void *context )
{
    TRACE( "%p\n", context );
    return FALSE;
}

HRESULT WINAPI EdpGetPrimaryIdentities( void *arg, void **identities )
{
    TRACE( "%p %p\n", arg, identities );
    return no_context( identities );
}

/*
 * Device management (MDM) policy
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
 * Windows.UI.Input.Inking.dll delay-loads PolicyManager_GetPolicy and PolicyManager_FreeGetPolicyData through
 * ext-ms-win-devmgmt-policy-l1-1-1. XAML brings the ink stack up as a viewport starts taking pointer input, and the
 * unresolved import's stub raised on XAML's UI thread: a fail-fast the first time Shapr3D's sketch viewport got a
 * click. No device-management policy is configured here, so every lookup answers "not found", as an unmanaged Windows
 * machine does, and callers take their defaults. Nothing is written through the output: callers only read it on
 * success.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(policymanager);

HRESULT WINAPI PolicyManager_GetPolicy( const WCHAR *area, const WCHAR *name, void *value )
{
    TRACE( "%s %s %p\n", debugstr_w(area), debugstr_w(name), value );
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

HRESULT WINAPI PolicyManager_FreeGetPolicyData( void *data )
{
    TRACE( "%p\n", data );
    return S_OK;   /* nothing is ever handed out */
}

HRESULT WINAPI PolicyManager_GetPolicyInt( const WCHAR *area, const WCHAR *name, int *value )
{
    TRACE( "%s %s %p\n", debugstr_w(area), debugstr_w(name), value );
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

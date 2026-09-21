/*
 * State repository cache (ext-ms-onecore-appmodel-staterepository-cache-*)
 *
 * Copyright 2026 the Wine project authors
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
 * The state repository is the database describing installed packages, maintained by the
 * StateRepository service. There is no such service here: package identity comes from the
 * environment (see kernelbase/version.c), so every cache lookup reports "no repository" and
 * callers fall back to reading the package manifest directly.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(srcache);

#define SR_E_UNAVAILABLE HRESULT_FROM_WIN32( ERROR_SERVICE_NOT_ACTIVE )

HRESULT WINAPI SRCacheManager_Open( void *params, void **manager )
{
    static int once;

    if (!once++) FIXME( "%p %p: no state repository\n", params, manager );
    if (manager) *manager = NULL;
    return SR_E_UNAVAILABLE;
}

void WINAPI SRCacheManager_Close( void *manager )
{
    TRACE( "%p\n", manager );
}

HRESULT WINAPI SRCacheManager_GetProperty_UInt32( void *manager, ULONG property, ULONG *value )
{
    TRACE( "%p %lu %p\n", manager, property, value );
    if (value) *value = 0;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheManager_SetProperty_UInt32( void *manager, ULONG property, ULONG value )
{
    TRACE( "%p %lu %lu\n", manager, property, value );
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheManager_SetProperty_UInt64( void *manager, ULONG property, ULONG64 value )
{
    TRACE( "%p %lu %s\n", manager, property, wine_dbgstr_longlong(value) );
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_Open( void *manager, const WCHAR *name, void **context )
{
    TRACE( "%p %s %p\n", manager, debugstr_w(name), context );
    if (context) *context = NULL;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_OpenSubContext( void *context, const WCHAR *name, void **sub_context )
{
    TRACE( "%p %s %p\n", context, debugstr_w(name), sub_context );
    if (sub_context) *sub_context = NULL;
    return SR_E_UNAVAILABLE;
}

void WINAPI SRCacheContext_Close( void *context )
{
    TRACE( "%p\n", context );
}

HRESULT WINAPI SRCacheContext_IsEmpty( void *context, BOOL *empty )
{
    TRACE( "%p %p\n", context, empty );
    if (empty) *empty = TRUE;
    return S_OK;
}

HRESULT WINAPI SRCacheContext_EnumerateData( void *context, void *callback, void *param )
{
    TRACE( "%p %p %p\n", context, callback, param );
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_AddToCache( void *context, void *data )
{
    TRACE( "%p %p\n", context, data );
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_GetField_UInt32( void *context, ULONG field, ULONG *value )
{
    TRACE( "%p %lu %p\n", context, field, value );
    if (value) *value = 0;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_GetField_UInt64( void *context, ULONG field, ULONG64 *value )
{
    TRACE( "%p %lu %p\n", context, field, value );
    if (value) *value = 0;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_GetField_String( void *context, ULONG field, WCHAR **value )
{
    TRACE( "%p %lu %p\n", context, field, value );
    if (value) *value = NULL;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_GetField_MultiString( void *context, ULONG field, WCHAR **value )
{
    TRACE( "%p %lu %p\n", context, field, value );
    if (value) *value = NULL;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_GetField_Binary( void *context, ULONG field, void **value, ULONG *size )
{
    TRACE( "%p %lu %p %p\n", context, field, value, size );
    if (value) *value = NULL;
    if (size) *size = 0;
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCacheContext_GetField_Guid( void *context, ULONG field, GUID *value )
{
    TRACE( "%p %lu %p\n", context, field, value );
    if (value) memset( value, 0, sizeof(*value) );
    return SR_E_UNAVAILABLE;
}

HRESULT WINAPI SRCache_AllocStringBuffer( ULONG len, WCHAR **buffer )
{
    TRACE( "%lu %p\n", len, buffer );
    if (!buffer) return E_INVALIDARG;
    if (!(*buffer = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    return S_OK;
}

HRESULT WINAPI SRCache_DuplicateString( const WCHAR *str, WCHAR **copy )
{
    SIZE_T size;

    TRACE( "%s %p\n", debugstr_w(str), copy );
    if (!copy) return E_INVALIDARG;
    if (!str) { *copy = NULL; return S_OK; }
    size = (lstrlenW( str ) + 1) * sizeof(WCHAR);
    if (!(*copy = HeapAlloc( GetProcessHeap(), 0, size ))) return E_OUTOFMEMORY;
    memcpy( *copy, str, size );
    return S_OK;
}

void WINAPI SRCache_Free( void *ptr )
{
    TRACE( "%p\n", ptr );
    HeapFree( GetProcessHeap(), 0, ptr );
}

HRESULT WINAPI SRCache_ExpandMacros( void *context, const WCHAR *str, WCHAR **expanded )
{
    TRACE( "%p %s %p\n", context, debugstr_w(str), expanded );
    return SRCache_DuplicateString( str, expanded );
}

HRESULT WINAPI SRCache_GetDefaultAccountSid( WCHAR **sid )
{
    FIXME( "%p: stub\n", sid );
    if (sid) *sid = NULL;
    return SR_E_UNAVAILABLE;
}

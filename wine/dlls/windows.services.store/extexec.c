/*
 * Windows.ApplicationModel.ExtendedExecution.ExtendedExecutionSession
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
 * Nothing registered this class, so Gears of War 4's `ref new ExtendedExecutionSession()` threw a Platform::Exception at
 * start-up, and throwing one needs vcruntime140_app's __GetPlatformExceptionInfo, a stub: the game ended there. Here an
 * app is never suspended, so an extension is always granted (Allowed) and never revoked. It lives beside StoreContext
 * because both are small stand-ins for brokered Windows classes.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(storecontext);

static const GUID IID_IExtendedExecutionSession = { 0xaf908a2d, 0x118b, 0x48f1, { 0x93, 0x08, 0x0c, 0x4f, 0xc4, 0x1e, 0x20, 0x0f } };
/* IAsyncOperation<ExtendedExecutionResult> (piid.py) */
static const GUID IID_IAsyncOperation_ExtendedExecutionResult = { 0x1dbb1bc9, 0x6cd7, 0x5947, { 0x8c, 0xd1, 0x29, 0x63, 0x2b, 0x5a, 0xa9, 0x50 } };
static const GUID IID_IClosable_ = { 0x30d5a829, 0x7fa4, 0x4026, { 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e } };
static const GUID IID_IAsyncInfo__ = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

const WCHAR extexec_class_name[] = L"Windows.ApplicationModel.ExtendedExecution.ExtendedExecutionSession";

struct session
{
    const void **vtbl;
    const void **closable_vtbl;
    LONG refs;
    INT32 reason;
    UINT32 progress;
    HSTRING description;
};

struct object { const void **vtbl; };

static const void *session_vtbl[15], *closable_vtbl[7], *factory_vtbl[7], *op_vtbl[9], *info_vtbl[11];
static struct object factory = { factory_vtbl }, op = { op_vtbl }, info = { info_vtbl };
static LONG64 next_token;

static ULONG WINAPI static_AddRef( void *iface ) { return 2; }
static ULONG WINAPI static_Release( void *iface ) { return 1; }
static HRESULT WINAPI any_GetIids( void *iface, ULONG *count, IID **iids )
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI any_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI session_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( extexec_class_name, wcslen( extexec_class_name ), name );
}

static struct session *from_closable( void *iface ) { return CONTAINING_RECORD( iface, struct session, closable_vtbl ); }

static HRESULT WINAPI session_QueryInterface( struct session *s, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IExtendedExecutionSession ))
        *out = &s->vtbl;
    else if (IsEqualGUID( iid, &IID_IClosable_ )) *out = &s->closable_vtbl;
    else
    {
        FIXME( "ExtendedExecutionSession: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &s->refs );
    return S_OK;
}
static ULONG WINAPI session_AddRef( struct session *s ) { return InterlockedIncrement( &s->refs ); }
static ULONG WINAPI session_Release( struct session *s )
{
    ULONG refs = InterlockedDecrement( &s->refs );
    if (!refs)
    {
        WindowsDeleteString( s->description );
        free( s );
    }
    return refs;
}
static HRESULT WINAPI session_get_Reason( struct session *s, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = s->reason;
    return S_OK;
}
static HRESULT WINAPI session_put_Reason( struct session *s, INT32 value )
{
    s->reason = value;
    return S_OK;
}
static HRESULT WINAPI session_get_Description( struct session *s, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( s->description, value );
}
static HRESULT WINAPI session_put_Description( struct session *s, HSTRING value )
{
    HSTRING copy;
    HRESULT hr = WindowsDuplicateString( value, &copy );
    if (FAILED(hr)) return hr;
    WindowsDeleteString( s->description );
    s->description = copy;
    return S_OK;
}
static HRESULT WINAPI session_get_PercentProgress( struct session *s, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = s->progress;
    return S_OK;
}
static HRESULT WINAPI session_put_PercentProgress( struct session *s, UINT32 value )
{
    s->progress = value;
    return S_OK;
}
static HRESULT WINAPI session_add_Revoked( struct session *s, IUnknown *handler, INT64 *token )
{
    TRACE( "handler %p: never raised.\n", handler );
    if (!token) return E_POINTER;
    *token = InterlockedIncrement64( &next_token );
    return S_OK;
}
static HRESULT WINAPI session_remove_Revoked( struct session *s, INT64 token ) { return S_OK; }
static HRESULT WINAPI session_RequestExtensionAsync( struct session *s, struct object **operation )
{
    TRACE( "session %p, reason %d -> Allowed.\n", s, s->reason );
    if (!operation) return E_POINTER;
    *operation = &op;
    return S_OK;
}

static HRESULT WINAPI closable_QueryInterface( void *iface, REFIID iid, void **out )
{
    return session_QueryInterface( from_closable( iface ), iid, out );
}
static ULONG WINAPI closable_AddRef( void *iface ) { return session_AddRef( from_closable( iface ) ); }
static ULONG WINAPI closable_Release( void *iface ) { return session_Release( from_closable( iface ) ); }
static HRESULT WINAPI closable_GetRuntimeClassName( void *iface, HSTRING *name ) { return session_GetRuntimeClassName( iface, name ); }
static HRESULT WINAPI closable_Close( void *iface ) { return S_OK; }

/* the operation: complete when handed out, its result Allowed (0) */
static HRESULT WINAPI op_QueryInterface( void *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_ExtendedExecutionResult ))
        *out = &op;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo__ )) *out = &info;
    else
    {
        FIXME( "RequestExtensionAsync operation: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}
static HRESULT WINAPI op_GetRuntimeClassName( void *iface, HSTRING *name )
{
    static const WCHAR str[] =
        L"Windows.Foundation.IAsyncOperation`1<Windows.ApplicationModel.ExtendedExecution.ExtendedExecutionResult>";
    return WindowsCreateString( str, wcslen( str ), name );
}
static HRESULT WINAPI op_put_Completed( void *iface, IUnknown *handler )
{
    HRESULT (WINAPI *invoke)( IUnknown *, void *, INT32 );

    if (!handler) return E_POINTER;
    invoke = (*(void ***)handler)[3];   /* a delegate: IUnknown, then Invoke */
    invoke( handler, &op, 1 /* Completed */ );
    return S_OK;
}
static HRESULT WINAPI op_get_Completed( void *iface, IUnknown **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    return S_OK;
}
static HRESULT WINAPI op_GetResults( void *iface, INT32 *result )
{
    if (!result) return E_POINTER;
    *result = 0;   /* ExtendedExecutionResult_Allowed */
    return S_OK;
}
static HRESULT WINAPI info_get_Id( void *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}
static HRESULT WINAPI info_get_Status( void *iface, INT32 *status )
{
    if (!status) return E_POINTER;
    *status = 1;   /* Completed */
    return S_OK;
}
static HRESULT WINAPI info_get_ErrorCode( void *iface, HRESULT *code )
{
    if (!code) return E_POINTER;
    *code = S_OK;
    return S_OK;
}
static HRESULT WINAPI info_Cancel( void *iface ) { return S_OK; }
static HRESULT WINAPI info_Close( void *iface ) { return S_OK; }

static HRESULT WINAPI factory_QueryInterface( void *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &factory;
        return S_OK;
    }
    FIXME( "ExtendedExecutionSession factory: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static HRESULT WINAPI factory_ActivateInstance( void *iface, IInspectable **instance )
{
    struct session *s;

    if (!instance) return E_POINTER;
    if (!(s = calloc( 1, sizeof(*s) ))) return E_OUTOFMEMORY;
    s->vtbl = session_vtbl;
    s->closable_vtbl = closable_vtbl;
    s->refs = 1;
    TRACE( "-> %p.\n", s );
    *instance = (IInspectable *)&s->vtbl;
    return S_OK;
}

IActivationFactory *extexec_factory( void )
{
    const void **v;

    if (factory_vtbl[0]) return (IActivationFactory *)&factory;

    v = session_vtbl;
    v[0] = session_QueryInterface; v[1] = session_AddRef; v[2] = session_Release; v[3] = any_GetIids;
    v[4] = session_GetRuntimeClassName; v[5] = any_GetTrustLevel;
    v[6] = session_get_Reason; v[7] = session_put_Reason; v[8] = session_get_Description; v[9] = session_put_Description;
    v[10] = session_get_PercentProgress; v[11] = session_put_PercentProgress; v[12] = session_add_Revoked;
    v[13] = session_remove_Revoked; v[14] = session_RequestExtensionAsync;

    v = closable_vtbl;
    v[0] = closable_QueryInterface; v[1] = closable_AddRef; v[2] = closable_Release; v[3] = any_GetIids;
    v[4] = closable_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = closable_Close;

    v = op_vtbl;
    v[0] = op_QueryInterface; v[1] = static_AddRef; v[2] = static_Release; v[3] = any_GetIids;
    v[4] = op_GetRuntimeClassName; v[5] = any_GetTrustLevel;
    v[6] = op_put_Completed; v[7] = op_get_Completed; v[8] = op_GetResults;

    v = info_vtbl;
    v[0] = op_QueryInterface; v[1] = static_AddRef; v[2] = static_Release; v[3] = any_GetIids;
    v[4] = op_GetRuntimeClassName; v[5] = any_GetTrustLevel;
    v[6] = info_get_Id; v[7] = info_get_Status; v[8] = info_get_ErrorCode; v[9] = info_Cancel; v[10] = info_Close;

    v = factory_vtbl;
    v[0] = factory_QueryInterface; v[1] = static_AddRef; v[2] = static_Release; v[3] = any_GetIids;
    v[4] = session_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = factory_ActivateInstance;

    return (IActivationFactory *)&factory;
}

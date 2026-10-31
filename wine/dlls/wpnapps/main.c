/*
 * Windows.Networking.PushNotifications.PushNotificationChannelManager
 *
 * Copyright 2026 Vibhav Pant for CodeWeavers
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
 * There is no Windows Push Notification Service here, but an app's channel request must not fail: Phantom Dust asks
 * for one in a PPL task at start-up, and CLASS_E_CLASSNOTAVAILABLE became an unobserved task exception, which
 * ppltasks.h turns into a __debugbreak in the app. So CreatePushNotificationChannelForApplicationAsync answers an
 * operation that is complete when handed out, with a channel whose URI no notification will ever arrive on. Its
 * PushNotificationReceived and the manager's ChannelsRevoked registrations are accepted and never raised. The layout
 * follows windows.services.store: static objects, hand-written vtables, IIDs from the .winmd and piid.py.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpnapps);

DEFINE_GUID( IID_IPushNotificationChannelManagerStatics, 0x8baf9b65, 0x77a1, 0x4588, 0xbd, 0x19, 0x86, 0x15, 0x29, 0xa9, 0xdc, 0xf0 );
DEFINE_GUID( IID_IPushNotificationChannelManagerStatics4, 0xbc540efb, 0x7820, 0x5a5b, 0x9c, 0x01, 0xb4, 0x75, 0x7f, 0x77, 0x40, 0x25 );
DEFINE_GUID( IID_IPushNotificationChannel, 0x2b28102e, 0xef0b, 0x4f39, 0x9b, 0x8a, 0xa3, 0xc1, 0x94, 0xde, 0x70, 0x81 );
/* IAsyncOperation<PushNotificationChannel> (piid.py) */
DEFINE_GUID( IID_IAsyncOperation_PushNotificationChannel, 0x70945a09, 0x331a, 0x5e40, 0xb8, 0x54, 0x66, 0xb7, 0xa3, 0x23, 0x3b, 0xab );
DEFINE_GUID( IID_IAsyncInfo_, 0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 );

static const WCHAR class_name[] = L"Windows.Networking.PushNotifications.PushNotificationChannelManager";

struct object
{
    const void **vtbl;
};

static const void *statics_vtbl[9], *statics4_vtbl[8], *channel_vtbl[11], *op_vtbl[9], *info_vtbl[11];
static struct object statics = { statics_vtbl }, statics4 = { statics4_vtbl }, channel = { channel_vtbl };
static struct object op = { op_vtbl }, info = { info_vtbl };
static LONG64 next_token;

/* every object is static: no reference counting */
static ULONG WINAPI object_AddRef( struct object *iface ) { return 2; }
static ULONG WINAPI object_Release( struct object *iface ) { return 1; }

static HRESULT WINAPI object_GetIids( struct object *iface, ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI object_GetTrustLevel( struct object *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI object_add_event( struct object *iface, IUnknown *handler, INT64 *token )
{
    TRACE( "handler %p, token %p: never raised.\n", handler, token );
    if (!token) return E_POINTER;
    *token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI object_remove_event( struct object *iface, INT64 token )
{
    TRACE( "token %#I64x.\n", token );
    return S_OK;
}

/* ---- the channel ---- */

static HRESULT WINAPI channel_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPushNotificationChannel ))
    {
        *out = &channel;
        return S_OK;
    }
    FIXME( "PushNotificationChannel: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static HRESULT WINAPI channel_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    static const WCHAR str[] = L"Windows.Networking.PushNotifications.PushNotificationChannel";
    return WindowsCreateString( str, wcslen( str ), name );
}
static HRESULT WINAPI channel_get_Uri( struct object *iface, HSTRING *value )
{
    static const WCHAR uri[] = L"https://wns.wine-uwp.invalid/?token=none";
    if (!value) return E_POINTER;
    return WindowsCreateString( uri, wcslen( uri ), value );
}
static HRESULT WINAPI channel_get_ExpirationTime( struct object *iface, INT64 *value )
{
    FILETIME now;

    if (!value) return E_POINTER;
    GetSystemTimeAsFileTime( &now );
    /* a real channel lasts 30 days; DateTime counts 100 ns from 1601, like FILETIME */
    *value = ((INT64)now.dwHighDateTime << 32 | now.dwLowDateTime) + (INT64)30 * 24 * 3600 * 10000000;
    return S_OK;
}
static HRESULT WINAPI channel_Close( struct object *iface ) { return S_OK; }

/* ---- the operation: complete when handed out, so .get() skips its wait and a Completed handler runs at once ---- */

static HRESULT WINAPI op_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_PushNotificationChannel ))
        *out = &op;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo_ )) *out = &info;
    else
    {
        FIXME( "channel operation: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}
static HRESULT WINAPI op_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    static const WCHAR str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Networking.PushNotifications.PushNotificationChannel>";
    return WindowsCreateString( str, wcslen( str ), name );
}
static HRESULT WINAPI op_put_Completed( struct object *iface, IUnknown *handler )
{
    HRESULT (WINAPI *invoke)( IUnknown *, struct object *, INT32 );

    if (!handler) return E_POINTER;
    invoke = (*(void ***)handler)[3];   /* a delegate: IUnknown, then Invoke */
    invoke( handler, &op, 1 /* Completed */ );
    return S_OK;
}
static HRESULT WINAPI op_get_Completed( struct object *iface, IUnknown **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    return S_OK;
}
static HRESULT WINAPI op_GetResults( struct object *iface, struct object **result )
{
    TRACE( "-> a channel nothing is pushed to.\n" );
    if (!result) return E_POINTER;
    *result = &channel;
    return S_OK;
}
static HRESULT WINAPI info_get_Id( struct object *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}
static HRESULT WINAPI info_get_Status( struct object *iface, INT32 *status )
{
    if (!status) return E_POINTER;
    *status = 1;   /* Completed */
    return S_OK;
}
static HRESULT WINAPI info_get_ErrorCode( struct object *iface, HRESULT *code )
{
    if (!code) return E_POINTER;
    *code = S_OK;
    return S_OK;
}
static HRESULT WINAPI info_Cancel( struct object *iface ) { return S_OK; }
static HRESULT WINAPI info_Close( struct object *iface ) { return S_OK; }

/* ---- the manager ---- */

static HRESULT WINAPI statics_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ) || IsEqualGUID( iid, &IID_IPushNotificationChannelManagerStatics ))
        *out = &statics;
    else if (IsEqualGUID( iid, &IID_IPushNotificationChannelManagerStatics4 )) *out = &statics4;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}
static HRESULT WINAPI statics_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

/* IActivationFactory::ActivateInstance and CreatePushNotificationChannelForApplicationAsync share slot 6; a static
 * class is never activated, so the slot answers the channel request */
static HRESULT WINAPI statics_CreateChannelForApplicationAsync( struct object *iface, struct object **operation )
{
    TRACE( "operation %p.\n", operation );
    if (!operation) return E_POINTER;
    *operation = &op;
    return S_OK;
}
static HRESULT WINAPI statics_CreateChannelForApplicationAsyncWithId( struct object *iface, HSTRING id,
                                                                      struct object **operation )
{
    TRACE( "id %s, operation %p.\n", debugstr_hstring( id ), operation );
    return statics_CreateChannelForApplicationAsync( iface, operation );
}
static HRESULT WINAPI statics_CreateChannelForSecondaryTileAsync( struct object *iface, HSTRING tile,
                                                                  struct object **operation )
{
    TRACE( "tile %s, operation %p.\n", debugstr_hstring( tile ), operation );
    return statics_CreateChannelForApplicationAsync( iface, operation );
}

static void init_vtables(void)
{
    const void **v;

    v = statics_vtbl;
    v[0] = statics_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = statics_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = statics_CreateChannelForApplicationAsync;
    v[7] = statics_CreateChannelForApplicationAsyncWithId;
    v[8] = statics_CreateChannelForSecondaryTileAsync;

    v = statics4_vtbl;
    v[0] = statics_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = statics_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = object_add_event;            /* ChannelsRevoked */
    v[7] = object_remove_event;

    v = channel_vtbl;
    v[0] = channel_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = channel_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = channel_get_Uri; v[7] = channel_get_ExpirationTime; v[8] = channel_Close;
    v[9] = object_add_event;            /* PushNotificationReceived */
    v[10] = object_remove_event;

    v = op_vtbl;
    v[0] = op_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = op_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = op_put_Completed; v[7] = op_get_Completed; v[8] = op_GetResults;

    v = info_vtbl;
    v[0] = op_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = op_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = info_get_Id; v[7] = info_get_Status; v[8] = info_get_ErrorCode;
    v[9] = info_Cancel; v[10] = info_Close;
}

HRESULT WINAPI DllGetClassObject( const CLSID *clsid, const IID *riid, void **out )
{
    FIXME( "(%s, %s, %p): stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "(%s, %p)\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), class_name )) return CLASS_E_CLASSNOTAVAILABLE;
    if (!statics_vtbl[0]) init_vtables();
    *factory = (IActivationFactory *)&statics;
    return S_OK;
}

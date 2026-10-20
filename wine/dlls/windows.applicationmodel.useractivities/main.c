/*
 * Windows.ApplicationModel.UserActivities.UserActivityChannel
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
 * On Windows the channel comes from twinapi.appcore through cdprt, the Connected Devices Platform, which needs a
 * cdp.dll service that a prefix does not have; the exception that raised escaped an EngineAppStartup subscriber in
 * Solitaire and ended its engine thread. Here the default channel can be had, and its timeline operations fail
 * plainly until there is a local activity store: never a successful operation with no activity in it.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_ApplicationModel_UserActivities
#include "windows.foundation.h"
#include "windows.applicationmodel.useractivities.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(useractivities);

static const WCHAR class_name[] = L"Windows.ApplicationModel.UserActivities.UserActivityChannel";

struct activity_statics
{
    IActivationFactory IActivationFactory_iface;
    IUserActivityChannelStatics IUserActivityChannelStatics_iface;
    IUserActivityChannel IUserActivityChannel_iface;
};

static struct activity_statics impl;

/* the factory, the statics and the channel are one static object: the channel's IIDs answer for the channel */
static HRESULT query_interface( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IUserActivityChannelStatics ))
        *out = &impl.IUserActivityChannelStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT get_class_name( HSTRING *name )
{
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    return query_interface( iid, out );
}
static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* channels come from GetDefault */
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static HRESULT WINAPI statics_QueryInterface( IUserActivityChannelStatics *iface, REFIID iid, void **out )
{
    return query_interface( iid, out );
}
static ULONG WINAPI statics_AddRef( IUserActivityChannelStatics *iface ) { return 2; }
static ULONG WINAPI statics_Release( IUserActivityChannelStatics *iface ) { return 1; }
static HRESULT WINAPI statics_GetIids( IUserActivityChannelStatics *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI statics_GetRuntimeClassName( IUserActivityChannelStatics *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI statics_GetTrustLevel( IUserActivityChannelStatics *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI statics_GetDefault( IUserActivityChannelStatics *iface, IUserActivityChannel **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = &impl.IUserActivityChannel_iface;
    return S_OK;
}

static const struct IUserActivityChannelStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IUserActivityChannelStatics methods */
    statics_GetDefault,
};

static HRESULT WINAPI channel_QueryInterface( IUserActivityChannel *iface, REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IUserActivityChannel ))
    {
        *out = iface;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI channel_AddRef( IUserActivityChannel *iface ) { return 2; }
static ULONG WINAPI channel_Release( IUserActivityChannel *iface ) { return 1; }
static HRESULT WINAPI channel_GetIids( IUserActivityChannel *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI channel_GetRuntimeClassName( IUserActivityChannel *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI channel_GetTrustLevel( IUserActivityChannel *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI channel_GetOrCreateUserActivityAsync( IUserActivityChannel *iface, HSTRING id,
                                                            IAsyncOperation_UserActivity **operation )
{
    FIXME( "iface %p, id %s, operation %p: no activity store.\n", iface, debugstr_hstring( id ), operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI channel_DeleteActivityAsync( IUserActivityChannel *iface, HSTRING id, IAsyncAction **operation )
{
    FIXME( "iface %p, id %s, operation %p: no activity store.\n", iface, debugstr_hstring( id ), operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI channel_DeleteAllActivitiesAsync( IUserActivityChannel *iface, IAsyncAction **operation )
{
    FIXME( "iface %p, operation %p: no activity store.\n", iface, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IUserActivityChannelVtbl channel_vtbl =
{
    channel_QueryInterface,
    channel_AddRef,
    channel_Release,
    /* IInspectable methods */
    channel_GetIids,
    channel_GetRuntimeClassName,
    channel_GetTrustLevel,
    /* IUserActivityChannel methods */
    channel_GetOrCreateUserActivityAsync,
    channel_DeleteActivityAsync,
    channel_DeleteAllActivitiesAsync,
};

static struct activity_statics impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&channel_vtbl},
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), class_name )) return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &impl.IActivationFactory_iface;
    return S_OK;
}

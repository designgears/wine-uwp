/*
 * Windows.UI.Notifications.ToastNotificationManager
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
 * Notifications go through the Windows Push Notification platform, a service a prefix does not have, and Windows'
 * CreateToastNotifier fails with WPN_E_PLATFORM_UNAVAILABLE without it; .NET Native and C++/CX callers turn that
 * into a throw. Here the notifier exists and reports DisabledForUser, which apps test before showing anything, and
 * accepts and drops what it is given: refusing Show or AddToSchedule made Solitaire's boot sequence stop on a throw
 * that never completed its bootflow item. Nothing is ever scheduled.
 *
 * Windows registers the class in wpnapps.dll, with the other notification classes. This module is not called that:
 * a builtin wins over a native DLL of the same name, and the apps' other notification classes (ScheduledToastNotification,
 * which Solitaire creates) come from Windows' own wpnapps.
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
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Data_Xml_Dom
#define WIDL_using_Windows_UI_Notifications
#include "windows.foundation.h"
#include "windows.data.xml.dom.h"
#include "windows.ui.notifications.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(toast);

static const WCHAR manager_name[] = L"Windows.UI.Notifications.ToastNotificationManager";
static const WCHAR notifier_name[] = L"Windows.UI.Notifications.ToastNotifier";
/* .NET Native types the RCW from this name, so it has to be the parameterised type's */
static const WCHAR scheduled_name[] =
    L"Windows.Foundation.Collections.IVectorView`1<Windows.UI.Notifications.ScheduledToastNotification>";

static struct
{
    IActivationFactory IActivationFactory_iface;
    IToastNotificationManagerStatics IToastNotificationManagerStatics_iface;
    IToastNotifier IToastNotifier_iface;
    IVectorView_ScheduledToastNotification IVectorView_ScheduledToastNotification_iface;
} impl;

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT make_name( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

static BOOL is_base_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject );
}

static HRESULT no_interface( REFIID iid, void **out )
{
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

/* ---- the factory and its statics ---- */

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &impl.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationManagerStatics )) *out = &impl.IToastNotificationManagerStatics_iface;
    else return no_interface( iid, out );
    return S_OK;
}
static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return make_name( manager_name, name ); }
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* a static class */
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

static HRESULT WINAPI statics_QueryInterface( IToastNotificationManagerStatics *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &impl.IActivationFactory_iface, iid, out );
}
static ULONG WINAPI statics_AddRef( IToastNotificationManagerStatics *iface ) { return 2; }
static ULONG WINAPI statics_Release( IToastNotificationManagerStatics *iface ) { return 1; }
static HRESULT WINAPI statics_GetIids( IToastNotificationManagerStatics *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI statics_GetRuntimeClassName( IToastNotificationManagerStatics *iface, HSTRING *name ) { return make_name( manager_name, name ); }
static HRESULT WINAPI statics_GetTrustLevel( IToastNotificationManagerStatics *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI statics_CreateToastNotifier( IToastNotificationManagerStatics *iface, IToastNotifier **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = &impl.IToastNotifier_iface;
    return S_OK;
}

static HRESULT WINAPI statics_CreateToastNotifierWithId( IToastNotificationManagerStatics *iface, HSTRING id, IToastNotifier **result )
{
    TRACE( "iface %p, id %s, result %p.\n", iface, debugstr_hstring( id ), result );
    return statics_CreateToastNotifier( iface, result );
}

static HRESULT WINAPI statics_GetTemplateContent( IToastNotificationManagerStatics *iface, ToastTemplateType type,
                                                  IXmlDocument **result )
{
    FIXME( "iface %p, type %d, result %p: no templates.\n", iface, type, result );
    if (!result) return E_POINTER;
    *result = NULL;
    return E_NOTIMPL;
}

static const struct IToastNotificationManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IToastNotificationManagerStatics methods */
    statics_CreateToastNotifier,
    statics_CreateToastNotifierWithId,
    statics_GetTemplateContent,
};

/* ---- the notifier ---- */

static HRESULT WINAPI notifier_QueryInterface( IToastNotifier *iface, REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_IToastNotifier )) return no_interface( iid, out );
    *out = iface;
    return S_OK;
}
static ULONG WINAPI notifier_AddRef( IToastNotifier *iface ) { return 2; }
static ULONG WINAPI notifier_Release( IToastNotifier *iface ) { return 1; }
static HRESULT WINAPI notifier_GetIids( IToastNotifier *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI notifier_GetRuntimeClassName( IToastNotifier *iface, HSTRING *name ) { return make_name( notifier_name, name ); }
static HRESULT WINAPI notifier_GetTrustLevel( IToastNotifier *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI notifier_Show( IToastNotifier *iface, IToastNotification *notification )
{
    TRACE( "iface %p, notification %p: dropped.\n", iface, notification );
    return S_OK;
}

static HRESULT WINAPI notifier_Hide( IToastNotifier *iface, IToastNotification *notification )
{
    TRACE( "iface %p, notification %p.\n", iface, notification );
    return S_OK;
}

static HRESULT WINAPI notifier_get_Setting( IToastNotifier *iface, NotificationSetting *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NotificationSetting_DisabledForUser;
    return S_OK;
}

static HRESULT WINAPI notifier_AddToSchedule( IToastNotifier *iface, IScheduledToastNotification *toast )
{
    TRACE( "iface %p, toast %p: dropped.\n", iface, toast );
    return S_OK;
}

static HRESULT WINAPI notifier_RemoveFromSchedule( IToastNotifier *iface, IScheduledToastNotification *toast )
{
    TRACE( "iface %p, toast %p.\n", iface, toast );
    return S_OK;
}

static HRESULT WINAPI notifier_GetScheduledToastNotifications( IToastNotifier *iface,
                                                                IVectorView_ScheduledToastNotification **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = &impl.IVectorView_ScheduledToastNotification_iface;
    return S_OK;
}

static const struct IToastNotifierVtbl notifier_vtbl =
{
    notifier_QueryInterface,
    notifier_AddRef,
    notifier_Release,
    /* IInspectable methods */
    notifier_GetIids,
    notifier_GetRuntimeClassName,
    notifier_GetTrustLevel,
    /* IToastNotifier methods */
    notifier_Show,
    notifier_Hide,
    notifier_get_Setting,
    notifier_AddToSchedule,
    notifier_RemoveFromSchedule,
    notifier_GetScheduledToastNotifications,
};

/* ---- the (always empty) list of scheduled toasts ---- */

static HRESULT WINAPI scheduled_QueryInterface( IVectorView_ScheduledToastNotification *iface, REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_IVectorView_ScheduledToastNotification )) return no_interface( iid, out );
    *out = iface;
    return S_OK;
}
static ULONG WINAPI scheduled_AddRef( IVectorView_ScheduledToastNotification *iface ) { return 2; }
static ULONG WINAPI scheduled_Release( IVectorView_ScheduledToastNotification *iface ) { return 1; }
static HRESULT WINAPI scheduled_GetIids( IVectorView_ScheduledToastNotification *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI scheduled_GetRuntimeClassName( IVectorView_ScheduledToastNotification *iface, HSTRING *name ) { return make_name( scheduled_name, name ); }
static HRESULT WINAPI scheduled_GetTrustLevel( IVectorView_ScheduledToastNotification *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI scheduled_GetAt( IVectorView_ScheduledToastNotification *iface, UINT32 index, IScheduledToastNotification **value )
{
    if (value) *value = NULL;
    return E_BOUNDS;
}

static HRESULT WINAPI scheduled_get_Size( IVectorView_ScheduledToastNotification *iface, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI scheduled_IndexOf( IVectorView_ScheduledToastNotification *iface, IScheduledToastNotification *element,
                                         UINT32 *index, boolean *found )
{
    if (index) *index = 0;
    if (found) *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI scheduled_GetMany( IVectorView_ScheduledToastNotification *iface, UINT32 start, UINT32 size,
                                         IScheduledToastNotification **items, UINT32 *count )
{
    if (count) *count = 0;
    return S_OK;
}

static const struct IVectorView_ScheduledToastNotificationVtbl scheduled_vtbl =
{
    scheduled_QueryInterface,
    scheduled_AddRef,
    scheduled_Release,
    /* IInspectable methods */
    scheduled_GetIids,
    scheduled_GetRuntimeClassName,
    scheduled_GetTrustLevel,
    /* IVectorView<ScheduledToastNotification> methods */
    scheduled_GetAt,
    scheduled_get_Size,
    scheduled_IndexOf,
    scheduled_GetMany,
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    impl.IActivationFactory_iface.lpVtbl = &factory_vtbl;
    impl.IToastNotificationManagerStatics_iface.lpVtbl = &statics_vtbl;
    impl.IToastNotifier_iface.lpVtbl = &notifier_vtbl;
    impl.IVectorView_ScheduledToastNotification_iface.lpVtbl = &scheduled_vtbl;

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), manager_name )) return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &impl.IActivationFactory_iface;
    return S_OK;
}

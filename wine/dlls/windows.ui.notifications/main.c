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
 * a builtin wins over a native DLL of the same name. Since Wine has a wpnapps of its own (push channels), Windows'
 * ScheduledToastNotification, which Solitaire creates, is not reachable there either, so it lives here too.
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


/* ---- ScheduledToastNotification ----
 *
 * Windows has it in wpnapps.dll, which a builtin of that name (push channels) now shadows. Solitaire builds one for
 * every reminder it schedules; with no class the C++/CX call threw and the request it belonged to never completed,
 * so its front end never came up. The object keeps what it is given; AddToSchedule drops it like any other. */

static const WCHAR scheduled_toast_name[] = L"Windows.UI.Notifications.ScheduledToastNotification";

struct scheduled_toast
{
    IScheduledToastNotification IScheduledToastNotification_iface;
    IScheduledToastNotification2 IScheduledToastNotification2_iface;
    IScheduledToastNotification3 IScheduledToastNotification3_iface;
    IScheduledToastNotification4 IScheduledToastNotification4_iface;
    LONG ref;
    IXmlDocument *content;
    DateTime delivery;
    TimeSpan snooze;
    BOOL recurring;
    UINT32 max_snooze;
    HSTRING id, tag, group, remote_id;
    boolean suppress;
    NotificationMirroring mirroring;
    IReference_DateTime *expiration;
};

#define TOAST_IMPL(n) \
    static inline struct scheduled_toast *impl_from_##n( n *iface ) { return CONTAINING_RECORD( iface, struct scheduled_toast, n##_iface ); } \
    static HRESULT WINAPI tx_##n##_QueryInterface( n *iface, REFIID iid, void **out ) \
    { return IScheduledToastNotification_QueryInterface( &impl_from_##n( iface )->IScheduledToastNotification_iface, iid, out ); } \
    static ULONG WINAPI tx_##n##_AddRef( n *iface ) { return IScheduledToastNotification_AddRef( &impl_from_##n( iface )->IScheduledToastNotification_iface ); } \
    static ULONG WINAPI tx_##n##_Release( n *iface ) { return IScheduledToastNotification_Release( &impl_from_##n( iface )->IScheduledToastNotification_iface ); } \
    static HRESULT WINAPI tx_##n##_GetIids( n *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); } \
    static HRESULT WINAPI tx_##n##_GetRuntimeClassName( n *iface, HSTRING *name ) { return make_name( scheduled_toast_name, name ); } \
    static HRESULT WINAPI tx_##n##_GetTrustLevel( n *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static inline struct scheduled_toast *impl_from_IScheduledToastNotification( IScheduledToastNotification *iface )
{
    return CONTAINING_RECORD( iface, struct scheduled_toast, IScheduledToastNotification_iface );
}

static HRESULT WINAPI toast_QueryInterface( IScheduledToastNotification *iface, REFIID iid, void **out )
{
    struct scheduled_toast *impl = impl_from_IScheduledToastNotification( iface );

    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IScheduledToastNotification )) *out = &impl->IScheduledToastNotification_iface;
    else if (IsEqualGUID( iid, &IID_IScheduledToastNotification2 )) *out = &impl->IScheduledToastNotification2_iface;
    else if (IsEqualGUID( iid, &IID_IScheduledToastNotification3 )) *out = &impl->IScheduledToastNotification3_iface;
    else if (IsEqualGUID( iid, &IID_IScheduledToastNotification4 )) *out = &impl->IScheduledToastNotification4_iface;
    else return no_interface( iid, out );
    IScheduledToastNotification_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI toast_AddRef( IScheduledToastNotification *iface )
{
    return InterlockedIncrement( &impl_from_IScheduledToastNotification( iface )->ref );
}

static ULONG WINAPI toast_Release( IScheduledToastNotification *iface )
{
    struct scheduled_toast *impl = impl_from_IScheduledToastNotification( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->content) IXmlDocument_Release( impl->content );
        if (impl->expiration) IReference_DateTime_Release( impl->expiration );
        WindowsDeleteString( impl->id );
        WindowsDeleteString( impl->tag );
        WindowsDeleteString( impl->group );
        WindowsDeleteString( impl->remote_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI toast_GetIids( IScheduledToastNotification *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI toast_GetRuntimeClassName( IScheduledToastNotification *iface, HSTRING *name ) { return make_name( scheduled_toast_name, name ); }
static HRESULT WINAPI toast_GetTrustLevel( IScheduledToastNotification *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI toast_get_Content( IScheduledToastNotification *iface, IXmlDocument **value )
{
    struct scheduled_toast *impl = impl_from_IScheduledToastNotification( iface );
    if ((*value = impl->content)) IXmlDocument_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI toast_get_DeliveryTime( IScheduledToastNotification *iface, DateTime *value )
{
    *value = impl_from_IScheduledToastNotification( iface )->delivery;
    return S_OK;
}

static HRESULT WINAPI toast_get_SnoozeInterval( IScheduledToastNotification *iface, IReference_TimeSpan **value )
{
    /* no IReference<TimeSpan> to box it in here; a snooze-less toast answers NULL on Windows too */
    FIXME( "iface %p, value %p: no snooze interval.\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI toast_get_MaximumSnoozeCount( IScheduledToastNotification *iface, UINT32 *value )
{
    *value = impl_from_IScheduledToastNotification( iface )->max_snooze;
    return S_OK;
}

static HRESULT set_string( HSTRING *field, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *field );
    *field = copy;
    return S_OK;
}

static HRESULT WINAPI toast_put_Id( IScheduledToastNotification *iface, HSTRING value )
{
    return set_string( &impl_from_IScheduledToastNotification( iface )->id, value );
}

static HRESULT WINAPI toast_get_Id( IScheduledToastNotification *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IScheduledToastNotification( iface )->id, value );
}

static const struct IScheduledToastNotificationVtbl toast_vtbl =
{
    toast_QueryInterface,
    toast_AddRef,
    toast_Release,
    toast_GetIids,
    toast_GetRuntimeClassName,
    toast_GetTrustLevel,
    toast_get_Content,
    toast_get_DeliveryTime,
    toast_get_SnoozeInterval,
    toast_get_MaximumSnoozeCount,
    toast_put_Id,
    toast_get_Id,
};

TOAST_IMPL(IScheduledToastNotification2)

static HRESULT WINAPI toast2_put_Tag( IScheduledToastNotification2 *iface, HSTRING value )
{
    return set_string( &impl_from_IScheduledToastNotification2( iface )->tag, value );
}
static HRESULT WINAPI toast2_get_Tag( IScheduledToastNotification2 *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IScheduledToastNotification2( iface )->tag, value );
}
static HRESULT WINAPI toast2_put_Group( IScheduledToastNotification2 *iface, HSTRING value )
{
    return set_string( &impl_from_IScheduledToastNotification2( iface )->group, value );
}
static HRESULT WINAPI toast2_get_Group( IScheduledToastNotification2 *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IScheduledToastNotification2( iface )->group, value );
}
static HRESULT WINAPI toast2_put_SuppressPopup( IScheduledToastNotification2 *iface, boolean value )
{
    impl_from_IScheduledToastNotification2( iface )->suppress = value;
    return S_OK;
}
static HRESULT WINAPI toast2_get_SuppressPopup( IScheduledToastNotification2 *iface, boolean *value )
{
    *value = impl_from_IScheduledToastNotification2( iface )->suppress;
    return S_OK;
}

static const struct IScheduledToastNotification2Vtbl toast2_vtbl =
{
    tx_IScheduledToastNotification2_QueryInterface,
    tx_IScheduledToastNotification2_AddRef,
    tx_IScheduledToastNotification2_Release,
    tx_IScheduledToastNotification2_GetIids,
    tx_IScheduledToastNotification2_GetRuntimeClassName,
    tx_IScheduledToastNotification2_GetTrustLevel,
    toast2_put_Tag,
    toast2_get_Tag,
    toast2_put_Group,
    toast2_get_Group,
    toast2_put_SuppressPopup,
    toast2_get_SuppressPopup,
};

TOAST_IMPL(IScheduledToastNotification3)

static HRESULT WINAPI toast3_get_NotificationMirroring( IScheduledToastNotification3 *iface, NotificationMirroring *value )
{
    *value = impl_from_IScheduledToastNotification3( iface )->mirroring;
    return S_OK;
}
static HRESULT WINAPI toast3_put_NotificationMirroring( IScheduledToastNotification3 *iface, NotificationMirroring value )
{
    impl_from_IScheduledToastNotification3( iface )->mirroring = value;
    return S_OK;
}
static HRESULT WINAPI toast3_get_RemoteId( IScheduledToastNotification3 *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IScheduledToastNotification3( iface )->remote_id, value );
}
static HRESULT WINAPI toast3_put_RemoteId( IScheduledToastNotification3 *iface, HSTRING value )
{
    return set_string( &impl_from_IScheduledToastNotification3( iface )->remote_id, value );
}

static const struct IScheduledToastNotification3Vtbl toast3_vtbl =
{
    tx_IScheduledToastNotification3_QueryInterface,
    tx_IScheduledToastNotification3_AddRef,
    tx_IScheduledToastNotification3_Release,
    tx_IScheduledToastNotification3_GetIids,
    tx_IScheduledToastNotification3_GetRuntimeClassName,
    tx_IScheduledToastNotification3_GetTrustLevel,
    toast3_get_NotificationMirroring,
    toast3_put_NotificationMirroring,
    toast3_get_RemoteId,
    toast3_put_RemoteId,
};

TOAST_IMPL(IScheduledToastNotification4)

static HRESULT WINAPI toast4_get_ExpirationTime( IScheduledToastNotification4 *iface, IReference_DateTime **value )
{
    struct scheduled_toast *impl = impl_from_IScheduledToastNotification4( iface );
    if ((*value = impl->expiration)) IReference_DateTime_AddRef( *value );
    return S_OK;
}
static HRESULT WINAPI toast4_put_ExpirationTime( IScheduledToastNotification4 *iface, IReference_DateTime *value )
{
    struct scheduled_toast *impl = impl_from_IScheduledToastNotification4( iface );
    if (value) IReference_DateTime_AddRef( value );
    if (impl->expiration) IReference_DateTime_Release( impl->expiration );
    impl->expiration = value;
    return S_OK;
}

static const struct IScheduledToastNotification4Vtbl toast4_vtbl =
{
    tx_IScheduledToastNotification4_QueryInterface,
    tx_IScheduledToastNotification4_AddRef,
    tx_IScheduledToastNotification4_Release,
    tx_IScheduledToastNotification4_GetIids,
    tx_IScheduledToastNotification4_GetRuntimeClassName,
    tx_IScheduledToastNotification4_GetTrustLevel,
    toast4_get_ExpirationTime,
    toast4_put_ExpirationTime,
};

static HRESULT scheduled_toast_create( IXmlDocument *content, DateTime delivery, const TimeSpan *snooze, UINT32 max_snooze,
                                       IScheduledToastNotification **value )
{
    struct scheduled_toast *impl;

    if (!value) return E_POINTER;
    if (!content) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IScheduledToastNotification_iface.lpVtbl = &toast_vtbl;
    impl->IScheduledToastNotification2_iface.lpVtbl = &toast2_vtbl;
    impl->IScheduledToastNotification3_iface.lpVtbl = &toast3_vtbl;
    impl->IScheduledToastNotification4_iface.lpVtbl = &toast4_vtbl;
    impl->ref = 1;
    IXmlDocument_AddRef( (impl->content = content) );
    impl->delivery = delivery;
    if (snooze)
    {
        impl->snooze = *snooze;
        impl->recurring = TRUE;
        impl->max_snooze = max_snooze;
    }
    *value = &impl->IScheduledToastNotification_iface;
    TRACE( "created %p, delivery %I64d.\n", impl, delivery.UniversalTime );
    return S_OK;
}

static struct
{
    IActivationFactory IActivationFactory_iface;
    IScheduledToastNotificationFactory IScheduledToastNotificationFactory_iface;
} scheduled_factory;

static HRESULT WINAPI sfactory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &scheduled_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IScheduledToastNotificationFactory ))
        *out = &scheduled_factory.IScheduledToastNotificationFactory_iface;
    else return no_interface( iid, out );
    return S_OK;
}
static ULONG WINAPI sfactory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI sfactory_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI sfactory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI sfactory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return make_name( scheduled_toast_name, name ); }
static HRESULT WINAPI sfactory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI sfactory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* the class is activatable only through its factory interface */
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl sfactory_vtbl =
{
    sfactory_QueryInterface,
    sfactory_AddRef,
    sfactory_Release,
    sfactory_GetIids,
    sfactory_GetRuntimeClassName,
    sfactory_GetTrustLevel,
    sfactory_ActivateInstance,
};

static HRESULT WINAPI screate_QueryInterface( IScheduledToastNotificationFactory *iface, REFIID iid, void **out )
{
    return sfactory_QueryInterface( &scheduled_factory.IActivationFactory_iface, iid, out );
}
static ULONG WINAPI screate_AddRef( IScheduledToastNotificationFactory *iface ) { return 2; }
static ULONG WINAPI screate_Release( IScheduledToastNotificationFactory *iface ) { return 1; }
static HRESULT WINAPI screate_GetIids( IScheduledToastNotificationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI screate_GetRuntimeClassName( IScheduledToastNotificationFactory *iface, HSTRING *name ) { return make_name( scheduled_toast_name, name ); }
static HRESULT WINAPI screate_GetTrustLevel( IScheduledToastNotificationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI screate_CreateScheduledToastNotification( IScheduledToastNotificationFactory *iface, IXmlDocument *content,
                                                                DateTime delivery, IScheduledToastNotification **value )
{
    TRACE( "content %p, delivery %I64d, value %p.\n", content, delivery.UniversalTime, value );
    return scheduled_toast_create( content, delivery, NULL, 0, value );
}

static HRESULT WINAPI screate_CreateScheduledToastNotificationRecurring( IScheduledToastNotificationFactory *iface, IXmlDocument *content,
                                                                         DateTime delivery, TimeSpan snooze, UINT32 max_snooze,
                                                                         IScheduledToastNotification **value )
{
    TRACE( "content %p, delivery %I64d, snooze %I64d, max %u, value %p.\n", content, delivery.UniversalTime, snooze.Duration,
           max_snooze, value );
    return scheduled_toast_create( content, delivery, &snooze, max_snooze, value );
}

static const struct IScheduledToastNotificationFactoryVtbl screate_vtbl =
{
    screate_QueryInterface,
    screate_AddRef,
    screate_Release,
    screate_GetIids,
    screate_GetRuntimeClassName,
    screate_GetTrustLevel,
    screate_CreateScheduledToastNotification,
    screate_CreateScheduledToastNotificationRecurring,
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

    scheduled_factory.IActivationFactory_iface.lpVtbl = &sfactory_vtbl;
    scheduled_factory.IScheduledToastNotificationFactory_iface.lpVtbl = &screate_vtbl;

    *factory = NULL;
    if (!wcscmp( WindowsGetStringRawBuffer( classid, NULL ), scheduled_toast_name ))
    {
        *factory = &scheduled_factory.IActivationFactory_iface;
        return S_OK;
    }
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), manager_name )) return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &impl.IActivationFactory_iface;
    return S_OK;
}

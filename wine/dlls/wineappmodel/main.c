/*
 * App model classes whose Windows implementations need services a prefix does not have:
 * Windows.ApplicationModel.Background.BackgroundExecutionManager and Windows.ApplicationModel.AppInstance
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
 * On Windows the class is biwinrt.dll's, a client of the Background Infrastructure service, which a prefix does not
 * have: nothing here would ever run an app's background task. So access is answered truthfully as denied by system
 * policy, and every request completes at once. Without the class at all, WhatsApp (a WinUI 3 app) threw
 * CLASS_E_CLASSNOTAVAILABLE out of its push-task setup.
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
#define WIDL_using_Windows_ApplicationModel
#define WIDL_using_Windows_ApplicationModel_Activation
#define WIDL_using_Windows_ApplicationModel_Background
#include "windows.foundation.h"
#include "windows.applicationmodel.h"
#include "windows.applicationmodel.activation.h"
#include "windows.applicationmodel.background.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appmodel);

static const WCHAR class_name[] = L"Windows.ApplicationModel.Background.BackgroundExecutionManager";

/* what every request and status query answers */
static const BackgroundAccessStatus access_status = BackgroundAccessStatus_DeniedBySystemPolicy;

/* ---- completed operations: IAsyncOperation<BackgroundAccessStatus> and IAsyncOperation<boolean> ---- */

struct completed_op
{
    IAsyncOperation_BackgroundAccessStatus IAsyncOperation_BackgroundAccessStatus_iface;
    IAsyncOperation_boolean IAsyncOperation_boolean_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    BOOL is_bool;
    INT32 value;
    IUnknown *handler;
};

static struct completed_op *impl_from_status_op( IAsyncOperation_BackgroundAccessStatus *iface )
{
    return CONTAINING_RECORD( iface, struct completed_op, IAsyncOperation_BackgroundAccessStatus_iface );
}

static struct completed_op *impl_from_bool_op( IAsyncOperation_boolean *iface )
{
    return CONTAINING_RECORD( iface, struct completed_op, IAsyncOperation_boolean_iface );
}

static struct completed_op *impl_from_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct completed_op, IAsyncInfo_iface );
}

static IInspectable *op_primary( struct completed_op *op )
{
    return op->is_bool ? (IInspectable *)&op->IAsyncOperation_boolean_iface
                       : (IInspectable *)&op->IAsyncOperation_BackgroundAccessStatus_iface;
}

static HRESULT op_query_interface( struct completed_op *op, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        (op->is_bool && IsEqualGUID( iid, &IID_IAsyncOperation_boolean )) ||
        (!op->is_bool && IsEqualGUID( iid, &IID_IAsyncOperation_BackgroundAccessStatus )))
        *out = op_primary( op );
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &op->IAsyncInfo_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG op_addref( struct completed_op *op )
{
    return InterlockedIncrement( &op->ref );
}

static ULONG op_release( struct completed_op *op )
{
    ULONG ref = InterlockedDecrement( &op->ref );
    if (!ref)
    {
        if (op->handler) IUnknown_Release( op->handler );
        free( op );
    }
    return ref;
}

/* the handler of either kind is a delegate whose Invoke is slot 3: (operation, status) */
static HRESULT op_put_completed( struct completed_op *op, IUnknown *handler )
{
    typedef HRESULT (WINAPI *invoke_func)( IUnknown *, IInspectable *, AsyncStatus );

    TRACE( "op %p, handler %p.\n", op, handler );
    if (!handler) return E_POINTER;
    if (op->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    op->handler = handler;
    IUnknown_AddRef( handler );
    return ((invoke_func)(*(void ***)handler)[3])( handler, op_primary( op ), Completed );
}

static HRESULT op_get_completed( struct completed_op *op, IUnknown **handler )
{
    if (!handler) return E_POINTER;
    if ((*handler = op->handler)) IUnknown_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI status_op_QueryInterface( IAsyncOperation_BackgroundAccessStatus *iface, REFIID iid, void **out )
{
    return op_query_interface( impl_from_status_op( iface ), iid, out );
}
static ULONG WINAPI status_op_AddRef( IAsyncOperation_BackgroundAccessStatus *iface )
{
    return op_addref( impl_from_status_op( iface ) );
}
static ULONG WINAPI status_op_Release( IAsyncOperation_BackgroundAccessStatus *iface )
{
    return op_release( impl_from_status_op( iface ) );
}
static HRESULT WINAPI status_op_GetIids( IAsyncOperation_BackgroundAccessStatus *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI status_op_GetRuntimeClassName( IAsyncOperation_BackgroundAccessStatus *iface, HSTRING *name )
{
    static const WCHAR name_str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.ApplicationModel.Background.BackgroundAccessStatus>";
    return WindowsCreateString( name_str, wcslen( name_str ), name );
}
static HRESULT WINAPI status_op_GetTrustLevel( IAsyncOperation_BackgroundAccessStatus *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI status_op_put_Completed( IAsyncOperation_BackgroundAccessStatus *iface,
                                               IAsyncOperationCompletedHandler_BackgroundAccessStatus *handler )
{
    return op_put_completed( impl_from_status_op( iface ), (IUnknown *)handler );
}
static HRESULT WINAPI status_op_get_Completed( IAsyncOperation_BackgroundAccessStatus *iface,
                                               IAsyncOperationCompletedHandler_BackgroundAccessStatus **handler )
{
    return op_get_completed( impl_from_status_op( iface ), (IUnknown **)handler );
}
static HRESULT WINAPI status_op_GetResults( IAsyncOperation_BackgroundAccessStatus *iface, BackgroundAccessStatus *result )
{
    if (!result) return E_POINTER;
    *result = impl_from_status_op( iface )->value;
    return S_OK;
}

static const struct IAsyncOperation_BackgroundAccessStatusVtbl status_op_vtbl =
{
    status_op_QueryInterface,
    status_op_AddRef,
    status_op_Release,
    /* IInspectable methods */
    status_op_GetIids,
    status_op_GetRuntimeClassName,
    status_op_GetTrustLevel,
    /* IAsyncOperation<BackgroundAccessStatus> methods */
    status_op_put_Completed,
    status_op_get_Completed,
    status_op_GetResults,
};

static HRESULT WINAPI bool_op_QueryInterface( IAsyncOperation_boolean *iface, REFIID iid, void **out )
{
    return op_query_interface( impl_from_bool_op( iface ), iid, out );
}
static ULONG WINAPI bool_op_AddRef( IAsyncOperation_boolean *iface )
{
    return op_addref( impl_from_bool_op( iface ) );
}
static ULONG WINAPI bool_op_Release( IAsyncOperation_boolean *iface )
{
    return op_release( impl_from_bool_op( iface ) );
}
static HRESULT WINAPI bool_op_GetIids( IAsyncOperation_boolean *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI bool_op_GetRuntimeClassName( IAsyncOperation_boolean *iface, HSTRING *name )
{
    static const WCHAR name_str[] = L"Windows.Foundation.IAsyncOperation`1<Boolean>";
    return WindowsCreateString( name_str, wcslen( name_str ), name );
}
static HRESULT WINAPI bool_op_GetTrustLevel( IAsyncOperation_boolean *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI bool_op_put_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean *handler )
{
    return op_put_completed( impl_from_bool_op( iface ), (IUnknown *)handler );
}
static HRESULT WINAPI bool_op_get_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean **handler )
{
    return op_get_completed( impl_from_bool_op( iface ), (IUnknown **)handler );
}
static HRESULT WINAPI bool_op_GetResults( IAsyncOperation_boolean *iface, boolean *result )
{
    if (!result) return E_POINTER;
    *result = !!impl_from_bool_op( iface )->value;
    return S_OK;
}

static const struct IAsyncOperation_booleanVtbl bool_op_vtbl =
{
    bool_op_QueryInterface,
    bool_op_AddRef,
    bool_op_Release,
    /* IInspectable methods */
    bool_op_GetIids,
    bool_op_GetRuntimeClassName,
    bool_op_GetTrustLevel,
    /* IAsyncOperation<boolean> methods */
    bool_op_put_Completed,
    bool_op_get_Completed,
    bool_op_GetResults,
};

static HRESULT WINAPI info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    return op_query_interface( impl_from_IAsyncInfo( iface ), iid, out );
}
static ULONG WINAPI info_AddRef( IAsyncInfo *iface )
{
    return op_addref( impl_from_IAsyncInfo( iface ) );
}
static ULONG WINAPI info_Release( IAsyncInfo *iface )
{
    return op_release( impl_from_IAsyncInfo( iface ) );
}
static HRESULT WINAPI info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )
{
    return IInspectable_GetRuntimeClassName( op_primary( impl_from_IAsyncInfo( iface ) ), name );
}
static HRESULT WINAPI info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}
static HRESULT WINAPI info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    *status = Completed;
    return S_OK;
}
static HRESULT WINAPI info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error )
{
    *error = S_OK;
    return S_OK;
}
static HRESULT WINAPI info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}
static HRESULT WINAPI info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const struct IAsyncInfoVtbl info_vtbl =
{
    info_QueryInterface,
    info_AddRef,
    info_Release,
    /* IInspectable methods */
    info_GetIids,
    info_GetRuntimeClassName,
    info_GetTrustLevel,
    /* IAsyncInfo methods */
    info_get_Id,
    info_get_Status,
    info_get_ErrorCode,
    info_Cancel,
    info_Close,
};

static HRESULT create_completed_op( BOOL is_bool, INT32 value, void **out )
{
    struct completed_op *op;

    if (!out) return E_POINTER;
    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->IAsyncOperation_BackgroundAccessStatus_iface.lpVtbl = &status_op_vtbl;
    op->IAsyncOperation_boolean_iface.lpVtbl = &bool_op_vtbl;
    op->IAsyncInfo_iface.lpVtbl = &info_vtbl;
    op->ref = 1;
    op->is_bool = is_bool;
    op->value = value;
    *out = op_primary( op );
    return S_OK;
}

/* ---- the factory and its statics ---- */

struct manager
{
    IActivationFactory IActivationFactory_iface;
    IBackgroundExecutionManagerStatics IBackgroundExecutionManagerStatics_iface;
    IBackgroundExecutionManagerStatics2 IBackgroundExecutionManagerStatics2_iface;
    IBackgroundExecutionManagerStatics3 IBackgroundExecutionManagerStatics3_iface;
};

static struct manager manager;

static HRESULT query_interface( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &manager.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IBackgroundExecutionManagerStatics ))
        *out = &manager.IBackgroundExecutionManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IBackgroundExecutionManagerStatics2 ))
        *out = &manager.IBackgroundExecutionManagerStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IBackgroundExecutionManagerStatics3 ))
        *out = &manager.IBackgroundExecutionManagerStatics3_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT get_class_name( HSTRING *name )
{
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

#define DEFINE_STATIC_IINSPECTABLE( prefix, iface_type )                                                           \
    static HRESULT WINAPI prefix##_QueryInterface( iface_type *iface, REFIID iid, void **out )                     \
    {                                                                                                              \
        return query_interface( iid, out );                                                                        \
    }                                                                                                              \
    static ULONG WINAPI prefix##_AddRef( iface_type *iface ) { return 2; }                                         \
    static ULONG WINAPI prefix##_Release( iface_type *iface ) { return 1; }                                        \
    static HRESULT WINAPI prefix##_GetIids( iface_type *iface, ULONG *count, IID **iids )                          \
    {                                                                                                              \
        *count = 0;                                                                                                \
        *iids = NULL;                                                                                              \
        return S_OK;                                                                                               \
    }                                                                                                              \
    static HRESULT WINAPI prefix##_GetRuntimeClassName( iface_type *iface, HSTRING *name )                         \
    {                                                                                                              \
        return get_class_name( name );                                                                             \
    }                                                                                                              \
    static HRESULT WINAPI prefix##_GetTrustLevel( iface_type *iface, TrustLevel *level )                           \
    {                                                                                                              \
        *level = BaseTrust;                                                                                        \
        return S_OK;                                                                                               \
    }

DEFINE_STATIC_IINSPECTABLE( factory, IActivationFactory )
DEFINE_STATIC_IINSPECTABLE( statics, IBackgroundExecutionManagerStatics )
DEFINE_STATIC_IINSPECTABLE( statics2, IBackgroundExecutionManagerStatics2 )
DEFINE_STATIC_IINSPECTABLE( statics3, IBackgroundExecutionManagerStatics3 )

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

static HRESULT WINAPI statics_RequestAccessAsync( IBackgroundExecutionManagerStatics *iface,
                                                  IAsyncOperation_BackgroundAccessStatus **operation )
{
    TRACE( "iface %p, operation %p: %d\n", iface, operation, access_status );
    return create_completed_op( FALSE, access_status, (void **)operation );
}

static HRESULT WINAPI statics_RequestAccessForApplicationAsync( IBackgroundExecutionManagerStatics *iface, HSTRING id,
                                                                IAsyncOperation_BackgroundAccessStatus **operation )
{
    TRACE( "iface %p, id %s, operation %p.\n", iface, debugstr_hstring( id ), operation );
    return create_completed_op( FALSE, access_status, (void **)operation );
}

static HRESULT WINAPI statics_RemoveAccess( IBackgroundExecutionManagerStatics *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI statics_RemoveAccessForApplication( IBackgroundExecutionManagerStatics *iface, HSTRING id )
{
    TRACE( "iface %p, id %s.\n", iface, debugstr_hstring( id ) );
    return S_OK;
}

static HRESULT WINAPI statics_GetAccessStatus( IBackgroundExecutionManagerStatics *iface, BackgroundAccessStatus *status )
{
    TRACE( "iface %p, status %p.\n", iface, status );
    if (!status) return E_POINTER;
    *status = access_status;
    return S_OK;
}

static HRESULT WINAPI statics_GetAccessStatusForApplication( IBackgroundExecutionManagerStatics *iface, HSTRING id,
                                                             BackgroundAccessStatus *status )
{
    TRACE( "iface %p, id %s, status %p.\n", iface, debugstr_hstring( id ), status );
    if (!status) return E_POINTER;
    *status = access_status;
    return S_OK;
}

static const struct IBackgroundExecutionManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IBackgroundExecutionManagerStatics methods */
    statics_RequestAccessAsync,
    statics_RequestAccessForApplicationAsync,
    statics_RemoveAccess,
    statics_RemoveAccessForApplication,
    statics_GetAccessStatus,
    statics_GetAccessStatusForApplication,
};

static HRESULT WINAPI statics2_RequestAccessKindAsync( IBackgroundExecutionManagerStatics2 *iface,
                                                       BackgroundAccessRequestKind kind, HSTRING reason,
                                                       IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, kind %d, reason %s, operation %p.\n", iface, kind, debugstr_hstring( reason ), operation );
    return create_completed_op( TRUE, FALSE, (void **)operation );
}

static const struct IBackgroundExecutionManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IBackgroundExecutionManagerStatics2 methods */
    statics2_RequestAccessKindAsync,
};

static HRESULT WINAPI statics3_RequestAccessKindForModernStandbyAsync( IBackgroundExecutionManagerStatics3 *iface,
                                                                       BackgroundAccessRequestKind kind, HSTRING reason,
                                                                       IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, kind %d, reason %s, operation %p.\n", iface, kind, debugstr_hstring( reason ), operation );
    return create_completed_op( TRUE, FALSE, (void **)operation );
}

static HRESULT WINAPI statics3_GetAccessStatusForModernStandby( IBackgroundExecutionManagerStatics3 *iface,
                                                                BackgroundAccessStatus *status )
{
    TRACE( "iface %p, status %p.\n", iface, status );
    if (!status) return E_POINTER;
    *status = access_status;
    return S_OK;
}

static HRESULT WINAPI statics3_GetAccessStatusForModernStandbyForApplication( IBackgroundExecutionManagerStatics3 *iface,
                                                                              HSTRING id, BackgroundAccessStatus *status )
{
    TRACE( "iface %p, id %s, status %p.\n", iface, debugstr_hstring( id ), status );
    if (!status) return E_POINTER;
    *status = access_status;
    return S_OK;
}

static const struct IBackgroundExecutionManagerStatics3Vtbl statics3_vtbl =
{
    statics3_QueryInterface,
    statics3_AddRef,
    statics3_Release,
    /* IInspectable methods */
    statics3_GetIids,
    statics3_GetRuntimeClassName,
    statics3_GetTrustLevel,
    /* IBackgroundExecutionManagerStatics3 methods */
    statics3_RequestAccessKindForModernStandbyAsync,
    statics3_GetAccessStatusForModernStandby,
    statics3_GetAccessStatusForModernStandbyForApplication,
};

static struct manager manager =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
};

/* LaunchActivatedEventArgs for GetActivatedEventArgs: an app the shell started is launched, and WhatsApp reads the
 * arguments of the one it gets (a NULL made its start-up throw NullReferenceException). Kind Launch, not running
 * before, the command line's arguments, the app's own tile; no splash screen, tile info, user or view switcher. The
 * interfaces past IActivatedEventArgs are in no Wine IDL: their vtables are written out here. */

static const GUID IID_ILaunchActivatedEventArgs_local = {0xfbc93e26, 0xa14a, 0x4b4f, {0x82, 0xb0, 0x33, 0xbe, 0xd9, 0x20, 0xaf, 0x52}};
static const GUID IID_ILaunchActivatedEventArgs2_local = {0x0fd37ebc, 0x9dc9, 0x46b5, {0x9a, 0xce, 0xbd, 0x95, 0xd4, 0x56, 0x53, 0x45}};
static const GUID IID_IPrelaunchActivatedEventArgs_local = {0x0c44717b, 0x19f7, 0x48d6, {0xb0, 0x46, 0xcf, 0x22, 0x82, 0x6e, 0xaa, 0x74}};
static const GUID IID_IApplicationViewActivatedEventArgs_local = {0x930cef4b, 0xb829, 0x40fc, {0x88, 0xf4, 0x85, 0x13, 0xe8, 0xa6, 0x47, 0x38}};
static const GUID IID_IActivatedEventArgsWithUser_local = {0x1cf09b9e, 0x9962, 0x4936, {0x80, 0xff, 0xaf, 0xc8, 0xe8, 0xae, 0x5c, 0x8c}};
static const GUID IID_IViewSwitcherProvider_local = {0x33f288a6, 0x5c2c, 0x4d27, {0xba, 0xc7, 0x75, 0x36, 0x08, 0x8f, 0x12, 0x19}};

static const WCHAR launch_args_class_name[] = L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs";

enum launch_view { VIEW_ACTIVATED, VIEW_LAUNCH, VIEW_LAUNCH2, VIEW_PRELAUNCH, VIEW_APPVIEW, VIEW_USER, VIEW_SWITCHER, VIEW_COUNT };

struct launch_args
{
    const void *const *views[VIEW_COUNT];   /* each a vtable pointer: the object's interfaces */
    LONG ref;
    HSTRING arguments, tile_id;
};

static const void *const launch_vtbls[VIEW_COUNT][16];

/* which view this is, from the vtable it carries; the object is that many pointers before it */
static struct launch_args *launch_args_from_view( void *iface )
{
    const void *const **view = iface;
    unsigned int i;

    for (i = 0; i < VIEW_COUNT; i++)
        if (*view == launch_vtbls[i]) return CONTAINING_RECORD( view - i, struct launch_args, views[0] );
    ERR( "not a view of ours: %p\n", iface );
    return NULL;
}

static HRESULT WINAPI la_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct launch_args *impl = launch_args_from_view( iface );
    int view = -1;

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivatedEventArgs ))
        view = VIEW_ACTIVATED;
    else if (IsEqualGUID( iid, &IID_ILaunchActivatedEventArgs_local )) view = VIEW_LAUNCH;
    else if (IsEqualGUID( iid, &IID_ILaunchActivatedEventArgs2_local )) view = VIEW_LAUNCH2;
    else if (IsEqualGUID( iid, &IID_IPrelaunchActivatedEventArgs_local )) view = VIEW_PRELAUNCH;
    else if (IsEqualGUID( iid, &IID_IApplicationViewActivatedEventArgs_local )) view = VIEW_APPVIEW;
    else if (IsEqualGUID( iid, &IID_IActivatedEventArgsWithUser_local )) view = VIEW_USER;
    else if (IsEqualGUID( iid, &IID_IViewSwitcherProvider_local )) view = VIEW_SWITCHER;
    if (view < 0)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    *out = &impl->views[view];
    InterlockedIncrement( &impl->ref );
    return S_OK;
}

static ULONG WINAPI la_AddRef( void *iface )
{
    return InterlockedIncrement( &launch_args_from_view( iface )->ref );
}

static ULONG WINAPI la_Release( void *iface )
{
    struct launch_args *impl = launch_args_from_view( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        WindowsDeleteString( impl->arguments );
        WindowsDeleteString( impl->tile_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI la_GetIids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI la_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( launch_args_class_name, wcslen( launch_args_class_name ), name );
}

static HRESULT WINAPI la_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI la_get_Kind( void *iface, ActivationKind *value )
{
    TRACE( "iface %p.\n", iface );
    *value = ActivationKind_Launch;
    return S_OK;
}

static HRESULT WINAPI la_get_PreviousExecutionState( void *iface, ApplicationExecutionState *value )
{
    TRACE( "iface %p.\n", iface );
    *value = ApplicationExecutionState_NotRunning;
    return S_OK;
}

/* NULL getters: an object the app was not given (splash screen, tile info, user, view switcher) */
static HRESULT WINAPI la_get_null( void *iface, void **value )
{
    TRACE( "iface %p.\n", iface );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI la_get_Arguments( void *iface, HSTRING *value )
{
    TRACE( "iface %p.\n", iface );
    return WindowsDuplicateString( launch_args_from_view( iface )->arguments, value );
}

static HRESULT WINAPI la_get_TileId( void *iface, HSTRING *value )
{
    TRACE( "iface %p.\n", iface );
    return WindowsDuplicateString( launch_args_from_view( iface )->tile_id, value );
}

static HRESULT WINAPI la_get_false( void *iface, boolean *value )
{
    TRACE( "iface %p.\n", iface );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI la_get_zero( void *iface, INT32 *value )
{
    TRACE( "iface %p.\n", iface );
    *value = 0;
    return S_OK;
}

#define LA_IINSPECTABLE la_QueryInterface, la_AddRef, la_Release, la_GetIids, la_GetRuntimeClassName, la_GetTrustLevel

static const void *const launch_vtbls[VIEW_COUNT][16] =
{
    [VIEW_ACTIVATED] = { LA_IINSPECTABLE, la_get_Kind, la_get_PreviousExecutionState, la_get_null },
    [VIEW_LAUNCH]    = { LA_IINSPECTABLE, la_get_Arguments, la_get_TileId },
    [VIEW_LAUNCH2]   = { LA_IINSPECTABLE, la_get_null },
    [VIEW_PRELAUNCH] = { LA_IINSPECTABLE, la_get_false },
    [VIEW_APPVIEW]   = { LA_IINSPECTABLE, la_get_zero },
    [VIEW_USER]      = { LA_IINSPECTABLE, la_get_null },
    [VIEW_SWITCHER]  = { LA_IINSPECTABLE, la_get_null },
};

/* the command line's arguments: everything after the executable */
static const WCHAR *command_line_arguments(void)
{
    const WCHAR *p = GetCommandLineW();

    if (*p == '"')
    {
        p = wcschr( p + 1, '"' );
        p = p ? p + 1 : L"";
    }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static HRESULT create_launch_args( IActivatedEventArgs **out )
{
    WCHAR aumid[512], *app_id;
    struct launch_args *impl;
    unsigned int i;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    for (i = 0; i < VIEW_COUNT; i++) impl->views[i] = launch_vtbls[i];
    impl->ref = 1;
    if (!GetEnvironmentVariableW( L"WINE_UWP_AUMID", aumid, ARRAY_SIZE(aumid) ) || !(app_id = wcschr( aumid, '!' )))
        app_id = (WCHAR *)L"!App";
    app_id++;
    if (FAILED(hr = WindowsCreateString( command_line_arguments(), wcslen( command_line_arguments() ), &impl->arguments )) ||
        FAILED(hr = WindowsCreateString( app_id, wcslen( app_id ), &impl->tile_id )))
    {
        WindowsDeleteString( impl->arguments );
        free( impl );
        return hr;
    }
    TRACE( "arguments %s, tile %s.\n", debugstr_hstring( impl->arguments ), debugstr_hstring( impl->tile_id ) );
    *out = (IActivatedEventArgs *)&impl->views[VIEW_ACTIVATED];
    return S_OK;
}

/* ---- Windows.ApplicationModel.AppInstance ----
 *
 * Windows' is execmodelclient.dll's, a client of the process lifetime manager. Here an app is always its own one
 * instance: FindOrRegisterInstanceForKey answers the current instance, there is no recommended other one, and
 * GetActivatedEventArgs reports the launch the launcher made. WhatsApp's start-up stopped on CLASS_E_CLASSNOTAVAILABLE
 * here. */

static const WCHAR app_instance_class_name[] = L"Windows.ApplicationModel.AppInstance";

struct app_instance
{
    IAppInstance IAppInstance_iface;
    LONG ref;
    HSTRING key;
};

static struct app_instance *impl_from_IAppInstance( IAppInstance *iface )
{
    return CONTAINING_RECORD( iface, struct app_instance, IAppInstance_iface );
}

static HRESULT WINAPI instance_QueryInterface( IAppInstance *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppInstance ))
    {
        IAppInstance_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI instance_AddRef( IAppInstance *iface )
{
    return InterlockedIncrement( &impl_from_IAppInstance( iface )->ref );
}

static ULONG WINAPI instance_Release( IAppInstance *iface )
{
    struct app_instance *impl = impl_from_IAppInstance( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        WindowsDeleteString( impl->key );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI instance_GetIids( IAppInstance *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI instance_GetRuntimeClassName( IAppInstance *iface, HSTRING *name )
{
    return WindowsCreateString( app_instance_class_name, wcslen( app_instance_class_name ), name );
}

static HRESULT WINAPI instance_GetTrustLevel( IAppInstance *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI instance_get_Key( IAppInstance *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IAppInstance( iface )->key, value );
}

static HRESULT WINAPI instance_get_IsCurrentInstance( IAppInstance *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI instance_RedirectActivationTo( IAppInstance *iface )
{
    /* only ever reached for another instance, and every instance here is the current one */
    FIXME( "iface %p: no other instance to redirect to.\n", iface );
    return E_ILLEGAL_METHOD_CALL;
}

static const struct IAppInstanceVtbl instance_vtbl =
{
    instance_QueryInterface,
    instance_AddRef,
    instance_Release,
    /* IInspectable methods */
    instance_GetIids,
    instance_GetRuntimeClassName,
    instance_GetTrustLevel,
    /* IAppInstance methods */
    instance_get_Key,
    instance_get_IsCurrentInstance,
    instance_RedirectActivationTo,
};

static HRESULT create_app_instance( HSTRING key, IAppInstance **out )
{
    struct app_instance *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppInstance_iface.lpVtbl = &instance_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( key, &impl->key )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IAppInstance_iface;
    return S_OK;
}

struct app_instance_statics
{
    IActivationFactory IActivationFactory_iface;
    IAppInstanceStatics IAppInstanceStatics_iface;
};

static struct app_instance_statics app_instance_statics;

static HRESULT app_instance_query_interface( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &app_instance_statics.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstanceStatics ))
        *out = &app_instance_statics.IAppInstanceStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT get_app_instance_class_name( HSTRING *name )
{
    return WindowsCreateString( app_instance_class_name, wcslen( app_instance_class_name ), name );
}

#define DEFINE_APP_INSTANCE_IINSPECTABLE( prefix, iface_type )                                                     \
    static HRESULT WINAPI prefix##_QueryInterface( iface_type *iface, REFIID iid, void **out )                     \
    {                                                                                                              \
        return app_instance_query_interface( iid, out );                                                           \
    }                                                                                                              \
    static ULONG WINAPI prefix##_AddRef( iface_type *iface ) { return 2; }                                         \
    static ULONG WINAPI prefix##_Release( iface_type *iface ) { return 1; }                                        \
    static HRESULT WINAPI prefix##_GetIids( iface_type *iface, ULONG *count, IID **iids )                          \
    {                                                                                                              \
        *count = 0;                                                                                                \
        *iids = NULL;                                                                                              \
        return S_OK;                                                                                               \
    }                                                                                                              \
    static HRESULT WINAPI prefix##_GetRuntimeClassName( iface_type *iface, HSTRING *name )                         \
    {                                                                                                              \
        return get_app_instance_class_name( name );                                                                \
    }                                                                                                              \
    static HRESULT WINAPI prefix##_GetTrustLevel( iface_type *iface, TrustLevel *level )                           \
    {                                                                                                              \
        *level = BaseTrust;                                                                                        \
        return S_OK;                                                                                               \
    }

DEFINE_APP_INSTANCE_IINSPECTABLE( instance_factory, IActivationFactory )
DEFINE_APP_INSTANCE_IINSPECTABLE( instance_statics, IAppInstanceStatics )

static HRESULT WINAPI instance_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* instances come from the statics */
}

static const struct IActivationFactoryVtbl instance_factory_vtbl =
{
    instance_factory_QueryInterface,
    instance_factory_AddRef,
    instance_factory_Release,
    /* IInspectable methods */
    instance_factory_GetIids,
    instance_factory_GetRuntimeClassName,
    instance_factory_GetTrustLevel,
    /* IActivationFactory methods */
    instance_factory_ActivateInstance,
};

static HRESULT WINAPI instance_statics_get_RecommendedInstance( IAppInstanceStatics *iface, IAppInstance **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI instance_statics_GetActivatedEventArgs( IAppInstanceStatics *iface, IActivatedEventArgs **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    return create_launch_args( result );
}

static HRESULT WINAPI instance_statics_FindOrRegisterInstanceForKey( IAppInstanceStatics *iface, HSTRING key,
                                                                     IAppInstance **result )
{
    TRACE( "iface %p, key %s, result %p.\n", iface, debugstr_hstring( key ), result );
    if (!result) return E_POINTER;
    return create_app_instance( key, result );
}

static HRESULT WINAPI instance_statics_Unregister( IAppInstanceStatics *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI instance_statics_GetInstances( IAppInstanceStatics *iface, IVector_AppInstance **result )
{
    FIXME( "iface %p, result %p: not implemented.\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    return E_NOTIMPL;
}

static const struct IAppInstanceStaticsVtbl instance_statics_vtbl =
{
    instance_statics_QueryInterface,
    instance_statics_AddRef,
    instance_statics_Release,
    /* IInspectable methods */
    instance_statics_GetIids,
    instance_statics_GetRuntimeClassName,
    instance_statics_GetTrustLevel,
    /* IAppInstanceStatics methods */
    instance_statics_get_RecommendedInstance,
    instance_statics_GetActivatedEventArgs,
    instance_statics_FindOrRegisterInstanceForKey,
    instance_statics_Unregister,
    instance_statics_GetInstances,
};

static struct app_instance_statics app_instance_statics =
{
    {&instance_factory_vtbl},
    {&instance_statics_vtbl},
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (!wcscmp( name, class_name )) *factory = &manager.IActivationFactory_iface;
    else if (!wcscmp( name, app_instance_class_name )) *factory = &app_instance_statics.IActivationFactory_iface;
    else return CLASS_E_CLASSNOTAVAILABLE;
    return S_OK;
}

/*
 * CoreDragOperation: drag and drop inside the app's own windows
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
 * On Windows a UWP drag is run by the system: XAML hands a CoreDragOperation to DataExchange, which gives it to the
 * shell's drag host, and the host calls the ICoreDropOperationTarget of whichever window the pointer is over - found
 * through that window's CoreDragDropManager.TargetRequested. There is no drag host here, so StartAsync runs the drag
 * itself on a thread of its own: it follows the cursor and the mouse buttons, asks the window under the pointer for a
 * target, and calls the target's Enter/Over/Leave/Drop on that window's UI thread through its CoreDispatcher. Only
 * windows of this process can be targets.
 */

#include "private.h"
#include "roapi.h"
#include "initguid.h"

WINE_DEFAULT_DEBUG_CHANNEL(dataexchange);

/* {9e5d3a5a-fe96-4f97-84ae-89e12476b66f}, cloaked on DataExchange's DragOperation; XAML starts drags through it */
DEFINE_GUID(IID_ICoreDragOperationInternal, 0x9e5d3a5a, 0xfe96, 0x4f97, 0x84, 0xae, 0x89, 0xe1, 0x24, 0x76, 0xb6, 0x6f);

typedef struct ICoreDragOperationInternal ICoreDragOperationInternal;
typedef struct ICoreDragOperationInternalVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreDragOperationInternal *iface, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(ICoreDragOperationInternal *iface);
    ULONG (STDMETHODCALLTYPE *Release)(ICoreDragOperationInternal *iface);
    HRESULT (STDMETHODCALLTYPE *GetIids)(ICoreDragOperationInternal *iface, ULONG *count, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(ICoreDragOperationInternal *iface, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(ICoreDragOperationInternal *iface, TrustLevel *level);
    HRESULT (STDMETHODCALLTYPE *StartAsync)(ICoreDragOperationInternal *iface, IInspectable *source, Point point,
                                            IAsyncOperation_DataPackageOperation **operation);
} ICoreDragOperationInternalVtbl;
struct ICoreDragOperationInternal { const ICoreDragOperationInternalVtbl *lpVtbl; };

CRITICAL_SECTION drag_cs;
static CRITICAL_SECTION_DEBUG drag_cs_debug =
{
    0, 0, &drag_cs,
    { &drag_cs_debug.ProcessLocksList, &drag_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": drag_cs") }
};
CRITICAL_SECTION drag_cs = { &drag_cs_debug, -1, 0, 0, 0, 0 };

/* one TargetRequested handler of one window */
struct target_registration
{
    struct list entry;
    HWND hwnd;
    ICoreDragDropManager *manager;
    ITypedEventHandler_CoreDragDropManager_CoreDropOperationTargetRequestedEventArgs *handler;
    ICoreDispatcher *dispatcher;
    INT64 token;
};

static struct list target_registrations = LIST_INIT( target_registrations );
static INT64 next_token = 1;

static ICoreDispatcher *current_dispatcher(void)
{
    static const WCHAR name[] = L"Windows.UI.Core.CoreWindow";
    ICoreDispatcher *dispatcher = NULL;
    ICoreWindowStatic *statics;
    ICoreWindow *window = NULL;
    HSTRING str;

    if (FAILED(WindowsCreateString( name, wcslen( name ), &str ))) return NULL;
    if (SUCCEEDED(RoGetActivationFactory( str, &IID_ICoreWindowStatic, (void **)&statics )))
    {
        if (SUCCEEDED(ICoreWindowStatic_GetForCurrentThread( statics, &window )) && window)
        {
            ICoreWindow_get_Dispatcher( window, &dispatcher );
            ICoreWindow_Release( window );
        }
        ICoreWindowStatic_Release( statics );
    }
    WindowsDeleteString( str );
    return dispatcher;
}

HRESULT register_drop_target( HWND hwnd, ICoreDragDropManager *manager,
                              ITypedEventHandler_CoreDragDropManager_CoreDropOperationTargetRequestedEventArgs *handler,
                              EventRegistrationToken *token )
{
    struct target_registration *reg;

    if (!handler) return E_INVALIDARG;
    if (!(reg = calloc( 1, sizeof(*reg) ))) return E_OUTOFMEMORY;
    reg->hwnd = hwnd;
    reg->manager = manager;
    ICoreDragDropManager_AddRef( manager );
    reg->handler = handler;
    ITypedEventHandler_CoreDragDropManager_CoreDropOperationTargetRequestedEventArgs_AddRef( handler );
    /* TargetRequested is registered by the window's own UI thread; targets are called there */
    reg->dispatcher = current_dispatcher();

    EnterCriticalSection( &drag_cs );
    reg->token = next_token++;
    list_add_tail( &target_registrations, &reg->entry );
    LeaveCriticalSection( &drag_cs );

    token->value = reg->token;
    TRACE( "hwnd %p, handler %p, dispatcher %p, token %I64d\n", hwnd, handler, reg->dispatcher, reg->token );
    return S_OK;
}

HRESULT unregister_drop_target( EventRegistrationToken token )
{
    struct target_registration *reg, *found = NULL;

    EnterCriticalSection( &drag_cs );
    LIST_FOR_EACH_ENTRY( reg, &target_registrations, struct target_registration, entry )
    {
        if (reg->token != token.value) continue;
        list_remove( &reg->entry );
        found = reg;
        break;
    }
    LeaveCriticalSection( &drag_cs );

    if (!found) return S_OK;
    ITypedEventHandler_CoreDragDropManager_CoreDropOperationTargetRequestedEventArgs_Release( found->handler );
    ICoreDragDropManager_Release( found->manager );
    if (found->dispatcher) ICoreDispatcher_Release( found->dispatcher );
    free( found );
    return S_OK;
}

/* the registration of the window under a screen point, referenced */
static struct target_registration *registration_at( POINT pt )
{
    struct target_registration *reg, *found = NULL;
    HWND hwnd = WindowFromPoint( pt ), root;

    if (!hwnd) return NULL;
    root = GetAncestor( hwnd, GA_ROOT );
    EnterCriticalSection( &drag_cs );
    LIST_FOR_EACH_ENTRY( reg, &target_registrations, struct target_registration, entry )
    {
        if (reg->hwnd != hwnd && GetAncestor( reg->hwnd, GA_ROOT ) != root) continue;
        found = reg;
        break;
    }
    LeaveCriticalSection( &drag_cs );
    return found;
}

/*
 * IAsyncOperation<DataPackageOperation>, completed by the drag thread
 */

struct drag_async
{
    IAsyncOperation_DataPackageOperation IAsyncOperation_DataPackageOperation_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    AsyncStatus status;
    DataPackageOperation result;
    IAsyncOperationCompletedHandler_DataPackageOperation *handler;
};

static inline struct drag_async *impl_from_IAsyncOperation_DataPackageOperation( IAsyncOperation_DataPackageOperation *iface )
{
    return CONTAINING_RECORD( iface, struct drag_async, IAsyncOperation_DataPackageOperation_iface );
}

static HRESULT WINAPI drag_async_QueryInterface( IAsyncOperation_DataPackageOperation *iface, REFIID iid, void **out )
{
    struct drag_async *impl = impl_from_IAsyncOperation_DataPackageOperation( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_DataPackageOperation ))
        *out = &impl->IAsyncOperation_DataPackageOperation_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &impl->IAsyncInfo_iface;
    else
    {
        TRACE( "%s not implemented\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI drag_async_AddRef( IAsyncOperation_DataPackageOperation *iface )
{
    struct drag_async *impl = impl_from_IAsyncOperation_DataPackageOperation( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI drag_async_Release( IAsyncOperation_DataPackageOperation *iface )
{
    struct drag_async *impl = impl_from_IAsyncOperation_DataPackageOperation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_DataPackageOperation_Release( impl->handler );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI drag_async_GetIids( IAsyncOperation_DataPackageOperation *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI drag_async_GetRuntimeClassName( IAsyncOperation_DataPackageOperation *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.IAsyncOperation`1<Windows.ApplicationModel.DataTransfer.DataPackageOperation>";
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI drag_async_GetTrustLevel( IAsyncOperation_DataPackageOperation *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI drag_async_put_Completed( IAsyncOperation_DataPackageOperation *iface,
                                                IAsyncOperationCompletedHandler_DataPackageOperation *handler )
{
    struct drag_async *impl = impl_from_IAsyncOperation_DataPackageOperation( iface );
    AsyncStatus status;

    TRACE( "iface %p, handler %p\n", iface, handler );
    if (!handler) return E_POINTER;

    EnterCriticalSection( &drag_cs );
    if (impl->handler)
    {
        LeaveCriticalSection( &drag_cs );
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    IAsyncOperationCompletedHandler_DataPackageOperation_AddRef( handler );
    if ((status = impl->status) == Started) impl->handler = handler;
    LeaveCriticalSection( &drag_cs );

    if (status != Started)
    {
        IAsyncOperationCompletedHandler_DataPackageOperation_Invoke( handler, iface, status );
        IAsyncOperationCompletedHandler_DataPackageOperation_Release( handler );
    }
    return S_OK;
}

static HRESULT WINAPI drag_async_get_Completed( IAsyncOperation_DataPackageOperation *iface,
                                                IAsyncOperationCompletedHandler_DataPackageOperation **handler )
{
    struct drag_async *impl = impl_from_IAsyncOperation_DataPackageOperation( iface );

    EnterCriticalSection( &drag_cs );
    if ((*handler = impl->handler)) IAsyncOperationCompletedHandler_DataPackageOperation_AddRef( *handler );
    LeaveCriticalSection( &drag_cs );
    return S_OK;
}

static HRESULT WINAPI drag_async_GetResults( IAsyncOperation_DataPackageOperation *iface, DataPackageOperation *result )
{
    struct drag_async *impl = impl_from_IAsyncOperation_DataPackageOperation( iface );

    TRACE( "iface %p, status %d, result %#x\n", iface, impl->status, impl->result );
    if (impl->status == Started) return E_ILLEGAL_METHOD_CALL;
    *result = impl->result;
    return S_OK;
}

static const struct IAsyncOperation_DataPackageOperationVtbl drag_async_vtbl =
{
    drag_async_QueryInterface,
    drag_async_AddRef,
    drag_async_Release,
    drag_async_GetIids,
    drag_async_GetRuntimeClassName,
    drag_async_GetTrustLevel,
    drag_async_put_Completed,
    drag_async_get_Completed,
    drag_async_GetResults,
};

DEFINE_IINSPECTABLE( drag_async_info, IAsyncInfo, struct drag_async, IAsyncOperation_DataPackageOperation_iface )

static HRESULT WINAPI drag_async_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI drag_async_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct drag_async *impl = impl_from_IAsyncInfo( iface );
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI drag_async_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error )
{
    *error = S_OK;
    return S_OK;
}

static HRESULT WINAPI drag_async_info_Cancel( IAsyncInfo *iface )
{
    TRACE( "iface %p\n", iface );
    return S_OK;
}

static HRESULT WINAPI drag_async_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const struct IAsyncInfoVtbl drag_async_info_vtbl =
{
    drag_async_info_QueryInterface,
    drag_async_info_AddRef,
    drag_async_info_Release,
    drag_async_info_GetIids,
    drag_async_info_GetRuntimeClassName,
    drag_async_info_GetTrustLevel,
    drag_async_info_get_Id,
    drag_async_info_get_Status,
    drag_async_info_get_ErrorCode,
    drag_async_info_Cancel,
    drag_async_info_Close,
};

static struct drag_async *create_drag_async(void)
{
    struct drag_async *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IAsyncOperation_DataPackageOperation_iface.lpVtbl = &drag_async_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &drag_async_info_vtbl;
    impl->ref = 1;
    impl->status = Started;
    return impl;
}

static void complete_drag_async( struct drag_async *impl, DataPackageOperation result )
{
    IAsyncOperationCompletedHandler_DataPackageOperation *handler;

    EnterCriticalSection( &drag_cs );
    impl->result = result;
    impl->status = Completed;
    handler = impl->handler;
    impl->handler = NULL;
    LeaveCriticalSection( &drag_cs );

    TRACE( "operation %p, result %#x, handler %p\n", impl, result, handler );
    if (!handler) return;
    IAsyncOperationCompletedHandler_DataPackageOperation_Invoke( handler, &impl->IAsyncOperation_DataPackageOperation_iface,
                                                                 Completed );
    IAsyncOperationCompletedHandler_DataPackageOperation_Release( handler );
}

/*
 * The completion handler the drag thread waits on, for a target's IAsyncOperation<DataPackageOperation> or IAsyncAction
 */

struct waiter
{
    IAsyncOperationCompletedHandler_DataPackageOperation IAsyncOperationCompletedHandler_DataPackageOperation_iface;
    IAsyncActionCompletedHandler IAsyncActionCompletedHandler_iface;
    LONG ref;
    HANDLE event;
    DataPackageOperation result;
};

static inline struct waiter *impl_from_IAsyncOperationCompletedHandler_DataPackageOperation(
        IAsyncOperationCompletedHandler_DataPackageOperation *iface )
{
    return CONTAINING_RECORD( iface, struct waiter, IAsyncOperationCompletedHandler_DataPackageOperation_iface );
}

static HRESULT WINAPI waiter_QueryInterface( IAsyncOperationCompletedHandler_DataPackageOperation *iface, REFIID iid, void **out )
{
    struct waiter *impl = impl_from_IAsyncOperationCompletedHandler_DataPackageOperation( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationCompletedHandler_DataPackageOperation ))
        *out = &impl->IAsyncOperationCompletedHandler_DataPackageOperation_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncActionCompletedHandler ))
        *out = &impl->IAsyncActionCompletedHandler_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI waiter_AddRef( IAsyncOperationCompletedHandler_DataPackageOperation *iface )
{
    struct waiter *impl = impl_from_IAsyncOperationCompletedHandler_DataPackageOperation( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI waiter_Release( IAsyncOperationCompletedHandler_DataPackageOperation *iface )
{
    struct waiter *impl = impl_from_IAsyncOperationCompletedHandler_DataPackageOperation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        CloseHandle( impl->event );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI waiter_Invoke( IAsyncOperationCompletedHandler_DataPackageOperation *iface,
                                     IAsyncOperation_DataPackageOperation *operation, AsyncStatus status )
{
    struct waiter *impl = impl_from_IAsyncOperationCompletedHandler_DataPackageOperation( iface );

    if (status != Completed || FAILED(IAsyncOperation_DataPackageOperation_GetResults( operation, &impl->result )))
        impl->result = DataPackageOperation_None;
    TRACE( "status %d, result %#x\n", status, impl->result );
    SetEvent( impl->event );
    return S_OK;
}

static const struct IAsyncOperationCompletedHandler_DataPackageOperationVtbl waiter_vtbl =
{
    waiter_QueryInterface,
    waiter_AddRef,
    waiter_Release,
    waiter_Invoke,
};

static inline struct waiter *impl_from_IAsyncActionCompletedHandler( IAsyncActionCompletedHandler *iface )
{
    return CONTAINING_RECORD( iface, struct waiter, IAsyncActionCompletedHandler_iface );
}

static HRESULT WINAPI waiter_action_QueryInterface( IAsyncActionCompletedHandler *iface, REFIID iid, void **out )
{
    struct waiter *impl = impl_from_IAsyncActionCompletedHandler( iface );
    return waiter_QueryInterface( &impl->IAsyncOperationCompletedHandler_DataPackageOperation_iface, iid, out );
}

static ULONG WINAPI waiter_action_AddRef( IAsyncActionCompletedHandler *iface )
{
    struct waiter *impl = impl_from_IAsyncActionCompletedHandler( iface );
    return waiter_AddRef( &impl->IAsyncOperationCompletedHandler_DataPackageOperation_iface );
}

static ULONG WINAPI waiter_action_Release( IAsyncActionCompletedHandler *iface )
{
    struct waiter *impl = impl_from_IAsyncActionCompletedHandler( iface );
    return waiter_Release( &impl->IAsyncOperationCompletedHandler_DataPackageOperation_iface );
}

static HRESULT WINAPI waiter_action_Invoke( IAsyncActionCompletedHandler *iface, IAsyncAction *action, AsyncStatus status )
{
    struct waiter *impl = impl_from_IAsyncActionCompletedHandler( iface );
    SetEvent( impl->event );
    return S_OK;
}

static const struct IAsyncActionCompletedHandlerVtbl waiter_action_vtbl =
{
    waiter_action_QueryInterface,
    waiter_action_AddRef,
    waiter_action_Release,
    waiter_action_Invoke,
};

static struct waiter *create_waiter(void)
{
    struct waiter *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IAsyncOperationCompletedHandler_DataPackageOperation_iface.lpVtbl = &waiter_vtbl;
    impl->IAsyncActionCompletedHandler_iface.lpVtbl = &waiter_action_vtbl;
    impl->ref = 1;
    impl->event = CreateEventW( NULL, TRUE, FALSE, NULL );
    return impl;
}

/*
 * CoreDragInfo: one snapshot of the drag, handed to a target
 */

struct drag_info
{
    ICoreDragInfo ICoreDragInfo_iface;
    ICoreDragInfo2 ICoreDragInfo2_iface;
    LONG ref;
    IDataPackageView *view;
    DragDropModifiers modifiers;
    Point position;
    DataPackageOperation allowed;
};

static inline struct drag_info *impl_from_ICoreDragInfo( ICoreDragInfo *iface )
{
    return CONTAINING_RECORD( iface, struct drag_info, ICoreDragInfo_iface );
}

static HRESULT WINAPI drag_info_QueryInterface( ICoreDragInfo *iface, REFIID iid, void **out )
{
    struct drag_info *impl = impl_from_ICoreDragInfo( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICoreDragInfo ))
        *out = &impl->ICoreDragInfo_iface;
    else if (IsEqualGUID( iid, &IID_ICoreDragInfo2 ))
        *out = &impl->ICoreDragInfo2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI drag_info_AddRef( ICoreDragInfo *iface )
{
    struct drag_info *impl = impl_from_ICoreDragInfo( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI drag_info_Release( ICoreDragInfo *iface )
{
    struct drag_info *impl = impl_from_ICoreDragInfo( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->view) IDataPackageView_Release( impl->view );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI drag_info_GetIids( ICoreDragInfo *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI drag_info_GetRuntimeClassName( ICoreDragInfo *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.ApplicationModel.DataTransfer.DragDrop.Core.CoreDragInfo";
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI drag_info_GetTrustLevel( ICoreDragInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI drag_info_get_Data( ICoreDragInfo *iface, IDataPackageView **value )
{
    struct drag_info *impl = impl_from_ICoreDragInfo( iface );

    TRACE( "iface %p, view %p\n", iface, impl->view );
    if (!(*value = impl->view)) return E_FAIL;
    IDataPackageView_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI drag_info_get_Modifiers( ICoreDragInfo *iface, DragDropModifiers *value )
{
    struct drag_info *impl = impl_from_ICoreDragInfo( iface );
    *value = impl->modifiers;
    return S_OK;
}

static HRESULT WINAPI drag_info_get_Position( ICoreDragInfo *iface, Point *value )
{
    struct drag_info *impl = impl_from_ICoreDragInfo( iface );
    *value = impl->position;
    return S_OK;
}

static const struct ICoreDragInfoVtbl drag_info_vtbl =
{
    drag_info_QueryInterface,
    drag_info_AddRef,
    drag_info_Release,
    drag_info_GetIids,
    drag_info_GetRuntimeClassName,
    drag_info_GetTrustLevel,
    drag_info_get_Data,
    drag_info_get_Modifiers,
    drag_info_get_Position,
};

DEFINE_IINSPECTABLE( drag_info2, ICoreDragInfo2, struct drag_info, ICoreDragInfo_iface )

static HRESULT WINAPI drag_info2_get_AllowedOperations( ICoreDragInfo2 *iface, DataPackageOperation *value )
{
    struct drag_info *impl = impl_from_ICoreDragInfo2( iface );
    *value = impl->allowed;
    return S_OK;
}

static const struct ICoreDragInfo2Vtbl drag_info2_vtbl =
{
    drag_info2_QueryInterface,
    drag_info2_AddRef,
    drag_info2_Release,
    drag_info2_GetIids,
    drag_info2_GetRuntimeClassName,
    drag_info2_GetTrustLevel,
    drag_info2_get_AllowedOperations,
};

/*
 * CoreDragUIOverride: what a target would like the drag to look like. There is no drag visual to change.
 */

struct drag_ui_override
{
    ICoreDragUIOverride ICoreDragUIOverride_iface;
    LONG ref;
    boolean content_visible, caption_visible, glyph_visible;
    HSTRING caption;
};

static inline struct drag_ui_override *impl_from_ICoreDragUIOverride( ICoreDragUIOverride *iface )
{
    return CONTAINING_RECORD( iface, struct drag_ui_override, ICoreDragUIOverride_iface );
}

static HRESULT WINAPI ui_override_QueryInterface( ICoreDragUIOverride *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICoreDragUIOverride ))
    {
        ICoreDragUIOverride_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ui_override_AddRef( ICoreDragUIOverride *iface )
{
    struct drag_ui_override *impl = impl_from_ICoreDragUIOverride( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI ui_override_Release( ICoreDragUIOverride *iface )
{
    struct drag_ui_override *impl = impl_from_ICoreDragUIOverride( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->caption );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ui_override_GetIids( ICoreDragUIOverride *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI ui_override_GetRuntimeClassName( ICoreDragUIOverride *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.ApplicationModel.DataTransfer.DragDrop.Core.CoreDragUIOverride";
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI ui_override_GetTrustLevel( ICoreDragUIOverride *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ui_override_SetContentFromSoftwareBitmap( ICoreDragUIOverride *iface, ISoftwareBitmap *bitmap )
{
    TRACE( "iface %p, bitmap %p\n", iface, bitmap );
    return S_OK;
}

static HRESULT WINAPI ui_override_SetContentFromSoftwareBitmapWithAnchorPoint( ICoreDragUIOverride *iface,
                                                                               ISoftwareBitmap *bitmap, Point anchor )
{
    TRACE( "iface %p, bitmap %p\n", iface, bitmap );
    return S_OK;
}

static HRESULT WINAPI ui_override_get_IsContentVisible( ICoreDragUIOverride *iface, boolean *value )
{
    *value = impl_from_ICoreDragUIOverride( iface )->content_visible;
    return S_OK;
}

static HRESULT WINAPI ui_override_put_IsContentVisible( ICoreDragUIOverride *iface, boolean value )
{
    impl_from_ICoreDragUIOverride( iface )->content_visible = value;
    return S_OK;
}

static HRESULT WINAPI ui_override_get_Caption( ICoreDragUIOverride *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ICoreDragUIOverride( iface )->caption, value );
}

static HRESULT WINAPI ui_override_put_Caption( ICoreDragUIOverride *iface, HSTRING value )
{
    struct drag_ui_override *impl = impl_from_ICoreDragUIOverride( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->caption );
    impl->caption = copy;
    return S_OK;
}

static HRESULT WINAPI ui_override_get_IsCaptionVisible( ICoreDragUIOverride *iface, boolean *value )
{
    *value = impl_from_ICoreDragUIOverride( iface )->caption_visible;
    return S_OK;
}

static HRESULT WINAPI ui_override_put_IsCaptionVisible( ICoreDragUIOverride *iface, boolean value )
{
    impl_from_ICoreDragUIOverride( iface )->caption_visible = value;
    return S_OK;
}

static HRESULT WINAPI ui_override_get_IsGlyphVisible( ICoreDragUIOverride *iface, boolean *value )
{
    *value = impl_from_ICoreDragUIOverride( iface )->glyph_visible;
    return S_OK;
}

static HRESULT WINAPI ui_override_put_IsGlyphVisible( ICoreDragUIOverride *iface, boolean value )
{
    impl_from_ICoreDragUIOverride( iface )->glyph_visible = value;
    return S_OK;
}

static HRESULT WINAPI ui_override_Clear( ICoreDragUIOverride *iface )
{
    struct drag_ui_override *impl = impl_from_ICoreDragUIOverride( iface );

    impl->content_visible = impl->caption_visible = impl->glyph_visible = TRUE;
    WindowsDeleteString( impl->caption );
    impl->caption = NULL;
    return S_OK;
}

static const struct ICoreDragUIOverrideVtbl ui_override_vtbl =
{
    ui_override_QueryInterface,
    ui_override_AddRef,
    ui_override_Release,
    ui_override_GetIids,
    ui_override_GetRuntimeClassName,
    ui_override_GetTrustLevel,
    ui_override_SetContentFromSoftwareBitmap,
    ui_override_SetContentFromSoftwareBitmapWithAnchorPoint,
    ui_override_get_IsContentVisible,
    ui_override_put_IsContentVisible,
    ui_override_get_Caption,
    ui_override_put_Caption,
    ui_override_get_IsCaptionVisible,
    ui_override_put_IsCaptionVisible,
    ui_override_get_IsGlyphVisible,
    ui_override_put_IsGlyphVisible,
    ui_override_Clear,
};

/*
 * CoreDropOperationTargetRequestedEventArgs
 */

struct target_args
{
    ICoreDropOperationTargetRequestedEventArgs ICoreDropOperationTargetRequestedEventArgs_iface;
    LONG ref;
    ICoreDropOperationTarget *target;
};

static inline struct target_args *impl_from_ICoreDropOperationTargetRequestedEventArgs(
        ICoreDropOperationTargetRequestedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct target_args, ICoreDropOperationTargetRequestedEventArgs_iface );
}

static HRESULT WINAPI target_args_QueryInterface( ICoreDropOperationTargetRequestedEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICoreDropOperationTargetRequestedEventArgs ))
    {
        ICoreDropOperationTargetRequestedEventArgs_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI target_args_AddRef( ICoreDropOperationTargetRequestedEventArgs *iface )
{
    struct target_args *impl = impl_from_ICoreDropOperationTargetRequestedEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI target_args_Release( ICoreDropOperationTargetRequestedEventArgs *iface )
{
    struct target_args *impl = impl_from_ICoreDropOperationTargetRequestedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->target) ICoreDropOperationTarget_Release( impl->target );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI target_args_GetIids( ICoreDropOperationTargetRequestedEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI target_args_GetRuntimeClassName( ICoreDropOperationTargetRequestedEventArgs *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.ApplicationModel.DataTransfer.DragDrop.Core.CoreDropOperationTargetRequestedEventArgs";
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI target_args_GetTrustLevel( ICoreDropOperationTargetRequestedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI target_args_SetTarget( ICoreDropOperationTargetRequestedEventArgs *iface,
                                             ICoreDropOperationTarget *target )
{
    struct target_args *impl = impl_from_ICoreDropOperationTargetRequestedEventArgs( iface );

    TRACE( "iface %p, target %p\n", iface, target );
    if (target) ICoreDropOperationTarget_AddRef( target );
    if (impl->target) ICoreDropOperationTarget_Release( impl->target );
    impl->target = target;
    return S_OK;
}

static const struct ICoreDropOperationTargetRequestedEventArgsVtbl target_args_vtbl =
{
    target_args_QueryInterface,
    target_args_AddRef,
    target_args_Release,
    target_args_GetIids,
    target_args_GetRuntimeClassName,
    target_args_GetTrustLevel,
    target_args_SetTarget,
};

/*
 * Calls into a target window, made on its UI thread
 */

enum target_call { CALL_REQUEST, CALL_ENTER, CALL_OVER, CALL_LEAVE, CALL_DROP, CALL_COMPLETE };

struct ui_call
{
    IDispatchedHandler IDispatchedHandler_iface;
    LONG ref;
    enum target_call call;
    struct target_registration *reg;
    ICoreDropOperationTarget *target;     /* in for the target calls, out for CALL_REQUEST */
    ICoreDragInfo *info;
    ICoreDragUIOverride *ui_override;
    struct waiter *waiter;
    HANDLE done;
    HRESULT hr;
    struct drag_async *async;             /* CALL_COMPLETE: the drag's own operation, and its result */
    DataPackageOperation result;
};

static const char *call_names[] = { "TargetRequested", "EnterAsync", "OverAsync", "LeaveAsync", "DropAsync", "complete" };

static void ui_call_run( struct ui_call *call )
{
    IAsyncOperation_DataPackageOperation *operation = NULL;
    IAsyncAction *action = NULL;
    struct target_args *args;

    switch (call->call)
    {
    case CALL_REQUEST:
        if (!(args = calloc( 1, sizeof(*args) ))) { call->hr = E_OUTOFMEMORY; break; }
        args->ICoreDropOperationTargetRequestedEventArgs_iface.lpVtbl = &target_args_vtbl;
        args->ref = 1;
        call->hr = ITypedEventHandler_CoreDragDropManager_CoreDropOperationTargetRequestedEventArgs_Invoke(
                call->reg->handler, call->reg->manager, &args->ICoreDropOperationTargetRequestedEventArgs_iface );
        if ((call->target = args->target)) ICoreDropOperationTarget_AddRef( call->target );
        ICoreDropOperationTargetRequestedEventArgs_Release( &args->ICoreDropOperationTargetRequestedEventArgs_iface );
        break;
    case CALL_ENTER:
        call->hr = ICoreDropOperationTarget_EnterAsync( call->target, call->info, call->ui_override, &operation );
        break;
    case CALL_OVER:
        call->hr = ICoreDropOperationTarget_OverAsync( call->target, call->info, call->ui_override, &operation );
        break;
    case CALL_LEAVE:
        call->hr = ICoreDropOperationTarget_LeaveAsync( call->target, call->info, &action );
        break;
    case CALL_DROP:
        call->hr = ICoreDropOperationTarget_DropAsync( call->target, call->info, &operation );
        break;
    case CALL_COMPLETE:
        complete_drag_async( call->async, call->result );
        call->hr = S_OK;
        break;
    }

    if (operation)
    {
        IAsyncOperation_DataPackageOperation_put_Completed( operation,
                &call->waiter->IAsyncOperationCompletedHandler_DataPackageOperation_iface );
        IAsyncOperation_DataPackageOperation_Release( operation );
    }
    else if (action)
    {
        IAsyncAction_put_Completed( action, &call->waiter->IAsyncActionCompletedHandler_iface );
        IAsyncAction_Release( action );
    }
    else SetEvent( call->waiter->event );
    SetEvent( call->done );
}

static inline struct ui_call *impl_from_IDispatchedHandler( IDispatchedHandler *iface )
{
    return CONTAINING_RECORD( iface, struct ui_call, IDispatchedHandler_iface );
}

static HRESULT WINAPI ui_call_QueryInterface( IDispatchedHandler *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatchedHandler ))
    {
        IDispatchedHandler_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ui_call_AddRef( IDispatchedHandler *iface )
{
    return InterlockedIncrement( &impl_from_IDispatchedHandler( iface )->ref );
}

static ULONG WINAPI ui_call_Release( IDispatchedHandler *iface )
{
    struct ui_call *impl = impl_from_IDispatchedHandler( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->target) ICoreDropOperationTarget_Release( impl->target );
        if (impl->info) ICoreDragInfo_Release( impl->info );
        if (impl->ui_override) ICoreDragUIOverride_Release( impl->ui_override );
        if (impl->async) IAsyncOperation_DataPackageOperation_Release( &impl->async->IAsyncOperation_DataPackageOperation_iface );
        waiter_Release( &impl->waiter->IAsyncOperationCompletedHandler_DataPackageOperation_iface );
        CloseHandle( impl->done );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ui_call_Invoke( IDispatchedHandler *iface )
{
    ui_call_run( impl_from_IDispatchedHandler( iface ) );
    return S_OK;
}

static const struct IDispatchedHandlerVtbl ui_call_vtbl =
{
    ui_call_QueryInterface,
    ui_call_AddRef,
    ui_call_Release,
    ui_call_Invoke,
};

/*
 * CoreDragOperation
 */

struct drag_operation
{
    ICoreDragOperation ICoreDragOperation_iface;
    ICoreDragOperation2 ICoreDragOperation2_iface;
    ICoreDragOperationInternal ICoreDragOperationInternal_iface;
    LONG ref;
    IDataPackage *package;
    IDataPackageView *view;
    UINT32 pointer_id;
    CoreDragUIContentMode ui_mode;
    DataPackageOperation allowed;
    struct drag_async *async;
    ICoreDispatcher *dispatcher;   /* of the thread that started the drag, which gets the completion */
    UINT button;          /* the mouse button holding the drag */
    /* the drag visual, premultiplied BGRA, under drag_cs; the drag thread shows it under the pointer */
    BYTE *ghost_bits;
    int ghost_width, ghost_height;
    POINT ghost_anchor;
    LONG ghost_generation;
};

/* IMemoryBufferByteAccess {5b0d3235-4dba-4d44-865e-8f1d0e4fd04d}: GetBuffer( BYTE **value, UINT32 *capacity ) */
DEFINE_GUID( IID_IMemoryBufferByteAccess_ours, 0x5b0d3235, 0x4dba, 0x4d44, 0x86, 0x5e, 0x8f, 0x1d, 0x0e, 0x4f, 0xd0, 0x4d );
typedef struct { IUnknown IUnknown_iface; } memory_byte_access;
typedef HRESULT (WINAPI *get_buffer_fn)( IUnknown *iface, BYTE **value, UINT32 *capacity );

/* XAML's DragVisual renders the dragged element into a SoftwareBitmap and hands it over here. Keep a premultiplied
 * copy: the bitmap is Microsoft's own (native Windows.Graphics), read through LockBuffer. */
static HRESULT set_drag_ghost( struct drag_operation *drag, ISoftwareBitmap *bitmap, const Point *anchor )
{
    IMemoryBufferReference *reference = NULL;
    BitmapPlaneDescription plane;
    IBitmapBuffer *buffer = NULL;
    IMemoryBuffer *memory = NULL;
    IUnknown *access = NULL;
    BitmapPixelFormat format;
    BitmapAlphaMode alpha;
    BYTE *data, *bits = NULL;
    UINT32 capacity;
    INT32 width, height, x, y;
    HRESULT hr;

    if (!bitmap) return S_OK;
    if (FAILED(hr = ISoftwareBitmap_get_BitmapPixelFormat( bitmap, &format ))) return hr;
    ISoftwareBitmap_get_BitmapAlphaMode( bitmap, &alpha );
    ISoftwareBitmap_get_PixelWidth( bitmap, &width );
    ISoftwareBitmap_get_PixelHeight( bitmap, &height );
    TRACE( "drag %p: bitmap %dx%d format %d alpha %d anchor %s\n", drag, width, height, format, alpha,
           anchor ? wine_dbg_sprintf( "%.1f,%.1f", anchor->X, anchor->Y ) : "none" );
    if (format != BitmapPixelFormat_Bgra8 || width <= 0 || height <= 0 || width > 2048 || height > 2048) return S_OK;

    if (SUCCEEDED(hr = ISoftwareBitmap_LockBuffer( bitmap, BitmapBufferAccessMode_Read, &buffer )) &&
        SUCCEEDED(hr = IBitmapBuffer_GetPlaneDescription( buffer, 0, &plane )) &&
        SUCCEEDED(hr = IBitmapBuffer_QueryInterface( buffer, &IID_IMemoryBuffer, (void **)&memory )) &&
        SUCCEEDED(hr = IMemoryBuffer_CreateReference( memory, &reference )) &&
        SUCCEEDED(hr = IMemoryBufferReference_QueryInterface( reference, &IID_IMemoryBufferByteAccess_ours, (void **)&access )) &&
        SUCCEEDED(hr = ((get_buffer_fn)(*(void ***)access)[3])( access, &data, &capacity )) &&
        plane.StartIndex >= 0 && plane.Stride >= width * 4 &&
        (UINT64)plane.StartIndex + (UINT64)plane.Stride * (height - 1) + width * 4 <= capacity &&
        (bits = malloc( (size_t)width * height * 4 )))
    {
        for (y = 0; y < height; y++)
        {
            const BYTE *in = data + plane.StartIndex + (size_t)y * plane.Stride;
            BYTE *out = bits + (size_t)y * width * 4;
            for (x = 0; x < width; x++, in += 4, out += 4)
            {
                BYTE a = alpha == BitmapAlphaMode_Ignore ? 255 : in[3];
                if (alpha == BitmapAlphaMode_Straight)
                {
                    out[0] = in[0] * a / 255; out[1] = in[1] * a / 255; out[2] = in[2] * a / 255;
                }
                else
                {
                    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
                }
                out[3] = a;
            }
        }
    }
    else WARN( "cannot read the bitmap, hr %#lx\n", hr );
    if (access) IUnknown_Release( access );
    if (reference)
    {
        IClosable *closable;
        if (SUCCEEDED(IMemoryBufferReference_QueryInterface( reference, &IID_IClosable, (void **)&closable )))
        {
            IClosable_Close( closable );
            IClosable_Release( closable );
        }
        IMemoryBufferReference_Release( reference );
    }
    if (memory) IMemoryBuffer_Release( memory );
    if (buffer)
    {
        IClosable *closable;
        if (SUCCEEDED(IBitmapBuffer_QueryInterface( buffer, &IID_IClosable, (void **)&closable )))
        {
            IClosable_Close( closable );
            IClosable_Release( closable );
        }
        IBitmapBuffer_Release( buffer );
    }
    if (!bits) return S_OK;

    EnterCriticalSection( &drag_cs );
    free( drag->ghost_bits );
    drag->ghost_bits = bits;
    drag->ghost_width = width;
    drag->ghost_height = height;
    if (anchor)
    {
        drag->ghost_anchor.x = anchor->X;
        drag->ghost_anchor.y = anchor->Y;
    }
    else
    {
        drag->ghost_anchor.x = width / 2;
        drag->ghost_anchor.y = height / 2;
    }
    drag->ghost_generation++;
    LeaveCriticalSection( &drag_cs );
    return S_OK;
}

/* The drag visual follows the pointer in a window of its own: layered and WS_EX_TRANSPARENT, so WindowFromPoint (and
 * the drop target lookup) sees through it, and shown without activation, so X maps it override-redirect. */
static LRESULT CALLBACK ghost_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void update_drag_ghost( struct drag_operation *drag, HWND *ghost, LONG *shown_generation, POINT pt )
{
    static const WCHAR class_name[] = L"WineDragVisual";
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 0xe0, AC_SRC_ALPHA };
    POINT dst, src = { 0, 0 };
    SIZE size;
    HBITMAP dib, old;
    HDC dc;
    void *bits;

    EnterCriticalSection( &drag_cs );
    if (!drag->ghost_bits)
    {
        LeaveCriticalSection( &drag_cs );
        return;
    }
    dst.x = pt.x - drag->ghost_anchor.x;
    dst.y = pt.y - drag->ghost_anchor.y;
    if (*ghost && *shown_generation == drag->ghost_generation)
    {
        LeaveCriticalSection( &drag_cs );
        SetWindowPos( *ghost, HWND_TOPMOST, dst.x, dst.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE );
        return;
    }
    if (!*ghost)
    {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = ghost_wndproc;
        wc.hInstance = GetModuleHandleW( NULL );
        wc.lpszClassName = class_name;
        RegisterClassW( &wc );
        *ghost = CreateWindowExW( WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                  class_name, NULL, WS_POPUP, dst.x, dst.y, drag->ghost_width, drag->ghost_height,
                                  NULL, NULL, wc.hInstance, NULL );
        if (!*ghost)
        {
            LeaveCriticalSection( &drag_cs );
            return;
        }
    }
    size.cx = drag->ghost_width;
    size.cy = drag->ghost_height;
    {
        BITMAPINFO info = { { sizeof(info.bmiHeader), size.cx, -size.cy, 1, 32, BI_RGB } };
        dc = CreateCompatibleDC( NULL );
        dib = CreateDIBSection( dc, &info, DIB_RGB_COLORS, &bits, NULL, 0 );
        if (dib) memcpy( bits, drag->ghost_bits, (size_t)size.cx * size.cy * 4 );
    }
    *shown_generation = drag->ghost_generation;
    LeaveCriticalSection( &drag_cs );
    if (dib)
    {
        old = SelectObject( dc, dib );
        UpdateLayeredWindow( *ghost, NULL, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA );
        SelectObject( dc, old );
        DeleteObject( dib );
        ShowWindow( *ghost, SW_SHOWNA );
        TRACE( "drag %p: visual %ldx%ld shown at %ld,%ld\n", drag, size.cx, size.cy, dst.x, dst.y );
    }
    DeleteDC( dc );
}

/* XAML's completion handler must run on the UI thread that started the drag: it looks up that thread's
 * XAML core, and faulted on a NULL one when called from the drag thread. */
static void complete_on_ui( struct drag_operation *drag, DataPackageOperation result )
{
    IAsyncAction *action = NULL;
    struct ui_call *call;

    if (drag->dispatcher && (call = calloc( 1, sizeof(*call) )))
    {
        call->IDispatchedHandler_iface.lpVtbl = &ui_call_vtbl;
        call->ref = 1;
        call->call = CALL_COMPLETE;
        call->done = CreateEventW( NULL, TRUE, FALSE, NULL );
        call->waiter = create_waiter();
        call->async = drag->async;
        IAsyncOperation_DataPackageOperation_AddRef( &call->async->IAsyncOperation_DataPackageOperation_iface );
        call->result = result;
        if (SUCCEEDED(ICoreDispatcher_RunAsync( drag->dispatcher, CoreDispatcherPriority_Normal,
                                                &call->IDispatchedHandler_iface, &action )))
        {
            if (action) IAsyncAction_Release( action );
            IDispatchedHandler_Release( &call->IDispatchedHandler_iface );
            return;
        }
        IDispatchedHandler_Release( &call->IDispatchedHandler_iface );
    }
    complete_drag_async( drag->async, result );
}

/* Calls a target on its window's UI thread, and waits for the IAsyncOperation or IAsyncAction it returns. */
static HRESULT call_target( struct drag_operation *drag, enum target_call kind, struct target_registration *reg,
                            ICoreDropOperationTarget **target, POINT pt, DWORD timeout, DataPackageOperation *result )
{
    struct ui_call *call;
    struct drag_info *info;
    struct drag_ui_override *ui_override;
    IAsyncAction *action = NULL;
    HRESULT hr;

    if (!(call = calloc( 1, sizeof(*call) ))) return E_OUTOFMEMORY;
    call->IDispatchedHandler_iface.lpVtbl = &ui_call_vtbl;
    call->ref = 1;
    call->call = kind;
    call->reg = reg;
    call->done = CreateEventW( NULL, TRUE, FALSE, NULL );
    call->waiter = create_waiter();
    call->hr = E_PENDING;

    if (kind != CALL_REQUEST)
    {
        UINT dpi = GetDpiForWindow( reg->hwnd );
        float scale = dpi ? dpi / 96.0f : 1.0f;
        POINT client = pt;

        ScreenToClient( reg->hwnd, &client );
        if ((info = calloc( 1, sizeof(*info) )))
        {
            info->ICoreDragInfo_iface.lpVtbl = &drag_info_vtbl;
            info->ICoreDragInfo2_iface.lpVtbl = &drag_info2_vtbl;
            info->ref = 1;
            if ((info->view = drag->view)) IDataPackageView_AddRef( info->view );
            info->position.X = client.x / scale;
            info->position.Y = client.y / scale;
            info->allowed = drag->allowed;
            if (GetAsyncKeyState( VK_SHIFT ) < 0) info->modifiers |= DragDropModifiers_Shift;
            if (GetAsyncKeyState( VK_CONTROL ) < 0) info->modifiers |= DragDropModifiers_Control;
            if (GetAsyncKeyState( VK_MENU ) < 0) info->modifiers |= DragDropModifiers_Alt;
            if (kind != CALL_DROP)
            {
                if (drag->button == VK_LBUTTON) info->modifiers |= DragDropModifiers_LeftButton;
                if (drag->button == VK_MBUTTON) info->modifiers |= DragDropModifiers_MiddleButton;
                if (drag->button == VK_RBUTTON) info->modifiers |= DragDropModifiers_RightButton;
            }
            call->info = &info->ICoreDragInfo_iface;
        }
        if ((ui_override = calloc( 1, sizeof(*ui_override) )))
        {
            ui_override->ICoreDragUIOverride_iface.lpVtbl = &ui_override_vtbl;
            ui_override->ref = 1;
            ui_override->content_visible = ui_override->caption_visible = ui_override->glyph_visible = TRUE;
            call->ui_override = &ui_override->ICoreDragUIOverride_iface;
        }
        call->target = *target;
        ICoreDropOperationTarget_AddRef( call->target );
        TRACE( "%s at %.1f,%.1f modifiers %#x\n", call_names[kind], info ? info->position.X : 0,
               info ? info->position.Y : 0, info ? info->modifiers : 0 );
    }

    if (reg->dispatcher)
    {
        hr = ICoreDispatcher_RunAsync( reg->dispatcher, CoreDispatcherPriority_Normal, &call->IDispatchedHandler_iface,
                                       &action );
        if (action) IAsyncAction_Release( action );
        if (FAILED(hr))
        {
            WARN( "RunAsync failed, hr %#lx\n", hr );
            IDispatchedHandler_Release( &call->IDispatchedHandler_iface );
            return hr;
        }
    }
    else ui_call_run( call );

    if (WaitForSingleObject( call->done, 5000 ))
    {
        WARN( "%s: the UI thread did not run it\n", call_names[kind] );
        IDispatchedHandler_Release( &call->IDispatchedHandler_iface );
        return E_PENDING;
    }
    if (FAILED(hr = call->hr)) WARN( "%s failed, hr %#lx\n", call_names[kind], hr );
    else if (WaitForSingleObject( call->waiter->event, timeout ))
    {
        WARN( "%s did not complete in %lu ms\n", call_names[kind], timeout );
        hr = E_PENDING;
    }
    else if (result) *result = call->waiter->result;

    if (kind == CALL_REQUEST)
    {
        *target = call->target;
        call->target = NULL;
        TRACE( "TargetRequested: hwnd %p gave target %p\n", reg->hwnd, *target );
    }
    else TRACE( "%s: hr %#lx, result %#x\n", call_names[kind], hr, call->waiter->result );
    IDispatchedHandler_Release( &call->IDispatchedHandler_iface );
    return hr;
}

static DWORD WINAPI drag_thread( void *arg )
{
    struct drag_operation *drag = arg;
    DataPackageOperation result = DataPackageOperation_None, accepted = DataPackageOperation_None;
    struct target_registration *reg = NULL, *under;
    ICoreDropOperationTarget *target = NULL;
    BOOL entered = FALSE;
    POINT pt, last = { -1, -1 };
    DWORD last_over = 0;
    HWND ghost = NULL;
    LONG ghost_generation = -1;
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"wine_dataexchange_drag" );
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    TRACE( "drag %p started, button %#x\n", drag, drag->button );

    for (;;)
    {
        BOOL down = GetAsyncKeyState( drag->button ) < 0;

        GetCursorPos( &pt );
        update_drag_ghost( drag, &ghost, &ghost_generation, pt );
        /* the drag visual's window is this thread's */
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
        if (GetAsyncKeyState( VK_ESCAPE ) < 0)
        {
            TRACE( "cancelled\n" );
            if (entered) call_target( drag, CALL_LEAVE, reg, &target, pt, 1000, NULL );
            entered = FALSE;
            break;
        }

        under = registration_at( pt );
        if (under != reg)
        {
            if (entered) call_target( drag, CALL_LEAVE, reg, &target, pt, 1000, NULL );
            entered = FALSE;
            if (target) ICoreDropOperationTarget_Release( target );
            target = NULL;
            if ((reg = under)) call_target( drag, CALL_REQUEST, reg, &target, pt, 1000, NULL );
            accepted = DataPackageOperation_None;
        }

        if (!down)
        {
            /* a drop where the target accepted nothing is a leave, as OLE does it */
            if (entered && accepted != DataPackageOperation_None)
                call_target( drag, CALL_DROP, reg, &target, pt, 30000, &result );
            else if (entered)
                call_target( drag, CALL_LEAVE, reg, &target, pt, 1000, NULL );
            entered = FALSE;
            break;
        }

        if (target && !entered)
        {
            entered = TRUE;
            call_target( drag, CALL_ENTER, reg, &target, pt, 1000, &accepted );
            last = pt;
            last_over = GetTickCount();
        }
        else if (entered && (pt.x != last.x || pt.y != last.y || GetTickCount() - last_over > 250))
        {
            call_target( drag, CALL_OVER, reg, &target, pt, 1000, &accepted );
            last = pt;
            last_over = GetTickCount();
        }
        Sleep( 8 );
    }

    TRACE( "drag %p finished, result %#x\n", drag, result );
    if (ghost) DestroyWindow( ghost );
    if (target) ICoreDropOperationTarget_Release( target );
    complete_on_ui( drag, result );
    ICoreDragOperation_Release( &drag->ICoreDragOperation_iface );
    CoUninitialize();
    return 0;
}

static inline struct drag_operation *impl_from_ICoreDragOperation( ICoreDragOperation *iface )
{
    return CONTAINING_RECORD( iface, struct drag_operation, ICoreDragOperation_iface );
}

static HRESULT WINAPI drag_operation_QueryInterface( ICoreDragOperation *iface, REFIID iid, void **out )
{
    struct drag_operation *impl = impl_from_ICoreDragOperation( iface );

    TRACE( "iface %p, iid %s\n", iface, debugstr_guid( iid ) );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICoreDragOperation ))
        *out = &impl->ICoreDragOperation_iface;
    else if (IsEqualGUID( iid, &IID_ICoreDragOperation2 ))
        *out = &impl->ICoreDragOperation2_iface;
    else if (IsEqualGUID( iid, &IID_ICoreDragOperationInternal ))
        *out = &impl->ICoreDragOperationInternal_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI drag_operation_AddRef( ICoreDragOperation *iface )
{
    struct drag_operation *impl = impl_from_ICoreDragOperation( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI drag_operation_Release( ICoreDragOperation *iface )
{
    struct drag_operation *impl = impl_from_ICoreDragOperation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->view) IDataPackageView_Release( impl->view );
        if (impl->package) IDataPackage_Release( impl->package );
        if (impl->async) IAsyncOperation_DataPackageOperation_Release( &impl->async->IAsyncOperation_DataPackageOperation_iface );
        if (impl->dispatcher) ICoreDispatcher_Release( impl->dispatcher );
        free( impl->ghost_bits );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI drag_operation_GetIids( ICoreDragOperation *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI drag_operation_GetRuntimeClassName( ICoreDragOperation *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.ApplicationModel.DataTransfer.DragDrop.Core.CoreDragOperation";
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI drag_operation_GetTrustLevel( ICoreDragOperation *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI drag_operation_get_Data( ICoreDragOperation *iface, IDataPackage **value )
{
    static const WCHAR name[] = L"Windows.ApplicationModel.DataTransfer.DataPackage";
    struct drag_operation *impl = impl_from_ICoreDragOperation( iface );
    IInspectable *inspectable;
    HSTRING str;
    HRESULT hr;

    if (!impl->package)
    {
        if (FAILED(hr = WindowsCreateString( name, wcslen( name ), &str ))) return hr;
        hr = RoActivateInstance( str, &inspectable );
        WindowsDeleteString( str );
        if (FAILED(hr))
        {
            ERR( "no DataPackage, hr %#lx\n", hr );
            return hr;
        }
        hr = IInspectable_QueryInterface( inspectable, &IID_IDataPackage, (void **)&impl->package );
        IInspectable_Release( inspectable );
        if (FAILED(hr)) return hr;
    }
    TRACE( "iface %p, package %p\n", iface, impl->package );
    *value = impl->package;
    IDataPackage_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI drag_operation_SetPointerId( ICoreDragOperation *iface, UINT32 id )
{
    TRACE( "iface %p, id %u\n", iface, id );
    impl_from_ICoreDragOperation( iface )->pointer_id = id;
    return S_OK;
}

static HRESULT WINAPI drag_operation_SetDragUIContentFromSoftwareBitmap( ICoreDragOperation *iface, ISoftwareBitmap *bitmap )
{
    TRACE( "iface %p, bitmap %p\n", iface, bitmap );
    return set_drag_ghost( impl_from_ICoreDragOperation( iface ), bitmap, NULL );
}

static HRESULT WINAPI drag_operation_SetDragUIContentFromSoftwareBitmapWithAnchorPoint( ICoreDragOperation *iface,
                                                                                         ISoftwareBitmap *bitmap, Point anchor )
{
    TRACE( "iface %p, bitmap %p, anchor %.1f,%.1f\n", iface, bitmap, anchor.X, anchor.Y );
    return set_drag_ghost( impl_from_ICoreDragOperation( iface ), bitmap, &anchor );
}

static HRESULT WINAPI drag_operation_get_DragUIContentMode( ICoreDragOperation *iface, CoreDragUIContentMode *value )
{
    *value = impl_from_ICoreDragOperation( iface )->ui_mode;
    return S_OK;
}

static HRESULT WINAPI drag_operation_put_DragUIContentMode( ICoreDragOperation *iface, CoreDragUIContentMode value )
{
    TRACE( "iface %p, value %d\n", iface, value );
    impl_from_ICoreDragOperation( iface )->ui_mode = value;
    return S_OK;
}

static HRESULT start_drag( struct drag_operation *impl, IAsyncOperation_DataPackageOperation **value )
{
    static const UINT buttons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    HANDLE thread;
    unsigned int i;

    if (impl->async) return E_ILLEGAL_METHOD_CALL;
    if (!(impl->async = create_drag_async())) return E_OUTOFMEMORY;

    /* the app fills the package in DragStarting, before the drag starts */
    if (impl->package && FAILED(IDataPackage_GetView( impl->package, &impl->view ))) impl->view = NULL;

    impl->dispatcher = current_dispatcher();
    impl->button = VK_LBUTTON;
    for (i = 0; i < ARRAY_SIZE(buttons); i++)
        if (GetAsyncKeyState( buttons[i] ) < 0) { impl->button = buttons[i]; break; }

    *value = &impl->async->IAsyncOperation_DataPackageOperation_iface;
    IAsyncOperation_DataPackageOperation_AddRef( *value );

    ICoreDragOperation_AddRef( &impl->ICoreDragOperation_iface );
    if (!(thread = CreateThread( NULL, 0, drag_thread, impl, 0, NULL )))
    {
        ICoreDragOperation_Release( &impl->ICoreDragOperation_iface );
        complete_drag_async( impl->async, DataPackageOperation_None );
        return S_OK;
    }
    CloseHandle( thread );
    return S_OK;
}

static HRESULT WINAPI drag_operation_StartAsync( ICoreDragOperation *iface, IAsyncOperation_DataPackageOperation **value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    return start_drag( impl_from_ICoreDragOperation( iface ), value );
}

static const struct ICoreDragOperationVtbl drag_operation_vtbl =
{
    drag_operation_QueryInterface,
    drag_operation_AddRef,
    drag_operation_Release,
    drag_operation_GetIids,
    drag_operation_GetRuntimeClassName,
    drag_operation_GetTrustLevel,
    drag_operation_get_Data,
    drag_operation_SetPointerId,
    drag_operation_SetDragUIContentFromSoftwareBitmap,
    drag_operation_SetDragUIContentFromSoftwareBitmapWithAnchorPoint,
    drag_operation_get_DragUIContentMode,
    drag_operation_put_DragUIContentMode,
    drag_operation_StartAsync,
};

DEFINE_IINSPECTABLE( drag_operation2, ICoreDragOperation2, struct drag_operation, ICoreDragOperation_iface )

static HRESULT WINAPI drag_operation2_get_AllowedOperations( ICoreDragOperation2 *iface, DataPackageOperation *value )
{
    *value = impl_from_ICoreDragOperation2( iface )->allowed;
    return S_OK;
}

static HRESULT WINAPI drag_operation2_put_AllowedOperations( ICoreDragOperation2 *iface, DataPackageOperation value )
{
    TRACE( "iface %p, value %#x\n", iface, value );
    impl_from_ICoreDragOperation2( iface )->allowed = value;
    return S_OK;
}

static const struct ICoreDragOperation2Vtbl drag_operation2_vtbl =
{
    drag_operation2_QueryInterface,
    drag_operation2_AddRef,
    drag_operation2_Release,
    drag_operation2_GetIids,
    drag_operation2_GetRuntimeClassName,
    drag_operation2_GetTrustLevel,
    drag_operation2_get_AllowedOperations,
    drag_operation2_put_AllowedOperations,
};

DEFINE_IINSPECTABLE( drag_internal, ICoreDragOperationInternal, struct drag_operation, ICoreDragOperation_iface )

static HRESULT WINAPI drag_internal_StartAsync( ICoreDragOperationInternal *iface, IInspectable *source, Point point,
                                                IAsyncOperation_DataPackageOperation **value )
{
    TRACE( "iface %p, source %p, point %.1f,%.1f, value %p\n", iface, source, point.X, point.Y, value );
    return start_drag( impl_from_ICoreDragOperationInternal( iface ), value );
}

static const struct ICoreDragOperationInternalVtbl drag_internal_vtbl =
{
    drag_internal_QueryInterface,
    drag_internal_AddRef,
    drag_internal_Release,
    drag_internal_GetIids,
    drag_internal_GetRuntimeClassName,
    drag_internal_GetTrustLevel,
    drag_internal_StartAsync,
};

HRESULT create_drag_operation( IInspectable **out )
{
    struct drag_operation *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICoreDragOperation_iface.lpVtbl = &drag_operation_vtbl;
    impl->ICoreDragOperation2_iface.lpVtbl = &drag_operation2_vtbl;
    impl->ICoreDragOperationInternal_iface.lpVtbl = &drag_internal_vtbl;
    impl->ref = 1;
    impl->allowed = DataPackageOperation_Copy | DataPackageOperation_Move | DataPackageOperation_Link;
    TRACE( "created %p\n", impl );
    *out = (IInspectable *)&impl->ICoreDragOperation_iface;
    return S_OK;
}

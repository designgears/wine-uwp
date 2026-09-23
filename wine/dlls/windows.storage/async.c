/* WinRT IAsync* implementation
 *
 * Copyright 2022 Bernhard Kölbl for CodeWeavers
 * Copyright 2022 Rémi Bernon for CodeWeavers
 * Copyright 2026 Conor McCarthy for CodeWeavers
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

#define WIDL_using_Wine_Internal
#include "private.h"
#include "initguid.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

#define Closed 4
#define HANDLER_NOT_SET ((void *)~(ULONG_PTR)0)

struct async_info
{
    IAsyncInfoImpl IAsyncInfoImpl_iface;
    IAsyncInfo IAsyncInfo_iface;
    IInspectable *IInspectable_outer;
    LONG ref;

    async_operation_callback callback;
    TP_WORK *async_run_work;
    IUnknown *invoker;
    IUnknown *param;

    CRITICAL_SECTION cs;
    IAsyncOperationCompletedHandlerImpl *handler;
    PROPVARIANT result;
    AsyncStatus status;
    HRESULT hr;
};

static inline struct async_info *impl_from_IAsyncInfoImpl( IAsyncInfoImpl *iface )
{
    return CONTAINING_RECORD( iface, struct async_info, IAsyncInfoImpl_iface );
}

static HRESULT WINAPI async_impl_QueryInterface( IAsyncInfoImpl *iface, REFIID iid, void **out )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncInfoImpl ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncInfoImpl_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI async_impl_AddRef( IAsyncInfoImpl *iface )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_impl_Release( IAsyncInfoImpl *iface )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->handler && impl->handler != HANDLER_NOT_SET) IAsyncOperationCompletedHandlerImpl_Release( impl->handler );
        IAsyncInfo_Close( &impl->IAsyncInfo_iface );
        if (impl->param) IUnknown_Release( impl->param );
        if (impl->invoker) IUnknown_Release( impl->invoker );
        PropVariantClear( &impl->result );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_impl_put_Completed( IAsyncInfoImpl *iface, IAsyncOperationCompletedHandlerImpl *handler )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, handler %p.\n", iface, handler );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Closed) hr = E_ILLEGAL_METHOD_CALL;
    else if (impl->handler != HANDLER_NOT_SET) hr = E_ILLEGAL_DELEGATE_ASSIGNMENT;
    else if ((impl->handler = handler))
    {
        IAsyncOperationCompletedHandlerImpl_AddRef( impl->handler );

        if (impl->status > Started)
        {
            IInspectable *operation = impl->IInspectable_outer;
            AsyncStatus status = impl->status;
            impl->handler = NULL; /* Prevent concurrent invoke. */
            LeaveCriticalSection( &impl->cs );

            IAsyncOperationCompletedHandlerImpl_Invoke( handler, operation, status );
            IAsyncOperationCompletedHandlerImpl_Release( handler );

            return S_OK;
        }
    }
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static HRESULT WINAPI async_impl_get_Completed( IAsyncInfoImpl *iface, IAsyncOperationCompletedHandlerImpl **handler )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, handler %p.\n", iface, handler );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Closed) hr = E_ILLEGAL_METHOD_CALL;
    if (impl->handler == NULL || impl->handler == HANDLER_NOT_SET) *handler = NULL;
    else IAsyncOperationCompletedHandlerImpl_AddRef( (*handler = impl->handler) );
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static HRESULT WINAPI async_impl_get_Result( IAsyncInfoImpl *iface, PROPVARIANT *result )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );
    HRESULT hr = E_ILLEGAL_METHOD_CALL;

    TRACE( "iface %p, result %p.\n", iface, result );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Completed || impl->status == Error)
    {
        PropVariantCopy( result, &impl->result );
        hr = impl->hr;
    }
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static BOOL async_info_complete( struct async_info *impl, BOOL called_async )
{
    IInspectable *operation = impl->IInspectable_outer;
    PROPVARIANT result = {0};
    HRESULT hr;

    hr = impl->callback( impl->invoker, impl->param, &result, called_async );
    if (!called_async && hr == STATUS_PENDING) return FALSE;

    EnterCriticalSection( &impl->cs );
    if (impl->status != Closed) impl->status = FAILED(hr) ? Error : Completed;
    PropVariantCopy( &impl->result, &result );
    impl->hr = hr;

    if (impl->handler != NULL && impl->handler != HANDLER_NOT_SET)
    {
        IAsyncOperationCompletedHandlerImpl *handler = impl->handler;
        AsyncStatus status = impl->status;
        impl->handler = NULL; /* Prevent concurrent invoke. */
        LeaveCriticalSection( &impl->cs );

        IAsyncOperationCompletedHandlerImpl_Invoke( handler, operation, status );
        IAsyncOperationCompletedHandlerImpl_Release( handler );
    }
    else LeaveCriticalSection( &impl->cs );

    /* release refcount acquired in Start */
    IInspectable_Release( operation );

    PropVariantClear( &result );
    return TRUE;
}

static HRESULT WINAPI async_impl_Start( IAsyncInfoImpl *iface )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );

    TRACE( "iface %p.\n", iface );

    /* keep the async alive in the callback */
    IInspectable_AddRef( impl->IInspectable_outer );
    if (!async_info_complete( impl, FALSE )) SubmitThreadpoolWork( impl->async_run_work );

    return S_OK;
}

static const struct IAsyncInfoImplVtbl async_impl_vtbl =
{
    /* IUnknown methods */
    async_impl_QueryInterface,
    async_impl_AddRef,
    async_impl_Release,
    /* IAsyncInfoImpl */
    async_impl_put_Completed,
    async_impl_get_Completed,
    async_impl_get_Result,
    async_impl_Start,
};

DEFINE_IINSPECTABLE_OUTER( async_info, IAsyncInfo, struct async_info, IInspectable_outer )

static HRESULT WINAPI async_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    struct async_info *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, id %p.\n", iface, id );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Closed) hr = E_ILLEGAL_METHOD_CALL;
    *id = 1;
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static HRESULT WINAPI async_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_info *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, status %p.\n", iface, status );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Closed) hr = E_ILLEGAL_METHOD_CALL;
    *status = impl->status;
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static HRESULT WINAPI async_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    struct async_info *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, error_code %p.\n", iface, error_code );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Closed) *error_code = hr = E_ILLEGAL_METHOD_CALL;
    else *error_code = impl->hr;
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static HRESULT WINAPI async_info_Cancel( IAsyncInfo *iface )
{
    struct async_info *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Closed) hr = E_ILLEGAL_METHOD_CALL;
    else if (impl->status == Started) impl->status = Canceled;
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static HRESULT WINAPI async_info_Close( IAsyncInfo *iface )
{
    struct async_info *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    if (impl->status == Started)
        hr = E_ILLEGAL_STATE_CHANGE;
    else if (impl->status != Closed)
    {
        CloseThreadpoolWork( impl->async_run_work );
        impl->async_run_work = NULL;
        impl->status = Closed;
    }
    LeaveCriticalSection( &impl->cs );

    return hr;
}

static const struct IAsyncInfoVtbl async_info_vtbl =
{
    /* IUnknown methods */
    async_info_QueryInterface,
    async_info_AddRef,
    async_info_Release,
    /* IInspectable methods */
    async_info_GetIids,
    async_info_GetRuntimeClassName,
    async_info_GetTrustLevel,
    /* IAsyncInfo */
    async_info_get_Id,
    async_info_get_Status,
    async_info_get_ErrorCode,
    async_info_Cancel,
    async_info_Close,
};

static void CALLBACK async_info_callback( TP_CALLBACK_INSTANCE *instance, void *iface, TP_WORK *work )
{
    struct async_info *impl = impl_from_IAsyncInfoImpl( iface );

    async_info_complete( impl, TRUE );
}

static HRESULT async_info_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                  IInspectable *outer, IAsyncInfoImpl **out )
{
    struct async_info *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(struct async_info) ))) return E_OUTOFMEMORY;
    impl->IAsyncInfoImpl_iface.lpVtbl = &async_impl_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_vtbl;
    impl->IInspectable_outer = outer;
    impl->ref = 1;

    impl->callback = callback;
    impl->handler = HANDLER_NOT_SET;
    impl->status = Started;
    if (!(impl->async_run_work = CreateThreadpoolWork( async_info_callback, &impl->IAsyncInfoImpl_iface, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        free( impl );
        return hr;
    }

    if ((impl->invoker = invoker)) IUnknown_AddRef( impl->invoker );
    if ((impl->param = param)) IUnknown_AddRef( impl->param );

    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)( __FILE__ ": async_info.cs" );

    *out = &impl->IAsyncInfoImpl_iface;
    return S_OK;
}

#define DEFINE_IASYNCOPERATIONWITHPROGRESS( iface_type, progress_type, completed_type, cs_type_str, impl_type,      \
        result_type, propvar_type, propvar_name, creation_fn )                                                      \
static inline struct impl_type *impl_from_##iface_type( iface_type *iface )                                         \
{                                                                                                                   \
    return CONTAINING_RECORD( iface, struct impl_type, iface_type##_iface );                                        \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_QueryInterface( iface_type *iface, REFIID iid, void **out )                       \
{                                                                                                                   \
    struct impl_type *impl = impl_from_##iface_type( iface );                                                       \
                                                                                                                    \
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );                                       \
                                                                                                                    \
    if (IsEqualGUID( iid, &IID_IUnknown ) ||                                                                        \
        IsEqualGUID( iid, &IID_IInspectable ) ||                                                                    \
        IsEqualGUID( iid, &IID_IAgileObject ) ||                                                                    \
        IsEqualGUID( iid, &IID_##iface_type ))                                                                      \
    {                                                                                                               \
        IInspectable_AddRef( (*out = &impl->iface_type##_iface) );                                                  \
        return S_OK;                                                                                                \
    }                                                                                                               \
                                                                                                                    \
    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );                                   \
}                                                                                                                   \
static ULONG WINAPI impl_type##_AddRef( iface_type *iface )                                                         \
{                                                                                                                   \
    struct impl_type *impl = impl_from_##iface_type( iface );                                                       \
    ULONG ref = InterlockedIncrement( &impl->ref );                                                                 \
    TRACE( "iface %p, ref %lu.\n", iface, ref );                                                                    \
    return ref;                                                                                                     \
}                                                                                                                   \
static ULONG WINAPI impl_type##_Release( iface_type *iface )                                                        \
{                                                                                                                   \
    struct impl_type *impl = impl_from_##iface_type( iface );                                                       \
    ULONG ref = InterlockedDecrement( &impl->ref );                                                                 \
    TRACE( "iface %p, ref %lu.\n", iface, ref );                                                                    \
                                                                                                                    \
    if (!ref)                                                                                                       \
    {                                                                                                               \
        /* guard against re-entry if inner releases an outer iface */                                               \
        InterlockedIncrement( &impl->ref );                                                                         \
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );                                                       \
        free( impl );                                                                                               \
    }                                                                                                               \
                                                                                                                    \
    return ref;                                                                                                     \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )                        \
{                                                                                                                   \
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );                                     \
    return E_NOTIMPL;                                                                                               \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )                     \
{                                                                                                                   \
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperationWithProgress`2<"cs_type_str">",                 \
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperationWithProgress`2<"cs_type_str">"),     \
                                class_name );                                                                       \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )                       \
{                                                                                                                   \
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );                                                \
    return E_NOTIMPL;                                                                                               \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_put_Progress( iface_type *iface, progress_type *handler )                         \
{                                                                                                                   \
    FIXME( "iface %p, handler %p stub!\n", iface, handler );                                                        \
    return E_NOTIMPL;                                                                                               \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_get_Progress( iface_type *iface, progress_type **handler )                        \
{                                                                                                                   \
    FIXME( "iface %p, handler %p stub!\n", iface, handler );                                                        \
    return E_NOTIMPL;                                                                                               \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_put_Completed( iface_type *iface, completed_type *typed_handler )                 \
{                                                                                                                   \
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)typed_handler;            \
    struct impl_type *impl = impl_from_##iface_type( iface );                                                       \
    TRACE( "iface %p, handler %p.\n", iface, handler );                                                             \
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );                                     \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_get_Completed( iface_type *iface, completed_type **typed_handler )                \
{                                                                                                                   \
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)typed_handler;          \
    struct impl_type *impl = impl_from_##iface_type( iface );                                                       \
    TRACE( "iface %p, handler %p.\n", iface, handler );                                                             \
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );                                     \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetResults( iface_type *iface, result_type *results )                             \
{                                                                                                                   \
    struct impl_type *impl = impl_from_##iface_type( iface );                                                       \
    PROPVARIANT result = {.vt = propvar_type};                                                                      \
    HRESULT hr;                                                                                                     \
                                                                                                                    \
    TRACE( "iface %p, results %p.\n", iface, results );                                                             \
                                                                                                                    \
    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );                                          \
                                                                                                                    \
    *results = (result_type)result.propvar_name;                                                                    \
    if (propvar_type == VT_UNKNOWN && result.punkVal) IUnknown_AddRef( result.punkVal );                            \
    PropVariantClear( &result );                                                                                    \
    return hr;                                                                                                      \
}                                                                                                                   \
static const struct iface_type##Vtbl impl_type##_vtbl =                                                             \
{                                                                                                                   \
    /* IUnknown methods */                                                                                          \
    impl_type##_QueryInterface,                                                                                     \
    impl_type##_AddRef,                                                                                             \
    impl_type##_Release,                                                                                            \
    /* IInspectable methods */                                                                                      \
    impl_type##_GetIids,                                                                                            \
    impl_type##_GetRuntimeClassName,                                                                                \
    impl_type##_GetTrustLevel,                                                                                      \
    /* IAsyncOperationWithProgress<result_type,prog_type> */                                                        \
    impl_type##_put_Progress,                                                                                       \
    impl_type##_get_Progress,                                                                                       \
    impl_type##_put_Completed,                                                                                      \
    impl_type##_get_Completed,                                                                                      \
    impl_type##_GetResults,                                                                                         \
};                                                                                                                  \
HRESULT creation_fn( IUnknown *invoker, IUnknown *param,                                                            \
        async_operation_callback callback, iface_type **out )                                                       \
{                                                                                                                   \
    struct impl_type *impl;                                                                                         \
    HRESULT hr;                                                                                                     \
                                                                                                                    \
    *out = NULL;                                                                                                    \
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;                                                 \
    impl->iface_type##_iface.lpVtbl = &impl_type##_vtbl;                                                            \
    impl->ref = 1;                                                                                                  \
                                                                                                                    \
    if (FAILED(hr = async_info_create( invoker, param, callback,                                                    \
            (IInspectable *)&impl->iface_type##_iface, &impl->IAsyncInfoImpl_inner ))                               \
            || FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))                                     \
    {                                                                                                               \
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );                       \
        free( impl );                                                                                               \
        return hr;                                                                                                  \
    }                                                                                                               \
                                                                                                                    \
    *out = &impl->iface_type##_iface;                                                                               \
    TRACE( "created IAsyncOperationWithProgress %p.\n", *out );                                                      \
    return S_OK;                                                                                                    \
}                                                                                                                   \

struct async_buffer_uint32
{
    IAsyncOperationWithProgress_IBuffer_UINT32 IAsyncOperationWithProgress_IBuffer_UINT32_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

DEFINE_IASYNCOPERATIONWITHPROGRESS( IAsyncOperationWithProgress_IBuffer_UINT32, \
        IAsyncOperationProgressHandler_IBuffer_UINT32, \
        IAsyncOperationWithProgressCompletedHandler_IBuffer_UINT32, \
        "IBuffer,UInt32", async_buffer_uint32, IBuffer*, VT_UNKNOWN, punkVal, async_operation_buffer_uint32_create )

struct async_uint32_uint32
{
    IAsyncOperationWithProgress_UINT32_UINT32 IAsyncOperationWithProgress_UINT32_UINT32_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

DEFINE_IASYNCOPERATIONWITHPROGRESS( IAsyncOperationWithProgress_UINT32_UINT32, \
        IAsyncOperationProgressHandler_UINT32_UINT32, \
        IAsyncOperationWithProgressCompletedHandler_UINT32_UINT32, \
        "UInt32,UInt32", async_uint32_uint32, UINT32, VT_UI4, ulVal, async_operation_uint32_uint32_create )

struct async_bool
{
    IAsyncOperation_boolean IAsyncOperation_boolean_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_bool *impl_from_IAsyncOperation_boolean( IAsyncOperation_boolean *iface )
{
    return CONTAINING_RECORD( iface, struct async_bool, IAsyncOperation_boolean_iface );
}

static HRESULT WINAPI async_bool_QueryInterface( IAsyncOperation_boolean *iface, REFIID iid, void **out )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_boolean ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_boolean_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_bool_AddRef( IAsyncOperation_boolean *iface )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_bool_Release( IAsyncOperation_boolean *iface )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_bool_GetIids( IAsyncOperation_boolean *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_bool_GetRuntimeClassName( IAsyncOperation_boolean *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Boolean>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Boolean>"),
                                class_name );
}

static HRESULT WINAPI async_bool_GetTrustLevel( IAsyncOperation_boolean *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_bool_put_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_bool_get_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_bool_GetResults( IAsyncOperation_boolean *iface, BOOLEAN *results )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );
    PROPVARIANT result = {.vt = VT_BOOL};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = result.boolVal;
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_booleanVtbl async_bool_vtbl =
{
    /* IUnknown methods */
    async_bool_QueryInterface,
    async_bool_AddRef,
    async_bool_Release,
    /* IInspectable methods */
    async_bool_GetIids,
    async_bool_GetRuntimeClassName,
    async_bool_GetTrustLevel,
    /* IAsyncOperation<boolean> */
    async_bool_put_Completed,
    async_bool_get_Completed,
    async_bool_GetResults,
};

HRESULT async_operation_boolean_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_boolean **out )
{
    struct async_bool *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_boolean_iface.lpVtbl = &async_bool_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_boolean_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_boolean_iface;
    TRACE( "created IAsyncOperation_boolean %p\n", *out );
    return S_OK;
}

struct async_storage_file
{
    IAsyncOperation_StorageFile IAsyncOperation_StorageFile_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_storage_file *impl_from_IAsyncOperation_StorageFile( IAsyncOperation_StorageFile *iface )
{
    return CONTAINING_RECORD( iface, struct async_storage_file, IAsyncOperation_StorageFile_iface );
}

static HRESULT WINAPI async_storage_file_QueryInterface( IAsyncOperation_StorageFile *iface, REFIID iid, void **out )
{
    struct async_storage_file *impl = impl_from_IAsyncOperation_StorageFile( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_StorageFile ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_StorageFile_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_storage_file_AddRef( IAsyncOperation_StorageFile *iface )
{
    struct async_storage_file *impl = impl_from_IAsyncOperation_StorageFile( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_storage_file_Release( IAsyncOperation_StorageFile *iface )
{
    struct async_storage_file *impl = impl_from_IAsyncOperation_StorageFile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_storage_file_GetIids( IAsyncOperation_StorageFile *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_storage_file_GetRuntimeClassName( IAsyncOperation_StorageFile *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>"),
                                class_name );
}

static HRESULT WINAPI async_storage_file_GetTrustLevel( IAsyncOperation_StorageFile *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_storage_file_put_Completed( IAsyncOperation_StorageFile *iface, IAsyncOperationCompletedHandler_StorageFile *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_storage_file *impl = impl_from_IAsyncOperation_StorageFile( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_storage_file_get_Completed( IAsyncOperation_StorageFile *iface, IAsyncOperationCompletedHandler_StorageFile **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_storage_file *impl = impl_from_IAsyncOperation_StorageFile( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_storage_file_GetResults( IAsyncOperation_StorageFile *iface, IStorageFile **results )
{
    struct async_storage_file *impl = impl_from_IAsyncOperation_StorageFile( iface );
    PROPVARIANT result = {.vt = VT_UNKNOWN};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = (IStorageFile *)result.punkVal;
    if (result.punkVal) IUnknown_AddRef( result.punkVal );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_StorageFileVtbl async_storage_file_vtbl =
{
    /* IUnknown methods */
    async_storage_file_QueryInterface,
    async_storage_file_AddRef,
    async_storage_file_Release,
    /* IInspectable methods */
    async_storage_file_GetIids,
    async_storage_file_GetRuntimeClassName,
    async_storage_file_GetTrustLevel,
    /* IAsyncOperation<StorageFile> */
    async_storage_file_put_Completed,
    async_storage_file_get_Completed,
    async_storage_file_GetResults,
};

HRESULT async_operation_storage_file_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_StorageFile **out )
{
    struct async_storage_file *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_StorageFile_iface.lpVtbl = &async_storage_file_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_StorageFile_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_StorageFile_iface;
    TRACE( "created IAsyncOperation_StorageFile %p\n", *out );
    return S_OK;
}

/* IAsyncOperation<IStorageItem*>, for IStorageFolder::GetItemAsync and
 * IStorageFolder2::TryGetItemAsync. A NULL result is a legitimate "not found". */
struct async_storage_item
{
    IAsyncOperation_IStorageItem IAsyncOperation_IStorageItem_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_storage_item *impl_from_IAsyncOperation_IStorageItem( IAsyncOperation_IStorageItem *iface )
{
    return CONTAINING_RECORD( iface, struct async_storage_item, IAsyncOperation_IStorageItem_iface );
}

static HRESULT WINAPI async_storage_item_QueryInterface( IAsyncOperation_IStorageItem *iface, REFIID iid, void **out )
{
    struct async_storage_item *impl = impl_from_IAsyncOperation_IStorageItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_IStorageItem ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_IStorageItem_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_storage_item_AddRef( IAsyncOperation_IStorageItem *iface )
{
    struct async_storage_item *impl = impl_from_IAsyncOperation_IStorageItem( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_storage_item_Release( IAsyncOperation_IStorageItem *iface )
{
    struct async_storage_item *impl = impl_from_IAsyncOperation_IStorageItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_storage_item_GetIids( IAsyncOperation_IStorageItem *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_storage_item_GetRuntimeClassName( IAsyncOperation_IStorageItem *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.IStorageItem>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.IStorageItem>"),
                                class_name );
}

static HRESULT WINAPI async_storage_item_GetTrustLevel( IAsyncOperation_IStorageItem *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_storage_item_put_Completed( IAsyncOperation_IStorageItem *iface, IAsyncOperationCompletedHandler_IStorageItem *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_storage_item *impl = impl_from_IAsyncOperation_IStorageItem( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_storage_item_get_Completed( IAsyncOperation_IStorageItem *iface, IAsyncOperationCompletedHandler_IStorageItem **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_storage_item *impl = impl_from_IAsyncOperation_IStorageItem( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_storage_item_GetResults( IAsyncOperation_IStorageItem *iface, IStorageItem **results )
{
    struct async_storage_item *impl = impl_from_IAsyncOperation_IStorageItem( iface );
    PROPVARIANT result = {.vt = VT_UNKNOWN};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = (IStorageItem *)result.punkVal;
    if (result.punkVal) IUnknown_AddRef( result.punkVal );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_IStorageItemVtbl async_storage_item_vtbl =
{
    /* IUnknown methods */
    async_storage_item_QueryInterface,
    async_storage_item_AddRef,
    async_storage_item_Release,
    /* IInspectable methods */
    async_storage_item_GetIids,
    async_storage_item_GetRuntimeClassName,
    async_storage_item_GetTrustLevel,
    /* IAsyncOperation<IStorageItem> */
    async_storage_item_put_Completed,
    async_storage_item_get_Completed,
    async_storage_item_GetResults,
};

HRESULT async_operation_storage_item_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_IStorageItem **out )
{
    struct async_storage_item *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_IStorageItem_iface.lpVtbl = &async_storage_item_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_IStorageItem_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_IStorageItem_iface;
    TRACE( "created IAsyncOperation_IStorageItem %p\n", *out );
    return S_OK;
}

/* IAsyncOperation<HSTRING>, for FileIO.ReadTextAsync: the callback leaves the text in the
 * PROPVARIANT as a VT_LPWSTR and GetResults turns it into an HSTRING. */
struct async_hstring
{
    __FIAsyncOperation_1_HSTRING __FIAsyncOperation_1_HSTRING_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_hstring *impl_from___FIAsyncOperation_1_HSTRING( __FIAsyncOperation_1_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct async_hstring, __FIAsyncOperation_1_HSTRING_iface );
}

static HRESULT WINAPI async_hstring_QueryInterface( __FIAsyncOperation_1_HSTRING *iface, REFIID iid, void **out )
{
    struct async_hstring *impl = impl_from___FIAsyncOperation_1_HSTRING( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID___FIAsyncOperation_1_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->__FIAsyncOperation_1_HSTRING_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_hstring_AddRef( __FIAsyncOperation_1_HSTRING *iface )
{
    struct async_hstring *impl = impl_from___FIAsyncOperation_1_HSTRING( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_hstring_Release( __FIAsyncOperation_1_HSTRING *iface )
{
    struct async_hstring *impl = impl_from___FIAsyncOperation_1_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_hstring_GetIids( __FIAsyncOperation_1_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_hstring_GetRuntimeClassName( __FIAsyncOperation_1_HSTRING *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<String>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<String>"),
                                class_name );
}

static HRESULT WINAPI async_hstring_GetTrustLevel( __FIAsyncOperation_1_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_hstring_put_Completed( __FIAsyncOperation_1_HSTRING *iface, __FIAsyncOperationCompletedHandler_1_HSTRING *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_hstring *impl = impl_from___FIAsyncOperation_1_HSTRING( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_hstring_get_Completed( __FIAsyncOperation_1_HSTRING *iface, __FIAsyncOperationCompletedHandler_1_HSTRING **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_hstring *impl = impl_from___FIAsyncOperation_1_HSTRING( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_hstring_GetResults( __FIAsyncOperation_1_HSTRING *iface, HSTRING *results )
{
    struct async_hstring *impl = impl_from___FIAsyncOperation_1_HSTRING( iface );
    PROPVARIANT result = {.vt = VT_LPWSTR};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    *results = NULL;
    if (FAILED(hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result ))) return hr;
    if (result.pwszVal) hr = WindowsCreateString( result.pwszVal, wcslen( result.pwszVal ), results );
    PropVariantClear( &result );
    return hr;
}

static const struct __FIAsyncOperation_1_HSTRINGVtbl async_hstring_vtbl =
{
    /* IUnknown methods */
    async_hstring_QueryInterface,
    async_hstring_AddRef,
    async_hstring_Release,
    /* IInspectable methods */
    async_hstring_GetIids,
    async_hstring_GetRuntimeClassName,
    async_hstring_GetTrustLevel,
    /* IAsyncOperation<HSTRING> */
    async_hstring_put_Completed,
    async_hstring_get_Completed,
    async_hstring_GetResults,
};

HRESULT async_operation_hstring_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        __FIAsyncOperation_1_HSTRING **out )
{
    struct async_hstring *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->__FIAsyncOperation_1_HSTRING_iface.lpVtbl = &async_hstring_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->__FIAsyncOperation_1_HSTRING_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->__FIAsyncOperation_1_HSTRING_iface;
    TRACE( "created __FIAsyncOperation_1_HSTRING %p\n", *out );
    return S_OK;
}

struct async_storage_folder
{
    IAsyncOperation_StorageFolder IAsyncOperation_StorageFolder_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_storage_folder *impl_from_IAsyncOperation_StorageFolder( IAsyncOperation_StorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct async_storage_folder, IAsyncOperation_StorageFolder_iface );
}

static HRESULT WINAPI async_storage_folder_QueryInterface( IAsyncOperation_StorageFolder *iface, REFIID iid, void **out )
{
    struct async_storage_folder *impl = impl_from_IAsyncOperation_StorageFolder( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_StorageFolder ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_StorageFolder_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_storage_folder_AddRef( IAsyncOperation_StorageFolder *iface )
{
    struct async_storage_folder *impl = impl_from_IAsyncOperation_StorageFolder( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_storage_folder_Release( IAsyncOperation_StorageFolder *iface )
{
    struct async_storage_folder *impl = impl_from_IAsyncOperation_StorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_storage_folder_GetIids( IAsyncOperation_StorageFolder *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_storage_folder_GetRuntimeClassName( IAsyncOperation_StorageFolder *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFolder>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFolder>"),
                                class_name );
}

static HRESULT WINAPI async_storage_folder_GetTrustLevel( IAsyncOperation_StorageFolder *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_storage_folder_put_Completed( IAsyncOperation_StorageFolder *iface, IAsyncOperationCompletedHandler_StorageFolder *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_storage_folder *impl = impl_from_IAsyncOperation_StorageFolder( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_storage_folder_get_Completed( IAsyncOperation_StorageFolder *iface, IAsyncOperationCompletedHandler_StorageFolder **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_storage_folder *impl = impl_from_IAsyncOperation_StorageFolder( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_storage_folder_GetResults( IAsyncOperation_StorageFolder *iface, IStorageFolder **results )
{
    struct async_storage_folder *impl = impl_from_IAsyncOperation_StorageFolder( iface );
    PROPVARIANT result = {.vt = VT_UNKNOWN};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = (IStorageFolder *)result.punkVal;
    if (result.punkVal) IUnknown_AddRef( result.punkVal );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_StorageFolderVtbl async_storage_folder_vtbl =
{
    /* IUnknown methods */
    async_storage_folder_QueryInterface,
    async_storage_folder_AddRef,
    async_storage_folder_Release,
    /* IInspectable methods */
    async_storage_folder_GetIids,
    async_storage_folder_GetRuntimeClassName,
    async_storage_folder_GetTrustLevel,
    /* IAsyncOperation<StorageFile> */
    async_storage_folder_put_Completed,
    async_storage_folder_get_Completed,
    async_storage_folder_GetResults,
};

HRESULT async_operation_storage_folder_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_StorageFolder **out )
{
    struct async_storage_folder *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_StorageFolder_iface.lpVtbl = &async_storage_folder_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_StorageFolder_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_StorageFolder_iface;
    TRACE( "created IAsyncOperation_StorageFolder %p\n", *out );
    return S_OK;
}

struct async_action
{
    IAsyncAction IAsyncAction_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_action *impl_from_IAsyncAction( IAsyncAction *iface )
{
    return CONTAINING_RECORD( iface, struct async_action, IAsyncAction_iface );
}

static HRESULT WINAPI async_action_QueryInterface( IAsyncAction *iface, REFIID iid, void **out )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncAction ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncAction_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_action_AddRef( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_action_Release( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_action_GetIids( IAsyncAction *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_action_GetRuntimeClassName( IAsyncAction *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncAction",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncAction"),
                                class_name );
}

static HRESULT WINAPI async_action_GetTrustLevel( IAsyncAction *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_action_put_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_action *impl = impl_from_IAsyncAction( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_action_get_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_action *impl = impl_from_IAsyncAction( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_action_GetResults( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    PROPVARIANT result = {.vt = VT_EMPTY};
    HRESULT hr;

    TRACE( "iface %p.\n", iface );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncActionVtbl async_action_vtbl =
{
    /* IUnknown methods */
    async_action_QueryInterface,
    async_action_AddRef,
    async_action_Release,
    /* IInspectable methods */
    async_action_GetIids,
    async_action_GetRuntimeClassName,
    async_action_GetTrustLevel,
    /* IAsyncAction */
    async_action_put_Completed,
    async_action_get_Completed,
    async_action_GetResults,
};

HRESULT async_action_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncAction **out )
{
    struct async_action *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncAction_iface.lpVtbl = &async_action_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncAction_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncAction_iface;
    TRACE( "created IAsyncAction %p\n", *out );
    return S_OK;
}


struct async_files
{
    IAsyncOperation_IVectorView_StorageFile IAsyncOperation_IVectorView_StorageFile_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_files *impl_from_IAsyncOperation_IVectorView_StorageFile( IAsyncOperation_IVectorView_StorageFile *iface )
{
    return CONTAINING_RECORD( iface, struct async_files, IAsyncOperation_IVectorView_StorageFile_iface );
}

static HRESULT WINAPI async_files_QueryInterface( IAsyncOperation_IVectorView_StorageFile *iface, REFIID iid, void **out )
{
    struct async_files *impl = impl_from_IAsyncOperation_IVectorView_StorageFile( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_IVectorView_StorageFile ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_IVectorView_StorageFile_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_files_AddRef( IAsyncOperation_IVectorView_StorageFile *iface )
{
    struct async_files *impl = impl_from_IAsyncOperation_IVectorView_StorageFile( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_files_Release( IAsyncOperation_IVectorView_StorageFile *iface )
{
    struct async_files *impl = impl_from_IAsyncOperation_IVectorView_StorageFile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_files_GetIids( IAsyncOperation_IVectorView_StorageFile *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_files_GetRuntimeClassName( IAsyncOperation_IVectorView_StorageFile *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>"),
                                class_name );
}

static HRESULT WINAPI async_files_GetTrustLevel( IAsyncOperation_IVectorView_StorageFile *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_files_put_Completed( IAsyncOperation_IVectorView_StorageFile *iface, IAsyncOperationCompletedHandler_IVectorView_StorageFile *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_files *impl = impl_from_IAsyncOperation_IVectorView_StorageFile( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_files_get_Completed( IAsyncOperation_IVectorView_StorageFile *iface, IAsyncOperationCompletedHandler_IVectorView_StorageFile **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_files *impl = impl_from_IAsyncOperation_IVectorView_StorageFile( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_files_GetResults( IAsyncOperation_IVectorView_StorageFile *iface, IVectorView_StorageFile **results )
{
    struct async_files *impl = impl_from_IAsyncOperation_IVectorView_StorageFile( iface );
    PROPVARIANT result = {.vt = VT_UNKNOWN};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = (IVectorView_StorageFile *)result.punkVal;
    if (result.punkVal) IUnknown_AddRef( result.punkVal );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_IVectorView_StorageFileVtbl async_files_vtbl =
{
    /* IUnknown methods */
    async_files_QueryInterface,
    async_files_AddRef,
    async_files_Release,
    /* IInspectable methods */
    async_files_GetIids,
    async_files_GetRuntimeClassName,
    async_files_GetTrustLevel,
    /* IAsyncOperation<StorageFile> */
    async_files_put_Completed,
    async_files_get_Completed,
    async_files_GetResults,
};

HRESULT async_operation_files_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_IVectorView_StorageFile **out )
{
    struct async_files *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_IVectorView_StorageFile_iface.lpVtbl = &async_files_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_IVectorView_StorageFile_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_IVectorView_StorageFile_iface;
    TRACE( "created IAsyncOperation_IVectorView_StorageFile %p\n", *out );
    return S_OK;
}


struct async_folders
{
    IAsyncOperation_IVectorView_StorageFolder IAsyncOperation_IVectorView_StorageFolder_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_folders *impl_from_IAsyncOperation_IVectorView_StorageFolder( IAsyncOperation_IVectorView_StorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct async_folders, IAsyncOperation_IVectorView_StorageFolder_iface );
}

static HRESULT WINAPI async_folders_QueryInterface( IAsyncOperation_IVectorView_StorageFolder *iface, REFIID iid, void **out )
{
    struct async_folders *impl = impl_from_IAsyncOperation_IVectorView_StorageFolder( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_IVectorView_StorageFolder ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_IVectorView_StorageFolder_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_folders_AddRef( IAsyncOperation_IVectorView_StorageFolder *iface )
{
    struct async_folders *impl = impl_from_IAsyncOperation_IVectorView_StorageFolder( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_folders_Release( IAsyncOperation_IVectorView_StorageFolder *iface )
{
    struct async_folders *impl = impl_from_IAsyncOperation_IVectorView_StorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_folders_GetIids( IAsyncOperation_IVectorView_StorageFolder *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_folders_GetRuntimeClassName( IAsyncOperation_IVectorView_StorageFolder *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>"),
                                class_name );
}

static HRESULT WINAPI async_folders_GetTrustLevel( IAsyncOperation_IVectorView_StorageFolder *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_folders_put_Completed( IAsyncOperation_IVectorView_StorageFolder *iface, IAsyncOperationCompletedHandler_IVectorView_StorageFolder *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_folders *impl = impl_from_IAsyncOperation_IVectorView_StorageFolder( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_folders_get_Completed( IAsyncOperation_IVectorView_StorageFolder *iface, IAsyncOperationCompletedHandler_IVectorView_StorageFolder **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_folders *impl = impl_from_IAsyncOperation_IVectorView_StorageFolder( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_folders_GetResults( IAsyncOperation_IVectorView_StorageFolder *iface, IVectorView_StorageFolder **results )
{
    struct async_folders *impl = impl_from_IAsyncOperation_IVectorView_StorageFolder( iface );
    PROPVARIANT result = {.vt = VT_UNKNOWN};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = (IVectorView_StorageFolder *)result.punkVal;
    if (result.punkVal) IUnknown_AddRef( result.punkVal );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_IVectorView_StorageFolderVtbl async_folders_vtbl =
{
    /* IUnknown methods */
    async_folders_QueryInterface,
    async_folders_AddRef,
    async_folders_Release,
    /* IInspectable methods */
    async_folders_GetIids,
    async_folders_GetRuntimeClassName,
    async_folders_GetTrustLevel,
    /* IAsyncOperation<StorageFile> */
    async_folders_put_Completed,
    async_folders_get_Completed,
    async_folders_GetResults,
};

HRESULT async_operation_folders_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_IVectorView_StorageFolder **out )
{
    struct async_folders *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_IVectorView_StorageFolder_iface.lpVtbl = &async_folders_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_IVectorView_StorageFolder_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_IVectorView_StorageFolder_iface;
    TRACE( "created IAsyncOperation_IVectorView_StorageFolder %p\n", *out );
    return S_OK;
}


struct async_items
{
    IAsyncOperation_IVectorView_IStorageItem IAsyncOperation_IVectorView_IStorageItem_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_items *impl_from_IAsyncOperation_IVectorView_IStorageItem( IAsyncOperation_IVectorView_IStorageItem *iface )
{
    return CONTAINING_RECORD( iface, struct async_items, IAsyncOperation_IVectorView_IStorageItem_iface );
}

static HRESULT WINAPI async_items_QueryInterface( IAsyncOperation_IVectorView_IStorageItem *iface, REFIID iid, void **out )
{
    struct async_items *impl = impl_from_IAsyncOperation_IVectorView_IStorageItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_IVectorView_IStorageItem ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_IVectorView_IStorageItem_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_items_AddRef( IAsyncOperation_IVectorView_IStorageItem *iface )
{
    struct async_items *impl = impl_from_IAsyncOperation_IVectorView_IStorageItem( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_items_Release( IAsyncOperation_IVectorView_IStorageItem *iface )
{
    struct async_items *impl = impl_from_IAsyncOperation_IVectorView_IStorageItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_items_GetIids( IAsyncOperation_IVectorView_IStorageItem *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_items_GetRuntimeClassName( IAsyncOperation_IVectorView_IStorageItem *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>"),
                                class_name );
}

static HRESULT WINAPI async_items_GetTrustLevel( IAsyncOperation_IVectorView_IStorageItem *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_items_put_Completed( IAsyncOperation_IVectorView_IStorageItem *iface, IAsyncOperationCompletedHandler_IVectorView_IStorageItem *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_items *impl = impl_from_IAsyncOperation_IVectorView_IStorageItem( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_items_get_Completed( IAsyncOperation_IVectorView_IStorageItem *iface, IAsyncOperationCompletedHandler_IVectorView_IStorageItem **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_items *impl = impl_from_IAsyncOperation_IVectorView_IStorageItem( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_items_GetResults( IAsyncOperation_IVectorView_IStorageItem *iface, IVectorView_IStorageItem **results )
{
    struct async_items *impl = impl_from_IAsyncOperation_IVectorView_IStorageItem( iface );
    PROPVARIANT result = {.vt = VT_UNKNOWN};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = (IVectorView_IStorageItem *)result.punkVal;
    if (result.punkVal) IUnknown_AddRef( result.punkVal );
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_IVectorView_IStorageItemVtbl async_items_vtbl =
{
    /* IUnknown methods */
    async_items_QueryInterface,
    async_items_AddRef,
    async_items_Release,
    /* IInspectable methods */
    async_items_GetIids,
    async_items_GetRuntimeClassName,
    async_items_GetTrustLevel,
    /* IAsyncOperation<StorageFile> */
    async_items_put_Completed,
    async_items_get_Completed,
    async_items_GetResults,
};

HRESULT async_operation_items_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_IVectorView_IStorageItem **out )
{
    struct async_items *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_IVectorView_IStorageItem_iface.lpVtbl = &async_items_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_IVectorView_IStorageItem_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_IVectorView_IStorageItem_iface;
    TRACE( "created IAsyncOperation_IVectorView_IStorageItem %p\n", *out );
    return S_OK;
}


struct async_count
{
    IAsyncOperation_UINT32 IAsyncOperation_UINT32_iface;
    IAsyncInfoImpl *IAsyncInfoImpl_inner;
    LONG ref;
};

static inline struct async_count *impl_from_IAsyncOperation_UINT32( IAsyncOperation_UINT32 *iface )
{
    return CONTAINING_RECORD( iface, struct async_count, IAsyncOperation_UINT32_iface );
}

static HRESULT WINAPI async_count_QueryInterface( IAsyncOperation_UINT32 *iface, REFIID iid, void **out )
{
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_UINT32 ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_UINT32_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static ULONG WINAPI async_count_AddRef( IAsyncOperation_UINT32 *iface )
{
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_count_Release( IAsyncOperation_UINT32 *iface )
{
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        /* guard against re-entry if inner releases an outer iface */
        InterlockedIncrement( &impl->ref );
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_count_GetIids( IAsyncOperation_UINT32 *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_count_GetRuntimeClassName( IAsyncOperation_UINT32 *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<UInt32>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<UInt32>"),
                                class_name );
}

static HRESULT WINAPI async_count_GetTrustLevel( IAsyncOperation_UINT32 *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_count_put_Completed( IAsyncOperation_UINT32 *iface, IAsyncOperationCompletedHandler_UINT32 *bool_handler )
{
    IAsyncOperationCompletedHandlerImpl *handler = (IAsyncOperationCompletedHandlerImpl *)bool_handler;
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_count_get_Completed( IAsyncOperation_UINT32 *iface, IAsyncOperationCompletedHandler_UINT32 **bool_handler )
{
    IAsyncOperationCompletedHandlerImpl **handler = (IAsyncOperationCompletedHandlerImpl **)bool_handler;
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );
    TRACE( "iface %p, handler %p.\n", iface, handler );
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner, handler );
}

static HRESULT WINAPI async_count_GetResults( IAsyncOperation_UINT32 *iface, UINT32 *results )
{
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );
    PROPVARIANT result = {.vt = VT_UI4};
    HRESULT hr;

    TRACE( "iface %p, results %p.\n", iface, results );

    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );

    *results = result.ulVal;
    PropVariantClear( &result );
    return hr;
}

static const struct IAsyncOperation_UINT32Vtbl async_count_vtbl =
{
    /* IUnknown methods */
    async_count_QueryInterface,
    async_count_AddRef,
    async_count_Release,
    /* IInspectable methods */
    async_count_GetIids,
    async_count_GetRuntimeClassName,
    async_count_GetTrustLevel,
    /* IAsyncOperation<StorageFile> */
    async_count_put_Completed,
    async_count_get_Completed,
    async_count_GetResults,
};

/* IAsyncOperation<Windows.Storage.Search.IndexedState>.
 *
 * widl does not generate this parameterisation - windows.storage.idl has the IndexedState enum but
 * no async operation over it - and the interface is not in any header, so the object is the UINT32
 * one (same ABI: a 4-byte enum returned by value) under its own IID. The IID is derived with the
 * standard WinRT rule, SHA-1 over {11f47ad5-7b73-42c0-abae-878b1e16adee} and the signature
 * pinterface({9fc2b0bb-...};enum(Windows.Storage.Search.IndexedState;i4)); the same derivation
 * reproduces the IIDs widl does generate for enum parameterisations, such as
 * IAsyncOperation<DeviceAccessStatus> {c00bc2f2-a7f8-5f3f-80d1-2808ef6bca10}. */
DEFINE_GUID(IID_IAsyncOperation_IndexedState, 0x88694b1f, 0xf380, 0x574d, 0x8a, 0x05, 0x4f, 0x67, 0xbd, 0x52, 0xcd, 0x11);

static HRESULT WINAPI async_indexed_state_QueryInterface( IAsyncOperation_UINT32 *iface, REFIID iid, void **out )
{
    struct async_count *impl = impl_from_IAsyncOperation_UINT32( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_IndexedState ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_UINT32_iface) );
        return S_OK;
    }

    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );
}

static HRESULT WINAPI async_indexed_state_GetRuntimeClassName( IAsyncOperation_UINT32 *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.Search.IndexedState>",
                                ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.Search.IndexedState>"),
                                class_name );
}

static const struct IAsyncOperation_UINT32Vtbl async_indexed_state_vtbl =
{
    /* IUnknown methods */
    async_indexed_state_QueryInterface,
    async_count_AddRef,
    async_count_Release,
    /* IInspectable methods */
    async_count_GetIids,
    async_indexed_state_GetRuntimeClassName,
    async_count_GetTrustLevel,
    /* IAsyncOperation<IndexedState> */
    async_count_put_Completed,
    async_count_get_Completed,
    async_count_GetResults,
};

HRESULT async_operation_indexed_state_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_UINT32 **out )
{
    struct async_count *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_UINT32_iface.lpVtbl = &async_indexed_state_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_UINT32_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_UINT32_iface;
    TRACE( "created IAsyncOperation_IndexedState %p\n", *out );
    return S_OK;
}

HRESULT async_operation_uint32_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_UINT32 **out )
{
    struct async_count *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_UINT32_iface.lpVtbl = &async_count_vtbl;
    impl->ref = 1;

    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_UINT32_iface, &impl->IAsyncInfoImpl_inner )) ||
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))
    {
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );
        free( impl );
        return hr;
    }

    *out = &impl->IAsyncOperation_UINT32_iface;
    TRACE( "created IAsyncOperation_UINT32 %p\n", *out );
    return S_OK;
}


/* IAsyncOperation<I*> whose result is an interface pointer: the callback stores that exact
 * interface pointer (not merely something QueryInterface-able to it) in result->punkVal. */
#define DEFINE_IASYNCOPERATION_IFACE( name, result_type, class_str )                                       \
struct async_##name                                                                                        \
{                                                                                                          \
    IAsyncOperation_##result_type IAsyncOperation_iface;                                                   \
    IAsyncInfoImpl *IAsyncInfoImpl_inner;                                                                  \
    LONG ref;                                                                                              \
};                                                                                                         \
                                                                                                           \
static inline struct async_##name *async_##name##_from_iface( IAsyncOperation_##result_type *iface )      \
{                                                                                                          \
    return CONTAINING_RECORD( iface, struct async_##name, IAsyncOperation_iface );                         \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_QueryInterface( IAsyncOperation_##result_type *iface, REFIID iid,     \
                                                     void **out )                                          \
{                                                                                                          \
    struct async_##name *impl = async_##name##_from_iface( iface );                                        \
                                                                                                           \
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );                             \
                                                                                                           \
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||                      \
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_##result_type ))   \
    {                                                                                                      \
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_iface) );                                      \
        return S_OK;                                                                                       \
    }                                                                                                      \
    return IAsyncInfoImpl_QueryInterface( impl->IAsyncInfoImpl_inner, iid, out );                          \
}                                                                                                          \
                                                                                                           \
static ULONG WINAPI async_##name##_AddRef( IAsyncOperation_##result_type *iface )                          \
{                                                                                                          \
    struct async_##name *impl = async_##name##_from_iface( iface );                                        \
    return InterlockedIncrement( &impl->ref );                                                             \
}                                                                                                          \
                                                                                                           \
static ULONG WINAPI async_##name##_Release( IAsyncOperation_##result_type *iface )                         \
{                                                                                                          \
    struct async_##name *impl = async_##name##_from_iface( iface );                                        \
    ULONG ref = InterlockedDecrement( &impl->ref );                                                        \
                                                                                                           \
    if (!ref)                                                                                              \
    {                                                                                                      \
        /* guard against re-entry if inner releases an outer iface */                                      \
        InterlockedIncrement( &impl->ref );                                                                \
        IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );                                             \
        free( impl );                                                                                      \
    }                                                                                                      \
    return ref;                                                                                            \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_GetIids( IAsyncOperation_##result_type *iface, ULONG *iid_count,      \
                                              IID **iids )                                                 \
{                                                                                                          \
    if (!iid_count || !iids) return E_POINTER;                                                             \
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;                                    \
    (*iids)[0] = IID_IAsyncOperation_##result_type;                                                        \
    *iid_count = 1;                                                                                        \
    return S_OK;                                                                                           \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_GetRuntimeClassName( IAsyncOperation_##result_type *iface,           \
                                                          HSTRING *class_name )                            \
{                                                                                                          \
    return WindowsCreateString( class_str, ARRAY_SIZE(class_str) - 1, class_name );                        \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_GetTrustLevel( IAsyncOperation_##result_type *iface,                 \
                                                    TrustLevel *trust_level )                              \
{                                                                                                          \
    *trust_level = BaseTrust;                                                                              \
    return S_OK;                                                                                           \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_put_Completed( IAsyncOperation_##result_type *iface,                 \
        IAsyncOperationCompletedHandler_##result_type *handler )                                           \
{                                                                                                          \
    struct async_##name *impl = async_##name##_from_iface( iface );                                        \
    TRACE( "iface %p, handler %p.\n", iface, handler );                                                    \
    return IAsyncInfoImpl_put_Completed( impl->IAsyncInfoImpl_inner,                                       \
                                         (IAsyncOperationCompletedHandlerImpl *)handler );                 \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_get_Completed( IAsyncOperation_##result_type *iface,                 \
        IAsyncOperationCompletedHandler_##result_type **handler )                                          \
{                                                                                                          \
    struct async_##name *impl = async_##name##_from_iface( iface );                                        \
    return IAsyncInfoImpl_get_Completed( impl->IAsyncInfoImpl_inner,                                       \
                                         (IAsyncOperationCompletedHandlerImpl **)handler );                \
}                                                                                                          \
                                                                                                           \
static HRESULT WINAPI async_##name##_GetResults( IAsyncOperation_##result_type *iface,                    \
                                                 result_type **results )                                   \
{                                                                                                          \
    struct async_##name *impl = async_##name##_from_iface( iface );                                        \
    PROPVARIANT result = {.vt = VT_UNKNOWN};                                                               \
    HRESULT hr;                                                                                            \
                                                                                                           \
    TRACE( "iface %p, results %p.\n", iface, results );                                                    \
                                                                                                           \
    hr = IAsyncInfoImpl_get_Result( impl->IAsyncInfoImpl_inner, &result );                                 \
    *results = (result_type *)result.punkVal;                                                              \
    if (result.punkVal) IUnknown_AddRef( result.punkVal );                                                 \
    PropVariantClear( &result );                                                                           \
    return hr;                                                                                             \
}                                                                                                          \
                                                                                                           \
static const struct IAsyncOperation_##result_type##Vtbl async_##name##_vtbl =                              \
{                                                                                                          \
    async_##name##_QueryInterface,                                                                         \
    async_##name##_AddRef,                                                                                 \
    async_##name##_Release,                                                                                \
    async_##name##_GetIids,                                                                                \
    async_##name##_GetRuntimeClassName,                                                                    \
    async_##name##_GetTrustLevel,                                                                          \
    async_##name##_put_Completed,                                                                          \
    async_##name##_get_Completed,                                                                          \
    async_##name##_GetResults,                                                                             \
};                                                                                                         \
                                                                                                           \
HRESULT async_operation_##name##_create( IUnknown *invoker, IUnknown *param,                               \
        async_operation_callback callback, IAsyncOperation_##result_type **out )                           \
{                                                                                                          \
    struct async_##name *impl;                                                                             \
    HRESULT hr;                                                                                            \
                                                                                                           \
    *out = NULL;                                                                                           \
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;                                        \
    impl->IAsyncOperation_iface.lpVtbl = &async_##name##_vtbl;                                             \
    impl->ref = 1;                                                                                         \
                                                                                                           \
    if (FAILED(hr = async_info_create( invoker, param, callback, (IInspectable *)&impl->IAsyncOperation_iface, \
                                       &impl->IAsyncInfoImpl_inner )) ||                                   \
        FAILED(hr = IAsyncInfoImpl_Start( impl->IAsyncInfoImpl_inner )))                                   \
    {                                                                                                      \
        if (impl->IAsyncInfoImpl_inner) IAsyncInfoImpl_Release( impl->IAsyncInfoImpl_inner );              \
        free( impl );                                                                                      \
        return hr;                                                                                         \
    }                                                                                                      \
    *out = &impl->IAsyncOperation_iface;                                                                   \
    return S_OK;                                                                                           \
}

DEFINE_IASYNCOPERATION_IFACE( input_stream, IInputStream,
        L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.Streams.IInputStream>" )
DEFINE_IASYNCOPERATION_IFACE( random_access_stream, IRandomAccessStream,
        L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.Streams.IRandomAccessStream>" )
DEFINE_IASYNCOPERATION_IFACE( random_access_stream_ct, IRandomAccessStreamWithContentType,
        L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.Streams.IRandomAccessStreamWithContentType>" )

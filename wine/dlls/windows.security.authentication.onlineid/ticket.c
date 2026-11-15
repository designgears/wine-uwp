/* WinRT Windows.Security.Authentication.Onlineid Implementation
 *
 * Copyright (C) 2024 Mohamad Al-Jaf
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

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(onlineid);

struct ticket_request_factory_statics
{
    IActivationFactory IActivationFactory_iface;
    IOnlineIdServiceTicketRequestFactory IOnlineIdServiceTicketRequestFactory_iface;
    LONG ref;
};

static inline struct ticket_request_factory_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct ticket_request_factory_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct ticket_request_factory_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IOnlineIdServiceTicketRequestFactory ))
    {
        *out = &impl->IOnlineIdServiceTicketRequestFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct ticket_request_factory_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct ticket_request_factory_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
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


/* OnlineIdServiceTicketRequest: the service and policy it was created with */
struct ticket_request
{
    IOnlineIdServiceTicketRequest IOnlineIdServiceTicketRequest_iface;
    LONG ref;
    HSTRING service, policy;
};

static inline struct ticket_request *impl_from_IOnlineIdServiceTicketRequest( IOnlineIdServiceTicketRequest *iface )
{
    return CONTAINING_RECORD( iface, struct ticket_request, IOnlineIdServiceTicketRequest_iface );
}

static HRESULT WINAPI ticket_request_QueryInterface( IOnlineIdServiceTicketRequest *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IOnlineIdServiceTicketRequest ))
    {
        IOnlineIdServiceTicketRequest_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ticket_request_AddRef( IOnlineIdServiceTicketRequest *iface )
{
    return InterlockedIncrement( &impl_from_IOnlineIdServiceTicketRequest( iface )->ref );
}

static ULONG WINAPI ticket_request_Release( IOnlineIdServiceTicketRequest *iface )
{
    struct ticket_request *impl = impl_from_IOnlineIdServiceTicketRequest( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->service );
        WindowsDeleteString( impl->policy );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ticket_request_GetIids( IOnlineIdServiceTicketRequest *iface, ULONG *count, IID **iids )
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI ticket_request_GetRuntimeClassName( IOnlineIdServiceTicketRequest *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Security.Authentication.OnlineId.OnlineIdServiceTicketRequest";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI ticket_request_GetTrustLevel( IOnlineIdServiceTicketRequest *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ticket_request_get_Service( IOnlineIdServiceTicketRequest *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IOnlineIdServiceTicketRequest( iface )->service, value );
}

static HRESULT WINAPI ticket_request_get_Policy( IOnlineIdServiceTicketRequest *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IOnlineIdServiceTicketRequest( iface )->policy, value );
}

static const struct IOnlineIdServiceTicketRequestVtbl ticket_request_vtbl =
{
    ticket_request_QueryInterface,
    ticket_request_AddRef,
    ticket_request_Release,
    ticket_request_GetIids,
    ticket_request_GetRuntimeClassName,
    ticket_request_GetTrustLevel,
    ticket_request_get_Service,
    ticket_request_get_Policy,
};

static HRESULT ticket_request_create( HSTRING service, HSTRING policy, IOnlineIdServiceTicketRequest **out )
{
    struct ticket_request *impl;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IOnlineIdServiceTicketRequest_iface.lpVtbl = &ticket_request_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( service, &impl->service )) ||
        FAILED(hr = WindowsDuplicateString( policy, &impl->policy )))
    {
        IOnlineIdServiceTicketRequest_Release( &impl->IOnlineIdServiceTicketRequest_iface );
        return hr;
    }
    *out = &impl->IOnlineIdServiceTicketRequest_iface;
    return S_OK;
}

/* OnlineIdSystemTicketResult without an identity: a status and the error behind it */
struct ticket_result
{
    IOnlineIdSystemTicketResult IOnlineIdSystemTicketResult_iface;
    LONG ref;
    OnlineIdSystemTicketStatus status;
    HRESULT error;
};

static inline struct ticket_result *impl_from_IOnlineIdSystemTicketResult( IOnlineIdSystemTicketResult *iface )
{
    return CONTAINING_RECORD( iface, struct ticket_result, IOnlineIdSystemTicketResult_iface );
}

static HRESULT WINAPI ticket_result_QueryInterface( IOnlineIdSystemTicketResult *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IOnlineIdSystemTicketResult ))
    {
        IOnlineIdSystemTicketResult_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ticket_result_AddRef( IOnlineIdSystemTicketResult *iface )
{
    return InterlockedIncrement( &impl_from_IOnlineIdSystemTicketResult( iface )->ref );
}

static ULONG WINAPI ticket_result_Release( IOnlineIdSystemTicketResult *iface )
{
    struct ticket_result *impl = impl_from_IOnlineIdSystemTicketResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI ticket_result_GetIids( IOnlineIdSystemTicketResult *iface, ULONG *count, IID **iids )
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI ticket_result_GetRuntimeClassName( IOnlineIdSystemTicketResult *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Security.Authentication.OnlineId.OnlineIdSystemTicketResult";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI ticket_result_GetTrustLevel( IOnlineIdSystemTicketResult *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ticket_result_get_Identity( IOnlineIdSystemTicketResult *iface, IOnlineIdSystemIdentity **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI ticket_result_get_Status( IOnlineIdSystemTicketResult *iface, OnlineIdSystemTicketStatus *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IOnlineIdSystemTicketResult( iface )->status;
    return S_OK;
}

static HRESULT WINAPI ticket_result_get_ExtendedError( IOnlineIdSystemTicketResult *iface, HRESULT *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IOnlineIdSystemTicketResult( iface )->error;
    return S_OK;
}

static const struct IOnlineIdSystemTicketResultVtbl ticket_result_vtbl =
{
    ticket_result_QueryInterface,
    ticket_result_AddRef,
    ticket_result_Release,
    ticket_result_GetIids,
    ticket_result_GetRuntimeClassName,
    ticket_result_GetTrustLevel,
    ticket_result_get_Identity,
    ticket_result_get_Status,
    ticket_result_get_ExtendedError,
};

/* IAsyncOperation<OnlineIdSystemTicketResult> that is complete from the start: .get() finds it Completed, and a
 * Completed handler is invoked as it is set */
struct ticket_operation
{
    IAsyncOperation_OnlineIdSystemTicketResult IAsyncOperation_OnlineIdSystemTicketResult_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IOnlineIdSystemTicketResult *result;
    IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult *handler;
};

static inline struct ticket_operation *impl_from_IAsyncOperation_OnlineIdSystemTicketResult( IAsyncOperation_OnlineIdSystemTicketResult *iface )
{
    return CONTAINING_RECORD( iface, struct ticket_operation, IAsyncOperation_OnlineIdSystemTicketResult_iface );
}

static HRESULT WINAPI ticket_operation_QueryInterface( IAsyncOperation_OnlineIdSystemTicketResult *iface, REFIID iid, void **out )
{
    struct ticket_operation *impl = impl_from_IAsyncOperation_OnlineIdSystemTicketResult( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_OnlineIdSystemTicketResult ))
    {
        IAsyncOperation_OnlineIdSystemTicketResult_AddRef( (*out = iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ticket_operation_AddRef( IAsyncOperation_OnlineIdSystemTicketResult *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperation_OnlineIdSystemTicketResult( iface )->ref );
}

static ULONG WINAPI ticket_operation_Release( IAsyncOperation_OnlineIdSystemTicketResult *iface )
{
    struct ticket_operation *impl = impl_from_IAsyncOperation_OnlineIdSystemTicketResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult_Release( impl->handler );
        IOnlineIdSystemTicketResult_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ticket_operation_GetIids( IAsyncOperation_OnlineIdSystemTicketResult *iface, ULONG *count, IID **iids )
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI ticket_operation_GetRuntimeClassName( IAsyncOperation_OnlineIdSystemTicketResult *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Security.Authentication.OnlineId.OnlineIdSystemTicketResult>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI ticket_operation_GetTrustLevel( IAsyncOperation_OnlineIdSystemTicketResult *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ticket_operation_put_Completed( IAsyncOperation_OnlineIdSystemTicketResult *iface,
                                                      IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult *handler )
{
    struct ticket_operation *impl = impl_from_IAsyncOperation_OnlineIdSystemTicketResult( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (!handler) return E_POINTER;
    if (impl->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult_AddRef( (impl->handler = handler) );
    return IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult_Invoke( handler, iface, Completed );
}

static HRESULT WINAPI ticket_operation_get_Completed( IAsyncOperation_OnlineIdSystemTicketResult *iface,
                                                      IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult **handler )
{
    struct ticket_operation *impl = impl_from_IAsyncOperation_OnlineIdSystemTicketResult( iface );

    if (!handler) return E_POINTER;
    if ((*handler = impl->handler)) IAsyncOperationCompletedHandler_OnlineIdSystemTicketResult_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI ticket_operation_GetResults( IAsyncOperation_OnlineIdSystemTicketResult *iface, IOnlineIdSystemTicketResult **results )
{
    struct ticket_operation *impl = impl_from_IAsyncOperation_OnlineIdSystemTicketResult( iface );

    TRACE( "iface %p, results %p.\n", iface, results );
    if (!results) return E_POINTER;
    IOnlineIdSystemTicketResult_AddRef( (*results = impl->result) );
    return S_OK;
}

static const struct IAsyncOperation_OnlineIdSystemTicketResultVtbl ticket_operation_vtbl =
{
    ticket_operation_QueryInterface,
    ticket_operation_AddRef,
    ticket_operation_Release,
    ticket_operation_GetIids,
    ticket_operation_GetRuntimeClassName,
    ticket_operation_GetTrustLevel,
    ticket_operation_put_Completed,
    ticket_operation_get_Completed,
    ticket_operation_GetResults,
};

DEFINE_IINSPECTABLE( ticket_operation_info, IAsyncInfo, struct ticket_operation, IAsyncOperation_OnlineIdSystemTicketResult_iface )

static HRESULT WINAPI ticket_operation_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI ticket_operation_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    if (!status) return E_POINTER;
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI ticket_operation_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    if (!error_code) return E_POINTER;
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI ticket_operation_info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI ticket_operation_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const struct IAsyncInfoVtbl ticket_operation_info_vtbl =
{
    ticket_operation_info_QueryInterface,
    ticket_operation_info_AddRef,
    ticket_operation_info_Release,
    ticket_operation_info_GetIids,
    ticket_operation_info_GetRuntimeClassName,
    ticket_operation_info_GetTrustLevel,
    ticket_operation_info_get_Id,
    ticket_operation_info_get_Status,
    ticket_operation_info_get_ErrorCode,
    ticket_operation_info_Cancel,
    ticket_operation_info_Close,
};

HRESULT ticket_result_operation_create( OnlineIdSystemTicketStatus status, HRESULT error,
                                        IAsyncOperation_OnlineIdSystemTicketResult **out )
{
    struct ticket_operation *impl;
    struct ticket_result *result;

    if (!(result = calloc( 1, sizeof(*result) ))) return E_OUTOFMEMORY;
    result->IOnlineIdSystemTicketResult_iface.lpVtbl = &ticket_result_vtbl;
    result->ref = 1;
    result->status = status;
    result->error = error;

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        free( result );
        return E_OUTOFMEMORY;
    }
    impl->IAsyncOperation_OnlineIdSystemTicketResult_iface.lpVtbl = &ticket_operation_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &ticket_operation_info_vtbl;
    impl->ref = 1;
    impl->result = &result->IOnlineIdSystemTicketResult_iface;
    TRACE( "ticket result: status %d, error %#lx.\n", status, error );
    *out = &impl->IAsyncOperation_OnlineIdSystemTicketResult_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( ticket_request_factory_statics, IOnlineIdServiceTicketRequestFactory, struct ticket_request_factory_statics, IActivationFactory_iface )

static HRESULT WINAPI ticket_request_factory_statics_CreateOnlineIdServiceTicketRequest( IOnlineIdServiceTicketRequestFactory *iface, HSTRING service,
                                                                                         HSTRING policy, IOnlineIdServiceTicketRequest **request )
{
    TRACE( "iface %p, service %s, policy %s, request %p.\n", iface, debugstr_hstring( service ), debugstr_hstring( policy ), request );
    return ticket_request_create( service, policy, request );
}

static HRESULT WINAPI ticket_request_factory_statics_CreateOnlineIdServiceTicketRequestAdvanced( IOnlineIdServiceTicketRequestFactory *iface, HSTRING service,
                                                                                                 IOnlineIdServiceTicketRequest **request )
{
    TRACE( "iface %p, service %s, request %p.\n", iface, debugstr_hstring( service ), request );
    return ticket_request_create( service, NULL, request );
}

static const struct IOnlineIdServiceTicketRequestFactoryVtbl ticket_request_factory_statics_vtbl =
{
    ticket_request_factory_statics_QueryInterface,
    ticket_request_factory_statics_AddRef,
    ticket_request_factory_statics_Release,
    /* IInspectable methods */
    ticket_request_factory_statics_GetIids,
    ticket_request_factory_statics_GetRuntimeClassName,
    ticket_request_factory_statics_GetTrustLevel,
    /* IOnlineIdServiceTicketRequestFactory methods */
    ticket_request_factory_statics_CreateOnlineIdServiceTicketRequest,
    ticket_request_factory_statics_CreateOnlineIdServiceTicketRequestAdvanced,
};

static struct ticket_request_factory_statics ticket_request_factory_statics =
{
    {&factory_vtbl},
    {&ticket_request_factory_statics_vtbl},
    1,
};

IActivationFactory *ticket_factory = &ticket_request_factory_statics.IActivationFactory_iface;

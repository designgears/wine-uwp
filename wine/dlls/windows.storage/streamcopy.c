/* WinRT Windows.Storage.Streams.RandomAccessStream statics: stream copies
 *
 * Copyright 2026 Wine-UWP contributors
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

WINE_DEFAULT_DEBUG_CHANNEL(storage);

#define COPY_CHUNK (64 * 1024)

/* ---- a private IBuffer: Windows.Storage.Streams.Buffer is not implemented in Wine ---- */

struct copy_buffer
{
    IBuffer IBuffer_iface;
    IBufferByteAccess IBufferByteAccess_iface;
    LONG ref;
    UINT32 capacity, length;
    BYTE data[];
};

static inline struct copy_buffer *impl_from_IBuffer( IBuffer *iface )
{
    return CONTAINING_RECORD( iface, struct copy_buffer, IBuffer_iface );
}

static HRESULT WINAPI copy_buffer_QueryInterface( IBuffer *iface, REFIID iid, void **out )
{
    struct copy_buffer *impl = impl_from_IBuffer( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IBuffer ))
        *out = &impl->IBuffer_iface;
    else if (IsEqualGUID( iid, &IID_IBufferByteAccess ))
        *out = &impl->IBufferByteAccess_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI copy_buffer_AddRef( IBuffer *iface )
{
    return InterlockedIncrement( &impl_from_IBuffer( iface )->ref );
}

static ULONG WINAPI copy_buffer_Release( IBuffer *iface )
{
    struct copy_buffer *impl = impl_from_IBuffer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI copy_buffer_GetIids( IBuffer *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }

static HRESULT WINAPI copy_buffer_GetRuntimeClassName( IBuffer *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.Streams.Buffer", ARRAY_SIZE(L"Windows.Storage.Streams.Buffer") - 1, name );
}

static HRESULT WINAPI copy_buffer_GetTrustLevel( IBuffer *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI copy_buffer_get_Capacity( IBuffer *iface, UINT32 *value )
{
    *value = impl_from_IBuffer( iface )->capacity;
    return S_OK;
}

static HRESULT WINAPI copy_buffer_get_Length( IBuffer *iface, UINT32 *value )
{
    *value = impl_from_IBuffer( iface )->length;
    return S_OK;
}

static HRESULT WINAPI copy_buffer_put_Length( IBuffer *iface, UINT32 value )
{
    struct copy_buffer *impl = impl_from_IBuffer( iface );
    if (value > impl->capacity) return E_INVALIDARG;
    impl->length = value;
    return S_OK;
}

static const struct IBufferVtbl copy_buffer_vtbl =
{
    copy_buffer_QueryInterface,
    copy_buffer_AddRef,
    copy_buffer_Release,
    copy_buffer_GetIids,
    copy_buffer_GetRuntimeClassName,
    copy_buffer_GetTrustLevel,
    copy_buffer_get_Capacity,
    copy_buffer_get_Length,
    copy_buffer_put_Length,
};

static inline struct copy_buffer *impl_from_IBufferByteAccess( IBufferByteAccess *iface )
{
    return CONTAINING_RECORD( iface, struct copy_buffer, IBufferByteAccess_iface );
}

static HRESULT WINAPI copy_buffer_access_QueryInterface( IBufferByteAccess *iface, REFIID iid, void **out )
{
    return copy_buffer_QueryInterface( &impl_from_IBufferByteAccess( iface )->IBuffer_iface, iid, out );
}

static ULONG WINAPI copy_buffer_access_AddRef( IBufferByteAccess *iface )
{
    return copy_buffer_AddRef( &impl_from_IBufferByteAccess( iface )->IBuffer_iface );
}

static ULONG WINAPI copy_buffer_access_Release( IBufferByteAccess *iface )
{
    return copy_buffer_Release( &impl_from_IBufferByteAccess( iface )->IBuffer_iface );
}

static HRESULT WINAPI copy_buffer_access_Buffer( IBufferByteAccess *iface, BYTE **value )
{
    *value = impl_from_IBufferByteAccess( iface )->data;
    return S_OK;
}

static const struct IBufferByteAccessVtbl copy_buffer_access_vtbl =
{
    copy_buffer_access_QueryInterface,
    copy_buffer_access_AddRef,
    copy_buffer_access_Release,
    copy_buffer_access_Buffer,
};

/* ---- waiting for a stream operation ---- */

/* A completion handler that signals an event. Every AsyncOperation(WithProgress)CompletedHandler
 * has Invoke at slot 3 with (operation, status), so one object serves them all. */
struct wait_handler
{
    IUnknown IUnknown_iface;
    LONG ref;
    HANDLE event;
};

static inline struct wait_handler *impl_from_wait_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct wait_handler, IUnknown_iface );
}

static HRESULT WINAPI wait_handler_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    /* Only the handler types it is registered as. Answering every IID made .NET take it for
     * something richer and call past the end of its four-entry vtable. */
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationWithProgressCompletedHandler_IBuffer_UINT32 ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationWithProgressCompletedHandler_UINT32_UINT32 ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationCompletedHandler_boolean ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    TRACE( "%s not implemented.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI wait_handler_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_wait_IUnknown( iface )->ref );
}

static ULONG WINAPI wait_handler_Release( IUnknown *iface )
{
    struct wait_handler *impl = impl_from_wait_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        CloseHandle( impl->event );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI wait_handler_Invoke( IUnknown *iface, IUnknown *operation, AsyncStatus status )
{
    SetEvent( impl_from_wait_IUnknown( iface )->event );
    return S_OK;
}

static const struct
{
    IUnknownVtbl unknown;
    void *invoke;
}
wait_handler_vtbl =
{
    { wait_handler_QueryInterface, wait_handler_AddRef, wait_handler_Release },
    wait_handler_Invoke,
};

/* Waits for an operation whose put_Completed is slot 8 (IAsyncOperationWithProgress; plain
 * IAsyncOperation has it at slot 6) and checks it succeeded. */
static HRESULT wait_operation( IInspectable *operation, BOOL with_progress )
{
    typedef HRESULT (WINAPI *put_completed_fn)( IInspectable *, IUnknown * );
    struct wait_handler *handler;
    IAsyncInfo *info;
    AsyncStatus status;
    HRESULT hr;

    if (!(handler = calloc( 1, sizeof(*handler) ))) return E_OUTOFMEMORY;
    handler->IUnknown_iface.lpVtbl = (IUnknownVtbl *)&wait_handler_vtbl;
    handler->ref = 1;
    handler->event = CreateEventW( NULL, TRUE, FALSE, NULL );

    hr = ((put_completed_fn *)operation->lpVtbl)[with_progress ? 8 : 6]( operation, &handler->IUnknown_iface );
    if (SUCCEEDED(hr)) WaitForSingleObject( handler->event, INFINITE );
    IUnknown_Release( &handler->IUnknown_iface );
    if (FAILED(hr)) return hr;

    if (FAILED(hr = IInspectable_QueryInterface( operation, &IID_IAsyncInfo, (void **)&info ))) return hr;
    if (SUCCEEDED(hr = IAsyncInfo_get_Status( info, &status )) && status != Completed)
    {
        if (FAILED(IAsyncInfo_get_ErrorCode( info, &hr )) || SUCCEEDED(hr)) hr = E_FAIL;
    }
    IAsyncInfo_Release( info );
    return hr;
}

/* ---- the copy ---- */

struct copy_params
{
    IUnknown IUnknown_iface;
    LONG ref;
    IInputStream *source;
    IOutputStream *dest;
    UINT64 limit;   /* bytes to copy, or ~0 for all */
    BOOL close;
};

static inline struct copy_params *impl_from_copy_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct copy_params, IUnknown_iface );
}

static HRESULT WINAPI copy_params_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI copy_params_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_copy_IUnknown( iface )->ref );
}

static ULONG WINAPI copy_params_Release( IUnknown *iface )
{
    struct copy_params *impl = impl_from_copy_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IInputStream_Release( impl->source );
        IOutputStream_Release( impl->dest );
        free( impl );
    }
    return ref;
}

static const IUnknownVtbl copy_params_vtbl = { copy_params_QueryInterface, copy_params_AddRef, copy_params_Release };

/* A Windows.Storage.Streams.Buffer of the given capacity, empty. */
HRESULT buffer_create( UINT32 capacity, IBuffer **out )
{
    struct copy_buffer *buffer;

    if (!(buffer = calloc( 1, offsetof( struct copy_buffer, data[capacity] ) ))) return E_OUTOFMEMORY;
    buffer->IBuffer_iface.lpVtbl = &copy_buffer_vtbl;
    buffer->IBufferByteAccess_iface.lpVtbl = &copy_buffer_access_vtbl;
    buffer->ref = 1;
    buffer->capacity = capacity;
    *out = &buffer->IBuffer_iface;
    return S_OK;
}

static HRESULT stream_copy_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct copy_params *params = impl_from_copy_IUnknown( param );
    struct copy_buffer *buffer;
    UINT64 total = 0;
    HRESULT hr = S_OK;

    /* the copy blocks on the streams; do it on the thread pool, not the caller's thread */
    if (!called_async) return STATUS_PENDING;

    if (!(buffer = calloc( 1, offsetof( struct copy_buffer, data[COPY_CHUNK] ) ))) return E_OUTOFMEMORY;
    buffer->IBuffer_iface.lpVtbl = &copy_buffer_vtbl;
    buffer->IBufferByteAccess_iface.lpVtbl = &copy_buffer_access_vtbl;
    buffer->ref = 1;

    while (total < params->limit)
    {
        IAsyncOperationWithProgress_IBuffer_UINT32 *read;
        IAsyncOperationWithProgress_UINT32_UINT32 *write;
        IBuffer *filled = NULL;
        UINT32 want = params->limit - total < COPY_CHUNK ? params->limit - total : COPY_CHUNK, got = 0, written;

        buffer->capacity = want;
        buffer->length = 0;
        if (FAILED(hr = IInputStream_ReadAsync( params->source, &buffer->IBuffer_iface, want,
                InputStreamOptions_None, &read ))) break;
        if (SUCCEEDED(hr = wait_operation( (IInspectable *)read, TRUE )))
            hr = IAsyncOperationWithProgress_IBuffer_UINT32_GetResults( read, &filled );
        IAsyncOperationWithProgress_IBuffer_UINT32_Release( read );
        if (FAILED(hr)) break;
        if (filled)
        {
            IBuffer_get_Length( filled, &got );
            /* the source may hand back its own buffer rather than filling ours */
            if (filled != &buffer->IBuffer_iface)
            {
                IBufferByteAccess *access;
                BYTE *bytes;
                if (SUCCEEDED(IBuffer_QueryInterface( filled, &IID_IBufferByteAccess, (void **)&access )))
                {
                    got = min( got, COPY_CHUNK );
                    IBufferByteAccess_Buffer( access, &bytes );
                    memcpy( buffer->data, bytes, got );
                    IBufferByteAccess_Release( access );
                    buffer->length = got;
                }
                else got = 0;
            }
            IBuffer_Release( filled );
        }
        if (!got) break;   /* end of stream */

        buffer->capacity = COPY_CHUNK;
        buffer->length = got;
        if (FAILED(hr = IOutputStream_WriteAsync( params->dest, &buffer->IBuffer_iface, &write ))) break;
        if (SUCCEEDED(hr = wait_operation( (IInspectable *)write, TRUE )))
            hr = IAsyncOperationWithProgress_UINT32_UINT32_GetResults( write, &written );
        IAsyncOperationWithProgress_UINT32_UINT32_Release( write );
        if (FAILED(hr)) break;
        total += got;
    }

    if (SUCCEEDED(hr))
    {
        IAsyncOperation_boolean *flush;
        if (SUCCEEDED(IOutputStream_FlushAsync( params->dest, &flush )))
        {
            wait_operation( (IInspectable *)flush, FALSE );
            IAsyncOperation_boolean_Release( flush );
        }
    }
    if (params->close)
    {
        IClosable *closable;
        if (SUCCEEDED(IInputStream_QueryInterface( params->source, &IID_IClosable, (void **)&closable )))
        {
            IClosable_Close( closable );
            IClosable_Release( closable );
        }
        if (SUCCEEDED(IOutputStream_QueryInterface( params->dest, &IID_IClosable, (void **)&closable )))
        {
            IClosable_Close( closable );
            IClosable_Release( closable );
        }
    }
    IBuffer_Release( &buffer->IBuffer_iface );

    TRACE( "copied %I64u bytes, hr %#lx.\n", total, hr );
    if (FAILED(hr)) return hr;
    result->vt = VT_UI8;
    result->uhVal.QuadPart = total;
    return S_OK;
}

/* ---- the statics ---- */

struct stream_statics
{
    IActivationFactory IActivationFactory_iface;
    IRandomAccessStreamStatics IRandomAccessStreamStatics_iface;
};

static inline struct stream_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct stream_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct stream_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IRandomAccessStreamStatics ))
        *out = &impl->IRandomAccessStreamStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return E_NOTIMPL; }

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { return E_NOTIMPL; }

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* a static class */
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( stream_statics, IRandomAccessStreamStatics, struct stream_statics, IActivationFactory_iface )

static HRESULT start_copy( IRandomAccessStreamStatics *iface, IInputStream *source, IOutputStream *dest, UINT64 limit,
        BOOL close, IAsyncOperationWithProgress_UINT64_UINT64 **operation )
{
    struct copy_params *params;
    HRESULT hr;

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!source || !dest) return E_POINTER;
    if (!(params = calloc( 1, sizeof(*params) ))) return E_OUTOFMEMORY;
    params->IUnknown_iface.lpVtbl = &copy_params_vtbl;
    params->ref = 1;
    IInputStream_AddRef( (params->source = source) );
    IOutputStream_AddRef( (params->dest = dest) );
    params->limit = limit;
    params->close = close;

    hr = async_operation_uint64_uint64_create( (IUnknown *)iface, &params->IUnknown_iface, stream_copy_async, operation );
    IUnknown_Release( &params->IUnknown_iface );
    return hr;
}

static HRESULT WINAPI stream_statics_CopyAsync( IRandomAccessStreamStatics *iface, IInputStream *source,
        IOutputStream *dest, IAsyncOperationWithProgress_UINT64_UINT64 **operation )
{
    TRACE( "iface %p, source %p, dest %p, operation %p.\n", iface, source, dest, operation );
    return start_copy( iface, source, dest, ~(UINT64)0, FALSE, operation );
}

static HRESULT WINAPI stream_statics_CopySizeAsync( IRandomAccessStreamStatics *iface, IInputStream *source,
        IOutputStream *dest, UINT64 bytes, IAsyncOperationWithProgress_UINT64_UINT64 **operation )
{
    TRACE( "iface %p, source %p, dest %p, bytes %I64u, operation %p.\n", iface, source, dest, bytes, operation );
    return start_copy( iface, source, dest, bytes, FALSE, operation );
}

static HRESULT WINAPI stream_statics_CopyAndCloseAsync( IRandomAccessStreamStatics *iface, IInputStream *source,
        IOutputStream *dest, IAsyncOperationWithProgress_UINT64_UINT64 **operation )
{
    TRACE( "iface %p, source %p, dest %p, operation %p.\n", iface, source, dest, operation );
    return start_copy( iface, source, dest, ~(UINT64)0, TRUE, operation );
}

static const struct IRandomAccessStreamStaticsVtbl stream_statics_vtbl =
{
    stream_statics_QueryInterface,
    stream_statics_AddRef,
    stream_statics_Release,
    stream_statics_GetIids,
    stream_statics_GetRuntimeClassName,
    stream_statics_GetTrustLevel,
    stream_statics_CopyAsync,
    stream_statics_CopySizeAsync,
    stream_statics_CopyAndCloseAsync,
};

static struct stream_statics stream_statics =
{
    {&factory_vtbl},
    {&stream_statics_vtbl},
};

IActivationFactory *random_access_stream_statics_factory = &stream_statics.IActivationFactory_iface;

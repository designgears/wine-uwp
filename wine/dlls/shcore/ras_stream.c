/*
 * CreateStreamOverRandomAccessStream: an IStream over a WinRT IRandomAccessStream
 *
 * Copyright 2026 Bobby Kircher
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

/* The genuine Windows.Graphics hands WIC an IStream made from the app's IRandomAccessStream
 * (BitmapDecoder.CreateAsync(stream)); that is this function. Reads and writes go through the
 * WinRT stream's ReadAsync/WriteAsync and are waited for here, since IStream is synchronous. A
 * read lands directly in the caller's memory through a small IBuffer over it. */

#include <stdarg.h>
#include <stdlib.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "roapi.h"
#include "winstring.h"
#include "inspectable.h"
#include "activation.h"
#define WIDL_using_Windows_Storage_Streams
#include "robuffer.h"
/* main.c defines the IIDs of the headers above; this file defines those of the two below */
#include "initguid.h"
#define WIDL_using_Windows_Foundation
#include "windows.foundation.h"
#include "windows.storage.streams.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shcore);

/* ---------------------------------------------------------------------------------------------
 * An IBuffer over caller-owned memory
 * ------------------------------------------------------------------------------------------- */
struct user_buffer
{
    IBuffer IBuffer_iface;
    IBufferByteAccess IBufferByteAccess_iface;
    LONG ref;
    BYTE *data;
    UINT32 capacity, length;
};

static inline struct user_buffer *impl_from_IBuffer( IBuffer *iface )
{
    return CONTAINING_RECORD( iface, struct user_buffer, IBuffer_iface );
}

static HRESULT WINAPI user_buffer_QueryInterface( IBuffer *iface, REFIID iid, void **out )
{
    struct user_buffer *impl = impl_from_IBuffer( iface );

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

static ULONG WINAPI user_buffer_AddRef( IBuffer *iface )
{
    return InterlockedIncrement( &impl_from_IBuffer( iface )->ref );
}

static ULONG WINAPI user_buffer_Release( IBuffer *iface )
{
    struct user_buffer *impl = impl_from_IBuffer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI user_buffer_GetIids( IBuffer *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI user_buffer_GetRuntimeClassName( IBuffer *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.Streams.Buffer", 30, name );
}

static HRESULT WINAPI user_buffer_GetTrustLevel( IBuffer *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI user_buffer_get_Capacity( IBuffer *iface, UINT32 *value )
{
    *value = impl_from_IBuffer( iface )->capacity;
    return S_OK;
}

static HRESULT WINAPI user_buffer_get_Length( IBuffer *iface, UINT32 *value )
{
    *value = impl_from_IBuffer( iface )->length;
    return S_OK;
}

static HRESULT WINAPI user_buffer_put_Length( IBuffer *iface, UINT32 value )
{
    struct user_buffer *impl = impl_from_IBuffer( iface );
    if (value > impl->capacity) return E_INVALIDARG;
    impl->length = value;
    return S_OK;
}

static const IBufferVtbl user_buffer_vtbl =
{
    user_buffer_QueryInterface,
    user_buffer_AddRef,
    user_buffer_Release,
    user_buffer_GetIids,
    user_buffer_GetRuntimeClassName,
    user_buffer_GetTrustLevel,
    user_buffer_get_Capacity,
    user_buffer_get_Length,
    user_buffer_put_Length,
};

static inline struct user_buffer *impl_from_IBufferByteAccess( IBufferByteAccess *iface )
{
    return CONTAINING_RECORD( iface, struct user_buffer, IBufferByteAccess_iface );
}

static HRESULT WINAPI user_buffer_access_QueryInterface( IBufferByteAccess *iface, REFIID iid, void **out )
{
    return user_buffer_QueryInterface( &impl_from_IBufferByteAccess( iface )->IBuffer_iface, iid, out );
}

static ULONG WINAPI user_buffer_access_AddRef( IBufferByteAccess *iface )
{
    return user_buffer_AddRef( &impl_from_IBufferByteAccess( iface )->IBuffer_iface );
}

static ULONG WINAPI user_buffer_access_Release( IBufferByteAccess *iface )
{
    return user_buffer_Release( &impl_from_IBufferByteAccess( iface )->IBuffer_iface );
}

static HRESULT WINAPI user_buffer_access_Buffer( IBufferByteAccess *iface, BYTE **value )
{
    *value = impl_from_IBufferByteAccess( iface )->data;
    return S_OK;
}

static const IBufferByteAccessVtbl user_buffer_access_vtbl =
{
    user_buffer_access_QueryInterface,
    user_buffer_access_AddRef,
    user_buffer_access_Release,
    user_buffer_access_Buffer,
};

static struct user_buffer *user_buffer_create( void *data, UINT32 capacity, UINT32 length )
{
    struct user_buffer *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IBuffer_iface.lpVtbl = &user_buffer_vtbl;
    impl->IBufferByteAccess_iface.lpVtbl = &user_buffer_access_vtbl;
    impl->ref = 1;
    impl->data = data;
    impl->capacity = capacity;
    impl->length = length;
    return impl;
}

/* Waits for any WinRT async operation to leave the Started state. */
static HRESULT wait_async( IUnknown *operation )
{
    AsyncStatus status = Started;
    IAsyncInfo *info;
    HRESULT hr, error = S_OK;

    if (FAILED(hr = IUnknown_QueryInterface( operation, &IID_IAsyncInfo, (void **)&info ))) return hr;
    while (SUCCEEDED(hr = IAsyncInfo_get_Status( info, &status )) && status == Started) Sleep( 1 );
    if (SUCCEEDED(hr) && status != Completed)
    {
        IAsyncInfo_get_ErrorCode( info, &error );
        hr = status == Canceled ? HRESULT_FROM_WIN32( ERROR_CANCELLED ) : (FAILED(error) ? error : E_FAIL);
    }
    IAsyncInfo_Release( info );
    return hr;
}

/* ---------------------------------------------------------------------------------------------
 * The IStream
 * ------------------------------------------------------------------------------------------- */
struct ras_stream
{
    IStream IStream_iface;
    LONG ref;
    IRandomAccessStream *stream;
};

static inline struct ras_stream *impl_from_IStream( IStream *iface )
{
    return CONTAINING_RECORD( iface, struct ras_stream, IStream_iface );
}

static HRESULT ras_stream_create( IRandomAccessStream *stream, IStream **out );

static HRESULT WINAPI ras_stream_QueryInterface( IStream *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_ISequentialStream ) ||
        IsEqualGUID( iid, &IID_IStream ))
    {
        IStream_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ras_stream_AddRef( IStream *iface )
{
    return InterlockedIncrement( &impl_from_IStream( iface )->ref );
}

static ULONG WINAPI ras_stream_Release( IStream *iface )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IRandomAccessStream_Release( impl->stream );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ras_stream_Read( IStream *iface, void *data, ULONG size, ULONG *read )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    IAsyncOperationWithProgress_IBuffer_UINT32 *operation;
    struct user_buffer *buffer;
    IBuffer *result = NULL;
    IInputStream *input;
    UINT32 length = 0;
    HRESULT hr;

    TRACE( "iface %p, data %p, size %lu, read %p.\n", iface, data, size, read );

    if (read) *read = 0;
    if (!size) return S_OK;
    if (FAILED(hr = IRandomAccessStream_QueryInterface( impl->stream, &IID_IInputStream, (void **)&input )))
        return STG_E_ACCESSDENIED;
    if (!(buffer = user_buffer_create( data, size, 0 )))
    {
        IInputStream_Release( input );
        return E_OUTOFMEMORY;
    }
    hr = IInputStream_ReadAsync( input, &buffer->IBuffer_iface, size, InputStreamOptions_None, &operation );
    IInputStream_Release( input );
    if (SUCCEEDED(hr))
    {
        if (SUCCEEDED(hr = wait_async( (IUnknown *)operation )) &&
            SUCCEEDED(hr = IAsyncOperationWithProgress_IBuffer_UINT32_GetResults( operation, &result )) && result)
        {
            IBuffer_get_Length( result, &length );
            if (length > size) length = size;
            /* a stream may hand back a buffer of its own rather than fill the one given */
            if (result != &buffer->IBuffer_iface)
            {
                IBufferByteAccess *access;
                BYTE *bytes;
                if (SUCCEEDED(IBuffer_QueryInterface( result, &IID_IBufferByteAccess, (void **)&access )))
                {
                    if (SUCCEEDED(IBufferByteAccess_Buffer( access, &bytes ))) memcpy( data, bytes, length );
                    else length = 0;
                    IBufferByteAccess_Release( access );
                }
                else length = 0;
            }
            IBuffer_Release( result );
        }
        IAsyncOperationWithProgress_IBuffer_UINT32_Release( operation );
    }
    IBuffer_Release( &buffer->IBuffer_iface );
    if (FAILED(hr)) return hr;
    if (read) *read = length;
    return length < size ? S_FALSE : S_OK;
}

static HRESULT WINAPI ras_stream_Write( IStream *iface, const void *data, ULONG size, ULONG *written )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    IAsyncOperationWithProgress_UINT32_UINT32 *operation;
    struct user_buffer *buffer;
    IOutputStream *output;
    UINT32 count = 0;
    HRESULT hr;

    TRACE( "iface %p, data %p, size %lu, written %p.\n", iface, data, size, written );

    if (written) *written = 0;
    if (!size) return S_OK;
    if (FAILED(hr = IRandomAccessStream_QueryInterface( impl->stream, &IID_IOutputStream, (void **)&output )))
        return STG_E_ACCESSDENIED;
    if (!(buffer = user_buffer_create( (void *)data, size, size )))
    {
        IOutputStream_Release( output );
        return E_OUTOFMEMORY;
    }
    hr = IOutputStream_WriteAsync( output, &buffer->IBuffer_iface, &operation );
    IOutputStream_Release( output );
    if (SUCCEEDED(hr))
    {
        if (SUCCEEDED(hr = wait_async( (IUnknown *)operation )))
            hr = IAsyncOperationWithProgress_UINT32_UINT32_GetResults( operation, &count );
        IAsyncOperationWithProgress_UINT32_UINT32_Release( operation );
    }
    IBuffer_Release( &buffer->IBuffer_iface );
    if (FAILED(hr)) return hr;
    if (written) *written = count;
    return S_OK;
}

static HRESULT WINAPI ras_stream_Seek( IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    UINT64 base = 0, target;
    HRESULT hr;

    TRACE( "iface %p, move %I64d, origin %lu, position %p.\n", iface, move.QuadPart, origin, position );

    switch (origin)
    {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: if (FAILED(hr = IRandomAccessStream_get_Position( impl->stream, &base ))) return hr; break;
    case STREAM_SEEK_END: if (FAILED(hr = IRandomAccessStream_get_Size( impl->stream, &base ))) return hr; break;
    default: return STG_E_INVALIDFUNCTION;
    }
    if (move.QuadPart < 0 && (UINT64)-move.QuadPart > base) return STG_E_INVALIDFUNCTION;
    target = base + move.QuadPart;
    if (FAILED(hr = IRandomAccessStream_Seek( impl->stream, target ))) return hr;
    if (position) position->QuadPart = target;
    return S_OK;
}

static HRESULT WINAPI ras_stream_SetSize( IStream *iface, ULARGE_INTEGER size )
{
    TRACE( "iface %p, size %I64u.\n", iface, size.QuadPart );
    return IRandomAccessStream_put_Size( impl_from_IStream( iface )->stream, size.QuadPart );
}

static HRESULT WINAPI ras_stream_CopyTo( IStream *iface, IStream *dest, ULARGE_INTEGER size,
                                         ULARGE_INTEGER *read, ULARGE_INTEGER *written )
{
    BYTE chunk[16384];
    ULONGLONG total_read = 0, total_written = 0;
    HRESULT hr = S_OK;

    TRACE( "iface %p, dest %p, size %I64u.\n", iface, dest, size.QuadPart );

    while (total_read < size.QuadPart)
    {
        ULONG want = min( sizeof(chunk), size.QuadPart - total_read ), got = 0, put = 0;
        if (FAILED(hr = IStream_Read( iface, chunk, want, &got )) || !got) break;
        total_read += got;
        if (FAILED(hr = IStream_Write( dest, chunk, got, &put ))) break;
        total_written += put;
        if (got < want) break;
    }
    if (read) read->QuadPart = total_read;
    if (written) written->QuadPart = total_written;
    return FAILED(hr) ? hr : S_OK;
}

static HRESULT WINAPI ras_stream_Commit( IStream *iface, DWORD flags )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    IAsyncOperation_boolean *operation;
    IOutputStream *output;
    HRESULT hr;

    TRACE( "iface %p, flags %#lx.\n", iface, flags );

    if (FAILED(IRandomAccessStream_QueryInterface( impl->stream, &IID_IOutputStream, (void **)&output )))
        return S_OK;
    if (SUCCEEDED(hr = IOutputStream_FlushAsync( output, &operation )))
    {
        hr = wait_async( (IUnknown *)operation );
        IAsyncOperation_boolean_Release( operation );
    }
    IOutputStream_Release( output );
    return hr;
}

static HRESULT WINAPI ras_stream_Revert( IStream *iface )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI ras_stream_LockRegion( IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type )
{
    return STG_E_INVALIDFUNCTION;
}

static HRESULT WINAPI ras_stream_UnlockRegion( IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type )
{
    return STG_E_INVALIDFUNCTION;
}

static HRESULT WINAPI ras_stream_Stat( IStream *iface, STATSTG *stat, DWORD flags )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    boolean can_read = FALSE, can_write = FALSE;
    UINT64 size = 0;
    HRESULT hr;

    TRACE( "iface %p, stat %p, flags %#lx.\n", iface, stat, flags );

    if (!stat) return STG_E_INVALIDPOINTER;
    if (FAILED(hr = IRandomAccessStream_get_Size( impl->stream, &size ))) return hr;
    IRandomAccessStream_get_CanRead( impl->stream, &can_read );
    IRandomAccessStream_get_CanWrite( impl->stream, &can_write );
    memset( stat, 0, sizeof(*stat) );
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size;
    stat->grfMode = can_write ? (can_read ? STGM_READWRITE : STGM_WRITE) : STGM_READ;
    return S_OK;
}

static HRESULT WINAPI ras_stream_Clone( IStream *iface, IStream **out )
{
    struct ras_stream *impl = impl_from_IStream( iface );
    IRandomAccessStream *clone;
    UINT64 position = 0;
    HRESULT hr;

    TRACE( "iface %p, out %p.\n", iface, out );

    if (!out) return STG_E_INVALIDPOINTER;
    *out = NULL;
    if (FAILED(hr = IRandomAccessStream_CloneStream( impl->stream, &clone ))) return hr;
    /* a clone of an IStream shares the seek position it had */
    if (SUCCEEDED(IRandomAccessStream_get_Position( impl->stream, &position )))
        IRandomAccessStream_Seek( clone, position );
    hr = ras_stream_create( clone, out );
    IRandomAccessStream_Release( clone );
    return hr;
}

static const IStreamVtbl ras_stream_vtbl =
{
    ras_stream_QueryInterface,
    ras_stream_AddRef,
    ras_stream_Release,
    ras_stream_Read,
    ras_stream_Write,
    ras_stream_Seek,
    ras_stream_SetSize,
    ras_stream_CopyTo,
    ras_stream_Commit,
    ras_stream_Revert,
    ras_stream_LockRegion,
    ras_stream_UnlockRegion,
    ras_stream_Stat,
    ras_stream_Clone,
};

static HRESULT ras_stream_create( IRandomAccessStream *stream, IStream **out )
{
    struct ras_stream *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStream_iface.lpVtbl = &ras_stream_vtbl;
    impl->ref = 1;
    IRandomAccessStream_AddRef( (impl->stream = stream) );
    *out = &impl->IStream_iface;
    return S_OK;
}

/*************************************************************************
 * CreateStreamOverRandomAccessStream        [SHCORE.@]
 */
HRESULT WINAPI CreateStreamOverRandomAccessStream( IUnknown *random_access_stream, REFIID riid, void **ppv )
{
    IRandomAccessStream *stream;
    IStream *adapter;
    HRESULT hr;

    TRACE( "%p, %s, %p\n", random_access_stream, debugstr_guid( riid ), ppv );

    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!random_access_stream) return E_INVALIDARG;
    if (FAILED(hr = IUnknown_QueryInterface( random_access_stream, &IID_IRandomAccessStream, (void **)&stream )))
        return hr;
    hr = ras_stream_create( stream, &adapter );
    IRandomAccessStream_Release( stream );
    if (FAILED(hr)) return hr;
    hr = IStream_QueryInterface( adapter, riid, ppv );
    IStream_Release( adapter );
    return hr;
}

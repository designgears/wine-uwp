/* WinRT Windows.Storage.Streams.FileRandomAccessStream Implementation
 *
 * Copyright (C) 2026 Bobby Kircher
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
#include "shlwapi.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/* One object answers for every stream a StorageFile hands out: FileRandomAccessStream from
 * OpenAsync/OpenReadAsync, and FileInputStream from OpenSequentialReadAsync. Reads and writes are
 * positional (OVERLAPPED offsets), so the handle's own file pointer is never relied on and a
 * stream from GetInputStreamAt or CloneStream only needs a duplicated handle and its own
 * position. */
struct file_stream
{
    IRandomAccessStreamWithContentType IRandomAccessStreamWithContentType_iface;
    IRandomAccessStream IRandomAccessStream_iface;
    IInputStream IInputStream_iface;
    IOutputStream IOutputStream_iface;
    IClosable IClosable_iface;
    IContentTypeProvider IContentTypeProvider_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    HANDLE file;
    UINT64 pos;
    BOOL can_write;
    BOOL sequential;
    WCHAR *content_type;
};

static HRESULT file_stream_create( HANDLE file, UINT64 pos, BOOL can_write, BOOL sequential,
                                   const WCHAR *content_type, struct file_stream **out );

const WCHAR *file_content_type( const WCHAR *path, WCHAR *buffer, DWORD size )
{
    const WCHAR *ext = PathFindExtensionW( path );

    /* the per-extension Content Type the shell keeps under HKCR, "application/octet-stream" when
     * there is nothing registered - which is what Windows falls back to as well */
    size *= sizeof(WCHAR);
    if (!*ext || RegGetValueW( HKEY_CLASSES_ROOT, ext, L"Content Type", RRF_RT_REG_SZ, NULL, buffer, &size ))
        wcscpy( buffer, L"application/octet-stream" );
    return buffer;
}

static inline struct file_stream *impl_from_IRandomAccessStreamWithContentType( IRandomAccessStreamWithContentType *iface )
{
    return CONTAINING_RECORD( iface, struct file_stream, IRandomAccessStreamWithContentType_iface );
}

static HRESULT WINAPI file_stream_QueryInterface( IRandomAccessStreamWithContentType *iface, REFIID iid, void **out )
{
    struct file_stream *impl = impl_from_IRandomAccessStreamWithContentType( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ))
        *out = impl->sequential ? (void *)&impl->IInputStream_iface : (void *)&impl->IRandomAccessStreamWithContentType_iface;
    else if (IsEqualGUID( iid, &IID_IInputStream ))
        *out = &impl->IInputStream_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    /* a FileInputStream is only an input stream; do not let a sequential reader be mistaken for
     * a seekable one */
    else if (impl->sequential)
        *out = NULL;
    else if (IsEqualGUID( iid, &IID_IRandomAccessStreamWithContentType ))
        *out = &impl->IRandomAccessStreamWithContentType_iface;
    else if (IsEqualGUID( iid, &IID_IRandomAccessStream ))
        *out = &impl->IRandomAccessStream_iface;
    else if (IsEqualGUID( iid, &IID_IOutputStream ))
        *out = &impl->IOutputStream_iface;
    else if (IsEqualGUID( iid, &IID_IContentTypeProvider ))
        *out = &impl->IContentTypeProvider_iface;
    else
        *out = NULL;

    if (!*out)
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI file_stream_AddRef( IRandomAccessStreamWithContentType *iface )
{
    struct file_stream *impl = impl_from_IRandomAccessStreamWithContentType( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI file_stream_Release( IRandomAccessStreamWithContentType *iface )
{
    struct file_stream *impl = impl_from_IRandomAccessStreamWithContentType( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->file) CloseHandle( impl->file );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl->content_type );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_stream_GetIids( IRandomAccessStreamWithContentType *iface, ULONG *iid_count, IID **iids )
{
    struct file_stream *impl = impl_from_IRandomAccessStreamWithContentType( iface );
    ULONG count = 0;

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 6 * sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[count++] = IID_IInputStream;
    (*iids)[count++] = IID_IClosable;
    if (!impl->sequential)
    {
        (*iids)[count++] = IID_IRandomAccessStream;
        (*iids)[count++] = IID_IOutputStream;
        (*iids)[count++] = IID_IRandomAccessStreamWithContentType;
        (*iids)[count++] = IID_IContentTypeProvider;
    }
    *iid_count = count;
    return S_OK;
}

/* .NET types the RCW from this string, so it has to name a real class */
static HRESULT WINAPI file_stream_GetRuntimeClassName( IRandomAccessStreamWithContentType *iface, HSTRING *class_name )
{
    struct file_stream *impl = impl_from_IRandomAccessStreamWithContentType( iface );
    const WCHAR *name = impl->sequential ? L"Windows.Storage.Streams.FileInputStream"
                                         : L"Windows.Storage.Streams.FileRandomAccessStream";

    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI file_stream_GetTrustLevel( IRandomAccessStreamWithContentType *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static const struct IRandomAccessStreamWithContentTypeVtbl file_stream_vtbl =
{
    file_stream_QueryInterface,
    file_stream_AddRef,
    file_stream_Release,
    /* IInspectable methods */
    file_stream_GetIids,
    file_stream_GetRuntimeClassName,
    file_stream_GetTrustLevel,
};

DEFINE_IINSPECTABLE( file_stream_ra, IRandomAccessStream, struct file_stream, IRandomAccessStreamWithContentType_iface )

static HRESULT WINAPI file_stream_ra_get_Size( IRandomAccessStream *iface, UINT64 *value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    LARGE_INTEGER size;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = 0;
    if (!impl->file) return RO_E_CLOSED;
    if (!GetFileSizeEx( impl->file, &size )) return HRESULT_FROM_WIN32( GetLastError() );
    *value = size.QuadPart;
    return S_OK;
}

static HRESULT WINAPI file_stream_ra_put_Size( IRandomAccessStream *iface, UINT64 value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    LARGE_INTEGER pos = {.QuadPart = value};
    HRESULT hr = S_OK;

    TRACE( "iface %p, value %I64u.\n", iface, value );

    if (!impl->file) return RO_E_CLOSED;
    if (!impl->can_write) return E_ACCESSDENIED;
    EnterCriticalSection( &impl->cs );
    if (!SetFilePointerEx( impl->file, pos, NULL, FILE_BEGIN ) || !SetEndOfFile( impl->file ))
        hr = HRESULT_FROM_WIN32( GetLastError() );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT file_stream_duplicate( struct file_stream *impl, UINT64 pos, BOOL sequential, struct file_stream **out )
{
    HANDLE file;
    HRESULT hr;

    *out = NULL;
    if (!impl->file) return RO_E_CLOSED;
    if (!DuplicateHandle( GetCurrentProcess(), impl->file, GetCurrentProcess(), &file, 0, FALSE,
                          DUPLICATE_SAME_ACCESS ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (FAILED(hr = file_stream_create( file, pos, impl->can_write, sequential, impl->content_type, out )))
        CloseHandle( file );
    return hr;
}

static HRESULT WINAPI file_stream_ra_GetInputStreamAt( IRandomAccessStream *iface, UINT64 position, IInputStream **stream )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface ), *dup;
    HRESULT hr;

    TRACE( "iface %p, position %I64u, stream %p.\n", iface, position, stream );

    if (!stream) return E_POINTER;
    *stream = NULL;
    if (FAILED(hr = file_stream_duplicate( impl, position, TRUE, &dup ))) return hr;
    *stream = &dup->IInputStream_iface;
    return S_OK;
}

static HRESULT WINAPI file_stream_ra_GetOutputStreamAt( IRandomAccessStream *iface, UINT64 position, IOutputStream **stream )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface ), *dup;
    HRESULT hr;

    TRACE( "iface %p, position %I64u, stream %p.\n", iface, position, stream );

    if (!stream) return E_POINTER;
    *stream = NULL;
    if (!impl->can_write) return E_ACCESSDENIED;
    if (FAILED(hr = file_stream_duplicate( impl, position, FALSE, &dup ))) return hr;
    *stream = &dup->IOutputStream_iface;
    return S_OK;
}

static HRESULT WINAPI file_stream_ra_get_Position( IRandomAccessStream *iface, UINT64 *value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = impl->pos;
    LeaveCriticalSection( &impl->cs );
    return impl->file ? S_OK : RO_E_CLOSED;
}

static HRESULT WINAPI file_stream_ra_Seek( IRandomAccessStream *iface, UINT64 position )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );

    TRACE( "iface %p, position %I64u.\n", iface, position );

    if (!impl->file) return RO_E_CLOSED;
    EnterCriticalSection( &impl->cs );
    impl->pos = position;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI file_stream_ra_CloneStream( IRandomAccessStream *iface, IRandomAccessStream **stream )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface ), *dup;
    HRESULT hr;

    TRACE( "iface %p, stream %p.\n", iface, stream );

    if (!stream) return E_POINTER;
    *stream = NULL;
    /* a clone starts at position 0, like Windows' */
    if (FAILED(hr = file_stream_duplicate( impl, 0, FALSE, &dup ))) return hr;
    *stream = &dup->IRandomAccessStream_iface;
    return S_OK;
}

static HRESULT WINAPI file_stream_ra_get_CanRead( IRandomAccessStream *iface, BOOLEAN *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI file_stream_ra_get_CanWrite( IRandomAccessStream *iface, BOOLEAN *value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->can_write;
    return S_OK;
}

static const struct IRandomAccessStreamVtbl file_stream_ra_vtbl =
{
    file_stream_ra_QueryInterface,
    file_stream_ra_AddRef,
    file_stream_ra_Release,
    /* IInspectable methods */
    file_stream_ra_GetIids,
    file_stream_ra_GetRuntimeClassName,
    file_stream_ra_GetTrustLevel,
    /* IRandomAccessStream methods */
    file_stream_ra_get_Size,
    file_stream_ra_put_Size,
    file_stream_ra_GetInputStreamAt,
    file_stream_ra_GetOutputStreamAt,
    file_stream_ra_get_Position,
    file_stream_ra_Seek,
    file_stream_ra_CloneStream,
    file_stream_ra_get_CanRead,
    file_stream_ra_get_CanWrite,
};

struct file_read_params
{
    IUnknown IUnknown_iface;
    LONG ref;

    IBuffer *buffer;
    UINT32 count;
};

static void file_read_params_destroy( struct file_read_params *params )
{
    IBuffer_Release( params->buffer );
    free( params );
}

DEFINE_ASYNC_PARAMS( file_read_params )

DEFINE_IINSPECTABLE( file_stream_input, IInputStream, struct file_stream, IRandomAccessStreamWithContentType_iface )

static BYTE *buffer_bytes( IBuffer *buffer )
{
    IBufferByteAccess *access;
    BYTE *data = NULL;

    if (FAILED(IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access ))) return NULL;
    IBufferByteAccess_Buffer( access, &data );
    IBufferByteAccess_Release( access );
    return data;
}

static HRESULT file_stream_read_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct file_stream *impl = impl_from_IInputStream( (IInputStream *)invoker );
    struct file_read_params *params = file_read_params_from_IUnknown( param );
    UINT32 capacity, done = 0;
    HRESULT hr = S_OK;
    BYTE *data;

    if (FAILED(hr = IBuffer_get_Capacity( params->buffer, &capacity ))) return hr;
    if (capacity < params->count) return E_INVALIDARG;
    if (!(data = buffer_bytes( params->buffer ))) return E_INVALIDARG;

    EnterCriticalSection( &impl->cs );
    if (!impl->file) hr = RO_E_CLOSED;
    /* a file read returns everything asked for unless it hits the end, whatever the options */
    while (SUCCEEDED(hr) && done < params->count)
    {
        OVERLAPPED ovl = {0};
        DWORD read;

        ovl.Offset = (DWORD)(impl->pos + done);
        ovl.OffsetHigh = (DWORD)((impl->pos + done) >> 32);
        if (!ReadFile( impl->file, data + done, params->count - done, &read, &ovl ))
        {
            if (GetLastError() != ERROR_HANDLE_EOF) hr = HRESULT_FROM_WIN32( GetLastError() );
            break;
        }
        if (!read) break;
        done += read;
    }
    if (SUCCEEDED(hr)) impl->pos += done;
    LeaveCriticalSection( &impl->cs );
    if (FAILED(hr)) return hr;

    IBuffer_put_Length( params->buffer, done );
    result->vt = VT_UNKNOWN;
    IBuffer_AddRef( params->buffer );
    result->punkVal = (IUnknown *)params->buffer;
    return S_OK;
}

static HRESULT WINAPI file_stream_input_ReadAsync( IInputStream *iface, IBuffer *buffer, UINT32 count,
        InputStreamOptions options, IAsyncOperationWithProgress_IBuffer_UINT32 **operation )
{
    struct file_stream *impl = impl_from_IInputStream( iface );
    struct file_read_params *params;
    HRESULT hr;

    TRACE( "iface %p, buffer %p, count %u, options %d, operation %p.\n", iface, buffer, count, options, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!buffer) return E_POINTER;
    if (!impl->file) return RO_E_CLOSED;
    if (!(params = file_read_params_alloc())) return E_OUTOFMEMORY;

    IBuffer_AddRef( buffer );
    params->buffer = buffer;
    params->count = count;
    hr = async_operation_buffer_uint32_create( (IUnknown *)iface, &params->IUnknown_iface, file_stream_read_async, operation );
    IUnknown_Release( &params->IUnknown_iface );
    return hr;
}

static const struct IInputStreamVtbl file_stream_input_vtbl =
{
    file_stream_input_QueryInterface,
    file_stream_input_AddRef,
    file_stream_input_Release,
    /* IInspectable methods */
    file_stream_input_GetIids,
    file_stream_input_GetRuntimeClassName,
    file_stream_input_GetTrustLevel,
    /* IInputStream methods */
    file_stream_input_ReadAsync,
};

DEFINE_IINSPECTABLE( file_stream_output, IOutputStream, struct file_stream, IRandomAccessStreamWithContentType_iface )

static HRESULT file_stream_write_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct file_stream *impl = impl_from_IOutputStream( (IOutputStream *)invoker );
    IBuffer *buffer = (IBuffer *)param;
    OVERLAPPED ovl = {0};
    HRESULT hr = S_OK;
    UINT32 length;
    DWORD written = 0;
    BYTE *data = NULL;

    IBuffer_get_Length( buffer, &length );
    if (length && !(data = buffer_bytes( buffer ))) return E_INVALIDARG;

    EnterCriticalSection( &impl->cs );
    if (!impl->file) hr = RO_E_CLOSED;
    else if (length)
    {
        ovl.Offset = (DWORD)impl->pos;
        ovl.OffsetHigh = (DWORD)(impl->pos >> 32);
        if (!WriteFile( impl->file, data, length, &written, &ovl )) hr = HRESULT_FROM_WIN32( GetLastError() );
        else impl->pos += written;
    }
    LeaveCriticalSection( &impl->cs );
    if (FAILED(hr)) return hr;

    result->vt = VT_UI4;
    result->ulVal = written;
    return S_OK;
}

static HRESULT WINAPI file_stream_output_WriteAsync( IOutputStream *iface, IBuffer *buffer,
        IAsyncOperationWithProgress_UINT32_UINT32 **operation )
{
    struct file_stream *impl = impl_from_IOutputStream( iface );

    TRACE( "iface %p, buffer %p, operation %p.\n", iface, buffer, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!buffer) return E_POINTER;
    if (!impl->file) return RO_E_CLOSED;
    if (!impl->can_write) return E_ACCESSDENIED;
    return async_operation_uint32_uint32_create( (IUnknown *)iface, (IUnknown *)buffer, file_stream_write_async, operation );
}

static HRESULT file_stream_flush_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct file_stream *impl = impl_from_IOutputStream( (IOutputStream *)invoker );
    BOOL ret;

    EnterCriticalSection( &impl->cs );
    ret = impl->file && FlushFileBuffers( impl->file );
    LeaveCriticalSection( &impl->cs );

    result->vt = VT_BOOL;
    result->boolVal = ret ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI file_stream_output_FlushAsync( IOutputStream *iface, IAsyncOperation_boolean **operation )
{
    struct file_stream *impl = impl_from_IOutputStream( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!impl->file) return RO_E_CLOSED;
    return async_operation_boolean_create( (IUnknown *)iface, NULL, file_stream_flush_async, operation );
}

static const struct IOutputStreamVtbl file_stream_output_vtbl =
{
    file_stream_output_QueryInterface,
    file_stream_output_AddRef,
    file_stream_output_Release,
    /* IInspectable methods */
    file_stream_output_GetIids,
    file_stream_output_GetRuntimeClassName,
    file_stream_output_GetTrustLevel,
    /* IOutputStream methods */
    file_stream_output_WriteAsync,
    file_stream_output_FlushAsync,
};

DEFINE_IINSPECTABLE( file_stream_closable, IClosable, struct file_stream, IRandomAccessStreamWithContentType_iface )

/* Close releases the file at once, so the app can reopen or delete it while stale references to
 * the stream are still alive; every later call answers RO_E_CLOSED. */
static HRESULT WINAPI file_stream_closable_Close( IClosable *iface )
{
    struct file_stream *impl = impl_from_IClosable( iface );
    HANDLE file;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    file = impl->file;
    impl->file = NULL;
    LeaveCriticalSection( &impl->cs );
    if (file) CloseHandle( file );
    return S_OK;
}

static const struct IClosableVtbl file_stream_closable_vtbl =
{
    file_stream_closable_QueryInterface,
    file_stream_closable_AddRef,
    file_stream_closable_Release,
    /* IInspectable methods */
    file_stream_closable_GetIids,
    file_stream_closable_GetRuntimeClassName,
    file_stream_closable_GetTrustLevel,
    /* IClosable methods */
    file_stream_closable_Close,
};

DEFINE_IINSPECTABLE( file_stream_ctp, IContentTypeProvider, struct file_stream, IRandomAccessStreamWithContentType_iface )

static HRESULT WINAPI file_stream_ctp_get_ContentType( IContentTypeProvider *iface, HSTRING *value )
{
    struct file_stream *impl = impl_from_IContentTypeProvider( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->content_type, wcslen( impl->content_type ), value );
}

static const struct IContentTypeProviderVtbl file_stream_ctp_vtbl =
{
    file_stream_ctp_QueryInterface,
    file_stream_ctp_AddRef,
    file_stream_ctp_Release,
    /* IInspectable methods */
    file_stream_ctp_GetIids,
    file_stream_ctp_GetRuntimeClassName,
    file_stream_ctp_GetTrustLevel,
    /* IContentTypeProvider methods */
    file_stream_ctp_get_ContentType,
};

static HRESULT file_stream_create( HANDLE file, UINT64 pos, BOOL can_write, BOOL sequential,
                                   const WCHAR *content_type, struct file_stream **out )
{
    struct file_stream *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (!(impl->content_type = wcsdup( content_type )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->IRandomAccessStreamWithContentType_iface.lpVtbl = &file_stream_vtbl;
    impl->IRandomAccessStream_iface.lpVtbl = &file_stream_ra_vtbl;
    impl->IInputStream_iface.lpVtbl = &file_stream_input_vtbl;
    impl->IOutputStream_iface.lpVtbl = &file_stream_output_vtbl;
    impl->IClosable_iface.lpVtbl = &file_stream_closable_vtbl;
    impl->IContentTypeProvider_iface.lpVtbl = &file_stream_ctp_vtbl;
    impl->ref = 1;
    impl->file = file;
    impl->pos = pos;
    impl->can_write = can_write;
    impl->sequential = sequential;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": file_stream.cs");
    *out = impl;
    return S_OK;
}

HRESULT file_stream_open( const WCHAR *path, FileAccessMode mode, BOOL sequential, IRandomAccessStreamWithContentType **out )
{
    BOOL can_write = mode == FileAccessMode_ReadWrite;
    struct file_stream *impl;
    WCHAR type[256];
    HANDLE file;
    HRESULT hr;

    TRACE( "path %s, mode %d, sequential %d.\n", debugstr_w( path ), mode, sequential );

    *out = NULL;
    /* Windows shares a read stream with other readers and writers, and a write stream with readers */
    file = CreateFileW( path, can_write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                        can_write ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        WARN( "failed to open %s, hr %#lx.\n", debugstr_w( path ), hr );
        return hr;
    }
    if (FAILED(hr = file_stream_create( file, 0, can_write, sequential, file_content_type( path, type, ARRAY_SIZE(type) ), &impl )))
    {
        CloseHandle( file );
        return hr;
    }
    *out = &impl->IRandomAccessStreamWithContentType_iface;
    return S_OK;
}

/* Windows.Storage.FileIO
 *
 * Copyright 2026 the Wine project authors.
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

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/* The text to write, alongside the target path, as one async parameter. */
struct write_param
{
    IUnknown IUnknown_iface;
    LONG ref;
    BOOL append;
    WCHAR *path;
    WCHAR *text;
};

static inline struct write_param *write_param_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct write_param, IUnknown_iface );
}

static HRESULT WINAPI write_param_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (!IsEqualGUID( iid, &IID_IUnknown )) { *out = NULL; return E_NOINTERFACE; }
    IUnknown_AddRef( iface );
    *out = iface;
    return S_OK;
}

static ULONG WINAPI write_param_AddRef( IUnknown *iface )
{
    struct write_param *impl = write_param_from_IUnknown( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI write_param_Release( IUnknown *iface )
{
    struct write_param *impl = write_param_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        free( impl->path );
        free( impl->text );
        free( impl );
    }
    return ref;
}

static const struct IUnknownVtbl write_param_vtbl =
{
    write_param_QueryInterface,
    write_param_AddRef,
    write_param_Release,
};

/* FileIO takes an IStorageFile, but everything below works on the path behind it. */
static HRESULT file_path( IStorageFile *file, WCHAR **out )
{
    IStorageItem *item;
    HSTRING path;
    HRESULT hr;

    *out = NULL;
    if (!file) return E_INVALIDARG;
    if (FAILED(hr = IStorageFile_QueryInterface( file, &IID_IStorageItem, (void **)&item ))) return hr;
    hr = IStorageItem_get_Path( item, &path );
    IStorageItem_Release( item );
    if (FAILED(hr)) return hr;

    if (!(*out = wcsdup( WindowsGetStringRawBuffer( path, NULL ) ))) hr = E_OUTOFMEMORY;
    WindowsDeleteString( path );
    return hr;
}

static HRESULT write_param_create( IStorageFile *file, HSTRING contents, BOOL append, IUnknown **out )
{
    struct write_param *impl;
    const WCHAR *text;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUnknown_iface.lpVtbl = &write_param_vtbl;
    impl->ref = 1;
    impl->append = append;

    if (FAILED(hr = file_path( file, &impl->path )))
    {
        free( impl );
        return hr;
    }
    text = contents ? WindowsGetStringRawBuffer( contents, NULL ) : L"";
    if (!(impl->text = wcsdup( text ? text : L"" )))
    {
        free( impl->path );
        free( impl );
        return E_OUTOFMEMORY;
    }

    *out = &impl->IUnknown_iface;
    return S_OK;
}

/* Windows writes UTF-8 without a BOM by default, and every consumer of these files here reads them
 * back with the same class, so the encoding argument is not honoured - only the default is. */
static HRESULT write_text( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct write_param *impl = write_param_from_IUnknown( param );
    DWORD written, size;
    char *utf8;
    HANDLE file;
    int len;

    file = CreateFileW( impl->path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        impl->append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if (impl->append) SetFilePointer( file, 0, NULL, FILE_END );

    len = WideCharToMultiByte( CP_UTF8, 0, impl->text, -1, NULL, 0, NULL, NULL );
    if (len <= 0 || !(utf8 = malloc( len )))
    {
        CloseHandle( file );
        return E_OUTOFMEMORY;
    }
    WideCharToMultiByte( CP_UTF8, 0, impl->text, -1, utf8, len, NULL, NULL );
    size = len - 1; /* without the terminator */

    if (!WriteFile( file, utf8, size, &written, NULL ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        free( utf8 );
        CloseHandle( file );
        return hr;
    }
    free( utf8 );
    CloseHandle( file );
    return S_OK;
}


/* Read the whole file as text. UTF-16 is detected by BOM, everything else is treated as UTF-8,
 * which is what FileIO writes above and what Windows defaults to. */
static HRESULT read_text( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct write_param *impl = write_param_from_IUnknown( param );
    DWORD size, read_size;
    WCHAR *text = NULL;
    char *raw;
    HANDLE file;

    file = CreateFileW( impl->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if ((size = GetFileSize( file, NULL )) == INVALID_FILE_SIZE)
    {
        CloseHandle( file );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    if (!(raw = malloc( size + 2 )))
    {
        CloseHandle( file );
        return E_OUTOFMEMORY;
    }
    if (!ReadFile( file, raw, size, &read_size, NULL ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        free( raw );
        CloseHandle( file );
        return hr;
    }
    CloseHandle( file );
    raw[read_size] = raw[read_size + 1] = 0;

    if (read_size >= 2 && (BYTE)raw[0] == 0xff && (BYTE)raw[1] == 0xfe)
    {
        if ((text = malloc( read_size ))) memcpy( text, raw + 2, read_size - 2 ), text[(read_size - 2) / 2] = 0;
    }
    else
    {
        const char *utf8 = raw;
        int len;
        if (read_size >= 3 && (BYTE)raw[0] == 0xef && (BYTE)raw[1] == 0xbb && (BYTE)raw[2] == 0xbf) utf8 = raw + 3;
        len = MultiByteToWideChar( CP_UTF8, 0, utf8, -1, NULL, 0 );
        if (len > 0 && (text = malloc( len * sizeof(WCHAR) )))
            MultiByteToWideChar( CP_UTF8, 0, utf8, -1, text, len );
    }
    free( raw );
    if (!text) return E_OUTOFMEMORY;

    result->vt = VT_LPWSTR;
    result->pwszVal = text;
    return S_OK;
}

struct file_io_statics
{
    IActivationFactory IActivationFactory_iface;
    IFileIOStatics IFileIOStatics_iface;
    LONG ref;
};

static inline struct file_io_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct file_io_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct file_io_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IFileIOStatics ))
    {
        *out = &impl->IFileIOStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_FileIO,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_FileIO), class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* FileIO is a static class: it has no constructor. */
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

DEFINE_IINSPECTABLE( file_io_statics, IFileIOStatics, struct file_io_statics, IActivationFactory_iface )

static HRESULT file_io_write( IFileIOStatics *iface, IStorageFile *file, HSTRING contents, BOOL append,
                              IAsyncAction **operation )
{
    IUnknown *param;
    HRESULT hr;

    if (!operation) return E_POINTER;
    if (FAILED(hr = write_param_create( file, contents, append, &param ))) return hr;
    hr = async_action_create( (IUnknown *)iface, param, write_text, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT file_io_read( IFileIOStatics *iface, IStorageFile *file, __FIAsyncOperation_1_HSTRING **operation )
{
    IUnknown *param;
    HRESULT hr;

    if (!operation) return E_POINTER;
    if (FAILED(hr = write_param_create( file, NULL, FALSE, &param ))) return hr;
    hr = async_operation_hstring_create( (IUnknown *)iface, param, read_text, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI file_io_statics_ReadTextAsync( IFileIOStatics *iface, IStorageFile *file,
                                                     __FIAsyncOperation_1_HSTRING **operation )
{
    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );
    return file_io_read( iface, file, operation );
}

static HRESULT WINAPI file_io_statics_ReadTextWithEncodingAsync( IFileIOStatics *iface, IStorageFile *file,
                                                                 UnicodeEncoding encoding,
                                                                 __FIAsyncOperation_1_HSTRING **operation )
{
    TRACE( "iface %p, file %p, encoding %d, operation %p.\n", iface, file, encoding, operation );
    return file_io_read( iface, file, operation );
}

static HRESULT WINAPI file_io_statics_WriteTextAsync( IFileIOStatics *iface, IStorageFile *file, HSTRING contents,
                                                      IAsyncAction **operation )
{
    TRACE( "iface %p, file %p, contents %s, operation %p.\n", iface, file, debugstr_hstring( contents ), operation );
    return file_io_write( iface, file, contents, FALSE, operation );
}

static HRESULT WINAPI file_io_statics_WriteTextWithEncodingAsync( IFileIOStatics *iface, IStorageFile *file,
                                                                  HSTRING contents, UnicodeEncoding encoding,
                                                                  IAsyncAction **operation )
{
    TRACE( "iface %p, file %p, contents %s, encoding %d, operation %p.\n", iface, file,
           debugstr_hstring( contents ), encoding, operation );
    return file_io_write( iface, file, contents, FALSE, operation );
}

static HRESULT WINAPI file_io_statics_AppendTextAsync( IFileIOStatics *iface, IStorageFile *file, HSTRING contents,
                                                       IAsyncAction **operation )
{
    TRACE( "iface %p, file %p, contents %s, operation %p.\n", iface, file, debugstr_hstring( contents ), operation );
    return file_io_write( iface, file, contents, TRUE, operation );
}

static HRESULT WINAPI file_io_statics_AppendTextWithEncodingAsync( IFileIOStatics *iface, IStorageFile *file,
                                                                   HSTRING contents, UnicodeEncoding encoding,
                                                                   IAsyncAction **operation )
{
    TRACE( "iface %p, file %p, contents %s, encoding %d, operation %p.\n", iface, file,
           debugstr_hstring( contents ), encoding, operation );
    return file_io_write( iface, file, contents, TRUE, operation );
}

static HRESULT WINAPI file_io_statics_ReadLinesAsync( IFileIOStatics *iface, IStorageFile *file, IInspectable **operation )
{
    FIXME( "iface %p, file %p, operation %p stub!\n", iface, file, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_io_statics_ReadLinesWithEncodingAsync( IFileIOStatics *iface, IStorageFile *file,
                                                                  UnicodeEncoding encoding, IInspectable **operation )
{
    FIXME( "iface %p, file %p, encoding %d, operation %p stub!\n", iface, file, encoding, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_io_statics_WriteLinesAsync( IFileIOStatics *iface, IStorageFile *file,
                                                       IIterable_HSTRING *lines, IAsyncAction **operation )
{
    FIXME( "iface %p, file %p, lines %p, operation %p stub!\n", iface, file, lines, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_io_statics_WriteLinesWithEncodingAsync( IFileIOStatics *iface, IStorageFile *file,
                                                                   IIterable_HSTRING *lines, UnicodeEncoding encoding,
                                                                   IAsyncAction **operation )
{
    FIXME( "iface %p, file %p, lines %p, encoding %d, operation %p stub!\n", iface, file, lines, encoding, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_io_statics_AppendLinesAsync( IFileIOStatics *iface, IStorageFile *file,
                                                        IIterable_HSTRING *lines, IAsyncAction **operation )
{
    FIXME( "iface %p, file %p, lines %p, operation %p stub!\n", iface, file, lines, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_io_statics_AppendLinesWithEncodingAsync( IFileIOStatics *iface, IStorageFile *file,
                                                                    IIterable_HSTRING *lines, UnicodeEncoding encoding,
                                                                    IAsyncAction **operation )
{
    FIXME( "iface %p, file %p, lines %p, encoding %d, operation %p stub!\n", iface, file, lines, encoding, operation );
    return E_NOTIMPL;
}

/* the whole file as a Buffer */
static HRESULT read_buffer( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct write_param *impl = write_param_from_IUnknown( param );
    IBufferByteAccess *access;
    LARGE_INTEGER size;
    IBuffer *buffer;
    DWORD read_size;
    HANDLE file;
    HRESULT hr;
    BYTE *data;

    if (!called_async) return STATUS_PENDING;

    file = CreateFileW( impl->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > 0x7fffffff)
    {
        CloseHandle( file );
        return E_FAIL;
    }
    if (FAILED(hr = buffer_create( size.LowPart, &buffer )))
    {
        CloseHandle( file );
        return hr;
    }
    IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access );
    IBufferByteAccess_Buffer( access, &data );
    IBufferByteAccess_Release( access );
    if (!ReadFile( file, data, size.LowPart, &read_size, NULL )) hr = HRESULT_FROM_WIN32( GetLastError() );
    CloseHandle( file );
    if (FAILED(hr))
    {
        IBuffer_Release( buffer );
        return hr;
    }
    IBuffer_put_Length( buffer, read_size );
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)buffer;
    return S_OK;
}

static HRESULT WINAPI file_io_statics_ReadBufferAsync( IFileIOStatics *iface, IStorageFile *file, IInspectable **operation )
{
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = write_param_create( file, NULL, FALSE, &param ))) return hr;
    hr = async_operation_ibuffer_create( (IUnknown *)iface, param, read_buffer,
                                         (IAsyncOperation_IBuffer **)operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI file_io_statics_WriteBufferAsync( IFileIOStatics *iface, IStorageFile *file, IBuffer *buffer,
                                                        IAsyncAction **operation )
{
    FIXME( "iface %p, file %p, buffer %p, operation %p stub!\n", iface, file, buffer, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_io_statics_WriteBytesAsync( IFileIOStatics *iface, IStorageFile *file, UINT32 count,
                                                       BYTE *buffer, IAsyncAction **operation )
{
    FIXME( "iface %p, file %p, count %u, buffer %p, operation %p stub!\n", iface, file, count, buffer, operation );
    return E_NOTIMPL;
}

static const struct IFileIOStaticsVtbl file_io_statics_vtbl =
{
    file_io_statics_QueryInterface,
    file_io_statics_AddRef,
    file_io_statics_Release,
    /* IInspectable methods */
    file_io_statics_GetIids,
    file_io_statics_GetRuntimeClassName,
    file_io_statics_GetTrustLevel,
    /* IFileIOStatics methods */
    file_io_statics_ReadTextAsync,
    file_io_statics_ReadTextWithEncodingAsync,
    file_io_statics_WriteTextAsync,
    file_io_statics_WriteTextWithEncodingAsync,
    file_io_statics_AppendTextAsync,
    file_io_statics_AppendTextWithEncodingAsync,
    file_io_statics_ReadLinesAsync,
    file_io_statics_ReadLinesWithEncodingAsync,
    file_io_statics_WriteLinesAsync,
    file_io_statics_WriteLinesWithEncodingAsync,
    file_io_statics_AppendLinesAsync,
    file_io_statics_AppendLinesWithEncodingAsync,
    file_io_statics_ReadBufferAsync,
    file_io_statics_WriteBufferAsync,
    file_io_statics_WriteBytesAsync,
};

static struct file_io_statics file_io_statics =
{
    {&factory_vtbl},
    {&file_io_statics_vtbl},
    1,
};

IActivationFactory *file_io_activation_factory = &file_io_statics.IActivationFactory_iface;

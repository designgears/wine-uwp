/* WinRT Windows.Storage Implementation
 *
 * Copyright (C) 2025 Mohamad Al-Jaf
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

#ifndef __WINE_WINDOWS_STORAGE_PRIVATE_H
#define __WINE_WINDOWS_STORAGE_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"

#include "wine/debug.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_System
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Storage
#define WIDL_using_Windows_Storage_Search
#define WIDL_using_Windows_Storage_Streams
#include "windows.storage.h"
#include "windows.storage.streams.h"
#include "robuffer.h"
#include "async_private.h"
#define WIDL_using_Wine_Internal
#include "queryoptions_private.h"

HRESULT async_operation_buffer_uint32_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperationWithProgress_IBuffer_UINT32 **out );
HRESULT async_operation_uint32_uint32_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperationWithProgress_UINT32_UINT32 **out );
HRESULT async_operation_uint64_uint64_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperationWithProgress_UINT64_UINT64 **out );
HRESULT storage_file_create( const WCHAR *path, IStorageFile **out );
HRESULT buffer_create( UINT32 capacity, IBuffer **out );
HRESULT async_operation_ibuffer_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                        IAsyncOperation_IBuffer **out );
HRESULT async_operation_input_stream_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IInputStream **out );
HRESULT async_operation_random_access_stream_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IRandomAccessStream **out );
HRESULT async_operation_random_access_stream_ct_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IRandomAccessStreamWithContentType **out );
HRESULT file_stream_open( const WCHAR *path, FileAccessMode mode, BOOL sequential, IRandomAccessStreamWithContentType **out );
const WCHAR *file_content_type( const WCHAR *path, WCHAR *buffer, DWORD size );
extern IActivationFactory *file_io_activation_factory;
HRESULT async_operation_indexed_state_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_UINT32 **out );
HRESULT async_operation_uint32_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_UINT32 **out );
HRESULT async_operation_files_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVectorView_StorageFile **out );
HRESULT async_operation_folders_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVectorView_StorageFolder **out );
HRESULT async_operation_items_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVectorView_IStorageItem **out );
HRESULT async_operation_hstring_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        __FIAsyncOperation_1_HSTRING **out );
HRESULT async_action_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncAction **out );
HRESULT async_operation_storage_folder_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_StorageFolder **out );
HRESULT async_operation_storage_file_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_StorageFile **out );
HRESULT async_operation_storage_item_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
                                             IAsyncOperation_IStorageItem **out );
HRESULT async_operation_boolean_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_boolean **out );

extern IActivationFactory *appdatapaths_factory;
HRESULT storage_path_param_create( const WCHAR *path, IUnknown **out );
const WCHAR *storage_path_param_buffer( IUnknown *param );
HRESULT WINAPI __wine_storage_folder_create( const WCHAR *path, IStorageFolder **out );

extern IActivationFactory *storage_file_factory;
extern IActivationFactory *storage_folder_factory;
extern IActivationFactory *known_folders_factory;
extern IActivationFactory *query_options_factory;
HRESULT empty_hstring_vector_create( IVector_HSTRING **out );
HRESULT content_properties_create( const WCHAR *path, IStorageItemContentPropertiesInternal **out );
HRESULT query_options_create( INT32 query, IIterable_HSTRING *file_type_filter, IQueryOptionsInternal **out );
extern IActivationFactory *userdatapaths_factory;
extern IActivationFactory *random_access_stream_reference_factory;
extern IActivationFactory *memory_stream_activation_factory;
extern IActivationFactory *random_access_stream_statics_factory;

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )
#define DEFINE_IINSPECTABLE_OUTER( pfx, iface_type, impl_type, outer_iface )                       \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, impl->outer_iface )

#define DEFINE_ASYNC_PARAMS( type )                                                                \
    static struct type *type##_from_IUnknown( IUnknown *iface )                                    \
    {                                                                                              \
        return CONTAINING_RECORD( iface, struct type, IUnknown_iface );                            \
    }                                                                                              \
    static HRESULT WINAPI type##_QueryInterface( IUnknown *iface, REFIID iid, void **out )         \
    {                                                                                              \
        if (IsEqualIID( iid, &IID_IUnknown ))                                                      \
        {                                                                                          \
            IUnknown_AddRef( iface );                                                              \
            *out = iface;                                                                          \
            return S_OK;                                                                           \
        }                                                                                          \
        *out = NULL;                                                                               \
        return E_NOINTERFACE;                                                                      \
    }                                                                                              \
    static ULONG WINAPI type##_AddRef( IUnknown *iface )                                           \
    {                                                                                              \
        struct type *object = type##_from_IUnknown( iface );                                       \
        return InterlockedIncrement( &object->ref );                                               \
    }                                                                                              \
    static ULONG WINAPI type##_Release( IUnknown *iface )                                          \
    {                                                                                              \
        struct type *object = type##_from_IUnknown( iface );                                       \
        ULONG ref = InterlockedDecrement( &object->ref );                                          \
        if (!ref) type##_##destroy( object );                                                      \
        return ref;                                                                                \
    }                                                                                              \
    static const IUnknownVtbl type##_vtbl =                                                        \
    {                                                                                              \
        type##_QueryInterface,                                                                     \
        type##_AddRef,                                                                             \
        type##_Release,                                                                            \
    };                                                                                             \
    static struct type *type##_alloc(void)                                                         \
    {                                                                                              \
        struct type *object;                                                                       \
        if (!(object = calloc( 1, sizeof(*object) ))) return NULL;                                 \
        object->IUnknown_iface.lpVtbl = &type##_vtbl;                                              \
        object->ref = 1;                                                                           \
        return object;                                                                             \
    }

#endif

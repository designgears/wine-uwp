/*
 * Windows.Storage.Search.QueryOptions
 *
 * Copyright 2026 the Wine project authors
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

/* Windows.Storage.Search.FolderDepth and IndexerOption; the public header has neither. */
#define FOLDER_DEPTH_SHALLOW 0
#define FOLDER_DEPTH_DEEP    1
#define INDEXER_OPTION_USE_INDEXER_WHEN_AVAILABLE 0
#define INDEXER_OPTION_ONLY_USE_INDEXER_AND_OPTIMIZE_PROPERTIES 1
#define INDEXER_OPTION_DO_NOT_USE_INDEXER 2

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/*
 * QueryOptions is a description of a search, not a search: the properties are a bag that
 * CreateFileQueryWithOptions later reads. The only one with any behaviour here is FileTypeFilter,
 * which has to be a live IVector<HSTRING> because callers build the extension list by appending to
 * the vector the object hands out.
 */

static HRESULT hstring_vector_create( IVector_HSTRING **out );

/* ---------------------------------------------------------------- IVector<HSTRING> */

struct hstring_vector
{
    IVector_HSTRING IVector_HSTRING_iface;
    IIterable_HSTRING IIterable_HSTRING_iface;
    LONG ref;

    HSTRING *elements;
    UINT32 count;
    UINT32 capacity;
};

static inline struct hstring_vector *impl_from_IVector_HSTRING( IVector_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IVector_HSTRING_iface );
}

static inline struct hstring_vector *impl_from_IIterable_HSTRING( IIterable_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IIterable_HSTRING_iface );
}

static HRESULT WINAPI hstring_vector_QueryInterface( IVector_HSTRING *iface, REFIID iid, void **out )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVector_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->IVector_HSTRING_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IIterable_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->IIterable_HSTRING_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_vector_AddRef( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI hstring_vector_Release( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        while (impl->count--) WindowsDeleteString( impl->elements[impl->count] );
        free( impl->elements );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI hstring_vector_GetIids( IVector_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_vector_GetRuntimeClassName( IVector_HSTRING *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IVector`1<String>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IVector`1<String>"),
                                class_name );
}

static HRESULT WINAPI hstring_vector_GetTrustLevel( IVector_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_vector_GetAt( IVector_HSTRING *iface, UINT32 index, HSTRING *value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    return WindowsDuplicateString( impl->elements[index], value );
}

static HRESULT WINAPI hstring_vector_get_Size( IVector_HSTRING *iface, UINT32 *value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_GetView( IVector_HSTRING *iface, IVectorView_HSTRING **value )
{
    /* The view would be a second object over the same elements; nothing asks for it yet, and an
     * empty one would silently lose the filter. */
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_vector_IndexOf( IVector_HSTRING *iface, HSTRING element, UINT32 *index,
                                              BOOLEAN *found )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    UINT32 i;
    INT32 order;

    TRACE( "iface %p, element %s, index %p, found %p.\n", iface, debugstr_hstring( element ), index, found );

    if (!index || !found) return E_POINTER;
    for (i = 0; i < impl->count; i++)
    {
        if (SUCCEEDED(WindowsCompareStringOrdinal( impl->elements[i], element, &order )) && !order)
        {
            *index = i;
            *found = TRUE;
            return S_OK;
        }
    }
    *index = 0;
    *found = FALSE;
    return S_OK;
}

static HRESULT hstring_vector_reserve( struct hstring_vector *impl, UINT32 needed )
{
    HSTRING *grown;
    UINT32 capacity;

    if (needed <= impl->capacity) return S_OK;
    capacity = max( needed, max( impl->capacity * 2, 8 ) );
    if (!(grown = realloc( impl->elements, capacity * sizeof(*grown) ))) return E_OUTOFMEMORY;
    impl->elements = grown;
    impl->capacity = capacity;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_SetAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, index %u, value %s.\n", iface, index, debugstr_hstring( value ) );

    if (index >= impl->count) return E_BOUNDS;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->elements[index] );
    impl->elements[index] = copy;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_InsertAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, index %u, value %s.\n", iface, index, debugstr_hstring( value ) );

    if (index > impl->count) return E_BOUNDS;
    if (FAILED(hr = hstring_vector_reserve( impl, impl->count + 1 ))) return hr;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    memmove( impl->elements + index + 1, impl->elements + index,
             (impl->count - index) * sizeof(*impl->elements) );
    impl->elements[index] = copy;
    impl->count++;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_RemoveAt( IVector_HSTRING *iface, UINT32 index )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, index %u.\n", iface, index );

    if (index >= impl->count) return E_BOUNDS;
    WindowsDeleteString( impl->elements[index] );
    impl->count--;
    memmove( impl->elements + index, impl->elements + index + 1,
             (impl->count - index) * sizeof(*impl->elements) );
    return S_OK;
}

static HRESULT WINAPI hstring_vector_Append( IVector_HSTRING *iface, HSTRING value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    return hstring_vector_InsertAt( iface, impl->count, value );
}

static HRESULT WINAPI hstring_vector_RemoveAtEnd( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p.\n", iface );

    if (!impl->count) return E_BOUNDS;
    return hstring_vector_RemoveAt( iface, impl->count - 1 );
}

static HRESULT WINAPI hstring_vector_Clear( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p.\n", iface );

    while (impl->count) WindowsDeleteString( impl->elements[--impl->count] );
    return S_OK;
}

static HRESULT WINAPI hstring_vector_GetMany( IVector_HSTRING *iface, UINT32 start_index, UINT32 count,
                                              HSTRING *items, UINT32 *value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    UINT32 i;
    HRESULT hr;

    TRACE( "iface %p, start_index %u, count %u, items %p, value %p.\n", iface, start_index, count, items, value );

    if (!value) return E_POINTER;
    *value = 0;
    if (start_index > impl->count) return E_BOUNDS;
    count = min( count, impl->count - start_index );
    for (i = 0; i < count; i++)
    {
        if (FAILED(hr = WindowsDuplicateString( impl->elements[start_index + i], items + i )))
        {
            while (i--) WindowsDeleteString( items[i] );
            return hr;
        }
    }
    *value = count;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_ReplaceAll( IVector_HSTRING *iface, UINT32 count, HSTRING *items )
{
    UINT32 i;
    HRESULT hr;

    TRACE( "iface %p, count %u, items %p.\n", iface, count, items );

    hstring_vector_Clear( iface );
    for (i = 0; i < count; i++)
        if (FAILED(hr = hstring_vector_Append( iface, items[i] ))) return hr;
    return S_OK;
}

static const struct IVector_HSTRINGVtbl hstring_vector_vtbl =
{
    /* IUnknown methods */
    hstring_vector_QueryInterface,
    hstring_vector_AddRef,
    hstring_vector_Release,
    /* IInspectable methods */
    hstring_vector_GetIids,
    hstring_vector_GetRuntimeClassName,
    hstring_vector_GetTrustLevel,
    /* IVector<HSTRING> methods */
    hstring_vector_GetAt,
    hstring_vector_get_Size,
    hstring_vector_GetView,
    hstring_vector_IndexOf,
    hstring_vector_SetAt,
    hstring_vector_InsertAt,
    hstring_vector_RemoveAt,
    hstring_vector_Append,
    hstring_vector_RemoveAtEnd,
    hstring_vector_Clear,
    hstring_vector_GetMany,
    hstring_vector_ReplaceAll,
};

/* IIterable<HSTRING> over the same storage. */
struct hstring_iterator
{
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG ref;
    IVector_HSTRING *vector;
    UINT32 index;
};

static inline struct hstring_iterator *impl_from_IIterator_HSTRING( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterator, IIterator_HSTRING_iface );
}

static HRESULT WINAPI hstring_iterator_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->IIterator_HSTRING_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_iterator_AddRef( IIterator_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_HSTRING( iface )->ref );
}

static ULONG WINAPI hstring_iterator_Release( IIterator_HSTRING *iface )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVector_HSTRING_Release( impl->vector );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI hstring_iterator_GetIids( IIterator_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterator_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IIterator`1<String>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IIterator`1<String>"),
                                class_name );
}

static HRESULT WINAPI hstring_iterator_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterator_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    return IVector_HSTRING_GetAt( impl->vector, impl->index, value );
}

static HRESULT WINAPI hstring_iterator_get_HasCurrent( IIterator_HSTRING *iface, BOOLEAN *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    UINT32 size;
    HRESULT hr;

    if (!value) return E_POINTER;
    if (FAILED(hr = IVector_HSTRING_get_Size( impl->vector, &size ))) return hr;
    *value = impl->index < size;
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_MoveNext( IIterator_HSTRING *iface, BOOLEAN *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    UINT32 size;
    HRESULT hr;

    if (FAILED(hr = IVector_HSTRING_get_Size( impl->vector, &size ))) return hr;
    if (impl->index < size) impl->index++;
    return hstring_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI hstring_iterator_GetMany( IIterator_HSTRING *iface, UINT32 count, HSTRING *items,
                                                UINT32 *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    HRESULT hr = IVector_HSTRING_GetMany( impl->vector, impl->index, count, items, value );
    if (SUCCEEDED(hr)) impl->index += *value;
    return hr;
}

static const struct IIterator_HSTRINGVtbl hstring_iterator_vtbl =
{
    hstring_iterator_QueryInterface,
    hstring_iterator_AddRef,
    hstring_iterator_Release,
    hstring_iterator_GetIids,
    hstring_iterator_GetRuntimeClassName,
    hstring_iterator_GetTrustLevel,
    hstring_iterator_get_Current,
    hstring_iterator_get_HasCurrent,
    hstring_iterator_MoveNext,
    hstring_iterator_GetMany,
};

static HRESULT WINAPI hstring_iterable_QueryInterface( IIterable_HSTRING *iface, REFIID iid, void **out )
{
    return hstring_vector_QueryInterface( &impl_from_IIterable_HSTRING( iface )->IVector_HSTRING_iface, iid, out );
}

static ULONG WINAPI hstring_iterable_AddRef( IIterable_HSTRING *iface )
{
    return hstring_vector_AddRef( &impl_from_IIterable_HSTRING( iface )->IVector_HSTRING_iface );
}

static ULONG WINAPI hstring_iterable_Release( IIterable_HSTRING *iface )
{
    return hstring_vector_Release( &impl_from_IIterable_HSTRING( iface )->IVector_HSTRING_iface );
}

static HRESULT WINAPI hstring_iterable_GetIids( IIterable_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterable_GetRuntimeClassName( IIterable_HSTRING *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IIterable`1<String>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IIterable`1<String>"),
                                class_name );
}

static HRESULT WINAPI hstring_iterable_GetTrustLevel( IIterable_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hstring_vector *impl = impl_from_IIterable_HSTRING( iface );
    struct hstring_iterator *iter;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_HSTRING_iface.lpVtbl = &hstring_iterator_vtbl;
    iter->ref = 1;
    IVector_HSTRING_AddRef( (iter->vector = &impl->IVector_HSTRING_iface) );
    *value = &iter->IIterator_HSTRING_iface;
    return S_OK;
}

static const struct IIterable_HSTRINGVtbl hstring_iterable_vtbl =
{
    hstring_iterable_QueryInterface,
    hstring_iterable_AddRef,
    hstring_iterable_Release,
    hstring_iterable_GetIids,
    hstring_iterable_GetRuntimeClassName,
    hstring_iterable_GetTrustLevel,
    hstring_iterable_First,
};

HRESULT empty_hstring_vector_create( IVector_HSTRING **out )
{
    return hstring_vector_create( out );
}

static HRESULT hstring_vector_create( IVector_HSTRING **out )
{
    struct hstring_vector *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVector_HSTRING_iface.lpVtbl = &hstring_vector_vtbl;
    impl->IIterable_HSTRING_iface.lpVtbl = &hstring_iterable_vtbl;
    impl->ref = 1;
    *out = &impl->IVector_HSTRING_iface;
    return S_OK;
}

/* ---------------------------------------------------------------- QueryOptions */

struct query_options
{
    IQueryOptionsInternal IQueryOptionsInternal_iface;
    LONG ref;

    IVector_HSTRING *file_type_filter;
    INT32 folder_depth;
    INT32 indexer_option;
    HSTRING application_search_filter;
    HSTRING user_search_filter;
    HSTRING language;
    HSTRING group_property_name;
    INT32 date_stack_option;
};

static inline struct query_options *impl_from_IQueryOptionsInternal( IQueryOptionsInternal *iface )
{
    return CONTAINING_RECORD( iface, struct query_options, IQueryOptionsInternal_iface );
}

static HRESULT WINAPI query_options_QueryInterface( IQueryOptionsInternal *iface, REFIID iid, void **out )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IQueryOptionsInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IQueryOptionsInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI query_options_AddRef( IQueryOptionsInternal *iface )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI query_options_Release( IQueryOptionsInternal *iface )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->file_type_filter) IVector_HSTRING_Release( impl->file_type_filter );
        WindowsDeleteString( impl->application_search_filter );
        WindowsDeleteString( impl->user_search_filter );
        WindowsDeleteString( impl->language );
        WindowsDeleteString( impl->group_property_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI query_options_GetIids( IQueryOptionsInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_options_GetRuntimeClassName( IQueryOptionsInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_Search_QueryOptions,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_Search_QueryOptions) - 1,
                                class_name );
}

static HRESULT WINAPI query_options_GetTrustLevel( IQueryOptionsInternal *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_options_get_FileTypeFilter( IQueryOptionsInternal *iface, IVector_HSTRING **value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IVector_HSTRING_AddRef( (*value = impl->file_type_filter) );
    return S_OK;
}

static HRESULT WINAPI query_options_get_FolderDepth( IQueryOptionsInternal *iface, INT32 *value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->folder_depth;
    return S_OK;
}

static HRESULT WINAPI query_options_put_FolderDepth( IQueryOptionsInternal *iface, INT32 value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    impl->folder_depth = value;
    return S_OK;
}

/* AQS filter strings. Nothing here evaluates them - the query walks the directory and matches
 * extensions - so they are stored and handed back unchanged rather than refused, which would make
 * a caller that round-trips its own options fail. */
#define DEFINE_QUERY_OPTIONS_HSTRING( name, field )                                                \
    static HRESULT WINAPI query_options_get_##name( IQueryOptionsInternal *iface, HSTRING *value )         \
    {                                                                                              \
        struct query_options *impl = impl_from_IQueryOptionsInternal( iface );                             \
        TRACE( "iface %p, value %p.\n", iface, value );                                            \
        if (!value) return E_POINTER;                                                              \
        return WindowsDuplicateString( impl->field, value );                                       \
    }                                                                                              \
    static HRESULT WINAPI query_options_put_##name( IQueryOptionsInternal *iface, HSTRING value )          \
    {                                                                                              \
        struct query_options *impl = impl_from_IQueryOptionsInternal( iface );                             \
        HSTRING copy;                                                                              \
        HRESULT hr;                                                                                \
        TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );                        \
        if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;                        \
        WindowsDeleteString( impl->field );                                                        \
        impl->field = copy;                                                                        \
        return S_OK;                                                                               \
    }

DEFINE_QUERY_OPTIONS_HSTRING( ApplicationSearchFilter, application_search_filter )
DEFINE_QUERY_OPTIONS_HSTRING( UserSearchFilter, user_search_filter )
DEFINE_QUERY_OPTIONS_HSTRING( Language, language )

static HRESULT WINAPI query_options_get_IndexerOption( IQueryOptionsInternal *iface, INT32 *value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->indexer_option;
    return S_OK;
}

static HRESULT WINAPI query_options_put_IndexerOption( IQueryOptionsInternal *iface, INT32 value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    impl->indexer_option = value;
    return S_OK;
}

/* SortOrder is an IVector<SortEntry>, GroupPropertyName and DateStackOption describe grouping.
 * The query implementation ignores all three, and handing back an empty sort order would silently
 * drop a caller's ordering, so report them as unimplemented. */
static HRESULT WINAPI query_options_get_SortOrder( IQueryOptionsInternal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI query_options_get_GroupPropertyName( IQueryOptionsInternal *iface, HSTRING *value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->group_property_name, value );
}

static HRESULT WINAPI query_options_get_DateStackOption( IQueryOptionsInternal *iface, INT32 *value )
{
    struct query_options *impl = impl_from_IQueryOptionsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->date_stack_option;
    return S_OK;
}

static HRESULT WINAPI query_options_SaveToString( IQueryOptionsInternal *iface, HSTRING *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI query_options_LoadFromString( IQueryOptionsInternal *iface, HSTRING value )
{
    FIXME( "iface %p, value %s stub!\n", iface, debugstr_hstring( value ) );
    return E_NOTIMPL;
}

/* Prefetch is an optimisation: the caller is saying which properties and thumbnails it will want,
 * so they can be fetched in one pass. Ignoring the hint is correct behaviour, not a refusal. */
static HRESULT WINAPI query_options_SetThumbnailPrefetch( IQueryOptionsInternal *iface, INT32 mode,
                                                          UINT32 requested_size, INT32 options )
{
    TRACE( "iface %p, mode %d, requested_size %u, options %d: ignored.\n", iface, mode, requested_size, options );
    return S_OK;
}

static HRESULT WINAPI query_options_SetPropertyPrefetch( IQueryOptionsInternal *iface, INT32 options,
                                                         IIterable_HSTRING *properties_to_retrieve )
{
    TRACE( "iface %p, options %d, properties_to_retrieve %p: ignored.\n", iface, options, properties_to_retrieve );
    return S_OK;
}

static const struct IQueryOptionsInternalVtbl query_options_vtbl =
{
    /* IUnknown methods */
    query_options_QueryInterface,
    query_options_AddRef,
    query_options_Release,
    /* IInspectable methods */
    query_options_GetIids,
    query_options_GetRuntimeClassName,
    query_options_GetTrustLevel,
    /* IQueryOptions methods */
    query_options_get_FileTypeFilter,
    query_options_get_FolderDepth,
    query_options_put_FolderDepth,
    query_options_get_ApplicationSearchFilter,
    query_options_put_ApplicationSearchFilter,
    query_options_get_UserSearchFilter,
    query_options_put_UserSearchFilter,
    query_options_get_Language,
    query_options_put_Language,
    query_options_get_IndexerOption,
    query_options_put_IndexerOption,
    query_options_get_SortOrder,
    query_options_get_GroupPropertyName,
    query_options_get_DateStackOption,
    query_options_SaveToString,
    query_options_LoadFromString,
    query_options_SetThumbnailPrefetch,
    query_options_SetPropertyPrefetch,
};

HRESULT query_options_create( INT32 query, IIterable_HSTRING *file_type_filter, IQueryOptionsInternal **out )
{
    struct query_options *impl;
    IIterator_HSTRING *iter = NULL;
    BOOLEAN has_current;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IQueryOptionsInternal_iface.lpVtbl = &query_options_vtbl;
    impl->ref = 1;
    /* CommonFileQuery selects an ordering, which nothing here applies; the folder depth it implies
     * is the part the query does honour. DefaultQuery is shallow, the rest are deep. */
    impl->folder_depth = (query == CommonFileQuery_DefaultQuery) ? FOLDER_DEPTH_SHALLOW : FOLDER_DEPTH_DEEP;
    impl->indexer_option = INDEXER_OPTION_DO_NOT_USE_INDEXER;

    if (FAILED(hr = hstring_vector_create( &impl->file_type_filter )))
    {
        free( impl );
        return hr;
    }

    if (file_type_filter && SUCCEEDED(IIterable_HSTRING_First( file_type_filter, &iter )))
    {
        while (SUCCEEDED(IIterator_HSTRING_get_HasCurrent( iter, &has_current )) && has_current)
        {
            HSTRING element;
            if (FAILED(IIterator_HSTRING_get_Current( iter, &element ))) break;
            IVector_HSTRING_Append( impl->file_type_filter, element );
            WindowsDeleteString( element );
            if (FAILED(IIterator_HSTRING_MoveNext( iter, &has_current ))) break;
        }
        IIterator_HSTRING_Release( iter );
    }

    *out = &impl->IQueryOptionsInternal_iface;
    return S_OK;
}

/* ---------------------------------------------------------------- activation factory */

struct query_options_statics
{
    IActivationFactory IActivationFactory_iface;
    IQueryOptionsFactoryInternal IQueryOptionsFactoryInternal_iface;
    LONG ref;
};

static inline struct query_options_statics *impl_from_IActivationFactory_qo( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct query_options_statics, IActivationFactory_iface );
}

static HRESULT WINAPI query_options_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct query_options_statics *impl = impl_from_IActivationFactory_qo( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IQueryOptionsFactoryInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IQueryOptionsFactoryInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI query_options_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory_qo( iface )->ref );
}

static ULONG WINAPI query_options_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory_qo( iface )->ref );
}

static HRESULT WINAPI query_options_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_options_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_Search_QueryOptions,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_Search_QueryOptions) - 1,
                                class_name );
}

static HRESULT WINAPI query_options_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_options_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    IQueryOptionsInternal *options;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    /* The default constructor is "everything, shallow". */
    if (FAILED(hr = query_options_create( CommonFileQuery_DefaultQuery, NULL, &options ))) return hr;
    *instance = (IInspectable *)options;
    return S_OK;
}

static const struct IActivationFactoryVtbl query_options_factory_vtbl =
{
    query_options_factory_QueryInterface,
    query_options_factory_AddRef,
    query_options_factory_Release,
    query_options_factory_GetIids,
    query_options_factory_GetRuntimeClassName,
    query_options_factory_GetTrustLevel,
    query_options_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( query_options_statics, IQueryOptionsFactoryInternal, struct query_options_statics,
                     IActivationFactory_iface )

static HRESULT WINAPI query_options_statics_CreateCommonFileQuery( IQueryOptionsFactoryInternal *iface,
        INT32 query, IIterable_HSTRING *file_type_filter, IQueryOptionsInternal **result )
{
    TRACE( "iface %p, query %d, file_type_filter %p, result %p.\n", iface, query, file_type_filter, result );

    if (!result) return E_POINTER;
    return query_options_create( query, file_type_filter, result );
}

static HRESULT WINAPI query_options_statics_CreateCommonFolderQuery( IQueryOptionsFactoryInternal *iface,
        INT32 query, IQueryOptionsInternal **result )
{
    TRACE( "iface %p, query %d, result %p.\n", iface, query, result );

    if (!result) return E_POINTER;
    /* Folder grouping is not implemented; the options are still a valid bag. */
    return query_options_create( CommonFileQuery_DefaultQuery, NULL, result );
}

static const struct IQueryOptionsFactoryInternalVtbl query_options_statics_vtbl =
{
    /* IUnknown methods */
    query_options_statics_QueryInterface,
    query_options_statics_AddRef,
    query_options_statics_Release,
    /* IInspectable methods */
    query_options_statics_GetIids,
    query_options_statics_GetRuntimeClassName,
    query_options_statics_GetTrustLevel,
    /* IQueryOptionsFactory methods */
    query_options_statics_CreateCommonFileQuery,
    query_options_statics_CreateCommonFolderQuery,
};

static struct query_options_statics query_options_statics =
{
    {&query_options_factory_vtbl},
    {&query_options_statics_vtbl},
    1,
};

IActivationFactory *query_options_factory = &query_options_statics.IActivationFactory_iface;

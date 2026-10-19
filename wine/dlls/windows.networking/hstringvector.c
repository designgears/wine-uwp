/*
 * IVector<HSTRING>, for MessageWebSocketControl.SupportedProtocols
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
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsock);

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

HRESULT hstring_vector_create( IVector_HSTRING **out )
{
    struct hstring_vector *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVector_HSTRING_iface.lpVtbl = &hstring_vector_vtbl;
    impl->IIterable_HSTRING_iface.lpVtbl = &hstring_iterable_vtbl;
    impl->ref = 1;
    *out = &impl->IVector_HSTRING_iface;
    return S_OK;
}

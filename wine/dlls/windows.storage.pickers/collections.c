/* The collections of Windows.Storage.Pickers
 *
 * Copyright 2026 wine-uwp contributors
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

WINE_DEFAULT_DEBUG_CHANNEL(pickers);

/* FileExtensionVector: an IVector<HSTRING> the app appends extensions to. */

struct hstring_vector
{
    IVector_HSTRING IVector_HSTRING_iface;
    IVectorView_HSTRING IVectorView_HSTRING_iface;
    IIterable_HSTRING IIterable_HSTRING_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    HSTRING *items;
    UINT32 count;
    UINT32 capacity;
};

struct hstring_iterator
{
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG ref;
    struct hstring_vector *vector;
    UINT32 index;
};

static inline struct hstring_vector *impl_from_IVector_HSTRING( IVector_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IVector_HSTRING_iface );
}

static HRESULT WINAPI vector_QueryInterface( IVector_HSTRING *iface, REFIID iid, void **out )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVector_HSTRING ))
        *out = &impl->IVector_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IVectorView_HSTRING ))
        *out = &impl->IVectorView_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_HSTRING ))
        *out = &impl->IIterable_HSTRING_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI vector_AddRef( IVector_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IVector_HSTRING( iface )->ref );
}

static ULONG WINAPI vector_Release( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) WindowsDeleteString( impl->items[i] );
        free( impl->items );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI vector_GetIids( IVector_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI vector_GetRuntimeClassName( IVector_HSTRING *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Storage.Pickers.FileExtensionVector";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI vector_GetTrustLevel( IVector_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT vector_get_at( struct hstring_vector *impl, UINT32 index, HSTRING *value )
{
    HRESULT hr;

    *value = NULL;
    EnterCriticalSection( &impl->cs );
    hr = index < impl->count ? WindowsDuplicateString( impl->items[index], value ) : E_BOUNDS;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT vector_index_of( struct hstring_vector *impl, HSTRING element, UINT32 *index, boolean *found )
{
    INT32 order;
    UINT32 i;

    *index = 0;
    *found = FALSE;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++)
    {
        if (FAILED(WindowsCompareStringOrdinal( impl->items[i], element, &order )) || order) continue;
        *index = i;
        *found = TRUE;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT vector_get_many( struct hstring_vector *impl, UINT32 start, UINT32 count, HSTRING *items, UINT32 *written )
{
    HRESULT hr = S_OK;

    *written = 0;
    EnterCriticalSection( &impl->cs );
    if (start > impl->count) hr = E_BOUNDS;
    while (SUCCEEDED(hr) && *written < count && start + *written < impl->count)
    {
        if (SUCCEEDED(hr = WindowsDuplicateString( impl->items[start + *written], &items[*written] ))) (*written)++;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vector_GetAt( IVector_HSTRING *iface, UINT32 index, HSTRING *value )
{
    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );
    return vector_get_at( impl_from_IVector_HSTRING( iface ), index, value );
}

static HRESULT WINAPI vector_get_Size( IVector_HSTRING *iface, UINT32 *value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI vector_GetView( IVector_HSTRING *iface, IVectorView_HSTRING **value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    IVectorView_HSTRING_AddRef( (*value = &impl->IVectorView_HSTRING_iface) );
    return S_OK;
}

static HRESULT WINAPI vector_IndexOf( IVector_HSTRING *iface, HSTRING element, UINT32 *index, boolean *found )
{
    TRACE( "iface %p, element %s, index %p, found %p.\n", iface, debugstr_hstring( element ), index, found );
    return vector_index_of( impl_from_IVector_HSTRING( iface ), element, index, found );
}

static HRESULT WINAPI vector_SetAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, index %u, value %s.\n", iface, index, debugstr_hstring( value ) );

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    EnterCriticalSection( &impl->cs );
    if (index < impl->count)
    {
        WindowsDeleteString( impl->items[index] );
        impl->items[index] = copy;
    }
    else hr = E_BOUNDS;
    LeaveCriticalSection( &impl->cs );
    if (FAILED(hr)) WindowsDeleteString( copy );
    return hr;
}

static HRESULT WINAPI vector_InsertAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    HSTRING copy, *items;
    HRESULT hr;

    TRACE( "iface %p, index %u, value %s.\n", iface, index, debugstr_hstring( value ) );

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    EnterCriticalSection( &impl->cs );
    if (index > impl->count) hr = E_BOUNDS;
    else if (impl->count == impl->capacity)
    {
        UINT32 capacity = max( 8, impl->capacity * 2 );
        if (!(items = realloc( impl->items, capacity * sizeof(*items) ))) hr = E_OUTOFMEMORY;
        else
        {
            impl->items = items;
            impl->capacity = capacity;
        }
    }
    if (SUCCEEDED(hr))
    {
        memmove( impl->items + index + 1, impl->items + index, (impl->count - index) * sizeof(*impl->items) );
        impl->items[index] = copy;
        impl->count++;
    }
    LeaveCriticalSection( &impl->cs );
    if (FAILED(hr)) WindowsDeleteString( copy );
    return hr;
}

static HRESULT WINAPI vector_RemoveAt( IVector_HSTRING *iface, UINT32 index )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, index %u.\n", iface, index );

    EnterCriticalSection( &impl->cs );
    if (index < impl->count)
    {
        WindowsDeleteString( impl->items[index] );
        memmove( impl->items + index, impl->items + index + 1, (impl->count - index - 1) * sizeof(*impl->items) );
        impl->count--;
    }
    else hr = E_BOUNDS;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vector_Append( IVector_HSTRING *iface, HSTRING value )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    return vector_InsertAt( iface, impl->count, value );
}

static HRESULT WINAPI vector_RemoveAtEnd( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );
    return impl->count ? vector_RemoveAt( iface, impl->count - 1 ) : E_BOUNDS;
}

static HRESULT WINAPI vector_Clear( IVector_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVector_HSTRING( iface );

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    while (impl->count) WindowsDeleteString( impl->items[--impl->count] );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI vector_GetMany( IVector_HSTRING *iface, UINT32 start, UINT32 count, HSTRING *items, UINT32 *written )
{
    TRACE( "iface %p, start %u, count %u, items %p, written %p.\n", iface, start, count, items, written );
    return vector_get_many( impl_from_IVector_HSTRING( iface ), start, count, items, written );
}

static HRESULT WINAPI vector_ReplaceAll( IVector_HSTRING *iface, UINT32 count, HSTRING *items )
{
    HRESULT hr;
    UINT32 i;

    TRACE( "iface %p, count %u, items %p.\n", iface, count, items );

    vector_Clear( iface );
    for (i = 0; i < count; i++) if (FAILED(hr = vector_Append( iface, items[i] ))) return hr;
    return S_OK;
}

static const struct IVector_HSTRINGVtbl vector_vtbl =
{
    vector_QueryInterface,
    vector_AddRef,
    vector_Release,
    /* IInspectable methods */
    vector_GetIids,
    vector_GetRuntimeClassName,
    vector_GetTrustLevel,
    /* IVector<HSTRING> methods */
    vector_GetAt,
    vector_get_Size,
    vector_GetView,
    vector_IndexOf,
    vector_SetAt,
    vector_InsertAt,
    vector_RemoveAt,
    vector_Append,
    vector_RemoveAtEnd,
    vector_Clear,
    vector_GetMany,
    vector_ReplaceAll,
};

DEFINE_IINSPECTABLE( vector_view, IVectorView_HSTRING, struct hstring_vector, IVector_HSTRING_iface )

static HRESULT WINAPI vector_view_GetAt( IVectorView_HSTRING *iface, UINT32 index, HSTRING *value )
{
    return vector_get_at( impl_from_IVectorView_HSTRING( iface ), index, value );
}

static HRESULT WINAPI vector_view_get_Size( IVectorView_HSTRING *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_HSTRING( iface )->count;
    return S_OK;
}

static HRESULT WINAPI vector_view_IndexOf( IVectorView_HSTRING *iface, HSTRING element, UINT32 *index, boolean *found )
{
    return vector_index_of( impl_from_IVectorView_HSTRING( iface ), element, index, found );
}

static HRESULT WINAPI vector_view_GetMany( IVectorView_HSTRING *iface, UINT32 start, UINT32 count, HSTRING *items,
                                           UINT32 *written )
{
    return vector_get_many( impl_from_IVectorView_HSTRING( iface ), start, count, items, written );
}

static const struct IVectorView_HSTRINGVtbl vector_view_vtbl =
{
    vector_view_QueryInterface,
    vector_view_AddRef,
    vector_view_Release,
    /* IInspectable methods */
    vector_view_GetIids,
    vector_view_GetRuntimeClassName,
    vector_view_GetTrustLevel,
    /* IVectorView<HSTRING> methods */
    vector_view_GetAt,
    vector_view_get_Size,
    vector_view_IndexOf,
    vector_view_GetMany,
};

static inline struct hstring_iterator *impl_from_IIterator_HSTRING( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterator, IIterator_HSTRING_iface );
}

static HRESULT WINAPI iterator_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_HSTRING ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef( IIterator_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_HSTRING( iface )->ref );
}

static ULONG WINAPI iterator_Release( IIterator_HSTRING *iface )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVector_HSTRING_Release( &impl->vector->IVector_HSTRING_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iterator_GetIids( IIterator_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterator`1<String>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI iterator_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iterator_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    return vector_get_at( impl->vector, impl->index, value );
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );

    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );

    if (impl->index < impl->vector->count) impl->index++;
    return iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI iterator_GetMany( IIterator_HSTRING *iface, UINT32 count, HSTRING *items, UINT32 *written )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    HRESULT hr = vector_get_many( impl->vector, impl->index, count, items, written );

    if (SUCCEEDED(hr)) impl->index += *written;
    return hr;
}

static const struct IIterator_HSTRINGVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    /* IInspectable methods */
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    /* IIterator<HSTRING> methods */
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

DEFINE_IINSPECTABLE( vector_iterable, IIterable_HSTRING, struct hstring_vector, IVector_HSTRING_iface )

static HRESULT WINAPI vector_iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hstring_vector *impl = impl_from_IIterable_HSTRING( iface );
    struct hstring_iterator *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_HSTRING_iface.lpVtbl = &iterator_vtbl;
    iter->ref = 1;
    iter->vector = impl;
    IVector_HSTRING_AddRef( &impl->IVector_HSTRING_iface );
    *value = &iter->IIterator_HSTRING_iface;
    return S_OK;
}

static const struct IIterable_HSTRINGVtbl vector_iterable_vtbl =
{
    vector_iterable_QueryInterface,
    vector_iterable_AddRef,
    vector_iterable_Release,
    /* IInspectable methods */
    vector_iterable_GetIids,
    vector_iterable_GetRuntimeClassName,
    vector_iterable_GetTrustLevel,
    /* IIterable<HSTRING> methods */
    vector_iterable_First,
};

HRESULT hstring_vector_create( IVector_HSTRING **out )
{
    struct hstring_vector *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVector_HSTRING_iface.lpVtbl = &vector_vtbl;
    impl->IVectorView_HSTRING_iface.lpVtbl = &vector_view_vtbl;
    impl->IIterable_HSTRING_iface.lpVtbl = &vector_iterable_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": hstring_vector.cs");
    *out = &impl->IVector_HSTRING_iface;
    return S_OK;
}

/* FilePickerFileTypesOrderedMap: the save picker's FileTypeChoices, an IMap<HSTRING, IVector<HSTRING>>
 * that keeps the order the app inserted its choices in, which is the order the dialog lists them.
 * Only the picker reads it, on the app's thread, so it is not locked. */

struct choice
{
    HSTRING key;
    IVector_HSTRING *value;
};

struct choices
{
    IMap_HSTRING_IVector_HSTRING IMap_HSTRING_IVector_HSTRING_iface;
    IIterable_IKeyValuePair_HSTRING_IVector_HSTRING IIterable_iface;
    LONG ref;
    struct choice *items;
    UINT32 count;
};

struct choice_pair
{
    IKeyValuePair_HSTRING_IVector_HSTRING IKeyValuePair_iface;
    LONG ref;
    HSTRING key;
    IVector_HSTRING *value;
};

struct choice_iterator
{
    IIterator_IKeyValuePair_HSTRING_IVector_HSTRING IIterator_iface;
    LONG ref;
    struct choices *map;
    UINT32 index;
};

static inline struct choices *impl_from_IMap( IMap_HSTRING_IVector_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct choices, IMap_HSTRING_IVector_HSTRING_iface );
}

static HRESULT WINAPI choices_QueryInterface( IMap_HSTRING_IVector_HSTRING *iface, REFIID iid, void **out )
{
    struct choices *impl = impl_from_IMap( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMap_HSTRING_IVector_HSTRING ))
        *out = &impl->IMap_HSTRING_IVector_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_IVector_HSTRING ))
        *out = &impl->IIterable_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI choices_AddRef( IMap_HSTRING_IVector_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IMap( iface )->ref );
}

static void choices_clear( struct choices *impl )
{
    while (impl->count)
    {
        impl->count--;
        WindowsDeleteString( impl->items[impl->count].key );
        IVector_HSTRING_Release( impl->items[impl->count].value );
    }
}

static ULONG WINAPI choices_Release( IMap_HSTRING_IVector_HSTRING *iface )
{
    struct choices *impl = impl_from_IMap( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        choices_clear( impl );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI choices_GetIids( IMap_HSTRING_IVector_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI choices_GetRuntimeClassName( IMap_HSTRING_IVector_HSTRING *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Storage.Pickers.FilePickerFileTypesOrderedMap";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI choices_GetTrustLevel( IMap_HSTRING_IVector_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static struct choice *choices_find( struct choices *impl, HSTRING key )
{
    INT32 order;
    UINT32 i;

    for (i = 0; i < impl->count; i++)
        if (SUCCEEDED(WindowsCompareStringOrdinal( impl->items[i].key, key, &order )) && !order) return &impl->items[i];
    return NULL;
}

static HRESULT WINAPI choices_Lookup( IMap_HSTRING_IVector_HSTRING *iface, HSTRING key, IVector_HSTRING **value )
{
    struct choice *item = choices_find( impl_from_IMap( iface ), key );

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!item)
    {
        *value = NULL;
        return E_BOUNDS;
    }
    IVector_HSTRING_AddRef( (*value = item->value) );
    return S_OK;
}

static HRESULT WINAPI choices_get_Size( IMap_HSTRING_IVector_HSTRING *iface, UINT32 *size )
{
    *size = impl_from_IMap( iface )->count;
    return S_OK;
}

static HRESULT WINAPI choices_HasKey( IMap_HSTRING_IVector_HSTRING *iface, HSTRING key, boolean *found )
{
    *found = !!choices_find( impl_from_IMap( iface ), key );
    return S_OK;
}

static HRESULT WINAPI choices_GetView( IMap_HSTRING_IVector_HSTRING *iface, IMapView_HSTRING_IVector_HSTRING **view )
{
    FIXME( "iface %p, view %p stub!\n", iface, view );
    *view = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI choices_Insert( IMap_HSTRING_IVector_HSTRING *iface, HSTRING key, IVector_HSTRING *value,
                                      boolean *replaced )
{
    struct choices *impl = impl_from_IMap( iface );
    struct choice *item, *items;
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, key %s, value %p, replaced %p.\n", iface, debugstr_hstring( key ), value, replaced );

    if (!value) return E_INVALIDARG;
    if ((item = choices_find( impl, key )))
    {
        IVector_HSTRING_Release( item->value );
        IVector_HSTRING_AddRef( (item->value = value) );
        *replaced = TRUE;
        return S_OK;
    }
    if (FAILED(hr = WindowsDuplicateString( key, &copy ))) return hr;
    if (!(items = realloc( impl->items, (impl->count + 1) * sizeof(*items) )))
    {
        WindowsDeleteString( copy );
        return E_OUTOFMEMORY;
    }
    impl->items = items;
    item = &impl->items[impl->count++];
    item->key = copy;
    IVector_HSTRING_AddRef( (item->value = value) );
    *replaced = FALSE;
    return S_OK;
}

static HRESULT WINAPI choices_Remove( IMap_HSTRING_IVector_HSTRING *iface, HSTRING key )
{
    struct choices *impl = impl_from_IMap( iface );
    struct choice *item;

    TRACE( "iface %p, key %s.\n", iface, debugstr_hstring( key ) );

    if (!(item = choices_find( impl, key ))) return E_BOUNDS;
    WindowsDeleteString( item->key );
    IVector_HSTRING_Release( item->value );
    memmove( item, item + 1, (impl->items + impl->count - item - 1) * sizeof(*item) );
    impl->count--;
    return S_OK;
}

static HRESULT WINAPI choices_Clear( IMap_HSTRING_IVector_HSTRING *iface )
{
    TRACE( "iface %p.\n", iface );
    choices_clear( impl_from_IMap( iface ) );
    return S_OK;
}

static const struct IMap_HSTRING_IVector_HSTRINGVtbl choices_vtbl =
{
    choices_QueryInterface,
    choices_AddRef,
    choices_Release,
    /* IInspectable methods */
    choices_GetIids,
    choices_GetRuntimeClassName,
    choices_GetTrustLevel,
    /* IMap<HSTRING, IVector<HSTRING>> methods */
    choices_Lookup,
    choices_get_Size,
    choices_HasKey,
    choices_GetView,
    choices_Insert,
    choices_Remove,
    choices_Clear,
};

static inline struct choice_pair *impl_from_IKeyValuePair( IKeyValuePair_HSTRING_IVector_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct choice_pair, IKeyValuePair_iface );
}

static HRESULT WINAPI pair_QueryInterface( IKeyValuePair_HSTRING_IVector_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_IVector_HSTRING ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI pair_AddRef( IKeyValuePair_HSTRING_IVector_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IKeyValuePair( iface )->ref );
}

static ULONG WINAPI pair_Release( IKeyValuePair_HSTRING_IVector_HSTRING *iface )
{
    struct choice_pair *impl = impl_from_IKeyValuePair( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->key );
        IVector_HSTRING_Release( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI pair_GetIids( IKeyValuePair_HSTRING_IVector_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI pair_GetRuntimeClassName( IKeyValuePair_HSTRING_IVector_HSTRING *iface, HSTRING *class_name )
{
    static const WCHAR name[] =
        L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Foundation.Collections.IVector`1<String>>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI pair_GetTrustLevel( IKeyValuePair_HSTRING_IVector_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI pair_get_Key( IKeyValuePair_HSTRING_IVector_HSTRING *iface, HSTRING *key )
{
    return WindowsDuplicateString( impl_from_IKeyValuePair( iface )->key, key );
}

static HRESULT WINAPI pair_get_Value( IKeyValuePair_HSTRING_IVector_HSTRING *iface, IVector_HSTRING **value )
{
    IVector_HSTRING_AddRef( (*value = impl_from_IKeyValuePair( iface )->value) );
    return S_OK;
}

static const struct IKeyValuePair_HSTRING_IVector_HSTRINGVtbl pair_vtbl =
{
    pair_QueryInterface,
    pair_AddRef,
    pair_Release,
    /* IInspectable methods */
    pair_GetIids,
    pair_GetRuntimeClassName,
    pair_GetTrustLevel,
    /* IKeyValuePair<HSTRING, IVector<HSTRING>> methods */
    pair_get_Key,
    pair_get_Value,
};

static inline struct choice_iterator *impl_from_IIterator( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct choice_iterator, IIterator_iface );
}

static HRESULT WINAPI choice_iterator_QueryInterface( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface, REFIID iid,
                                                      void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_IVector_HSTRING ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI choice_iterator_AddRef( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator( iface )->ref );
}

static ULONG WINAPI choice_iterator_Release( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface )
{
    struct choice_iterator *impl = impl_from_IIterator( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IMap_HSTRING_IVector_HSTRING_Release( &impl->map->IMap_HSTRING_IVector_HSTRING_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI choice_iterator_GetIids( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface, ULONG *iid_count,
                                               IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI choice_iterator_GetRuntimeClassName( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface,
                                                           HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections."
                                L"IKeyValuePair`2<String, Windows.Foundation.Collections.IVector`1<String>>>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI choice_iterator_GetTrustLevel( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface,
                                                     TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI choice_iterator_get_Current( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface,
                                                   IKeyValuePair_HSTRING_IVector_HSTRING **value )
{
    struct choice_iterator *impl = impl_from_IIterator( iface );
    struct choice_pair *pair;
    HRESULT hr;

    *value = NULL;
    if (impl->index >= impl->map->count) return E_BOUNDS;
    if (!(pair = calloc( 1, sizeof(*pair) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = WindowsDuplicateString( impl->map->items[impl->index].key, &pair->key )))
    {
        free( pair );
        return hr;
    }
    pair->IKeyValuePair_iface.lpVtbl = &pair_vtbl;
    pair->ref = 1;
    IVector_HSTRING_AddRef( (pair->value = impl->map->items[impl->index].value) );
    *value = &pair->IKeyValuePair_iface;
    return S_OK;
}

static HRESULT WINAPI choice_iterator_get_HasCurrent( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface, boolean *value )
{
    struct choice_iterator *impl = impl_from_IIterator( iface );

    *value = impl->index < impl->map->count;
    return S_OK;
}

static HRESULT WINAPI choice_iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface, boolean *value )
{
    struct choice_iterator *impl = impl_from_IIterator( iface );

    if (impl->index < impl->map->count) impl->index++;
    return choice_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI choice_iterator_GetMany( IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iface, UINT32 count,
                                               IKeyValuePair_HSTRING_IVector_HSTRING **items, UINT32 *written )
{
    struct choice_iterator *impl = impl_from_IIterator( iface );
    HRESULT hr = S_OK;

    *written = 0;
    while (*written < count && impl->index < impl->map->count)
    {
        if (FAILED(hr = choice_iterator_get_Current( iface, &items[*written] ))) break;
        impl->index++;
        (*written)++;
    }
    return hr;
}

static const struct IIterator_IKeyValuePair_HSTRING_IVector_HSTRINGVtbl choice_iterator_vtbl =
{
    choice_iterator_QueryInterface,
    choice_iterator_AddRef,
    choice_iterator_Release,
    /* IInspectable methods */
    choice_iterator_GetIids,
    choice_iterator_GetRuntimeClassName,
    choice_iterator_GetTrustLevel,
    /* IIterator<IKeyValuePair<HSTRING, IVector<HSTRING>>> methods */
    choice_iterator_get_Current,
    choice_iterator_get_HasCurrent,
    choice_iterator_MoveNext,
    choice_iterator_GetMany,
};

DEFINE_IINSPECTABLE_( choices_iterable, IIterable_IKeyValuePair_HSTRING_IVector_HSTRING, struct choices,
                      impl_from_choices_iterable, IIterable_iface, &impl->IMap_HSTRING_IVector_HSTRING_iface )

static HRESULT WINAPI choices_iterable_First( IIterable_IKeyValuePair_HSTRING_IVector_HSTRING *iface,
                                              IIterator_IKeyValuePair_HSTRING_IVector_HSTRING **value )
{
    struct choices *impl = impl_from_choices_iterable( iface );
    struct choice_iterator *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_iface.lpVtbl = &choice_iterator_vtbl;
    iter->ref = 1;
    iter->map = impl;
    IMap_HSTRING_IVector_HSTRING_AddRef( &impl->IMap_HSTRING_IVector_HSTRING_iface );
    *value = &iter->IIterator_iface;
    return S_OK;
}

static const struct IIterable_IKeyValuePair_HSTRING_IVector_HSTRINGVtbl choices_iterable_vtbl =
{
    choices_iterable_QueryInterface,
    choices_iterable_AddRef,
    choices_iterable_Release,
    /* IInspectable methods */
    choices_iterable_GetIids,
    choices_iterable_GetRuntimeClassName,
    choices_iterable_GetTrustLevel,
    /* IIterable<IKeyValuePair<HSTRING, IVector<HSTRING>>> methods */
    choices_iterable_First,
};

HRESULT file_type_choices_create( IMap_HSTRING_IVector_HSTRING **out )
{
    struct choices *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMap_HSTRING_IVector_HSTRING_iface.lpVtbl = &choices_vtbl;
    impl->IIterable_iface.lpVtbl = &choices_iterable_vtbl;
    impl->ref = 1;
    *out = &impl->IMap_HSTRING_IVector_HSTRING_iface;
    return S_OK;
}

/* FilePickerSelectedFilesArray: the files of a multiple pick. */

struct file_list
{
    IVectorView_StorageFile IVectorView_StorageFile_iface;
    IIterable_StorageFile IIterable_StorageFile_iface;
    LONG ref;
    IStorageFile **files;
    UINT32 count;
};

struct file_iterator
{
    IIterator_StorageFile IIterator_StorageFile_iface;
    LONG ref;
    struct file_list *list;
    UINT32 index;
};

static inline struct file_list *impl_from_IVectorView_StorageFile( IVectorView_StorageFile *iface )
{
    return CONTAINING_RECORD( iface, struct file_list, IVectorView_StorageFile_iface );
}

static HRESULT WINAPI file_list_QueryInterface( IVectorView_StorageFile *iface, REFIID iid, void **out )
{
    struct file_list *impl = impl_from_IVectorView_StorageFile( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVectorView_StorageFile ))
        *out = &impl->IVectorView_StorageFile_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_StorageFile ))
        *out = &impl->IIterable_StorageFile_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI file_list_AddRef( IVectorView_StorageFile *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_StorageFile( iface )->ref );
}

static ULONG WINAPI file_list_Release( IVectorView_StorageFile *iface )
{
    struct file_list *impl = impl_from_IVectorView_StorageFile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IStorageFile_Release( impl->files[i] );
        free( impl->files );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_list_GetIids( IVectorView_StorageFile *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_list_GetRuntimeClassName( IVectorView_StorageFile *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Storage.Pickers.FilePickerSelectedFilesArray";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI file_list_GetTrustLevel( IVectorView_StorageFile *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI file_list_GetAt( IVectorView_StorageFile *iface, UINT32 index, IStorageFile **value )
{
    struct file_list *impl = impl_from_IVectorView_StorageFile( iface );

    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IStorageFile_AddRef( (*value = impl->files[index]) );
    return S_OK;
}

static HRESULT WINAPI file_list_get_Size( IVectorView_StorageFile *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_StorageFile( iface )->count;
    return S_OK;
}

static HRESULT WINAPI file_list_IndexOf( IVectorView_StorageFile *iface, IStorageFile *element, UINT32 *index, boolean *found )
{
    struct file_list *impl = impl_from_IVectorView_StorageFile( iface );
    UINT32 i;

    *index = 0;
    *found = FALSE;
    for (i = 0; i < impl->count; i++)
    {
        if (impl->files[i] != element) continue;
        *index = i;
        *found = TRUE;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI file_list_GetMany( IVectorView_StorageFile *iface, UINT32 start, UINT32 count,
                                         IStorageFile **items, UINT32 *written )
{
    struct file_list *impl = impl_from_IVectorView_StorageFile( iface );

    *written = 0;
    if (start > impl->count) return E_BOUNDS;
    while (*written < count && start + *written < impl->count)
    {
        IStorageFile_AddRef( (items[*written] = impl->files[start + *written]) );
        (*written)++;
    }
    return S_OK;
}

static const struct IVectorView_StorageFileVtbl file_list_vtbl =
{
    file_list_QueryInterface,
    file_list_AddRef,
    file_list_Release,
    /* IInspectable methods */
    file_list_GetIids,
    file_list_GetRuntimeClassName,
    file_list_GetTrustLevel,
    /* IVectorView<StorageFile> methods */
    file_list_GetAt,
    file_list_get_Size,
    file_list_IndexOf,
    file_list_GetMany,
};

static inline struct file_iterator *impl_from_IIterator_StorageFile( IIterator_StorageFile *iface )
{
    return CONTAINING_RECORD( iface, struct file_iterator, IIterator_StorageFile_iface );
}

static HRESULT WINAPI file_iterator_QueryInterface( IIterator_StorageFile *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_StorageFile ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_iterator_AddRef( IIterator_StorageFile *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_StorageFile( iface )->ref );
}

static ULONG WINAPI file_iterator_Release( IIterator_StorageFile *iface )
{
    struct file_iterator *impl = impl_from_IIterator_StorageFile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_StorageFile_Release( &impl->list->IVectorView_StorageFile_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_iterator_GetIids( IIterator_StorageFile *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_iterator_GetRuntimeClassName( IIterator_StorageFile *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.StorageFile>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI file_iterator_GetTrustLevel( IIterator_StorageFile *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI file_iterator_get_Current( IIterator_StorageFile *iface, IStorageFile **value )
{
    struct file_iterator *impl = impl_from_IIterator_StorageFile( iface );
    return file_list_GetAt( &impl->list->IVectorView_StorageFile_iface, impl->index, value );
}

static HRESULT WINAPI file_iterator_get_HasCurrent( IIterator_StorageFile *iface, boolean *value )
{
    struct file_iterator *impl = impl_from_IIterator_StorageFile( iface );

    *value = impl->index < impl->list->count;
    return S_OK;
}

static HRESULT WINAPI file_iterator_MoveNext( IIterator_StorageFile *iface, boolean *value )
{
    struct file_iterator *impl = impl_from_IIterator_StorageFile( iface );

    if (impl->index < impl->list->count) impl->index++;
    return file_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI file_iterator_GetMany( IIterator_StorageFile *iface, UINT32 count, IStorageFile **items,
                                             UINT32 *written )
{
    struct file_iterator *impl = impl_from_IIterator_StorageFile( iface );
    HRESULT hr = file_list_GetMany( &impl->list->IVectorView_StorageFile_iface, impl->index, count, items, written );

    if (SUCCEEDED(hr)) impl->index += *written;
    return hr;
}

static const struct IIterator_StorageFileVtbl file_iterator_vtbl =
{
    file_iterator_QueryInterface,
    file_iterator_AddRef,
    file_iterator_Release,
    /* IInspectable methods */
    file_iterator_GetIids,
    file_iterator_GetRuntimeClassName,
    file_iterator_GetTrustLevel,
    /* IIterator<StorageFile> methods */
    file_iterator_get_Current,
    file_iterator_get_HasCurrent,
    file_iterator_MoveNext,
    file_iterator_GetMany,
};

DEFINE_IINSPECTABLE( file_iterable, IIterable_StorageFile, struct file_list, IVectorView_StorageFile_iface )

static HRESULT WINAPI file_iterable_First( IIterable_StorageFile *iface, IIterator_StorageFile **value )
{
    struct file_list *impl = impl_from_IIterable_StorageFile( iface );
    struct file_iterator *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_StorageFile_iface.lpVtbl = &file_iterator_vtbl;
    iter->ref = 1;
    iter->list = impl;
    IVectorView_StorageFile_AddRef( &impl->IVectorView_StorageFile_iface );
    *value = &iter->IIterator_StorageFile_iface;
    return S_OK;
}

static const struct IIterable_StorageFileVtbl file_iterable_vtbl =
{
    file_iterable_QueryInterface,
    file_iterable_AddRef,
    file_iterable_Release,
    /* IInspectable methods */
    file_iterable_GetIids,
    file_iterable_GetRuntimeClassName,
    file_iterable_GetTrustLevel,
    /* IIterable<StorageFile> methods */
    file_iterable_First,
};

/* Takes the references in files, and the array. */
HRESULT file_list_create( IStorageFile **files, UINT32 count, IVectorView_StorageFile **out )
{
    struct file_list *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_StorageFile_iface.lpVtbl = &file_list_vtbl;
    impl->IIterable_StorageFile_iface.lpVtbl = &file_iterable_vtbl;
    impl->ref = 1;
    impl->files = files;
    impl->count = count;
    *out = &impl->IVectorView_StorageFile_iface;
    return S_OK;
}

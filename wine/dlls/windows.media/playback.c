/* WinRT Windows.Media.Playback implementation
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
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(media);

/*
 * The observable vector behind MediaPlaybackList.Items. Nothing in Wine produces a
 * MediaPlaybackItem yet, so in practice it stays empty, but an app is free to fill it.
 */

struct item_vector
{
    IObservableVector_MediaPlaybackItem IObservableVector_MediaPlaybackItem_iface;
    IVector_MediaPlaybackItem IVector_MediaPlaybackItem_iface;
    IIterable_MediaPlaybackItem IIterable_MediaPlaybackItem_iface;
    IVectorView_MediaPlaybackItem IVectorView_MediaPlaybackItem_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    IMediaPlaybackItem **items;
    UINT32 count;
    UINT32 capacity;
};

static inline struct item_vector *impl_from_IVector_MediaPlaybackItem( IVector_MediaPlaybackItem *iface )
{
    return CONTAINING_RECORD( iface, struct item_vector, IVector_MediaPlaybackItem_iface );
}

static HRESULT WINAPI item_vector_QueryInterface( IVector_MediaPlaybackItem *iface, REFIID iid, void **out )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IObservableVector_MediaPlaybackItem ))
    {
        *out = &impl->IObservableVector_MediaPlaybackItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IVector_MediaPlaybackItem ))
    {
        *out = &impl->IVector_MediaPlaybackItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IIterable_MediaPlaybackItem ))
    {
        *out = &impl->IIterable_MediaPlaybackItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IVectorView_MediaPlaybackItem ))
    {
        *out = &impl->IVectorView_MediaPlaybackItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI item_vector_AddRef( IVector_MediaPlaybackItem *iface )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI item_vector_Release( IVector_MediaPlaybackItem *iface )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) if (impl->items[i]) IMediaPlaybackItem_Release( impl->items[i] );
        free( impl->items );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_vector_GetIids( IVector_MediaPlaybackItem *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

/* .NET Native types a runtime object from this name when it builds the wrapper, so a stub here
 * leaves the object untyped and the managed cast to the collection interface fails with
 * "Unable to cast object of type 'EETypeRva:...'". It has to be the real parameterised name. */
static HRESULT WINAPI item_vector_GetRuntimeClassName( IVector_MediaPlaybackItem *iface, HSTRING *class_name )
{
    static const WCHAR *name =
        L"Windows.Foundation.Collections.IObservableVector`1<Windows.Media.Playback.MediaPlaybackItem>";

    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI item_vector_GetTrustLevel( IVector_MediaPlaybackItem *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_vector_GetAt( IVector_MediaPlaybackItem *iface, UINT32 index, IMediaPlaybackItem **value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    HRESULT hr = E_BOUNDS;

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;

    EnterCriticalSection( &impl->cs );
    if (index < impl->count)
    {
        IMediaPlaybackItem_AddRef( (*value = impl->items[index]) );
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI item_vector_get_Size( IVector_MediaPlaybackItem *iface, UINT32 *value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = impl->count;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI item_vector_GetView( IVector_MediaPlaybackItem *iface, IVectorView_MediaPlaybackItem **value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IVectorView_MediaPlaybackItem_AddRef( (*value = &impl->IVectorView_MediaPlaybackItem_iface) );
    return S_OK;
}

static HRESULT WINAPI item_vector_IndexOf( IVector_MediaPlaybackItem *iface, IMediaPlaybackItem *element,
                                           UINT32 *index, BOOLEAN *found )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = FALSE;

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++) if (impl->items[i] == element)
    {
        *index = i;
        *found = TRUE;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

/* callers hold impl->cs */
static HRESULT item_vector_reserve( struct item_vector *impl, UINT32 needed )
{
    IMediaPlaybackItem **items;
    UINT32 capacity;

    if (needed <= impl->capacity) return S_OK;
    capacity = max( needed, max( impl->capacity * 2, 4 ) );
    if (!(items = realloc( impl->items, capacity * sizeof(*items) ))) return E_OUTOFMEMORY;
    impl->items = items;
    impl->capacity = capacity;
    return S_OK;
}

static HRESULT WINAPI item_vector_SetAt( IVector_MediaPlaybackItem *iface, UINT32 index, IMediaPlaybackItem *value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    HRESULT hr = E_BOUNDS;

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    EnterCriticalSection( &impl->cs );
    if (index < impl->count)
    {
        if (value) IMediaPlaybackItem_AddRef( value );
        if (impl->items[index]) IMediaPlaybackItem_Release( impl->items[index] );
        impl->items[index] = value;
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI item_vector_InsertAt( IVector_MediaPlaybackItem *iface, UINT32 index, IMediaPlaybackItem *value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    HRESULT hr;

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    EnterCriticalSection( &impl->cs );
    if (index > impl->count) hr = E_BOUNDS;
    else if (SUCCEEDED(hr = item_vector_reserve( impl, impl->count + 1 )))
    {
        memmove( impl->items + index + 1, impl->items + index, (impl->count - index) * sizeof(*impl->items) );
        if (value) IMediaPlaybackItem_AddRef( value );
        impl->items[index] = value;
        impl->count++;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI item_vector_RemoveAt( IVector_MediaPlaybackItem *iface, UINT32 index )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    HRESULT hr = E_BOUNDS;

    TRACE( "iface %p, index %u.\n", iface, index );

    EnterCriticalSection( &impl->cs );
    if (index < impl->count)
    {
        if (impl->items[index]) IMediaPlaybackItem_Release( impl->items[index] );
        memmove( impl->items + index, impl->items + index + 1, (impl->count - index - 1) * sizeof(*impl->items) );
        impl->count--;
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI item_vector_Append( IVector_MediaPlaybackItem *iface, IMediaPlaybackItem *value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    HRESULT hr;

    ERR( "vector %p: appending item %p.\n", iface, value );

    EnterCriticalSection( &impl->cs );
    if (SUCCEEDED(hr = item_vector_reserve( impl, impl->count + 1 )))
    {
        if (value) IMediaPlaybackItem_AddRef( value );
        impl->items[impl->count++] = value;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI item_vector_RemoveAtEnd( IVector_MediaPlaybackItem *iface )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    HRESULT hr = E_BOUNDS;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    if (impl->count)
    {
        if (impl->items[impl->count - 1]) IMediaPlaybackItem_Release( impl->items[impl->count - 1] );
        impl->count--;
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI item_vector_Clear( IVector_MediaPlaybackItem *iface )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    UINT32 i;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++) if (impl->items[i]) IMediaPlaybackItem_Release( impl->items[i] );
    impl->count = 0;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI item_vector_GetMany( IVector_MediaPlaybackItem *iface, UINT32 start_index, UINT32 items_size,
                                           IMediaPlaybackItem **items, UINT32 *value )
{
    struct item_vector *impl = impl_from_IVector_MediaPlaybackItem( iface );
    UINT32 i;

    TRACE( "iface %p, start_index %u, items_size %u, items %p, value %p.\n", iface, start_index, items_size, items, value );

    if (!value) return E_POINTER;
    *value = 0;

    EnterCriticalSection( &impl->cs );
    if (start_index < impl->count)
    {
        for (i = 0; i < items_size && start_index + i < impl->count; i++)
            IMediaPlaybackItem_AddRef( (items[i] = impl->items[start_index + i]) );
        *value = i;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI item_vector_ReplaceAll( IVector_MediaPlaybackItem *iface, UINT32 count, IMediaPlaybackItem **items )
{
    HRESULT hr;
    UINT32 i;

    TRACE( "iface %p, count %u, items %p.\n", iface, count, items );

    if (FAILED(hr = item_vector_Clear( iface ))) return hr;
    for (i = 0; i < count; i++) if (FAILED(hr = item_vector_Append( iface, items[i] ))) return hr;
    return S_OK;
}

static const struct IVector_MediaPlaybackItemVtbl item_vector_vtbl =
{
    item_vector_QueryInterface,
    item_vector_AddRef,
    item_vector_Release,
    /* IInspectable methods */
    item_vector_GetIids,
    item_vector_GetRuntimeClassName,
    item_vector_GetTrustLevel,
    /* IVector<MediaPlaybackItem> methods */
    item_vector_GetAt,
    item_vector_get_Size,
    item_vector_GetView,
    item_vector_IndexOf,
    item_vector_SetAt,
    item_vector_InsertAt,
    item_vector_RemoveAt,
    item_vector_Append,
    item_vector_RemoveAtEnd,
    item_vector_Clear,
    item_vector_GetMany,
    item_vector_ReplaceAll,
};


/* IVectorView<MediaPlaybackItem> over the same storage. GetView and ShuffledItems both have to
 * hand back a real view - a caller that gets NULL from ShuffledItems has nothing to enumerate. */
DEFINE_IINSPECTABLE_( items_view, IVectorView_MediaPlaybackItem, struct item_vector,
                      impl_from_IVectorView_MediaPlaybackItem, IVectorView_MediaPlaybackItem_iface,
                      &impl->IVector_MediaPlaybackItem_iface )

static HRESULT WINAPI items_view_GetAt( IVectorView_MediaPlaybackItem *iface, UINT32 index,
                                        IMediaPlaybackItem **value )
{
    struct item_vector *impl = impl_from_IVectorView_MediaPlaybackItem( iface );
    return IVector_MediaPlaybackItem_GetAt( &impl->IVector_MediaPlaybackItem_iface, index, value );
}

static HRESULT WINAPI items_view_get_Size( IVectorView_MediaPlaybackItem *iface, UINT32 *value )
{
    struct item_vector *impl = impl_from_IVectorView_MediaPlaybackItem( iface );
    return IVector_MediaPlaybackItem_get_Size( &impl->IVector_MediaPlaybackItem_iface, value );
}

static HRESULT WINAPI items_view_IndexOf( IVectorView_MediaPlaybackItem *iface,
                                          IMediaPlaybackItem *element, UINT32 *index, BOOLEAN *found )
{
    struct item_vector *impl = impl_from_IVectorView_MediaPlaybackItem( iface );
    return IVector_MediaPlaybackItem_IndexOf( &impl->IVector_MediaPlaybackItem_iface, element, index, found );
}

static HRESULT WINAPI items_view_GetMany( IVectorView_MediaPlaybackItem *iface, UINT32 start_index,
                                          UINT32 items_size, IMediaPlaybackItem **items, UINT32 *value )
{
    struct item_vector *impl = impl_from_IVectorView_MediaPlaybackItem( iface );
    return IVector_MediaPlaybackItem_GetMany( &impl->IVector_MediaPlaybackItem_iface, start_index,
                                              items_size, items, value );
}

static const struct IVectorView_MediaPlaybackItemVtbl items_view_vtbl =
{
    items_view_QueryInterface,
    items_view_AddRef,
    items_view_Release,
    items_view_GetIids,
    items_view_GetRuntimeClassName,
    items_view_GetTrustLevel,
    items_view_GetAt,
    items_view_get_Size,
    items_view_IndexOf,
    items_view_GetMany,
};

DEFINE_IINSPECTABLE( observable_items, IObservableVector_MediaPlaybackItem, struct item_vector, IVector_MediaPlaybackItem_iface )

static HRESULT WINAPI observable_items_add_VectorChanged( IObservableVector_MediaPlaybackItem *iface,
                                                          IVectorChangedEventHandler_MediaPlaybackItem *handler,
                                                          EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p stub!\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI observable_items_remove_VectorChanged( IObservableVector_MediaPlaybackItem *iface,
                                                             EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static const struct IObservableVector_MediaPlaybackItemVtbl observable_items_vtbl =
{
    observable_items_QueryInterface,
    observable_items_AddRef,
    observable_items_Release,
    /* IInspectable methods */
    observable_items_GetIids,
    observable_items_GetRuntimeClassName,
    observable_items_GetTrustLevel,
    /* IObservableVector<MediaPlaybackItem> methods */
    observable_items_add_VectorChanged,
    observable_items_remove_VectorChanged,
};


/*
 * The iterator behind IIterable<MediaPlaybackItem>: managed foreach over Items goes through this,
 * and a vector that cannot be enumerated fail-fasts the app rather than returning an error.
 */

struct item_iterator
{
    IIterator_MediaPlaybackItem IIterator_MediaPlaybackItem_iface;
    LONG ref;

    IVector_MediaPlaybackItem *vector;
    UINT32 index;
};

static inline struct item_iterator *impl_from_IIterator_MediaPlaybackItem( IIterator_MediaPlaybackItem *iface )
{
    return CONTAINING_RECORD( iface, struct item_iterator, IIterator_MediaPlaybackItem_iface );
}

static HRESULT WINAPI item_iterator_QueryInterface( IIterator_MediaPlaybackItem *iface, REFIID iid, void **out )
{
    struct item_iterator *impl = impl_from_IIterator_MediaPlaybackItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_MediaPlaybackItem ))
    {
        *out = &impl->IIterator_MediaPlaybackItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI item_iterator_AddRef( IIterator_MediaPlaybackItem *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_MediaPlaybackItem( iface )->ref );
}

static ULONG WINAPI item_iterator_Release( IIterator_MediaPlaybackItem *iface )
{
    struct item_iterator *impl = impl_from_IIterator_MediaPlaybackItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVector_MediaPlaybackItem_Release( impl->vector );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_iterator_GetIids( IIterator_MediaPlaybackItem *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI item_iterator_GetRuntimeClassName( IIterator_MediaPlaybackItem *iface, HSTRING *class_name )
{
    static const WCHAR *name =
        L"Windows.Foundation.Collections.IIterator`1<Windows.Media.Playback.MediaPlaybackItem>";

    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI item_iterator_GetTrustLevel( IIterator_MediaPlaybackItem *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_iterator_get_Current( IIterator_MediaPlaybackItem *iface, IMediaPlaybackItem **value )
{
    struct item_iterator *impl = impl_from_IIterator_MediaPlaybackItem( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    return IVector_MediaPlaybackItem_GetAt( impl->vector, impl->index, value );
}

static HRESULT WINAPI item_iterator_get_HasCurrent( IIterator_MediaPlaybackItem *iface, boolean *value )
{
    struct item_iterator *impl = impl_from_IIterator_MediaPlaybackItem( iface );
    UINT32 size = 0;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IVector_MediaPlaybackItem_get_Size( impl->vector, &size );
    *value = impl->index < size;
    return S_OK;
}

static HRESULT WINAPI item_iterator_MoveNext( IIterator_MediaPlaybackItem *iface, boolean *value )
{
    struct item_iterator *impl = impl_from_IIterator_MediaPlaybackItem( iface );
    UINT32 size = 0;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IVector_MediaPlaybackItem_get_Size( impl->vector, &size );
    if (impl->index < size) impl->index++;
    *value = impl->index < size;
    return S_OK;
}

static HRESULT WINAPI item_iterator_GetMany( IIterator_MediaPlaybackItem *iface, UINT32 items_size,
                                             IMediaPlaybackItem **items, UINT32 *value )
{
    struct item_iterator *impl = impl_from_IIterator_MediaPlaybackItem( iface );
    HRESULT hr;

    TRACE( "iface %p, items_size %u, items %p, value %p.\n", iface, items_size, items, value );

    if (FAILED(hr = IVector_MediaPlaybackItem_GetMany( impl->vector, impl->index, items_size, items, value )))
        return hr;
    impl->index += *value;
    return S_OK;
}

static const struct IIterator_MediaPlaybackItemVtbl item_iterator_vtbl =
{
    item_iterator_QueryInterface,
    item_iterator_AddRef,
    item_iterator_Release,
    /* IInspectable methods */
    item_iterator_GetIids,
    item_iterator_GetRuntimeClassName,
    item_iterator_GetTrustLevel,
    /* IIterator<MediaPlaybackItem> methods */
    item_iterator_get_Current,
    item_iterator_get_HasCurrent,
    item_iterator_MoveNext,
    item_iterator_GetMany,
};

DEFINE_IINSPECTABLE( item_iterable, IIterable_MediaPlaybackItem, struct item_vector, IVector_MediaPlaybackItem_iface )

static HRESULT WINAPI item_iterable_First( IIterable_MediaPlaybackItem *iface, IIterator_MediaPlaybackItem **value )
{
    struct item_vector *impl = impl_from_IIterable_MediaPlaybackItem( iface );
    struct item_iterator *iter;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_MediaPlaybackItem_iface.lpVtbl = &item_iterator_vtbl;
    iter->ref = 1;
    IVector_MediaPlaybackItem_AddRef( (iter->vector = &impl->IVector_MediaPlaybackItem_iface) );

    *value = &iter->IIterator_MediaPlaybackItem_iface;
    return S_OK;
}

static const struct IIterable_MediaPlaybackItemVtbl item_iterable_vtbl =
{
    item_iterable_QueryInterface,
    item_iterable_AddRef,
    item_iterable_Release,
    /* IInspectable methods */
    item_iterable_GetIids,
    item_iterable_GetRuntimeClassName,
    item_iterable_GetTrustLevel,
    /* IIterable<MediaPlaybackItem> methods */
    item_iterable_First,
};

static HRESULT item_vector_create( struct item_vector **out )
{
    struct item_vector *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IObservableVector_MediaPlaybackItem_iface.lpVtbl = &observable_items_vtbl;
    impl->IVector_MediaPlaybackItem_iface.lpVtbl = &item_vector_vtbl;
    impl->IVectorView_MediaPlaybackItem_iface.lpVtbl = &items_view_vtbl;
    impl->IIterable_MediaPlaybackItem_iface.lpVtbl = &item_iterable_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": item_vector.cs");

    *out = impl;
    return S_OK;
}

/*
 * MediaPlaybackList
 */

struct playback_list
{
    IMediaPlaybackList IMediaPlaybackList_iface;
    IMediaPlaybackSource IMediaPlaybackSource_iface;
    IWineMediaPath IWineMediaPath_iface;
    IMediaPlaybackList2Internal IMediaPlaybackList2Internal_iface;
    LONG ref;

    IInspectable *max_prefetch_time;
    IInspectable *starting_item;
    struct item_vector *items;
    boolean auto_repeat;
    boolean shuffle;
    UINT32 current;
};

static inline struct playback_list *impl_from_IMediaPlaybackList( IMediaPlaybackList *iface )
{
    return CONTAINING_RECORD( iface, struct playback_list, IMediaPlaybackList_iface );
}

static HRESULT WINAPI playback_list_QueryInterface( IMediaPlaybackList *iface, REFIID iid, void **out )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaPlaybackList ))
    {
        *out = &impl->IMediaPlaybackList_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    /* A player handed a list needs the file of whatever is current; the list forwards the
     * question to that item. Without this the player has a source it cannot open. */
    if (IsEqualGUID( iid, &IID_IWineMediaPath ))
    {
        *out = &impl->IWineMediaPath_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlaybackSource ))
    {
        *out = &impl->IMediaPlaybackSource_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlaybackList2Internal ))
    {
        *out = &impl->IMediaPlaybackList2Internal_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI playback_list_AddRef( IMediaPlaybackList *iface )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI playback_list_Release( IMediaPlaybackList *iface )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        IVector_MediaPlaybackItem_Release( &impl->items->IVector_MediaPlaybackItem_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI playback_list_GetIids( IMediaPlaybackList *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_list_GetRuntimeClassName( IMediaPlaybackList *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Media_Playback_MediaPlaybackList,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Playback_MediaPlaybackList) - 1, class_name );
}

static HRESULT WINAPI playback_list_GetTrustLevel( IMediaPlaybackList *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* Nothing here ever changes the list on its own, so the three events never fire. */
static HRESULT WINAPI playback_list_add_ItemFailed( IMediaPlaybackList *iface,
        ITypedEventHandler_MediaPlaybackList_MediaPlaybackItemFailedEventArgs *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p stub!\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_list_remove_ItemFailed( IMediaPlaybackList *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_list_add_CurrentItemChanged( IMediaPlaybackList *iface,
        ITypedEventHandler_MediaPlaybackList_CurrentMediaPlaybackItemChangedEventArgs *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p stub!\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_list_remove_CurrentItemChanged( IMediaPlaybackList *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_list_add_ItemOpened( IMediaPlaybackList *iface,
        ITypedEventHandler_MediaPlaybackList_MediaPlaybackItemOpenedEventArgs *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p stub!\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_list_remove_ItemOpened( IMediaPlaybackList *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}


/* MediaPlaybackList revision 2. Nothing here prefetches or shuffles, but every getter has to
 * answer with something the app can use: ShuffledItems in particular is enumerated. */
DEFINE_IINSPECTABLE_( playback_list2, IMediaPlaybackList2Internal, struct playback_list,
                      impl_from_IMediaPlaybackList2Internal, IMediaPlaybackList2Internal_iface,
                      &impl->IMediaPlaybackList_iface )

static HRESULT WINAPI playback_list2_get_MaxPrefetchTime( IMediaPlaybackList2Internal *iface,
                                                          IInspectable **value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList2Internal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    /* IReference<TimeSpan> is nullable and NULL means "not set", which is the honest answer for
     * a player that does not prefetch. */
    if (!value) return E_POINTER;
    if ((*value = impl->max_prefetch_time)) IInspectable_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI playback_list2_put_MaxPrefetchTime( IMediaPlaybackList2Internal *iface,
                                                          IInspectable *value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList2Internal( iface );

    TRACE( "iface %p, value %p: nothing is prefetched, but the value is kept.\n", iface, value );

    if (value) IInspectable_AddRef( value );
    if (impl->max_prefetch_time) IInspectable_Release( impl->max_prefetch_time );
    impl->max_prefetch_time = value;
    return S_OK;
}

static HRESULT WINAPI playback_list2_get_StartingItem( IMediaPlaybackList2Internal *iface,
                                                       IInspectable **value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList2Internal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->starting_item)) IInspectable_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI playback_list2_put_StartingItem( IMediaPlaybackList2Internal *iface,
                                                       IInspectable *value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList2Internal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (value) IInspectable_AddRef( value );
    if (impl->starting_item) IInspectable_Release( impl->starting_item );
    impl->starting_item = value;
    return S_OK;
}

/* Shuffle is off, so the shuffled order is the real order. */
static HRESULT WINAPI playback_list2_get_ShuffledItems( IMediaPlaybackList2Internal *iface,
                                                        IInspectable **value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList2Internal( iface );
    IVectorView_MediaPlaybackItem *view;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = IVector_MediaPlaybackItem_GetView( &impl->items->IVector_MediaPlaybackItem_iface,
                                                       &view )))
        return hr;
    *value = (IInspectable *)view;
    return S_OK;
}

static HRESULT WINAPI playback_list2_SetShuffledItems( IMediaPlaybackList2Internal *iface,
                                                       IInspectable *value )
{
    FIXME( "iface %p, value %p: shuffle is not implemented, the list order is kept.\n", iface, value );
    return S_OK;
}

static const struct IMediaPlaybackList2InternalVtbl playback_list2_vtbl =
{
    playback_list2_QueryInterface,
    playback_list2_AddRef,
    playback_list2_Release,
    playback_list2_GetIids,
    playback_list2_GetRuntimeClassName,
    playback_list2_GetTrustLevel,
    playback_list2_get_MaxPrefetchTime,
    playback_list2_put_MaxPrefetchTime,
    playback_list2_get_StartingItem,
    playback_list2_put_StartingItem,
    playback_list2_get_ShuffledItems,
    playback_list2_SetShuffledItems,
};

static HRESULT WINAPI playback_list_get_Items( IMediaPlaybackList *iface, IObservableVector_MediaPlaybackItem **value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IObservableVector_MediaPlaybackItem_AddRef( (*value = &impl->items->IObservableVector_MediaPlaybackItem_iface) );
    return S_OK;
}

static HRESULT WINAPI playback_list_get_AutoRepeatEnabled( IMediaPlaybackList *iface, boolean *value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->auto_repeat;
    return S_OK;
}

static HRESULT WINAPI playback_list_put_AutoRepeatEnabled( IMediaPlaybackList *iface, boolean value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %u.\n", iface, value );

    impl->auto_repeat = value;
    return S_OK;
}

static HRESULT WINAPI playback_list_get_ShuffleEnabled( IMediaPlaybackList *iface, boolean *value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->shuffle;
    return S_OK;
}

static HRESULT WINAPI playback_list_put_ShuffleEnabled( IMediaPlaybackList *iface, boolean value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %u.\n", iface, value );

    impl->shuffle = value;
    return S_OK;
}

static HRESULT WINAPI playback_list_get_CurrentItem( IMediaPlaybackList *iface, IMediaPlaybackItem **value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (impl->current == ~0u) return S_OK;
    return IVector_MediaPlaybackItem_GetAt( &impl->items->IVector_MediaPlaybackItem_iface, impl->current, value );
}

static HRESULT WINAPI playback_list_get_CurrentItemIndex( IMediaPlaybackList *iface, UINT32 *value )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->current;
    return S_OK;
}

static HRESULT playback_list_move_to( struct playback_list *impl, UINT32 index, IMediaPlaybackItem **item )
{
    HRESULT hr;

    if (!item) return E_POINTER;
    *item = NULL;
    if (FAILED(hr = IVector_MediaPlaybackItem_GetAt( &impl->items->IVector_MediaPlaybackItem_iface, index, item )))
        return hr;
    impl->current = index;
    return S_OK;
}

static HRESULT WINAPI playback_list_MoveNext( IMediaPlaybackList *iface, IMediaPlaybackItem **item )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );
    UINT32 size = 0, next;

    TRACE( "iface %p, item %p.\n", iface, item );

    IVector_MediaPlaybackItem_get_Size( &impl->items->IVector_MediaPlaybackItem_iface, &size );
    if (!size) return E_BOUNDS;
    next = impl->current == ~0u ? 0 : impl->current + 1;
    if (next >= size)
    {
        if (!impl->auto_repeat) return E_BOUNDS;
        next = 0;
    }
    return playback_list_move_to( impl, next, item );
}

static HRESULT WINAPI playback_list_MovePrevious( IMediaPlaybackList *iface, IMediaPlaybackItem **item )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );
    UINT32 size = 0, prev;

    TRACE( "iface %p, item %p.\n", iface, item );

    IVector_MediaPlaybackItem_get_Size( &impl->items->IVector_MediaPlaybackItem_iface, &size );
    if (!size) return E_BOUNDS;
    if (impl->current == ~0u || !impl->current)
    {
        if (!impl->auto_repeat) return E_BOUNDS;
        prev = size - 1;
    }
    else prev = impl->current - 1;
    return playback_list_move_to( impl, prev, item );
}

static HRESULT WINAPI playback_list_MoveTo( IMediaPlaybackList *iface, UINT32 item_index, IMediaPlaybackItem **item )
{
    struct playback_list *impl = impl_from_IMediaPlaybackList( iface );

    TRACE( "iface %p, item_index %u, item %p.\n", iface, item_index, item );

    return playback_list_move_to( impl, item_index, item );
}

DEFINE_IINSPECTABLE_( playback_list_path, IWineMediaPath, struct playback_list,
                      impl_from_IWineMediaPath_list, IWineMediaPath_iface,
                      &impl->IMediaPlaybackList_iface )

static HRESULT WINAPI playback_list_path_get_Path( IWineMediaPath *iface, HSTRING *value )
{
    struct playback_list *impl = impl_from_IWineMediaPath_list( iface );
    IMediaPlaybackItemInternal *item = NULL;
    IWineMediaPath *item_path;
    UINT32 index;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    /* The current item is what plays; a list that has not started yet plays its first. */
    index = impl->current;
    if (FAILED(hr = IVector_MediaPlaybackItem_GetAt( &impl->items->IVector_MediaPlaybackItem_iface,
                                                     index, (IMediaPlaybackItem **)&item )))
    {
        ERR( "list %p has %u items, current %u, hr %#lx.\n", impl, impl->items->count, index, hr );
        return hr;
    }

    hr = IMediaPlaybackItemInternal_QueryInterface( item, &IID_IWineMediaPath, (void **)&item_path );
    if (SUCCEEDED(hr))
    {
        hr = IWineMediaPath_get_Path( item_path, value );
        IWineMediaPath_Release( item_path );
    }
    IMediaPlaybackItemInternal_Release( item );
    return hr;
}

static const struct IWineMediaPathVtbl playback_list_path_vtbl =
{
    playback_list_path_QueryInterface,
    playback_list_path_AddRef,
    playback_list_path_Release,
    playback_list_path_GetIids,
    playback_list_path_GetRuntimeClassName,
    playback_list_path_GetTrustLevel,
    playback_list_path_get_Path,
};

static const struct IMediaPlaybackListVtbl playback_list_vtbl =
{
    playback_list_QueryInterface,
    playback_list_AddRef,
    playback_list_Release,
    /* IInspectable methods */
    playback_list_GetIids,
    playback_list_GetRuntimeClassName,
    playback_list_GetTrustLevel,
    /* IMediaPlaybackList methods */
    playback_list_add_ItemFailed,
    playback_list_remove_ItemFailed,
    playback_list_add_CurrentItemChanged,
    playback_list_remove_CurrentItemChanged,
    playback_list_add_ItemOpened,
    playback_list_remove_ItemOpened,
    playback_list_get_Items,
    playback_list_get_AutoRepeatEnabled,
    playback_list_put_AutoRepeatEnabled,
    playback_list_get_ShuffleEnabled,
    playback_list_put_ShuffleEnabled,
    playback_list_get_CurrentItem,
    playback_list_get_CurrentItemIndex,
    playback_list_MoveNext,
    playback_list_MovePrevious,
    playback_list_MoveTo,
};

DEFINE_IINSPECTABLE( playback_source, IMediaPlaybackSource, struct playback_list, IMediaPlaybackList_iface )

static const struct IMediaPlaybackSourceVtbl playback_source_vtbl =
{
    playback_source_QueryInterface,
    playback_source_AddRef,
    playback_source_Release,
    /* IInspectable methods */
    playback_source_GetIids,
    playback_source_GetRuntimeClassName,
    playback_source_GetTrustLevel,
};

/*
 * MediaPlaybackList activation factory
 */

struct playback_list_statics
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct playback_list_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct playback_list_statics, IActivationFactory_iface );
}

static HRESULT WINAPI playback_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct playback_list_statics *impl = impl_from_IActivationFactory( iface );

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

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI playback_factory_AddRef( IActivationFactory *iface )
{
    struct playback_list_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI playback_factory_Release( IActivationFactory *iface )
{
    struct playback_list_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI playback_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct playback_list *impl;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    *instance = NULL;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = item_vector_create( &impl->items )))
    {
        free( impl );
        return hr;
    }
    impl->IMediaPlaybackList_iface.lpVtbl = &playback_list_vtbl;
    impl->IMediaPlaybackList2Internal_iface.lpVtbl = &playback_list2_vtbl;
    impl->IMediaPlaybackSource_iface.lpVtbl = &playback_source_vtbl;
    impl->IWineMediaPath_iface.lpVtbl = &playback_list_path_vtbl;
    impl->ref = 1;
    impl->current = ~0u;

    *instance = (IInspectable *)&impl->IMediaPlaybackList_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl playback_factory_vtbl =
{
    playback_factory_QueryInterface,
    playback_factory_AddRef,
    playback_factory_Release,
    /* IInspectable methods */
    playback_factory_GetIids,
    playback_factory_GetRuntimeClassName,
    playback_factory_GetTrustLevel,
    /* IActivationFactory methods */
    playback_factory_ActivateInstance,
};

static struct playback_list_statics playback_list_statics =
{
    {&playback_factory_vtbl},
    0,
};

IActivationFactory *playback_list_factory = &playback_list_statics.IActivationFactory_iface;

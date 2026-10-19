/* WinRT Windows.Media.Core.TimedTextSource, and the vector MediaSource.ExternalTimedTextSources
 * hands out to hold them.
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

WINE_DEFAULT_DEBUG_CHANNEL(media);

static const WCHAR RuntimeClass_Windows_Media_Core_TimedTextSource[] = L"Windows.Media.Core.TimedTextSource";

/* Derived with piid.py from rc(Windows.Media.Core.TimedTextSource;{c4ed9ba6-...}). */
static const GUID IID_IVector_TimedTextSource = {0xd0a72378,0x5432,0x50bf,{0x93,0xc3,0xc5,0xf3,0x5f,0xc9,0x35,0x17}};
static const GUID IID_IObservableVector_TimedTextSource = {0x95e6c852,0x6e9c,0x50a9,{0x89,0x35,0x05,0x43,0x20,0x24,0x29,0x45}};
static const GUID IID_IIterable_TimedTextSource = {0x4fc57282,0x0edf,0x5d85,{0x9d,0x89,0xdd,0xc2,0xa5,0x69,0x2c,0x13}};
static const GUID IID_IIterator_TimedTextSource = {0xbdded7c0,0x5c73,0x57f5,{0x9b,0x08,0x65,0x29,0x33,0x58,0x6d,0xd5}};

/*
 * A vector of runtime-class objects, answering for IVector<T>, IObservableVector<T> and
 * IIterable<T> under the IIDs derived for one T. Elements are held as IInspectable *.
 */
struct vector_iids
{
    const GUID *vector;
    const GUID *observable;
    const GUID *iterable;
    const GUID *iterator;
    const WCHAR *class_name;
};

struct inspectable_vector
{
    IWineInspectableVector IWineInspectableVector_iface;
    IWineInspectableObservable IWineInspectableObservable_iface;
    IWineInspectableIterable IWineInspectableIterable_iface;
    LONG ref;
    const struct vector_iids *iids;
    CRITICAL_SECTION cs;
    IInspectable **elements;
    UINT32 size, capacity;
    /* VectorChanged handlers are registered and never raised: nothing reads the change events
     * of a list only the app writes to. */
    EventRegistrationToken next_token;
};

static inline struct inspectable_vector *impl_from_IWineInspectableVector( IWineInspectableVector *iface )
{
    return CONTAINING_RECORD( iface, struct inspectable_vector, IWineInspectableVector_iface );
}

static HRESULT WINAPI vector_QueryInterface( IWineInspectableVector *iface, REFIID iid, void **out )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, impl->iids->vector ))
    {
        IInspectable_AddRef( (*out = &impl->IWineInspectableVector_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, impl->iids->observable ))
    {
        IInspectable_AddRef( (*out = &impl->IWineInspectableObservable_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, impl->iids->iterable ))
    {
        IInspectable_AddRef( (*out = &impl->IWineInspectableIterable_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI vector_AddRef( IWineInspectableVector *iface )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI vector_Release( IWineInspectableVector *iface )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        for (i = 0; i < impl->size; i++) IInspectable_Release( impl->elements[i] );
        free( impl->elements );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI vector_GetIids( IWineInspectableVector *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI vector_GetRuntimeClassName( IWineInspectableVector *iface, HSTRING *class_name )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    return WindowsCreateString( impl->iids->class_name, wcslen( impl->iids->class_name ), class_name );
}

static HRESULT WINAPI vector_GetTrustLevel( IWineInspectableVector *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI vector_GetAt( IWineInspectableVector *iface, UINT32 index, IInspectable **value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    if (index >= impl->size) { *value = NULL; hr = E_BOUNDS; }
    else IInspectable_AddRef( (*value = impl->elements[index]) );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vector_get_Size( IWineInspectableVector *iface, UINT32 *value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->size;
    return S_OK;
}

static HRESULT WINAPI vector_GetView( IWineInspectableVector *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI vector_IndexOf( IWineInspectableVector *iface, IInspectable *element, UINT32 *index, boolean *found )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = FALSE;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->size; i++)
    {
        if (impl->elements[i] != element) continue;
        *index = i;
        *found = TRUE;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static BOOL vector_reserve( struct inspectable_vector *impl, UINT32 count )
{
    IInspectable **elements;
    UINT32 capacity;

    if (count <= impl->capacity) return TRUE;
    capacity = max( count, max( 4, impl->capacity * 2 ) );
    if (!(elements = realloc( impl->elements, capacity * sizeof(*elements) ))) return FALSE;
    impl->elements = elements;
    impl->capacity = capacity;
    return TRUE;
}

static HRESULT WINAPI vector_SetAt( IWineInspectableVector *iface, UINT32 index, IInspectable *value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    EnterCriticalSection( &impl->cs );
    if (index >= impl->size) hr = E_BOUNDS;
    else
    {
        IInspectable_AddRef( value );
        IInspectable_Release( impl->elements[index] );
        impl->elements[index] = value;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vector_InsertAt( IWineInspectableVector *iface, UINT32 index, IInspectable *value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    EnterCriticalSection( &impl->cs );
    if (index > impl->size) hr = E_BOUNDS;
    else if (!vector_reserve( impl, impl->size + 1 )) hr = E_OUTOFMEMORY;
    else
    {
        memmove( impl->elements + index + 1, impl->elements + index, (impl->size - index) * sizeof(*impl->elements) );
        IInspectable_AddRef( (impl->elements[index] = value) );
        impl->size++;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vector_RemoveAt( IWineInspectableVector *iface, UINT32 index )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    IInspectable *removed = NULL;
    HRESULT hr = S_OK;

    TRACE( "iface %p, index %u.\n", iface, index );

    EnterCriticalSection( &impl->cs );
    if (index >= impl->size) hr = E_BOUNDS;
    else
    {
        removed = impl->elements[index];
        impl->size--;
        memmove( impl->elements + index, impl->elements + index + 1, (impl->size - index) * sizeof(*impl->elements) );
    }
    LeaveCriticalSection( &impl->cs );
    if (removed) IInspectable_Release( removed );
    return hr;
}

static HRESULT WINAPI vector_Append( IWineInspectableVector *iface, IInspectable *value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return vector_InsertAt( iface, impl->size, value );
}

static HRESULT WINAPI vector_RemoveAtEnd( IWineInspectableVector *iface )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    TRACE( "iface %p.\n", iface );
    if (!impl->size) return E_BOUNDS;
    return vector_RemoveAt( iface, impl->size - 1 );
}

static HRESULT WINAPI vector_Clear( IWineInspectableVector *iface )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    IInspectable **elements;
    UINT32 i, size;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    elements = impl->elements;
    size = impl->size;
    impl->elements = NULL;
    impl->size = impl->capacity = 0;
    LeaveCriticalSection( &impl->cs );

    for (i = 0; i < size; i++) IInspectable_Release( elements[i] );
    free( elements );
    return S_OK;
}

static HRESULT WINAPI vector_GetMany( IWineInspectableVector *iface, UINT32 start, UINT32 items_size,
                                      IInspectable **items, UINT32 *value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( iface );
    UINT32 i, count = 0;

    TRACE( "iface %p, start %u, items_size %u, items %p, value %p.\n", iface, start, items_size, items, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    for (i = start; i < impl->size && count < items_size; i++, count++)
        IInspectable_AddRef( (items[count] = impl->elements[i]) );
    LeaveCriticalSection( &impl->cs );
    *value = count;
    return S_OK;
}

static HRESULT WINAPI vector_ReplaceAll( IWineInspectableVector *iface, UINT32 count, IInspectable **items )
{
    HRESULT hr;
    UINT32 i;

    TRACE( "iface %p, count %u, items %p.\n", iface, count, items );

    vector_Clear( iface );
    for (i = 0; i < count; i++)
        if (FAILED(hr = vector_Append( iface, items[i] ))) return hr;
    return S_OK;
}

static const struct IWineInspectableVectorVtbl vector_vtbl =
{
    vector_QueryInterface,
    vector_AddRef,
    vector_Release,
    /* IInspectable methods */
    vector_GetIids,
    vector_GetRuntimeClassName,
    vector_GetTrustLevel,
    /* IVector<T> methods */
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

DEFINE_IINSPECTABLE_( vector_observable, IWineInspectableObservable, struct inspectable_vector,
                      impl_from_IWineInspectableObservable, IWineInspectableObservable_iface,
                      &impl->IWineInspectableVector_iface )

static HRESULT WINAPI vector_observable_add_VectorChanged( IWineInspectableObservable *iface, IInspectable *handler,
                                                           EventRegistrationToken *token )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableObservable( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    if (!token) return E_POINTER;
    token->value = InterlockedIncrement64( &impl->next_token.value );
    return S_OK;
}

static HRESULT WINAPI vector_observable_remove_VectorChanged( IWineInspectableObservable *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct IWineInspectableObservableVtbl vector_observable_vtbl =
{
    vector_observable_QueryInterface,
    vector_observable_AddRef,
    vector_observable_Release,
    /* IInspectable methods */
    vector_observable_GetIids,
    vector_observable_GetRuntimeClassName,
    vector_observable_GetTrustLevel,
    /* IObservableVector<T> methods */
    vector_observable_add_VectorChanged,
    vector_observable_remove_VectorChanged,
};

/* IIterator<T> over a snapshot of the vector taken when First() was called. */
struct vector_iterator
{
    IWineInspectableIterator IWineInspectableIterator_iface;
    LONG ref;
    const GUID *iid;
    IInspectable **elements;
    UINT32 size, index;
};

static inline struct vector_iterator *impl_from_IWineInspectableIterator( IWineInspectableIterator *iface )
{
    return CONTAINING_RECORD( iface, struct vector_iterator, IWineInspectableIterator_iface );
}

static HRESULT WINAPI iterator_QueryInterface( IWineInspectableIterator *iface, REFIID iid, void **out )
{
    struct vector_iterator *impl = impl_from_IWineInspectableIterator( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, impl->iid ))
    {
        IInspectable_AddRef( (*out = &impl->IWineInspectableIterator_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef( IWineInspectableIterator *iface )
{
    return InterlockedIncrement( &impl_from_IWineInspectableIterator( iface )->ref );
}

static ULONG WINAPI iterator_Release( IWineInspectableIterator *iface )
{
    struct vector_iterator *impl = impl_from_IWineInspectableIterator( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->size; i++) IInspectable_Release( impl->elements[i] );
        free( impl->elements );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iterator_GetIids( IWineInspectableIterator *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI iterator_GetRuntimeClassName( IWineInspectableIterator *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI iterator_GetTrustLevel( IWineInspectableIterator *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iterator_get_Current( IWineInspectableIterator *iface, IInspectable **value )
{
    struct vector_iterator *impl = impl_from_IWineInspectableIterator( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->index >= impl->size) { *value = NULL; return E_BOUNDS; }
    IInspectable_AddRef( (*value = impl->elements[impl->index]) );
    return S_OK;
}

static HRESULT WINAPI iterator_get_HasCurrent( IWineInspectableIterator *iface, boolean *value )
{
    struct vector_iterator *impl = impl_from_IWineInspectableIterator( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->index < impl->size;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IWineInspectableIterator *iface, boolean *value )
{
    struct vector_iterator *impl = impl_from_IWineInspectableIterator( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->index < impl->size) impl->index++;
    *value = impl->index < impl->size;
    return S_OK;
}

static HRESULT WINAPI iterator_GetMany( IWineInspectableIterator *iface, UINT32 items_size, IInspectable **items, UINT32 *value )
{
    struct vector_iterator *impl = impl_from_IWineInspectableIterator( iface );
    UINT32 count = 0;

    TRACE( "iface %p, items_size %u, items %p, value %p.\n", iface, items_size, items, value );

    if (!value) return E_POINTER;
    while (impl->index < impl->size && count < items_size)
        IInspectable_AddRef( (items[count++] = impl->elements[impl->index++]) );
    *value = count;
    return S_OK;
}

static const struct IWineInspectableIteratorVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    /* IInspectable methods */
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    /* IIterator<T> methods */
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

DEFINE_IINSPECTABLE_( vector_iterable, IWineInspectableIterable, struct inspectable_vector,
                      impl_from_IWineInspectableIterable, IWineInspectableIterable_iface,
                      &impl->IWineInspectableVector_iface )

static HRESULT WINAPI vector_iterable_First( IWineInspectableIterable *iface, IWineInspectableIterator **value )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableIterable( iface );
    struct vector_iterator *iter;
    UINT32 i;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IWineInspectableIterator_iface.lpVtbl = &iterator_vtbl;
    iter->ref = 1;
    iter->iid = impl->iids->iterator;

    EnterCriticalSection( &impl->cs );
    if (impl->size && !(iter->elements = malloc( impl->size * sizeof(*iter->elements) )))
    {
        LeaveCriticalSection( &impl->cs );
        free( iter );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < impl->size; i++) IInspectable_AddRef( (iter->elements[i] = impl->elements[i]) );
    iter->size = impl->size;
    LeaveCriticalSection( &impl->cs );

    *value = &iter->IWineInspectableIterator_iface;
    return S_OK;
}

static const struct IWineInspectableIterableVtbl vector_iterable_vtbl =
{
    vector_iterable_QueryInterface,
    vector_iterable_AddRef,
    vector_iterable_Release,
    /* IInspectable methods */
    vector_iterable_GetIids,
    vector_iterable_GetRuntimeClassName,
    vector_iterable_GetTrustLevel,
    /* IIterable<T> methods */
    vector_iterable_First,
};

static HRESULT inspectable_vector_create( const struct vector_iids *iids, IInspectable **out )
{
    struct inspectable_vector *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWineInspectableVector_iface.lpVtbl = &vector_vtbl;
    impl->IWineInspectableObservable_iface.lpVtbl = &vector_observable_vtbl;
    impl->IWineInspectableIterable_iface.lpVtbl = &vector_iterable_vtbl;
    impl->ref = 1;
    impl->iids = iids;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": inspectable_vector.cs");

    *out = (IInspectable *)&impl->IWineInspectableVector_iface;
    return S_OK;
}

static const struct vector_iids timed_text_source_vector_iids =
{
    &IID_IVector_TimedTextSource,
    &IID_IObservableVector_TimedTextSource,
    &IID_IIterable_TimedTextSource,
    &IID_IIterator_TimedTextSource,
    L"Windows.Foundation.Collections.IObservableVector`1<Windows.Media.Core.TimedTextSource>",
};

/* MediaSource.ExternalTimedTextSources: the app appends the sources it built and nothing else
 * reads them back yet. */
HRESULT timed_text_source_vector_create( IInspectable **out )
{
    return inspectable_vector_create( &timed_text_source_vector_iids, out );
}

/*
 * TimedTextSource
 */
struct timed_text_source
{
    ITimedTextSourceInternal ITimedTextSourceInternal_iface;
    LONG ref;
    HSTRING uri;            /* set when made from a Uri */
    IInspectable *stream;   /* set when made from an IRandomAccessStream */
    HSTRING language;

    /* Resolved fires once, when the player opens the media this source is attached to. Apps
     * wait for it: Shapr3D's MediaOpened handler blocks on a CountdownEvent that only its
     * Resolved handlers signal, so a Resolved that never fires hangs the player thread. */
    CRITICAL_SECTION cs;
    struct
    {
        IWineEventHandler *handler;
        EventRegistrationToken token;
    } handlers[8];
    UINT32 handler_count;
    EventRegistrationToken next_token;
    BOOL resolved;
};

static inline struct timed_text_source *impl_from_ITimedTextSourceInternal( ITimedTextSourceInternal *iface )
{
    return CONTAINING_RECORD( iface, struct timed_text_source, ITimedTextSourceInternal_iface );
}

static HRESULT WINAPI timed_text_source_QueryInterface( ITimedTextSourceInternal *iface, REFIID iid, void **out )
{
    struct timed_text_source *impl = impl_from_ITimedTextSourceInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ITimedTextSourceInternal ))
    {
        IInspectable_AddRef( (*out = &impl->ITimedTextSourceInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI timed_text_source_AddRef( ITimedTextSourceInternal *iface )
{
    struct timed_text_source *impl = impl_from_ITimedTextSourceInternal( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI timed_text_source_Release( ITimedTextSourceInternal *iface )
{
    struct timed_text_source *impl = impl_from_ITimedTextSourceInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        UINT32 i;

        for (i = 0; i < impl->handler_count; i++)
            impl->handlers[i].handler->lpVtbl->Release( impl->handlers[i].handler );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        WindowsDeleteString( impl->uri );
        WindowsDeleteString( impl->language );
        if (impl->stream) IInspectable_Release( impl->stream );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI timed_text_source_GetIids( ITimedTextSourceInternal *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI timed_text_source_GetRuntimeClassName( ITimedTextSourceInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_TimedTextSource,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_TimedTextSource) - 1, class_name );
}

static HRESULT WINAPI timed_text_source_GetTrustLevel( ITimedTextSourceInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI timed_text_source_add_Resolved( ITimedTextSourceInternal *iface, IInspectable *handler,
                                                      EventRegistrationToken *token )
{
    struct timed_text_source *impl = impl_from_ITimedTextSourceInternal( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    if (!handler) return E_INVALIDARG;
    if (!token) return E_POINTER;

    EnterCriticalSection( &impl->cs );
    if (impl->handler_count == ARRAY_SIZE(impl->handlers))
    {
        LeaveCriticalSection( &impl->cs );
        FIXME( "too many Resolved handlers.\n" );
        return E_OUTOFMEMORY;
    }
    token->value = ++impl->next_token.value;
    impl->handlers[impl->handler_count].handler = (IWineEventHandler *)handler;
    impl->handlers[impl->handler_count].token = *token;
    impl->handler_count++;
    IInspectable_AddRef( handler );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI timed_text_source_remove_Resolved( ITimedTextSourceInternal *iface, EventRegistrationToken token )
{
    struct timed_text_source *impl = impl_from_ITimedTextSourceInternal( iface );
    IWineEventHandler *handler = NULL;
    UINT32 i;

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->handler_count; i++)
    {
        if (impl->handlers[i].token.value != token.value) continue;
        handler = impl->handlers[i].handler;
        impl->handlers[i] = impl->handlers[--impl->handler_count];
        break;
    }
    LeaveCriticalSection( &impl->cs );

    if (handler) handler->lpVtbl->Release( handler );
    return S_OK;
}

static const struct ITimedTextSourceInternalVtbl timed_text_source_vtbl =
{
    timed_text_source_QueryInterface,
    timed_text_source_AddRef,
    timed_text_source_Release,
    /* IInspectable methods */
    timed_text_source_GetIids,
    timed_text_source_GetRuntimeClassName,
    timed_text_source_GetTrustLevel,
    /* ITimedTextSource methods */
    timed_text_source_add_Resolved,
    timed_text_source_remove_Resolved,
};

static HRESULT timed_text_source_create( IInspectable *uri, IInspectable *stream, HSTRING language,
                                         ITimedTextSourceInternal **value )
{
    struct timed_text_source *impl;
    IUriRuntimeClass *uri_class;
    HRESULT hr = S_OK;

    if (!value) return E_POINTER;
    *value = NULL;
    if (!uri && !stream) return E_INVALIDARG;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ITimedTextSourceInternal_iface.lpVtbl = &timed_text_source_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": timed_text_source.cs");

    if (uri && SUCCEEDED(hr = IInspectable_QueryInterface( uri, &IID_IUriRuntimeClass, (void **)&uri_class )))
    {
        hr = IUriRuntimeClass_get_RawUri( uri_class, &impl->uri );
        IUriRuntimeClass_Release( uri_class );
    }
    if (uri && FAILED(hr))
    {
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
        return hr;
    }
    if (stream) IInspectable_AddRef( (impl->stream = stream) );
    WindowsDuplicateString( language, &impl->language );

    TRACE( "created TimedTextSource %p for %s, stream %p, language %s.\n", impl,
           debugstr_hstring( impl->uri ), impl->stream, debugstr_hstring( impl->language ) );
    *value = &impl->ITimedTextSourceInternal_iface;
    return S_OK;
}

/*
 * TimedTextSourceResolveResultEventArgs
 */
struct resolve_args
{
    ITimedTextSourceResolveResultEventArgsInternal ITimedTextSourceResolveResultEventArgsInternal_iface;
    LONG ref;
    IInspectable *tracks;   /* IVectorView<TimedMetadataTrack> */
};

static inline struct resolve_args *impl_from_ITimedTextSourceResolveResultEventArgsInternal(
        ITimedTextSourceResolveResultEventArgsInternal *iface )
{
    return CONTAINING_RECORD( iface, struct resolve_args, ITimedTextSourceResolveResultEventArgsInternal_iface );
}

static HRESULT WINAPI resolve_args_QueryInterface( ITimedTextSourceResolveResultEventArgsInternal *iface,
                                                   REFIID iid, void **out )
{
    struct resolve_args *impl = impl_from_ITimedTextSourceResolveResultEventArgsInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ITimedTextSourceResolveResultEventArgsInternal ))
    {
        IInspectable_AddRef( (*out = &impl->ITimedTextSourceResolveResultEventArgsInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI resolve_args_AddRef( ITimedTextSourceResolveResultEventArgsInternal *iface )
{
    return InterlockedIncrement( &impl_from_ITimedTextSourceResolveResultEventArgsInternal( iface )->ref );
}

static ULONG WINAPI resolve_args_Release( ITimedTextSourceResolveResultEventArgsInternal *iface )
{
    struct resolve_args *impl = impl_from_ITimedTextSourceResolveResultEventArgsInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->tracks) IInspectable_Release( impl->tracks );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI resolve_args_GetIids( ITimedTextSourceResolveResultEventArgsInternal *iface,
                                            ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI resolve_args_GetRuntimeClassName( ITimedTextSourceResolveResultEventArgsInternal *iface,
                                                        HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Media.Core.TimedTextSourceResolveResultEventArgs";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI resolve_args_GetTrustLevel( ITimedTextSourceResolveResultEventArgsInternal *iface,
                                                  TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI resolve_args_get_Error( ITimedTextSourceResolveResultEventArgsInternal *iface,
                                              IInspectable **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI resolve_args_get_Tracks( ITimedTextSourceResolveResultEventArgsInternal *iface,
                                               IInspectable **value )
{
    struct resolve_args *impl = impl_from_ITimedTextSourceResolveResultEventArgsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IInspectable_AddRef( (*value = impl->tracks) );
    return S_OK;
}

static const struct ITimedTextSourceResolveResultEventArgsInternalVtbl resolve_args_vtbl =
{
    resolve_args_QueryInterface,
    resolve_args_AddRef,
    resolve_args_Release,
    /* IInspectable methods */
    resolve_args_GetIids,
    resolve_args_GetRuntimeClassName,
    resolve_args_GetTrustLevel,
    /* ITimedTextSourceResolveResultEventArgs methods */
    resolve_args_get_Error,
    resolve_args_get_Tracks,
};

/* Raise Resolved, once. The handlers run on the caller's thread - the player's open thread, as
 * Windows raises it from its pipeline - and outside the lock, so a handler may unregister. Cues
 * are not parsed yet, so the track is empty; that is still a successful resolve. */
static void timed_text_source_resolve( struct timed_text_source *impl )
{
    IWineEventHandler *handlers[ARRAY_SIZE(impl->handlers)];
    struct resolve_args *args;
    UINT32 i, count;

    EnterCriticalSection( &impl->cs );
    if (impl->resolved)
    {
        LeaveCriticalSection( &impl->cs );
        return;
    }
    impl->resolved = TRUE;
    count = impl->handler_count;
    for (i = 0; i < count; i++)
    {
        handlers[i] = impl->handlers[i].handler;
        handlers[i]->lpVtbl->AddRef( handlers[i] );
    }
    LeaveCriticalSection( &impl->cs );

    if (!(args = calloc( 1, sizeof(*args) ))) goto done;
    args->ITimedTextSourceResolveResultEventArgsInternal_iface.lpVtbl = &resolve_args_vtbl;
    args->ref = 1;
    if (FAILED(timed_text_track_list_create( impl->language, &args->tracks )))
    {
        free( args );
        goto done;
    }

    for (i = 0; i < count; i++)
    {
        TRACE( "reporting Resolved for %p to %p.\n", impl, handlers[i] );
        handlers[i]->lpVtbl->Invoke( handlers[i], &impl->ITimedTextSourceInternal_iface,
                                     &args->ITimedTextSourceResolveResultEventArgsInternal_iface );
    }
    ITimedTextSourceResolveResultEventArgsInternal_Release( &args->ITimedTextSourceResolveResultEventArgsInternal_iface );

done:
    for (i = 0; i < count; i++) handlers[i]->lpVtbl->Release( handlers[i] );
}

/* The player has opened the media the vector's MediaSource describes: resolve every source in
 * it. Snapshot first, so a handler that edits the vector cannot pull it out from under us. */
void timed_text_source_vector_resolve( IInspectable *vector )
{
    struct inspectable_vector *impl = impl_from_IWineInspectableVector( (IWineInspectableVector *)vector );
    IInspectable **sources;
    UINT32 i, count;

    EnterCriticalSection( &impl->cs );
    count = impl->size;
    if (!(sources = malloc( count * sizeof(*sources) )))
    {
        LeaveCriticalSection( &impl->cs );
        return;
    }
    for (i = 0; i < count; i++) IInspectable_AddRef( (sources[i] = impl->elements[i]) );
    LeaveCriticalSection( &impl->cs );

    TRACE( "resolving %u timed text sources.\n", count );
    for (i = 0; i < count; i++)
    {
        ITimedTextSourceInternal *source;

        if (SUCCEEDED(IInspectable_QueryInterface( sources[i], &IID_ITimedTextSourceInternal, (void **)&source )))
        {
            if (source->lpVtbl == &timed_text_source_vtbl)
                timed_text_source_resolve( impl_from_ITimedTextSourceInternal( source ) );
            else
                FIXME( "source %p is not one of ours.\n", sources[i] );
            ITimedTextSourceInternal_Release( source );
        }
        IInspectable_Release( sources[i] );
    }
    free( sources );
}

/*
 * The factory: IActivationFactory plus ITimedTextSourceStatics and ITimedTextSourceStatics2.
 */
struct timed_text_source_statics
{
    IActivationFactory IActivationFactory_iface;
    ITimedTextSourceStaticsInternal ITimedTextSourceStaticsInternal_iface;
    ITimedTextSourceStatics2Internal ITimedTextSourceStatics2Internal_iface;
    LONG ref;
};

static inline struct timed_text_source_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct timed_text_source_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct timed_text_source_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_ITimedTextSourceStaticsInternal ))
    {
        IInspectable_AddRef( (*out = &impl->ITimedTextSourceStaticsInternal_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_ITimedTextSourceStatics2Internal ))
    {
        IInspectable_AddRef( (*out = &impl->ITimedTextSourceStatics2Internal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_TimedTextSource,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_TimedTextSource) - 1, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* TimedTextSource has no default constructor on Windows either. */
    if (instance) *instance = NULL;
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

DEFINE_IINSPECTABLE( statics, ITimedTextSourceStaticsInternal, struct timed_text_source_statics, IActivationFactory_iface )

static HRESULT WINAPI statics_CreateFromStream( ITimedTextSourceStaticsInternal *iface, IInspectable *stream,
                                                ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, stream %p, value %p.\n", iface, stream, value );
    return timed_text_source_create( NULL, stream, NULL, value );
}

static HRESULT WINAPI statics_CreateFromUri( ITimedTextSourceStaticsInternal *iface, IInspectable *uri,
                                             ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, uri %p, value %p.\n", iface, uri, value );
    return timed_text_source_create( uri, NULL, NULL, value );
}

static HRESULT WINAPI statics_CreateFromStreamWithLanguage( ITimedTextSourceStaticsInternal *iface, IInspectable *stream,
                                                            HSTRING language, ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, stream %p, language %s, value %p.\n", iface, stream, debugstr_hstring( language ), value );
    return timed_text_source_create( NULL, stream, language, value );
}

static HRESULT WINAPI statics_CreateFromUriWithLanguage( ITimedTextSourceStaticsInternal *iface, IInspectable *uri,
                                                         HSTRING language, ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, uri %p, language %s, value %p.\n", iface, uri, debugstr_hstring( language ), value );
    return timed_text_source_create( uri, NULL, language, value );
}

static const struct ITimedTextSourceStaticsInternalVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* ITimedTextSourceStatics methods */
    statics_CreateFromStream,
    statics_CreateFromUri,
    statics_CreateFromStreamWithLanguage,
    statics_CreateFromUriWithLanguage,
};

DEFINE_IINSPECTABLE( statics2, ITimedTextSourceStatics2Internal, struct timed_text_source_statics, IActivationFactory_iface )

/* The index file (an idx for VobSub) only matters to a renderer; the source is the same. */
static HRESULT WINAPI statics2_CreateFromStreamWithIndex( ITimedTextSourceStatics2Internal *iface, IInspectable *stream,
                                                          IInspectable *index_stream, ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, stream %p, index_stream %p, value %p.\n", iface, stream, index_stream, value );
    return timed_text_source_create( NULL, stream, NULL, value );
}

static HRESULT WINAPI statics2_CreateFromUriWithIndex( ITimedTextSourceStatics2Internal *iface, IInspectable *uri,
                                                       IInspectable *index_uri, ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, uri %p, index_uri %p, value %p.\n", iface, uri, index_uri, value );
    return timed_text_source_create( uri, NULL, NULL, value );
}

static HRESULT WINAPI statics2_CreateFromStreamWithIndexAndLanguage( ITimedTextSourceStatics2Internal *iface,
        IInspectable *stream, IInspectable *index_stream, HSTRING language, ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, stream %p, index_stream %p, language %s, value %p.\n", iface, stream, index_stream,
           debugstr_hstring( language ), value );
    return timed_text_source_create( NULL, stream, language, value );
}

static HRESULT WINAPI statics2_CreateFromUriWithIndexAndLanguage( ITimedTextSourceStatics2Internal *iface,
        IInspectable *uri, IInspectable *index_uri, HSTRING language, ITimedTextSourceInternal **value )
{
    TRACE( "iface %p, uri %p, index_uri %p, language %s, value %p.\n", iface, uri, index_uri,
           debugstr_hstring( language ), value );
    return timed_text_source_create( uri, NULL, language, value );
}

static const struct ITimedTextSourceStatics2InternalVtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* ITimedTextSourceStatics2 methods */
    statics2_CreateFromStreamWithIndex,
    statics2_CreateFromUriWithIndex,
    statics2_CreateFromStreamWithIndexAndLanguage,
    statics2_CreateFromUriWithIndexAndLanguage,
};

static struct timed_text_source_statics timed_text_source_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    1,
};

IActivationFactory *timed_text_source_factory = &timed_text_source_statics.IActivationFactory_iface;

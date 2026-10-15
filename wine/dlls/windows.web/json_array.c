/* WinRT Windows.Data.Json.JsonArray Implementation
 *
 * Copyright (C) 2026 Olivia Ryan
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

WINE_DEFAULT_DEBUG_CHANNEL(web);

struct json_array
{
    IJsonArray IJsonArray_iface;
    IJsonValue IJsonValue_iface;
    IVector_IJsonValue IVector_IJsonValue_iface;
    IIterable_IJsonValue IIterable_IJsonValue_iface;
    IStringable IStringable_iface;
    LONG ref;
    IJsonValue **elements;
    ULONG capacity;
    ULONG length;
};

static inline struct json_array *impl_from_IJsonArray( IJsonArray *iface )
{
    return CONTAINING_RECORD( iface, struct json_array, IJsonArray_iface );
}

HRESULT json_array_push( IJsonArray *iface, IJsonValue *value )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (impl->length == impl->capacity)
    {
        UINT32 capacity = max( 32, impl->capacity * 3 / 2 );
        IJsonValue **new = impl->elements;
        if (!(new = realloc( new, capacity * sizeof(*new) ))) return E_OUTOFMEMORY;
        impl->elements = new;
        impl->capacity = capacity;
    }

    impl->elements[impl->length++] = value;
    IJsonValue_AddRef( value );
    return S_OK;
}

static HRESULT WINAPI json_array_QueryInterface( IJsonArray *iface, REFIID iid, void **out )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IJsonArray ))
    {
        *out = &impl->IJsonArray_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IJsonValue ))
    {
        *out = &impl->IJsonValue_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IVector_IJsonValue ))
    {
        *out = &impl->IVector_IJsonValue_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_IJsonValue ))
    {
        *out = &impl->IIterable_IJsonValue_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IStringable ))
    {
        *out = &impl->IStringable_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI json_array_AddRef( IJsonArray *iface )
{
    struct json_array *impl = impl_from_IJsonArray( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI json_array_Release( IJsonArray *iface )
{
    struct json_array *impl = impl_from_IJsonArray( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        for (UINT32 i = 0; i < impl->length; i++)
            IJsonValue_Release( impl->elements[i] );

        free( impl->elements );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI json_array_GetIids( IJsonArray *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI json_array_GetRuntimeClassName( IJsonArray *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI json_array_GetTrustLevel( IJsonArray *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI json_array_GetObjectAt( IJsonArray *iface, UINT32 index, IJsonObject **value )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, index %u, value %p\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    if (index >= impl->length) return E_BOUNDS;

    return IJsonValue_GetObject( impl->elements[index], value );
}

static HRESULT WINAPI json_array_GetArrayAt( IJsonArray *iface, UINT32 index, IJsonArray **value )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, index %u, value %p\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    if (index >= impl->length) return E_BOUNDS;

    return IJsonValue_GetArray( impl->elements[index], value );
}

static HRESULT WINAPI json_array_GetStringAt( IJsonArray *iface, UINT32 index, HSTRING *value )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, index %u, value %p\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    if (index >= impl->length) return E_BOUNDS;

    return IJsonValue_GetString( impl->elements[index], value );
}

static HRESULT WINAPI json_array_GetNumberAt( IJsonArray *iface, UINT32 index, DOUBLE *value )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, index %u, value %p\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    if (index >= impl->length) return E_BOUNDS;

    return IJsonValue_GetNumber( impl->elements[index], value );
}

static HRESULT WINAPI json_array_GetBooleanAt( IJsonArray *iface, UINT32 index, boolean *value )
{
    struct json_array *impl = impl_from_IJsonArray( iface );

    TRACE( "iface %p, index %u, value %p\n", iface, index, value );

    if (!value) return E_INVALIDARG;
    if (index >= impl->length) return E_BOUNDS;

    return IJsonValue_GetBoolean( impl->elements[index], value );
}

static const struct IJsonArrayVtbl json_array_vtbl =
{
    json_array_QueryInterface,
    json_array_AddRef,
    json_array_Release,
    /* IInspectable methods */
    json_array_GetIids,
    json_array_GetRuntimeClassName,
    json_array_GetTrustLevel,
    /* IJsonArray methods */
    json_array_GetObjectAt,
    json_array_GetArrayAt,
    json_array_GetStringAt,
    json_array_GetNumberAt,
    json_array_GetBooleanAt,
};

/* IJsonValue: a JsonArray is also its own JSON value */
DEFINE_IINSPECTABLE( json_array_value, IJsonValue, struct json_array, IJsonArray_iface )

DEFINE_IINSPECTABLE_( json_array_stringable, IStringable, struct json_array, impl_from_json_array_stringable,
                      IStringable_iface, &impl->IJsonArray_iface )
static HRESULT WINAPI json_array_stringable_ToString( IStringable *iface, HSTRING *value );
static const struct IStringableVtbl json_array_stringable_vtbl =
{
    json_array_stringable_QueryInterface,
    json_array_stringable_AddRef,
    json_array_stringable_Release,
    json_array_stringable_GetIids,
    json_array_stringable_GetRuntimeClassName,
    json_array_stringable_GetTrustLevel,
    json_array_stringable_ToString,
};

static HRESULT WINAPI json_array_value_get_ValueType( IJsonValue *iface, JsonValueType *value )
{
    if (!value) return E_POINTER;
    *value = JsonValueType_Array;
    return S_OK;
}
static HRESULT WINAPI json_array_value_Stringify( IJsonValue *iface, HSTRING *value )
{
    struct json_buf buf = {0};
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = json_write_value( iface, &buf ))) { json_buf_free( &buf ); return hr; }
    return json_buf_to_hstring( &buf, value );
}
static HRESULT WINAPI json_array_value_GetString( IJsonValue *iface, HSTRING *value ) { return E_ILLEGAL_METHOD_CALL; }
static HRESULT WINAPI json_array_value_GetNumber( IJsonValue *iface, DOUBLE *value ) { return E_ILLEGAL_METHOD_CALL; }
static HRESULT WINAPI json_array_value_GetBoolean( IJsonValue *iface, boolean *value ) { return E_ILLEGAL_METHOD_CALL; }
static HRESULT WINAPI json_array_value_GetArray( IJsonValue *iface, IJsonArray **value )
{
    struct json_array *impl = impl_from_IJsonValue( iface );
    if (!value) return E_POINTER;
    IJsonArray_AddRef( (*value = &impl->IJsonArray_iface) );
    return S_OK;
}
static HRESULT WINAPI json_array_value_GetObject( IJsonValue *iface, IJsonObject **value ) { return E_ILLEGAL_METHOD_CALL; }

static const struct IJsonValueVtbl json_array_value_vtbl =
{
    json_array_value_QueryInterface,
    json_array_value_AddRef,
    json_array_value_Release,
    /* IInspectable methods */
    json_array_value_GetIids,
    json_array_value_GetRuntimeClassName,
    json_array_value_GetTrustLevel,
    /* IJsonValue methods */
    json_array_value_get_ValueType,
    json_array_value_Stringify,
    json_array_value_GetString,
    json_array_value_GetNumber,
    json_array_value_GetBoolean,
    json_array_value_GetArray,
    json_array_value_GetObject,
};

/* IVector<IJsonValue *>: what projections index and count the array through */
DEFINE_IINSPECTABLE( json_array_vector, IVector_IJsonValue, struct json_array, IJsonArray_iface )

static HRESULT WINAPI json_array_vector_GetAt( IVector_IJsonValue *iface, UINT32 index, IJsonValue **value )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->length) return E_BOUNDS;
    IJsonValue_AddRef( (*value = impl->elements[index]) );
    return S_OK;
}
static HRESULT WINAPI json_array_vector_get_Size( IVector_IJsonValue *iface, UINT32 *value )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->length;
    return S_OK;
}
static HRESULT WINAPI json_array_vector_GetView( IVector_IJsonValue *iface, IVectorView_IJsonValue **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}
static HRESULT WINAPI json_array_vector_IndexOf( IVector_IJsonValue *iface, IJsonValue *element, UINT32 *index, boolean *found )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    UINT32 i;

    if (!index || !found) return E_POINTER;
    for (i = 0; i < impl->length; i++) if (impl->elements[i] == element) break;
    *found = i < impl->length;
    *index = *found ? i : 0;
    return S_OK;
}
static HRESULT WINAPI json_array_vector_SetAt( IVector_IJsonValue *iface, UINT32 index, IJsonValue *value )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );

    if (index >= impl->length) return E_BOUNDS;
    if (!value) return E_POINTER;
    IJsonValue_AddRef( value );
    IJsonValue_Release( impl->elements[index] );
    impl->elements[index] = value;
    return S_OK;
}
static HRESULT WINAPI json_array_vector_InsertAt( IVector_IJsonValue *iface, UINT32 index, IJsonValue *value )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    HRESULT hr;

    if (index > impl->length) return E_BOUNDS;
    if (!value) return E_POINTER;
    if (FAILED(hr = json_array_push( &impl->IJsonArray_iface, value ))) return hr;
    memmove( impl->elements + index + 1, impl->elements + index, (impl->length - 1 - index) * sizeof(*impl->elements) );
    impl->elements[index] = value;
    return S_OK;
}
static HRESULT WINAPI json_array_vector_RemoveAt( IVector_IJsonValue *iface, UINT32 index )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );

    if (index >= impl->length) return E_BOUNDS;
    IJsonValue_Release( impl->elements[index] );
    memmove( impl->elements + index, impl->elements + index + 1, (impl->length - index - 1) * sizeof(*impl->elements) );
    impl->length--;
    return S_OK;
}
static HRESULT WINAPI json_array_vector_Append( IVector_IJsonValue *iface, IJsonValue *value )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    if (!value) return E_POINTER;
    return json_array_push( &impl->IJsonArray_iface, value );
}
static HRESULT WINAPI json_array_vector_RemoveAtEnd( IVector_IJsonValue *iface )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    if (!impl->length) return E_BOUNDS;
    return json_array_vector_RemoveAt( iface, impl->length - 1 );
}
static HRESULT WINAPI json_array_vector_Clear( IVector_IJsonValue *iface )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    while (impl->length) IJsonValue_Release( impl->elements[--impl->length] );
    return S_OK;
}
static HRESULT WINAPI json_array_vector_GetMany( IVector_IJsonValue *iface, UINT32 start, UINT32 size, IJsonValue **items, UINT32 *count )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    UINT32 i;

    if (!count) return E_POINTER;
    *count = 0;
    if (start > impl->length) return E_BOUNDS;
    for (i = start; i < impl->length && i - start < size; i++)
        IJsonValue_AddRef( (items[i - start] = impl->elements[i]) );
    *count = i - start;
    return S_OK;
}
static HRESULT WINAPI json_array_vector_ReplaceAll( IVector_IJsonValue *iface, UINT32 count, IJsonValue **items )
{
    struct json_array *impl = impl_from_IVector_IJsonValue( iface );
    UINT32 i;
    HRESULT hr;

    json_array_vector_Clear( iface );
    for (i = 0; i < count; i++)
        if (FAILED(hr = json_array_push( &impl->IJsonArray_iface, items[i] ))) return hr;
    return S_OK;
}

static const struct IVector_IJsonValueVtbl json_array_vector_vtbl =
{
    json_array_vector_QueryInterface,
    json_array_vector_AddRef,
    json_array_vector_Release,
    /* IInspectable methods */
    json_array_vector_GetIids,
    json_array_vector_GetRuntimeClassName,
    json_array_vector_GetTrustLevel,
    /* IVector<IJsonValue *> methods */
    json_array_vector_GetAt,
    json_array_vector_get_Size,
    json_array_vector_GetView,
    json_array_vector_IndexOf,
    json_array_vector_SetAt,
    json_array_vector_InsertAt,
    json_array_vector_RemoveAt,
    json_array_vector_Append,
    json_array_vector_RemoveAtEnd,
    json_array_vector_Clear,
    json_array_vector_GetMany,
    json_array_vector_ReplaceAll,
};

/* IIterable<IJsonValue *> and its iterator */
struct json_array_iterator
{
    IIterator_IJsonValue IIterator_IJsonValue_iface;
    LONG ref;
    struct json_array *array;
    UINT32 index;
};

static inline struct json_array_iterator *impl_from_IIterator_IJsonValue( IIterator_IJsonValue *iface )
{
    return CONTAINING_RECORD( iface, struct json_array_iterator, IIterator_IJsonValue_iface );
}
static HRESULT WINAPI json_array_iterator_QueryInterface( IIterator_IJsonValue *iface, REFIID iid, void **out )
{
    struct json_array_iterator *impl = impl_from_IIterator_IJsonValue( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IJsonValue ))
    {
        IInspectable_AddRef( (*out = &impl->IIterator_IJsonValue_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI json_array_iterator_AddRef( IIterator_IJsonValue *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_IJsonValue( iface )->ref );
}
static ULONG WINAPI json_array_iterator_Release( IIterator_IJsonValue *iface )
{
    struct json_array_iterator *impl = impl_from_IIterator_IJsonValue( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IJsonArray_Release( &impl->array->IJsonArray_iface );
        free( impl );
    }
    return ref;
}
static HRESULT WINAPI json_array_iterator_GetIids( IIterator_IJsonValue *iface, ULONG *iid_count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI json_array_iterator_GetRuntimeClassName( IIterator_IJsonValue *iface, HSTRING *class_name ) { return E_NOTIMPL; }
static HRESULT WINAPI json_array_iterator_GetTrustLevel( IIterator_IJsonValue *iface, TrustLevel *trust_level ) { return E_NOTIMPL; }
static HRESULT WINAPI json_array_iterator_get_Current( IIterator_IJsonValue *iface, IJsonValue **value )
{
    struct json_array_iterator *impl = impl_from_IIterator_IJsonValue( iface );
    return json_array_vector_GetAt( &impl->array->IVector_IJsonValue_iface, impl->index, value );
}
static HRESULT WINAPI json_array_iterator_get_HasCurrent( IIterator_IJsonValue *iface, boolean *value )
{
    struct json_array_iterator *impl = impl_from_IIterator_IJsonValue( iface );
    if (!value) return E_POINTER;
    *value = impl->index < impl->array->length;
    return S_OK;
}
static HRESULT WINAPI json_array_iterator_MoveNext( IIterator_IJsonValue *iface, boolean *value )
{
    struct json_array_iterator *impl = impl_from_IIterator_IJsonValue( iface );
    if (!value) return E_POINTER;
    if (impl->index < impl->array->length) impl->index++;
    *value = impl->index < impl->array->length;
    return S_OK;
}
static HRESULT WINAPI json_array_iterator_GetMany( IIterator_IJsonValue *iface, UINT32 size, IJsonValue **items, UINT32 *count )
{
    struct json_array_iterator *impl = impl_from_IIterator_IJsonValue( iface );
    HRESULT hr = json_array_vector_GetMany( &impl->array->IVector_IJsonValue_iface, impl->index, size, items, count );
    if (SUCCEEDED(hr)) impl->index += *count;
    return hr;
}
static const struct IIterator_IJsonValueVtbl json_array_iterator_vtbl =
{
    json_array_iterator_QueryInterface,
    json_array_iterator_AddRef,
    json_array_iterator_Release,
    json_array_iterator_GetIids,
    json_array_iterator_GetRuntimeClassName,
    json_array_iterator_GetTrustLevel,
    json_array_iterator_get_Current,
    json_array_iterator_get_HasCurrent,
    json_array_iterator_MoveNext,
    json_array_iterator_GetMany,
};

DEFINE_IINSPECTABLE( json_array_iterable, IIterable_IJsonValue, struct json_array, IJsonArray_iface )

static HRESULT WINAPI json_array_iterable_First( IIterable_IJsonValue *iface, IIterator_IJsonValue **value )
{
    struct json_array *impl = impl_from_IIterable_IJsonValue( iface );
    struct json_array_iterator *iter;

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_IJsonValue_iface.lpVtbl = &json_array_iterator_vtbl;
    iter->ref = 1;
    IJsonArray_AddRef( &impl->IJsonArray_iface );
    iter->array = impl;
    *value = &iter->IIterator_IJsonValue_iface;
    return S_OK;
}
static const struct IIterable_IJsonValueVtbl json_array_iterable_vtbl =
{
    json_array_iterable_QueryInterface,
    json_array_iterable_AddRef,
    json_array_iterable_Release,
    json_array_iterable_GetIids,
    json_array_iterable_GetRuntimeClassName,
    json_array_iterable_GetTrustLevel,
    json_array_iterable_First,
};

static void json_array_init_views( struct json_array *impl )
{
    impl->IJsonValue_iface.lpVtbl = &json_array_value_vtbl;
    impl->IVector_IJsonValue_iface.lpVtbl = &json_array_vector_vtbl;
    impl->IIterable_IJsonValue_iface.lpVtbl = &json_array_iterable_vtbl;
}

struct json_array_statics
{
    IActivationFactory IActivationFactory_iface;
    IJsonArrayStatics IJsonArrayStatics_iface;
    LONG ref;
};

static HRESULT WINAPI json_array_stringable_ToString( IStringable *iface, HSTRING *value )
{
    struct json_array *impl = impl_from_json_array_stringable( iface );
    struct json_buf buf = {0};
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = json_write_array( &impl->IJsonArray_iface, &buf ))) { json_buf_free( &buf ); return hr; }
    return json_buf_to_hstring( &buf, value );
}

static inline struct json_array_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct json_array_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct json_array_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IJsonArrayStatics ))
    {
        *out = &impl->IJsonArrayStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct json_array_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct json_array_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct json_array *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    *instance = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IJsonArray_iface.lpVtbl = &json_array_vtbl;
    json_array_init_views( impl );
    impl->IStringable_iface.lpVtbl = &json_array_stringable_vtbl;
    impl->ref = 1;

    *instance = (IInspectable *)&impl->IJsonArray_iface;
    return S_OK;
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

DEFINE_IINSPECTABLE( json_array_statics, IJsonArrayStatics, struct json_array_statics, IActivationFactory_iface )

static HRESULT WINAPI json_array_statics_Parse( IJsonArrayStatics *iface, HSTRING input, IJsonArray **value )
{
    IJsonValue *parsed;
    HRESULT hr;

    TRACE( "iface %p, input %s, value %p.\n", iface, debugstr_hstring( input ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = json_parse( input, &parsed ))) return hr;
    hr = IJsonValue_GetArray( parsed, value );
    IJsonValue_Release( parsed );
    return FAILED(hr) ? WEB_E_JSON_VALUE_NOT_FOUND : S_OK;
}

static HRESULT WINAPI json_array_statics_TryParse( IJsonArrayStatics *iface, HSTRING input, IJsonArray **result, boolean *succeeded )
{
    TRACE( "iface %p, input %s, result %p, succeeded %p.\n", iface, debugstr_hstring( input ), result, succeeded );

    if (!result || !succeeded) return E_POINTER;
    *succeeded = SUCCEEDED(json_array_statics_Parse( iface, input, result ));
    return S_OK;
}

static const struct IJsonArrayStaticsVtbl json_array_statics_vtbl =
{
    json_array_statics_QueryInterface,
    json_array_statics_AddRef,
    json_array_statics_Release,
    /* IInspectable methods */
    json_array_statics_GetIids,
    json_array_statics_GetRuntimeClassName,
    json_array_statics_GetTrustLevel,
    /* IJsonArrayStatics methods */
    json_array_statics_Parse,
    json_array_statics_TryParse,
};

static struct json_array_statics json_array_statics =
{
    {&factory_vtbl},
    {&json_array_statics_vtbl},
    1,
};

IActivationFactory *json_array_factory = &json_array_statics.IActivationFactory_iface;

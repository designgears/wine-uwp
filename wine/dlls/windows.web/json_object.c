/* WinRT Windows.Data.Json.JsonObject Implementation
 *
 * Copyright (C) 2024 Mohamad Al-Jaf
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

struct json_object
{
    IJsonObject IJsonObject_iface;
    IStringable IStringable_iface;
    IMap_HSTRING_IJsonValue IMap_iface;
    IIterable_IKeyValuePair_HSTRING_IJsonValue IIterable_iface;
    IJsonValue IJsonValue_iface;
    struct { const struct json_object_defaults_vtbl *lpVtbl; } defaults_iface; /* IJsonObjectWithDefaultValues */
    LONG ref;
    IMap_HSTRING_IInspectable *members;
};

/* IJsonObjectWithDefaultValues: only forward-declared in windows.data.json.idl */
static const GUID IID_IJsonObjectWithDefaultValues_ = {0xd960d2a2,0xb7f0,0x4f00,{0x8e,0x44,0xd8,0x2c,0xf4,0x15,0xea,0x13}};
struct json_object_defaults_vtbl
{
    HRESULT (WINAPI *QueryInterface)( void *iface, REFIID iid, void **out );
    ULONG (WINAPI *AddRef)( void *iface );
    ULONG (WINAPI *Release)( void *iface );
    HRESULT (WINAPI *GetIids)( void *iface, ULONG *count, IID **iids );
    HRESULT (WINAPI *GetRuntimeClassName)( void *iface, HSTRING *name );
    HRESULT (WINAPI *GetTrustLevel)( void *iface, TrustLevel *level );
    HRESULT (WINAPI *GetNamedValue)( void *iface, HSTRING name, IJsonValue *def, IJsonValue **value );
    HRESULT (WINAPI *GetNamedObject)( void *iface, HSTRING name, IJsonObject *def, IJsonObject **value );
    HRESULT (WINAPI *GetNamedString)( void *iface, HSTRING name, HSTRING def, HSTRING *value );
    HRESULT (WINAPI *GetNamedArray)( void *iface, HSTRING name, IJsonArray *def, IJsonArray **value );
    HRESULT (WINAPI *GetNamedNumber)( void *iface, HSTRING name, DOUBLE def, DOUBLE *value );
    HRESULT (WINAPI *GetNamedBoolean)( void *iface, HSTRING name, boolean def, boolean *value );
};
static const struct json_object_defaults_vtbl json_object_defaults_vtbl;

static inline struct json_object *impl_from_IJsonObject( IJsonObject *iface )
{
    return CONTAINING_RECORD( iface, struct json_object, IJsonObject_iface );
}

DEFINE_IINSPECTABLE_( json_object_stringable, IStringable, struct json_object, impl_from_json_object_stringable,
                      IStringable_iface, &impl->IJsonObject_iface )
static HRESULT WINAPI json_object_stringable_ToString( IStringable *iface, HSTRING *value );
static const struct IStringableVtbl json_object_stringable_vtbl =
{
    json_object_stringable_QueryInterface,
    json_object_stringable_AddRef,
    json_object_stringable_Release,
    json_object_stringable_GetIids,
    json_object_stringable_GetRuntimeClassName,
    json_object_stringable_GetTrustLevel,
    json_object_stringable_ToString,
};

static const struct IMap_HSTRING_IJsonValueVtbl json_object_map_vtbl;
static const struct IJsonValueVtbl json_object_value_vtbl;
static const struct IIterable_IKeyValuePair_HSTRING_IJsonValueVtbl json_object_iterable_vtbl;

static HRESULT WINAPI json_object_QueryInterface( IJsonObject *iface, REFIID iid, void **out )
{
    struct json_object *impl = impl_from_IJsonObject( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IJsonObject ))
    {
        *out = &impl->IJsonObject_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IStringable ))
    {
        *out = &impl->IStringable_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IJsonValue ))
    {
        *out = &impl->IJsonValue_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IMap_HSTRING_IJsonValue ))
    {
        *out = &impl->IMap_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_IJsonValue ))
    {
        *out = &impl->IIterable_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IJsonObjectWithDefaultValues_ ))
    {
        *out = &impl->defaults_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI json_object_AddRef( IJsonObject *iface )
{
    struct json_object *impl = impl_from_IJsonObject( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI json_object_Release( IJsonObject *iface )
{
    struct json_object *impl = impl_from_IJsonObject( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        IMap_HSTRING_IInspectable_Release( impl->members );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI json_object_GetIids( IJsonObject *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI json_object_GetRuntimeClassName( IJsonObject *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI json_object_GetTrustLevel( IJsonObject *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI json_object_GetNamedValue( IJsonObject *iface, HSTRING name, IJsonValue **value )
{
    struct json_object *impl = impl_from_IJsonObject( iface );
    boolean exists;
    HRESULT hr;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;

    hr = IMap_HSTRING_IInspectable_HasKey( impl->members, name, &exists );
    if (FAILED(hr) || !exists) return WEB_E_JSON_VALUE_NOT_FOUND;

    return IMap_HSTRING_IInspectable_Lookup( impl->members, name, (IInspectable **)value );
}

static HRESULT WINAPI json_object_SetNamedValue( IJsonObject *iface, HSTRING name, IJsonValue *value )
{
    struct json_object *impl = impl_from_IJsonObject( iface );
    boolean dummy;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    return IMap_HSTRING_IInspectable_Insert( impl->members, name, (IInspectable *)value, &dummy );
}

static HRESULT WINAPI json_object_GetNamedObject( IJsonObject *iface, HSTRING name, IJsonObject **value )
{
    IJsonValue *internal_value;
    JsonValueType value_type;
    HRESULT hr;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;
    if (FAILED(hr = IJsonObject_GetNamedValue( iface, name, &internal_value )))
        return hr;

    IJsonValue_Release( internal_value );
    IJsonValue_get_ValueType( internal_value, &value_type );
    if (value_type != JsonValueType_Object) return E_ILLEGAL_METHOD_CALL;

    return IJsonValue_GetObject( internal_value, value );
}

static HRESULT WINAPI json_object_GetNamedArray( IJsonObject *iface, HSTRING name, IJsonArray **value )
{
    IJsonValue *internal_value;
    JsonValueType value_type;
    HRESULT hr;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;
    if (FAILED(hr = IJsonObject_GetNamedValue( iface, name, &internal_value )))
        return hr;

    IJsonValue_Release( internal_value );
    IJsonValue_get_ValueType( internal_value, &value_type );
    if (value_type != JsonValueType_Array) return E_ILLEGAL_METHOD_CALL;

    return IJsonValue_GetArray( internal_value, value );
}

static HRESULT WINAPI json_object_GetNamedString( IJsonObject *iface, HSTRING name, HSTRING *value )
{
    IJsonValue *internal_value;
    JsonValueType value_type;
    HRESULT hr;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;
    if (FAILED(hr = IJsonObject_GetNamedValue( iface, name, &internal_value )))
        return hr;

    IJsonValue_Release( internal_value );
    IJsonValue_get_ValueType( internal_value, &value_type );
    if (value_type != JsonValueType_String) return E_ILLEGAL_METHOD_CALL;

    return IJsonValue_GetString( internal_value, value );
}

static HRESULT WINAPI json_object_GetNamedNumber( IJsonObject *iface, HSTRING name, DOUBLE *value )
{
    IJsonValue *internal_value;
    JsonValueType value_type;
    HRESULT hr;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;
    if (FAILED(hr = IJsonObject_GetNamedValue( iface, name, &internal_value )))
        return hr;

    IJsonValue_Release( internal_value );
    IJsonValue_get_ValueType( internal_value, &value_type );
    if (value_type != JsonValueType_Number) return E_ILLEGAL_METHOD_CALL;

    return IJsonValue_GetNumber( internal_value, value );
}

static HRESULT WINAPI json_object_GetNamedBoolean( IJsonObject *iface, HSTRING name, boolean *value )
{
    IJsonValue *internal_value;
    JsonValueType value_type;
    HRESULT hr;

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;
    if (FAILED(hr = IJsonObject_GetNamedValue( iface, name, &internal_value )))
        return hr;

    IJsonValue_Release( internal_value );
    IJsonValue_get_ValueType( internal_value, &value_type );
    if (value_type != JsonValueType_Boolean) return E_ILLEGAL_METHOD_CALL;

    return IJsonValue_GetBoolean( internal_value, value );
}

static const struct IJsonObjectVtbl json_object_vtbl =
{
    json_object_QueryInterface,
    json_object_AddRef,
    json_object_Release,
    /* IInspectable methods */
    json_object_GetIids,
    json_object_GetRuntimeClassName,
    json_object_GetTrustLevel,
    /* IJsonObject methods */
    json_object_GetNamedValue,
    json_object_SetNamedValue,
    json_object_GetNamedObject,
    json_object_GetNamedArray,
    json_object_GetNamedString,
    json_object_GetNamedNumber,
    json_object_GetNamedBoolean,
};

/* IJsonObjectWithDefaultValues: the GetNamed* getters with a value to return when the name is missing. A value of
 * another type is still an error, as without a default. */
static inline struct json_object *impl_from_defaults( void *iface )
{
    return CONTAINING_RECORD( iface, struct json_object, defaults_iface );
}

static BOOL json_object_has( struct json_object *impl, HSTRING name )
{
    boolean exists = FALSE;
    return SUCCEEDED(IMap_HSTRING_IInspectable_HasKey( impl->members, name, &exists )) && exists;
}

static HRESULT WINAPI json_object_defaults_QueryInterface( void *iface, REFIID iid, void **out )
{
    return IJsonObject_QueryInterface( &impl_from_defaults( iface )->IJsonObject_iface, iid, out );
}
static ULONG WINAPI json_object_defaults_AddRef( void *iface )
{
    return IJsonObject_AddRef( &impl_from_defaults( iface )->IJsonObject_iface );
}
static ULONG WINAPI json_object_defaults_Release( void *iface )
{
    return IJsonObject_Release( &impl_from_defaults( iface )->IJsonObject_iface );
}
static HRESULT WINAPI json_object_defaults_GetIids( void *iface, ULONG *count, IID **iids )
{
    return IJsonObject_GetIids( &impl_from_defaults( iface )->IJsonObject_iface, count, iids );
}
static HRESULT WINAPI json_object_defaults_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return IJsonObject_GetRuntimeClassName( &impl_from_defaults( iface )->IJsonObject_iface, name );
}
static HRESULT WINAPI json_object_defaults_GetTrustLevel( void *iface, TrustLevel *level )
{
    return IJsonObject_GetTrustLevel( &impl_from_defaults( iface )->IJsonObject_iface, level );
}

static HRESULT WINAPI json_object_defaults_GetNamedValue( void *iface, HSTRING name, IJsonValue *def, IJsonValue **value )
{
    struct json_object *impl = impl_from_defaults( iface );

    TRACE( "iface %p, name %s, def %p, value %p.\n", iface, debugstr_hstring( name ), def, value );

    if (!value) return E_POINTER;
    if (json_object_has( impl, name )) return IJsonObject_GetNamedValue( &impl->IJsonObject_iface, name, value );
    if ((*value = def)) IJsonValue_AddRef( def );
    return S_OK;
}

static HRESULT WINAPI json_object_defaults_GetNamedObject( void *iface, HSTRING name, IJsonObject *def, IJsonObject **value )
{
    struct json_object *impl = impl_from_defaults( iface );

    TRACE( "iface %p, name %s, def %p, value %p.\n", iface, debugstr_hstring( name ), def, value );

    if (!value) return E_POINTER;
    if (json_object_has( impl, name )) return IJsonObject_GetNamedObject( &impl->IJsonObject_iface, name, value );
    if ((*value = def)) IJsonObject_AddRef( def );
    return S_OK;
}

static HRESULT WINAPI json_object_defaults_GetNamedString( void *iface, HSTRING name, HSTRING def, HSTRING *value )
{
    struct json_object *impl = impl_from_defaults( iface );

    TRACE( "iface %p, name %s, def %s, value %p.\n", iface, debugstr_hstring( name ), debugstr_hstring( def ), value );

    if (!value) return E_POINTER;
    if (json_object_has( impl, name )) return IJsonObject_GetNamedString( &impl->IJsonObject_iface, name, value );
    return WindowsDuplicateString( def, value );
}

static HRESULT WINAPI json_object_defaults_GetNamedArray( void *iface, HSTRING name, IJsonArray *def, IJsonArray **value )
{
    struct json_object *impl = impl_from_defaults( iface );

    TRACE( "iface %p, name %s, def %p, value %p.\n", iface, debugstr_hstring( name ), def, value );

    if (!value) return E_POINTER;
    if (json_object_has( impl, name )) return IJsonObject_GetNamedArray( &impl->IJsonObject_iface, name, value );
    if ((*value = def)) IJsonArray_AddRef( def );
    return S_OK;
}

static HRESULT WINAPI json_object_defaults_GetNamedNumber( void *iface, HSTRING name, DOUBLE def, DOUBLE *value )
{
    struct json_object *impl = impl_from_defaults( iface );

    TRACE( "iface %p, name %s, def %f, value %p.\n", iface, debugstr_hstring( name ), def, value );

    if (!value) return E_POINTER;
    if (json_object_has( impl, name )) return IJsonObject_GetNamedNumber( &impl->IJsonObject_iface, name, value );
    *value = def;
    return S_OK;
}

static HRESULT WINAPI json_object_defaults_GetNamedBoolean( void *iface, HSTRING name, boolean def, boolean *value )
{
    struct json_object *impl = impl_from_defaults( iface );

    TRACE( "iface %p, name %s, def %d, value %p.\n", iface, debugstr_hstring( name ), def, value );

    if (!value) return E_POINTER;
    if (json_object_has( impl, name )) return IJsonObject_GetNamedBoolean( &impl->IJsonObject_iface, name, value );
    *value = def;
    return S_OK;
}

static const struct json_object_defaults_vtbl json_object_defaults_vtbl =
{
    json_object_defaults_QueryInterface,
    json_object_defaults_AddRef,
    json_object_defaults_Release,
    json_object_defaults_GetIids,
    json_object_defaults_GetRuntimeClassName,
    json_object_defaults_GetTrustLevel,
    json_object_defaults_GetNamedValue,
    json_object_defaults_GetNamedObject,
    json_object_defaults_GetNamedString,
    json_object_defaults_GetNamedArray,
    json_object_defaults_GetNamedNumber,
    json_object_defaults_GetNamedBoolean,
};

struct json_object_statics
{
    IActivationFactory IActivationFactory_iface;
    IJsonObjectStatics IJsonObjectStatics_iface;
    LONG ref;
};

static inline struct json_object_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct json_object_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct json_object_statics *impl = impl_from_IActivationFactory( iface );

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
    if (IsEqualGUID( iid, &IID_IJsonObjectStatics ))
    {
        *out = &impl->IJsonObjectStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct json_object_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct json_object_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
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
    IPropertySet *property_set;
    HSTRING property_set_class;
    struct json_object *impl;
    HSTRING_HEADER header;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    *instance = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IJsonObject_iface.lpVtbl = &json_object_vtbl;
    impl->defaults_iface.lpVtbl = &json_object_defaults_vtbl;
    impl->IStringable_iface.lpVtbl = &json_object_stringable_vtbl;
    impl->IMap_iface.lpVtbl = &json_object_map_vtbl;
    impl->IIterable_iface.lpVtbl = &json_object_iterable_vtbl;
    impl->IJsonValue_iface.lpVtbl = &json_object_value_vtbl;
    impl->ref = 1;

    WindowsCreateStringReference( RuntimeClass_Windows_Foundation_Collections_PropertySet, wcslen( RuntimeClass_Windows_Foundation_Collections_PropertySet ),
                                  &header, &property_set_class );
    if (FAILED(hr = RoActivateInstance( property_set_class, (IInspectable **)&property_set ))) goto failed;

    hr = IPropertySet_QueryInterface( property_set, &IID_IMap_HSTRING_IInspectable, (void **)&impl->members );
    IPropertySet_Release( property_set );
    if (FAILED(hr)) goto failed;

    *instance = (IInspectable *)&impl->IJsonObject_iface;
    return S_OK;

failed:
    free( impl );
    return hr;
}

/* IJsonValue on a JsonObject: Windows' JsonObject derives from JsonValue, so Stringify and get_ValueType are
 * reached through it. The other getters are illegal for an object, as they are on the JsonValue of one. */
DEFINE_IINSPECTABLE_( json_object_value, IJsonValue, struct json_object, impl_from_json_object_value,
                      IJsonValue_iface, &impl->IJsonObject_iface )

static HRESULT WINAPI json_object_value_get_ValueType( IJsonValue *iface, JsonValueType *value )
{
    if (!value) return E_POINTER;
    *value = JsonValueType_Object;
    return S_OK;
}

static HRESULT WINAPI json_object_value_Stringify( IJsonValue *iface, HSTRING *value )
{
    struct json_object *impl = impl_from_json_object_value( iface );
    struct json_buf buf = {0};
    HRESULT hr;

    if (!value) return E_POINTER;
    if (FAILED(hr = json_write_object( &impl->IJsonObject_iface, &buf ))) { json_buf_free( &buf ); return hr; }
    return json_buf_to_hstring( &buf, value );
}

static HRESULT WINAPI json_object_value_GetString( IJsonValue *iface, HSTRING *value ) { return E_ILLEGAL_METHOD_CALL; }
static HRESULT WINAPI json_object_value_GetNumber( IJsonValue *iface, DOUBLE *value ) { return E_ILLEGAL_METHOD_CALL; }
static HRESULT WINAPI json_object_value_GetBoolean( IJsonValue *iface, boolean *value ) { return E_ILLEGAL_METHOD_CALL; }
static HRESULT WINAPI json_object_value_GetArray( IJsonValue *iface, IJsonArray **value ) { return E_ILLEGAL_METHOD_CALL; }

static HRESULT WINAPI json_object_value_GetObject( IJsonValue *iface, IJsonObject **value )
{
    struct json_object *impl = impl_from_json_object_value( iface );

    if (!value) return E_POINTER;
    *value = &impl->IJsonObject_iface;
    IJsonObject_AddRef( *value );
    return S_OK;
}

static const struct IJsonValueVtbl json_object_value_vtbl =
{
    json_object_value_QueryInterface,
    json_object_value_AddRef,
    json_object_value_Release,
    json_object_value_GetIids,
    json_object_value_GetRuntimeClassName,
    json_object_value_GetTrustLevel,
    json_object_value_get_ValueType,
    json_object_value_Stringify,
    json_object_value_GetString,
    json_object_value_GetNumber,
    json_object_value_GetBoolean,
    json_object_value_GetArray,
    json_object_value_GetObject,
};

static HRESULT WINAPI json_object_stringable_ToString( IStringable *iface, HSTRING *value )
{
    struct json_object *impl = impl_from_json_object_stringable( iface );
    struct json_buf buf = {0};
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = json_write_object( &impl->IJsonObject_iface, &buf ))) { json_buf_free( &buf ); return hr; }
    return json_buf_to_hstring( &buf, value );
}

HRESULT json_object_for_each( IJsonObject *iface, HRESULT (*callback)( HSTRING key, IJsonValue *value, void *context ),
                              void *context )
{
    struct json_object *impl = impl_from_IJsonObject( iface );
    IIterable_IKeyValuePair_HSTRING_IInspectable *iterable;
    IIterator_IKeyValuePair_HSTRING_IInspectable *iterator;
    boolean has = FALSE;
    HRESULT hr;

    if (FAILED(hr = IMap_HSTRING_IInspectable_QueryInterface( impl->members, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable,
                                                              (void **)&iterable ))) return hr;
    hr = IIterable_IKeyValuePair_HSTRING_IInspectable_First( iterable, &iterator );
    IIterable_IKeyValuePair_HSTRING_IInspectable_Release( iterable );
    if (FAILED(hr)) return hr;

    hr = IIterator_IKeyValuePair_HSTRING_IInspectable_get_HasCurrent( iterator, &has );
    while (SUCCEEDED(hr) && has)
    {
        IKeyValuePair_HSTRING_IInspectable *pair;

        if (SUCCEEDED(hr = IIterator_IKeyValuePair_HSTRING_IInspectable_get_Current( iterator, &pair )))
        {
            HSTRING key = NULL;
            IInspectable *inspectable = NULL;
            IJsonValue *value = NULL;

            if (SUCCEEDED(hr = IKeyValuePair_HSTRING_IInspectable_get_Key( pair, &key )) &&
                SUCCEEDED(hr = IKeyValuePair_HSTRING_IInspectable_get_Value( pair, &inspectable )))
            {
                hr = IInspectable_QueryInterface( inspectable, &IID_IJsonValue, (void **)&value );
                if (SUCCEEDED(hr))
                {
                    hr = callback( key, value, context );
                    IJsonValue_Release( value );
                }
            }
            if (inspectable) IInspectable_Release( inspectable );
            if (key) WindowsDeleteString( key );
            IKeyValuePair_HSTRING_IInspectable_Release( pair );
        }
        if (SUCCEEDED(hr)) hr = IIterator_IKeyValuePair_HSTRING_IInspectable_MoveNext( iterator, &has );
    }
    IIterator_IKeyValuePair_HSTRING_IInspectable_Release( iterator );
    return hr;
}

/* IMap<HSTRING, IJsonValue> and IIterable<IKeyValuePair<HSTRING, IJsonValue>>: the typed faces of the same members.
 * C++/WinRT's consume_IMap asks for these from a JsonObject, and Windows' JsonObject answers them. The members
 * hold IJsonValue objects as IInspectable, so the typed values are the same objects read through IJsonValue. */

#define JSON_NOTIMPL_IINSPECTABLE( pfx, T ) \
    static HRESULT WINAPI pfx##_GetIids( T *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; } \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( T *iface, HSTRING *name ) { return E_NOTIMPL; } \
    static HRESULT WINAPI pfx##_GetTrustLevel( T *iface, TrustLevel *level ) { return E_NOTIMPL; }

static HRESULT WINAPI json_object_map_Lookup( IMap_HSTRING_IJsonValue *iface, HSTRING key, IJsonValue **value );
static HRESULT WINAPI json_object_map_get_Size( IMap_HSTRING_IJsonValue *iface, UINT32 *size );
static HRESULT WINAPI json_object_map_HasKey( IMap_HSTRING_IJsonValue *iface, HSTRING key, boolean *found );
static HRESULT WINAPI json_object_map_GetView( IMap_HSTRING_IJsonValue *iface, IMapView_HSTRING_IJsonValue **view );
static HRESULT WINAPI json_object_map_Insert( IMap_HSTRING_IJsonValue *iface, HSTRING key, IJsonValue *value, boolean *replaced );
static HRESULT WINAPI json_object_map_Remove( IMap_HSTRING_IJsonValue *iface, HSTRING key );
static HRESULT WINAPI json_object_map_Clear( IMap_HSTRING_IJsonValue *iface );
static HRESULT WINAPI json_object_iterable_First( IIterable_IKeyValuePair_HSTRING_IJsonValue *iface,
                                                  IIterator_IKeyValuePair_HSTRING_IJsonValue **value );

DEFINE_IINSPECTABLE_( json_object_map, IMap_HSTRING_IJsonValue, struct json_object, impl_from_json_object_map,
                      IMap_iface, &impl->IJsonObject_iface )
static const struct IMap_HSTRING_IJsonValueVtbl json_object_map_vtbl =
{
    json_object_map_QueryInterface,
    json_object_map_AddRef,
    json_object_map_Release,
    json_object_map_GetIids,
    json_object_map_GetRuntimeClassName,
    json_object_map_GetTrustLevel,
    json_object_map_Lookup,
    json_object_map_get_Size,
    json_object_map_HasKey,
    json_object_map_GetView,
    json_object_map_Insert,
    json_object_map_Remove,
    json_object_map_Clear,
};

DEFINE_IINSPECTABLE_( json_object_iterable, IIterable_IKeyValuePair_HSTRING_IJsonValue, struct json_object,
                      impl_from_json_object_iterable, IIterable_iface, &impl->IJsonObject_iface )
static const struct IIterable_IKeyValuePair_HSTRING_IJsonValueVtbl json_object_iterable_vtbl =
{
    json_object_iterable_QueryInterface,
    json_object_iterable_AddRef,
    json_object_iterable_Release,
    json_object_iterable_GetIids,
    json_object_iterable_GetRuntimeClassName,
    json_object_iterable_GetTrustLevel,
    json_object_iterable_First,
};

static HRESULT WINAPI json_object_map_Lookup( IMap_HSTRING_IJsonValue *iface, HSTRING key, IJsonValue **value )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    return json_object_GetNamedValue( &impl->IJsonObject_iface, key, value );
}

static HRESULT WINAPI json_object_map_get_Size( IMap_HSTRING_IJsonValue *iface, UINT32 *size )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    return IMap_HSTRING_IInspectable_get_Size( impl->members, size );
}

static HRESULT WINAPI json_object_map_HasKey( IMap_HSTRING_IJsonValue *iface, HSTRING key, boolean *found )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    return IMap_HSTRING_IInspectable_HasKey( impl->members, key, found );
}

static HRESULT WINAPI json_object_map_Insert( IMap_HSTRING_IJsonValue *iface, HSTRING key, IJsonValue *value, boolean *replaced )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    return IMap_HSTRING_IInspectable_Insert( impl->members, key, (IInspectable *)value, replaced );
}

static HRESULT WINAPI json_object_map_Remove( IMap_HSTRING_IJsonValue *iface, HSTRING key )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    return IMap_HSTRING_IInspectable_Remove( impl->members, key );
}

static HRESULT WINAPI json_object_map_Clear( IMap_HSTRING_IJsonValue *iface )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    return IMap_HSTRING_IInspectable_Clear( impl->members );
}

/* A live read-only view of the object, IMapView<HSTRING, IJsonValue>. */
struct json_mapview
{
    IMapView_HSTRING_IJsonValue iface;
    LONG ref;
    IJsonObject *owner;
};

static inline struct json_mapview *impl_from_json_mapview( IMapView_HSTRING_IJsonValue *iface )
{
    return CONTAINING_RECORD( iface, struct json_mapview, iface );
}

static HRESULT WINAPI json_mapview_QueryInterface( IMapView_HSTRING_IJsonValue *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMapView_HSTRING_IJsonValue ))
    {
        *out = iface;
        IMapView_HSTRING_IJsonValue_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI json_mapview_AddRef( IMapView_HSTRING_IJsonValue *iface )
{
    struct json_mapview *impl = impl_from_json_mapview( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI json_mapview_Release( IMapView_HSTRING_IJsonValue *iface )
{
    struct json_mapview *impl = impl_from_json_mapview( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IJsonObject_Release( impl->owner );
        free( impl );
    }
    return ref;
}

JSON_NOTIMPL_IINSPECTABLE( json_mapview, IMapView_HSTRING_IJsonValue )

static HRESULT WINAPI json_mapview_Lookup( IMapView_HSTRING_IJsonValue *iface, HSTRING key, IJsonValue **value )
{
    struct json_mapview *impl = impl_from_json_mapview( iface );
    return json_object_GetNamedValue( impl->owner, key, value );
}

static HRESULT WINAPI json_mapview_get_Size( IMapView_HSTRING_IJsonValue *iface, UINT32 *size )
{
    struct json_mapview *impl = impl_from_json_mapview( iface );
    IMap_HSTRING_IJsonValue *map;
    HRESULT hr;

    if (FAILED(hr = IJsonObject_QueryInterface( impl->owner, &IID_IMap_HSTRING_IJsonValue, (void **)&map ))) return hr;
    hr = IMap_HSTRING_IJsonValue_get_Size( map, size );
    IMap_HSTRING_IJsonValue_Release( map );
    return hr;
}

static HRESULT WINAPI json_mapview_HasKey( IMapView_HSTRING_IJsonValue *iface, HSTRING key, boolean *found )
{
    struct json_mapview *impl = impl_from_json_mapview( iface );
    IMap_HSTRING_IJsonValue *map;
    HRESULT hr;

    if (FAILED(hr = IJsonObject_QueryInterface( impl->owner, &IID_IMap_HSTRING_IJsonValue, (void **)&map ))) return hr;
    hr = IMap_HSTRING_IJsonValue_HasKey( map, key, found );
    IMap_HSTRING_IJsonValue_Release( map );
    return hr;
}

static HRESULT WINAPI json_mapview_Split( IMapView_HSTRING_IJsonValue *iface, IMapView_HSTRING_IJsonValue **first,
                                          IMapView_HSTRING_IJsonValue **second )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct IMapView_HSTRING_IJsonValueVtbl json_mapview_vtbl =
{
    json_mapview_QueryInterface,
    json_mapview_AddRef,
    json_mapview_Release,
    json_mapview_GetIids,
    json_mapview_GetRuntimeClassName,
    json_mapview_GetTrustLevel,
    json_mapview_Lookup,
    json_mapview_get_Size,
    json_mapview_HasKey,
    json_mapview_Split,
};

static HRESULT WINAPI json_object_map_GetView( IMap_HSTRING_IJsonValue *iface, IMapView_HSTRING_IJsonValue **view )
{
    struct json_object *impl = impl_from_json_object_map( iface );
    struct json_mapview *mapview;

    if (!view) return E_POINTER;
    *view = NULL;
    if (!(mapview = calloc( 1, sizeof(*mapview) ))) return E_OUTOFMEMORY;
    mapview->iface.lpVtbl = &json_mapview_vtbl;
    mapview->ref = 1;
    mapview->owner = &impl->IJsonObject_iface;
    IJsonObject_AddRef( mapview->owner );
    *view = &mapview->iface;
    return S_OK;
}

/* IKeyValuePair<HSTRING, IJsonValue> over an IKeyValuePair<HSTRING, IInspectable> from the members. */
struct json_pair
{
    IKeyValuePair_HSTRING_IJsonValue iface;
    LONG ref;
    IKeyValuePair_HSTRING_IInspectable *inner;
};

static inline struct json_pair *impl_from_json_pair( IKeyValuePair_HSTRING_IJsonValue *iface )
{
    return CONTAINING_RECORD( iface, struct json_pair, iface );
}

static HRESULT WINAPI json_pair_QueryInterface( IKeyValuePair_HSTRING_IJsonValue *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_IJsonValue ))
    {
        *out = iface;
        IKeyValuePair_HSTRING_IJsonValue_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI json_pair_AddRef( IKeyValuePair_HSTRING_IJsonValue *iface )
{
    return InterlockedIncrement( &impl_from_json_pair( iface )->ref );
}

static ULONG WINAPI json_pair_Release( IKeyValuePair_HSTRING_IJsonValue *iface )
{
    struct json_pair *impl = impl_from_json_pair( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IKeyValuePair_HSTRING_IInspectable_Release( impl->inner );
        free( impl );
    }
    return ref;
}

JSON_NOTIMPL_IINSPECTABLE( json_pair, IKeyValuePair_HSTRING_IJsonValue )

static HRESULT WINAPI json_pair_get_Key( IKeyValuePair_HSTRING_IJsonValue *iface, HSTRING *key )
{
    return IKeyValuePair_HSTRING_IInspectable_get_Key( impl_from_json_pair( iface )->inner, key );
}

static HRESULT WINAPI json_pair_get_Value( IKeyValuePair_HSTRING_IJsonValue *iface, IJsonValue **value )
{
    IInspectable *inspectable;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = IKeyValuePair_HSTRING_IInspectable_get_Value( impl_from_json_pair( iface )->inner, &inspectable )))
        return hr;
    hr = IInspectable_QueryInterface( inspectable, &IID_IJsonValue, (void **)value );
    IInspectable_Release( inspectable );
    return hr;
}

static const struct IKeyValuePair_HSTRING_IJsonValueVtbl json_pair_vtbl =
{
    json_pair_QueryInterface,
    json_pair_AddRef,
    json_pair_Release,
    json_pair_GetIids,
    json_pair_GetRuntimeClassName,
    json_pair_GetTrustLevel,
    json_pair_get_Key,
    json_pair_get_Value,
};

/* IIterator<IKeyValuePair<HSTRING, IJsonValue>> over the members' iterator. */
struct json_pair_iterator
{
    IIterator_IKeyValuePair_HSTRING_IJsonValue iface;
    LONG ref;
    IIterator_IKeyValuePair_HSTRING_IInspectable *inner;
};

static inline struct json_pair_iterator *impl_from_json_pair_iterator( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface )
{
    return CONTAINING_RECORD( iface, struct json_pair_iterator, iface );
}

static HRESULT WINAPI json_pair_iterator_QueryInterface( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_IJsonValue ))
    {
        *out = iface;
        IIterator_IKeyValuePair_HSTRING_IJsonValue_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI json_pair_iterator_AddRef( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface )
{
    return InterlockedIncrement( &impl_from_json_pair_iterator( iface )->ref );
}

static ULONG WINAPI json_pair_iterator_Release( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface )
{
    struct json_pair_iterator *impl = impl_from_json_pair_iterator( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IIterator_IKeyValuePair_HSTRING_IInspectable_Release( impl->inner );
        free( impl );
    }
    return ref;
}

JSON_NOTIMPL_IINSPECTABLE( json_pair_iterator, IIterator_IKeyValuePair_HSTRING_IJsonValue )

static HRESULT WINAPI json_pair_iterator_get_Current( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface,
                                                      IKeyValuePair_HSTRING_IJsonValue **value )
{
    struct json_pair_iterator *impl = impl_from_json_pair_iterator( iface );
    IKeyValuePair_HSTRING_IInspectable *inner;
    struct json_pair *pair;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = IIterator_IKeyValuePair_HSTRING_IInspectable_get_Current( impl->inner, &inner ))) return hr;
    if (!(pair = calloc( 1, sizeof(*pair) )))
    {
        IKeyValuePair_HSTRING_IInspectable_Release( inner );
        return E_OUTOFMEMORY;
    }
    pair->iface.lpVtbl = &json_pair_vtbl;
    pair->ref = 1;
    pair->inner = inner;
    *value = &pair->iface;
    return S_OK;
}

static HRESULT WINAPI json_pair_iterator_get_HasCurrent( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface, boolean *value )
{
    return IIterator_IKeyValuePair_HSTRING_IInspectable_get_HasCurrent( impl_from_json_pair_iterator( iface )->inner, value );
}

static HRESULT WINAPI json_pair_iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface, boolean *value )
{
    return IIterator_IKeyValuePair_HSTRING_IInspectable_MoveNext( impl_from_json_pair_iterator( iface )->inner, value );
}

static HRESULT WINAPI json_pair_iterator_GetMany( IIterator_IKeyValuePair_HSTRING_IJsonValue *iface, UINT32 size,
                                                  IKeyValuePair_HSTRING_IJsonValue **items, UINT32 *count )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static const struct IIterator_IKeyValuePair_HSTRING_IJsonValueVtbl json_pair_iterator_vtbl =
{
    json_pair_iterator_QueryInterface,
    json_pair_iterator_AddRef,
    json_pair_iterator_Release,
    json_pair_iterator_GetIids,
    json_pair_iterator_GetRuntimeClassName,
    json_pair_iterator_GetTrustLevel,
    json_pair_iterator_get_Current,
    json_pair_iterator_get_HasCurrent,
    json_pair_iterator_MoveNext,
    json_pair_iterator_GetMany,
};

static HRESULT WINAPI json_object_iterable_First( IIterable_IKeyValuePair_HSTRING_IJsonValue *iface,
                                                  IIterator_IKeyValuePair_HSTRING_IJsonValue **value )
{
    struct json_object *impl = impl_from_json_object_iterable( iface );
    IIterable_IKeyValuePair_HSTRING_IInspectable *iterable;
    IIterator_IKeyValuePair_HSTRING_IInspectable *inner;
    struct json_pair_iterator *iter;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = IMap_HSTRING_IInspectable_QueryInterface( impl->members, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable,
                                                              (void **)&iterable ))) return hr;
    hr = IIterable_IKeyValuePair_HSTRING_IInspectable_First( iterable, &inner );
    IIterable_IKeyValuePair_HSTRING_IInspectable_Release( iterable );
    if (FAILED(hr)) return hr;
    if (!(iter = calloc( 1, sizeof(*iter) )))
    {
        IIterator_IKeyValuePair_HSTRING_IInspectable_Release( inner );
        return E_OUTOFMEMORY;
    }
    iter->iface.lpVtbl = &json_pair_iterator_vtbl;
    iter->ref = 1;
    iter->inner = inner;
    *value = &iter->iface;
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

DEFINE_IINSPECTABLE( json_object_statics, IJsonObjectStatics, struct json_object_statics, IActivationFactory_iface )

static HRESULT WINAPI json_object_statics_Parse( IJsonObjectStatics *iface, HSTRING input, IJsonObject **value )
{
    IJsonValue *parsed;
    HRESULT hr;

    TRACE( "iface %p, input %s, value %p.\n", iface, debugstr_hstring( input ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = json_parse( input, &parsed ))) return hr;
    hr = IJsonValue_GetObject( parsed, value );
    IJsonValue_Release( parsed );
    return FAILED(hr) ? WEB_E_JSON_VALUE_NOT_FOUND : S_OK;
}

static HRESULT WINAPI json_object_statics_TryParse( IJsonObjectStatics *iface, HSTRING input, IJsonObject **result, boolean *succeeded )
{
    TRACE( "iface %p, input %s, result %p, succeeded %p.\n", iface, debugstr_hstring( input ), result, succeeded );

    if (!result || !succeeded) return E_POINTER;
    *succeeded = SUCCEEDED(json_object_statics_Parse( iface, input, result ));
    return S_OK;
}

static const struct IJsonObjectStaticsVtbl json_object_statics_vtbl =
{
    json_object_statics_QueryInterface,
    json_object_statics_AddRef,
    json_object_statics_Release,
    /* IInspectable methods */
    json_object_statics_GetIids,
    json_object_statics_GetRuntimeClassName,
    json_object_statics_GetTrustLevel,
    /* IJsonObjectStatics methods */
    json_object_statics_Parse,
    json_object_statics_TryParse,
};

static struct json_object_statics json_object_statics =
{
    {&factory_vtbl},
    {&json_object_statics_vtbl},
    1,
};

IActivationFactory *json_object_factory = &json_object_statics.IActivationFactory_iface;

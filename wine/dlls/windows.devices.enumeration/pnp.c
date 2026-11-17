/* WinRT Windows.Devices.Enumeration.Pnp.PnpObject implementation
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
#include "devquery.h"
#include "aqs.h"
#include "winreg.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(enumeration);

/* The computer's own device container: its model and manufacturer are the machine's (Minecraft asks for them). */
static const WCHAR root_container_id[] = L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";

struct pnp_object
{
    IPnpObject IPnpObject_iface;
    LONG ref;
    PnpObjectType type;
    HSTRING id;
    IMap_HSTRING_IInspectable *properties;
};

static inline struct pnp_object *impl_from_IPnpObject( IPnpObject *iface )
{
    return CONTAINING_RECORD( iface, struct pnp_object, IPnpObject_iface );
}

static HRESULT WINAPI pnp_object_QueryInterface( IPnpObject *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPnpObject ))
    {
        *out = iface;
        IPnpObject_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI pnp_object_AddRef( IPnpObject *iface )
{
    struct pnp_object *impl = impl_from_IPnpObject( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI pnp_object_Release( IPnpObject *iface )
{
    struct pnp_object *impl = impl_from_IPnpObject( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->id );
        IMap_HSTRING_IInspectable_Release( impl->properties );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI pnp_object_GetIids( IPnpObject *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI pnp_object_GetRuntimeClassName( IPnpObject *iface, HSTRING *class_name )
{
    const WCHAR *name = RuntimeClass_Windows_Devices_Enumeration_Pnp_PnpObject;
    TRACE( "iface %p, class_name %p\n", iface, class_name );
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI pnp_object_GetTrustLevel( IPnpObject *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI pnp_object_get_Type( IPnpObject *iface, PnpObjectType *value )
{
    *value = impl_from_IPnpObject( iface )->type;
    return S_OK;
}

static HRESULT WINAPI pnp_object_get_Id( IPnpObject *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IPnpObject( iface )->id, value );
}

static HRESULT WINAPI pnp_object_get_Properties( IPnpObject *iface, IMapView_HSTRING_IInspectable **value )
{
    return IMap_HSTRING_IInspectable_GetView( impl_from_IPnpObject( iface )->properties, value );
}

static HRESULT WINAPI pnp_object_Update( IPnpObject *iface, IPnpObjectUpdate *info )
{
    FIXME( "iface %p, info %p stub!\n", iface, info );
    return S_OK;
}

static const IPnpObjectVtbl pnp_object_vtbl =
{
    pnp_object_QueryInterface,
    pnp_object_AddRef,
    pnp_object_Release,
    pnp_object_GetIids,
    pnp_object_GetRuntimeClassName,
    pnp_object_GetTrustLevel,
    pnp_object_get_Type,
    pnp_object_get_Id,
    pnp_object_get_Properties,
    pnp_object_Update,
};

static HRESULT pnp_object_create( PnpObjectType type, const WCHAR *id, const DEVPROPERTY *props, ULONG len, IPnpObject **out )
{
    struct pnp_object *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPnpObject_iface.lpVtbl = &pnp_object_vtbl;
    impl->ref = 1;
    impl->type = type;
    if (FAILED(hr = create_device_properties( props, len, &impl->properties )))
    {
        free( impl );
        return hr;
    }
    if (FAILED(hr = WindowsCreateString( id, wcslen( id ), &impl->id )))
    {
        IMap_HSTRING_IInspectable_Release( impl->properties );
        free( impl );
        return hr;
    }
    *out = &impl->IPnpObject_iface;
    return S_OK;
}

/* the parameters of one query: devquery_params, plus the object id for CreateFromIdAsync */
struct pnp_query
{
    IUnknown IUnknown_iface;
    LONG ref;
    IUnknown *params;
    WCHAR *id;
};

static inline struct pnp_query *pnp_query_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct pnp_query, IUnknown_iface );
}

static HRESULT WINAPI pnp_query_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI pnp_query_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &pnp_query_from_IUnknown( iface )->ref );
}

static ULONG WINAPI pnp_query_Release( IUnknown *iface )
{
    struct pnp_query *impl = pnp_query_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IUnknown_Release( impl->params );
        free( impl->id );
        free( impl );
    }
    return ref;
}

static const IUnknownVtbl pnp_query_vtbl =
{
    pnp_query_QueryInterface,
    pnp_query_AddRef,
    pnp_query_Release,
};

static HRESULT pnp_query_create( PnpObjectType type, HSTRING id, IIterable_HSTRING *requested, HSTRING filter, IUnknown **out )
{
    DEVPROPCOMPKEY *prop_keys = NULL;
    struct aqs_expr *expr = NULL;
    ULONG prop_keys_len = 0;
    struct pnp_query *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUnknown_iface.lpVtbl = &pnp_query_vtbl;
    impl->ref = 1;
    if (id && !(impl->id = wcsdup( WindowsGetStringRawBuffer( id, NULL ) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    if ((requested && FAILED(hr = devpropcompkeys_append_names( &prop_keys, &prop_keys_len, requested ))) ||
        FAILED(hr = aqs_parse_query( WindowsGetStringRawBuffer( filter, NULL ), &expr, NULL )) ||
        FAILED(hr = devquery_params_create( (DEV_OBJECT_TYPE)type, expr, prop_keys, prop_keys_len, &impl->params )))
    {
        free( prop_keys );
        free_aqs_expr( expr );
        free( impl->id );
        free( impl );
        return hr;
    }
    *out = &impl->IUnknown_iface;
    return S_OK;
}

static BOOL read_bios_string( const WCHAR *name, WCHAR *buffer, DWORD size )
{
    size *= sizeof(WCHAR);
    return !RegGetValueW( HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, NULL,
                          buffer, &size ) && *buffer;
}

/* the computer's container, whatever cfgmgr32 knows of containers: its model and manufacturer from SMBIOS */
static HRESULT root_container_create( const struct devquery_params *params, IPnpObject **out )
{
    static const struct { const WCHAR *property, *bios; } values[] =
    {
        { L"System.Devices.ModelName", L"SystemProductName" },
        { L"System.Devices.Manufacturer", L"SystemManufacturer" },
    };
    DEVPROPERTY props[ARRAY_SIZE(values)];
    WCHAR strings[ARRAY_SIZE(values)][256];
    ULONG i, j, count = 0;
    HRESULT hr;

    for (i = 0; i < params->prop_keys_len; i++)
    {
        WCHAR *name;

        if (FAILED(PSGetNameFromPropertyKey( (PROPERTYKEY *)&params->prop_keys[i].Key, &name ))) continue;
        for (j = 0; j < ARRAY_SIZE(values) && count < ARRAY_SIZE(props); j++)
        {
            if (wcscmp( name, values[j].property ) || !read_bios_string( values[j].bios, strings[count], 256 )) continue;
            props[count].CompKey = params->prop_keys[i];
            props[count].Type = DEVPROP_TYPE_STRING;
            props[count].BufferSize = (wcslen( strings[count] ) + 1) * sizeof(WCHAR);
            props[count].Buffer = strings[count];
            count++;
        }
        CoTaskMemFree( name );
    }
    hr = pnp_object_create( PnpObjectType_DeviceContainer, root_container_id, props, count, out );
    TRACE( "computer container with %lu properties: %#lx\n", count, hr );
    return hr;
}

static HRESULT create_from_id_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct pnp_query *query = pnp_query_from_IUnknown( param );
    const struct devquery_params *params = CONTAINING_RECORD( query->params, struct devquery_params, IUnknown_iface );
    const DEVPROPERTY *props;
    IPnpObject *object;
    ULONG len;
    HRESULT hr;

    if (!called_async) return STATUS_PENDING;

    TRACE( "type %d, id %s\n", params->type, debugstr_w(query->id) );

    if (params->type == DevObjectTypeDeviceContainer && !wcsicmp( query->id, root_container_id ))
        hr = root_container_create( params, &object );
    else if (SUCCEEDED(hr = DevGetObjectProperties( params->type, query->id, DevQueryFlagNone, params->prop_keys_len,
                                                    params->prop_keys, &len, &props )))
    {
        hr = pnp_object_create( params->type, query->id, props, len, &object );
        DevFreeObjectProperties( len, props );
    }
    if (FAILED(hr)) return hr;

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)object;
    return S_OK;
}

static HRESULT find_all_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    static const struct vector_iids iids =
    {
        .vector = &IID_IVector_IInspectable,
        .view = &IID_IVectorView_PnpObject,
        .iterable = &IID_IIterable_PnpObject,
        .iterator = &IID_IIterator_PnpObject,
    };
    struct pnp_query *query = pnp_query_from_IUnknown( param );
    const struct devquery_params *params = CONTAINING_RECORD( query->params, struct devquery_params, IUnknown_iface );
    const DEVPROP_FILTER_EXPRESSION *filters = NULL;
    IVectorView_PnpObject *view;
    IVector_IInspectable *vector;
    ULONG filters_len = 0, len, i;
    const DEV_OBJECT *objects;
    HRESULT hr;

    if (!called_async) return STATUS_PENDING;

    if (params->expr)
    {
        filters = params->expr->filters;
        filters_len = params->expr->len;
    }
    if (FAILED(hr = vector_create( &iids, (void *)&vector ))) return hr;
    if (SUCCEEDED(hr = DevGetObjects( params->type, DevQueryFlagNone, params->prop_keys_len, params->prop_keys,
                                      filters_len, filters, &len, &objects )))
    {
        TRACE( "type %d: %lu objects\n", params->type, len );
        for (i = 0; i < len && SUCCEEDED(hr); i++)
        {
            IPnpObject *object;
            if (SUCCEEDED(hr = pnp_object_create( (PnpObjectType)objects[i].ObjectType, objects[i].pszObjectId,
                                                  objects[i].pProperties, objects[i].cPropertyCount, &object )))
            {
                hr = IVector_IInspectable_Append( vector, (IInspectable *)object );
                IPnpObject_Release( object );
            }
        }
        DevFreeObjects( len, objects );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( vector, (void *)&view );
    IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

struct pnp_object_statics
{
    IActivationFactory IActivationFactory_iface;
    IPnpObjectStatics IPnpObjectStatics_iface;
    LONG ref;
};

static inline struct pnp_object_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct pnp_object_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct pnp_object_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IActivationFactory_AddRef( iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IPnpObjectStatics ))
    {
        *out = &impl->IPnpObjectStatics_iface;
        IActivationFactory_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
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
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( statics, IPnpObjectStatics, struct pnp_object_statics, IActivationFactory_iface );

static HRESULT WINAPI statics_CreateFromIdAsync( IPnpObjectStatics *iface, PnpObjectType type, HSTRING id,
                                                 IIterable_HSTRING *requested, IAsyncOperation_PnpObject **op )
{
    IUnknown *query;
    HRESULT hr;

    TRACE( "iface %p, type %d, id %s, requested %p, op %p\n", iface, type, debugstr_hstring(id), requested, op );

    if (!op) return E_POINTER;
    if (FAILED(hr = pnp_query_create( type, id, requested, NULL, &query ))) return hr;
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_PnpObject, (IUnknown *)iface, query,
                                             create_from_id_async, (IAsyncOperation_IInspectable **)op );
    IUnknown_Release( query );
    return hr;
}

static HRESULT WINAPI statics_FindAllAsyncAqsFilter( IPnpObjectStatics *iface, PnpObjectType type, IIterable_HSTRING *requested,
                                                     HSTRING filter, IAsyncOperation_PnpObjectCollection **op )
{
    IUnknown *query;
    HRESULT hr;

    TRACE( "iface %p, type %d, requested %p, filter %s, op %p\n", iface, type, requested, debugstr_hstring(filter), op );

    if (!op) return E_POINTER;
    if (FAILED(hr = pnp_query_create( type, NULL, requested, filter, &query ))) return hr;
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_PnpObjectCollection, (IUnknown *)iface, query,
                                             find_all_async, (IAsyncOperation_IInspectable **)op );
    IUnknown_Release( query );
    return hr;
}

static HRESULT WINAPI statics_FindAllAsync( IPnpObjectStatics *iface, PnpObjectType type, IIterable_HSTRING *requested,
                                            IAsyncOperation_PnpObjectCollection **op )
{
    return statics_FindAllAsyncAqsFilter( iface, type, requested, NULL, op );
}

static HRESULT WINAPI statics_CreateWatcherAqsFilter( IPnpObjectStatics *iface, PnpObjectType type, IIterable_HSTRING *requested,
                                                      HSTRING filter, IPnpObjectWatcher **watcher )
{
    FIXME( "iface %p, type %d, requested %p, filter %s, watcher %p stub!\n", iface, type, requested,
           debugstr_hstring(filter), watcher );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_CreateWatcher( IPnpObjectStatics *iface, PnpObjectType type, IIterable_HSTRING *requested,
                                             IPnpObjectWatcher **watcher )
{
    return statics_CreateWatcherAqsFilter( iface, type, requested, NULL, watcher );
}

static const struct IPnpObjectStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    statics_CreateFromIdAsync,
    statics_FindAllAsync,
    statics_FindAllAsyncAqsFilter,
    statics_CreateWatcher,
    statics_CreateWatcherAqsFilter,
};

static struct pnp_object_statics pnp_object_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1
};

IActivationFactory *pnp_object_factory = &pnp_object_statics.IActivationFactory_iface;

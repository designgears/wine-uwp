/* Windows.Storage.ApplicationDataContainerSettings
 *
 * The settings map differs from a plain PropertySet in one way that matters: looking up a key that
 * is not there succeeds and returns null, instead of failing with E_BOUNDS. Apps rely on it -
 * Calculator reads its saved theme with a plain indexer on first launch, and a failure there
 * becomes a KeyNotFoundException that escapes OnLaunched. So the settings are a thin wrapper over
 * a PropertySet that changes that one answer.
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

#include <stdlib.h>

#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(data);

struct settings
{
    IPropertySet IPropertySet_iface;
    IMap_HSTRING_IInspectable IMap_HSTRING_IInspectable_iface;
    IObservableMap_HSTRING_IInspectable IObservableMap_HSTRING_IInspectable_iface;
    IIterable_IKeyValuePair_HSTRING_IInspectable IIterable_IKeyValuePair_HSTRING_IInspectable_iface;
    LONG ref;
    const WCHAR *class_name;
    BOOL container;            /* a container's Values, whose changes are persisted */

    IPropertySet *inner;
    IMap_HSTRING_IInspectable *inner_map;
    IObservableMap_HSTRING_IInspectable *inner_observable;
    IIterable_IKeyValuePair_HSTRING_IInspectable *inner_iterable;
};

static inline struct settings *impl_from_IPropertySet( IPropertySet *iface )
{
    return CONTAINING_RECORD( iface, struct settings, IPropertySet_iface );
}

static HRESULT WINAPI settings_QueryInterface( IPropertySet *iface, REFIID iid, void **out )
{
    struct settings *impl = impl_from_IPropertySet( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPropertySet ))
        *out = &impl->IPropertySet_iface;
    else if (IsEqualGUID( iid, &IID_IMap_HSTRING_IInspectable ))
        *out = &impl->IMap_HSTRING_IInspectable_iface;
    else if (IsEqualGUID( iid, &IID_IObservableMap_HSTRING_IInspectable ))
        *out = &impl->IObservableMap_HSTRING_IInspectable_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable ))
        *out = &impl->IIterable_IKeyValuePair_HSTRING_IInspectable_iface;
    else
    {
        /* anything else - IMapView and the like - is the inner set's business */
        TRACE( "forwarding %s to the property set.\n", debugstr_guid( iid ) );
        return IPropertySet_QueryInterface( impl->inner, iid, out );
    }

    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI settings_AddRef( IPropertySet *iface )
{
    return InterlockedIncrement( &impl_from_IPropertySet( iface )->ref );
}

static ULONG WINAPI settings_Release( IPropertySet *iface )
{
    struct settings *impl = impl_from_IPropertySet( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IIterable_IKeyValuePair_HSTRING_IInspectable_Release( impl->inner_iterable );
        IObservableMap_HSTRING_IInspectable_Release( impl->inner_observable );
        IMap_HSTRING_IInspectable_Release( impl->inner_map );
        IPropertySet_Release( impl->inner );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI settings_GetIids( IPropertySet *iface, ULONG *count, IID **iids )
{
    struct settings *impl = impl_from_IPropertySet( iface );
    return IPropertySet_GetIids( impl->inner, count, iids );
}

/* .NET types the wrapper from this name, so a composite value must say it is one. */
static HRESULT WINAPI settings_GetRuntimeClassName( IPropertySet *iface, HSTRING *name )
{
    struct settings *impl = impl_from_IPropertySet( iface );
    return WindowsCreateString( impl->class_name, wcslen( impl->class_name ), name );
}

static HRESULT WINAPI settings_GetTrustLevel( IPropertySet *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static const struct IPropertySetVtbl settings_vtbl =
{
    settings_QueryInterface,
    settings_AddRef,
    settings_Release,
    settings_GetIids,
    settings_GetRuntimeClassName,
    settings_GetTrustLevel,
};

DEFINE_IINSPECTABLE( settings_map, IMap_HSTRING_IInspectable, struct settings, IPropertySet_iface )

/* A copy of the current values. Windows' ApplicationDataContainerSettings is a view of the settings store, and
 * enumerating it walks what was there when the enumeration began: the Microsoft Store removes stale cached
 * accounts while enumerating LocalSettings.Values, which a live PropertySet answers with E_CHANGED_STATE. */
static HRESULT settings_snapshot( struct settings *impl, IPropertySet **out )
{
    static const WCHAR class[] = L"Windows.Foundation.Collections.PropertySet";
    IIterator_IKeyValuePair_HSTRING_IInspectable *iter;
    IMap_HSTRING_IInspectable *map;
    IInspectable *inspectable;
    HSTRING_HEADER header;
    HSTRING name;
    boolean more;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = WindowsCreateStringReference( class, ARRAY_SIZE(class) - 1, &header, &name )) ||
        FAILED(hr = RoActivateInstance( name, &inspectable ))) return hr;
    hr = IInspectable_QueryInterface( inspectable, &IID_IPropertySet, (void **)out );
    IInspectable_Release( inspectable );
    if (FAILED(hr)) return hr;
    if (FAILED(hr = IPropertySet_QueryInterface( *out, &IID_IMap_HSTRING_IInspectable, (void **)&map ))) goto failed;
    if (SUCCEEDED(hr = IIterable_IKeyValuePair_HSTRING_IInspectable_First( impl->inner_iterable, &iter )))
    {
        for (hr = IIterator_IKeyValuePair_HSTRING_IInspectable_get_HasCurrent( iter, &more );
             SUCCEEDED(hr) && more; hr = IIterator_IKeyValuePair_HSTRING_IInspectable_MoveNext( iter, &more ))
        {
            IKeyValuePair_HSTRING_IInspectable *pair;
            IInspectable *value = NULL;
            HSTRING key = NULL;
            boolean replaced;

            if (FAILED(hr = IIterator_IKeyValuePair_HSTRING_IInspectable_get_Current( iter, &pair ))) break;
            if (SUCCEEDED(hr = IKeyValuePair_HSTRING_IInspectable_get_Key( pair, &key )) &&
                SUCCEEDED(hr = IKeyValuePair_HSTRING_IInspectable_get_Value( pair, &value )))
                hr = IMap_HSTRING_IInspectable_Insert( map, key, value, &replaced );
            if (value) IInspectable_Release( value );
            WindowsDeleteString( key );
            IKeyValuePair_HSTRING_IInspectable_Release( pair );
            if (FAILED(hr)) break;
        }
        IIterator_IKeyValuePair_HSTRING_IInspectable_Release( iter );
    }
    IMap_HSTRING_IInspectable_Release( map );
    if (SUCCEEDED(hr)) return S_OK;
failed:
    IPropertySet_Release( *out );
    *out = NULL;
    return hr;
}

static HRESULT WINAPI settings_map_Lookup( IMap_HSTRING_IInspectable *iface, HSTRING key, IInspectable **value )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    boolean found = FALSE;
    HRESULT hr;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = IMap_HSTRING_IInspectable_HasKey( impl->inner_map, key, &found ))) return hr;
    if (!found)
    {
        TRACE( "no setting %s, returning null.\n", debugstr_hstring( key ) );
        return S_OK;
    }
    return IMap_HSTRING_IInspectable_Lookup( impl->inner_map, key, value );
}

static HRESULT WINAPI settings_map_get_Size( IMap_HSTRING_IInspectable *iface, UINT32 *size )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    return IMap_HSTRING_IInspectable_get_Size( impl->inner_map, size );
}

static HRESULT WINAPI settings_map_HasKey( IMap_HSTRING_IInspectable *iface, HSTRING key, boolean *found )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    HRESULT hr = IMap_HSTRING_IInspectable_HasKey( impl->inner_map, key, found );
    TRACE( "key %s -> %d.\n", debugstr_hstring( key ), found ? *found : 0 );
    return hr;
}

static HRESULT WINAPI settings_map_GetView( IMap_HSTRING_IInspectable *iface, IMapView_HSTRING_IInspectable **view )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    IMap_HSTRING_IInspectable *map;
    IPropertySet *copy;
    HRESULT hr;

    if (!view) return E_POINTER;
    if (FAILED(hr = settings_snapshot( impl, &copy ))) return hr;
    if (SUCCEEDED(hr = IPropertySet_QueryInterface( copy, &IID_IMap_HSTRING_IInspectable, (void **)&map )))
    {
        hr = IMap_HSTRING_IInspectable_GetView( map, view );
        IMap_HSTRING_IInspectable_Release( map );
    }
    IPropertySet_Release( copy );
    return hr;
}

static HRESULT WINAPI settings_map_Insert( IMap_HSTRING_IInspectable *iface, HSTRING key,
                                           IInspectable *value, boolean *replaced )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    HRESULT hr;

    IInspectable *copy;

    TRACE( "key %s, value %p.\n", debugstr_hstring( key ), value );
    if (FAILED(hr = copy_setting_value( value, &copy ))) return hr;
    hr = IMap_HSTRING_IInspectable_Insert( impl->inner_map, key, copy, replaced );
    if (copy) IInspectable_Release( copy );
    if (SUCCEEDED(hr) && impl->container) settings_changed();
    return hr;
}

static HRESULT WINAPI settings_map_Remove( IMap_HSTRING_IInspectable *iface, HSTRING key )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    HRESULT hr = IMap_HSTRING_IInspectable_Remove( impl->inner_map, key );
    if (SUCCEEDED(hr) && impl->container) settings_changed();
    return hr;
}

static HRESULT WINAPI settings_map_Clear( IMap_HSTRING_IInspectable *iface )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    HRESULT hr = IMap_HSTRING_IInspectable_Clear( impl->inner_map );
    if (SUCCEEDED(hr) && impl->container) settings_changed();
    return hr;
}

static const struct IMap_HSTRING_IInspectableVtbl settings_map_vtbl =
{
    settings_map_QueryInterface,
    settings_map_AddRef,
    settings_map_Release,
    settings_map_GetIids,
    settings_map_GetRuntimeClassName,
    settings_map_GetTrustLevel,
    settings_map_Lookup,
    settings_map_get_Size,
    settings_map_HasKey,
    settings_map_GetView,
    settings_map_Insert,
    settings_map_Remove,
    settings_map_Clear,
};

DEFINE_IINSPECTABLE( settings_observable, IObservableMap_HSTRING_IInspectable, struct settings,
                     IPropertySet_iface )

static HRESULT WINAPI settings_observable_add_MapChanged( IObservableMap_HSTRING_IInspectable *iface,
        __FIMapChangedEventHandler_2_HSTRING_IInspectable *handler, EventRegistrationToken *token )
{
    struct settings *impl = impl_from_IObservableMap_HSTRING_IInspectable( iface );
    return IObservableMap_HSTRING_IInspectable_add_MapChanged( impl->inner_observable, handler, token );
}

static HRESULT WINAPI settings_observable_remove_MapChanged( IObservableMap_HSTRING_IInspectable *iface,
        EventRegistrationToken token )
{
    struct settings *impl = impl_from_IObservableMap_HSTRING_IInspectable( iface );
    return IObservableMap_HSTRING_IInspectable_remove_MapChanged( impl->inner_observable, token );
}

static const struct IObservableMap_HSTRING_IInspectableVtbl settings_observable_vtbl =
{
    settings_observable_QueryInterface,
    settings_observable_AddRef,
    settings_observable_Release,
    settings_observable_GetIids,
    settings_observable_GetRuntimeClassName,
    settings_observable_GetTrustLevel,
    settings_observable_add_MapChanged,
    settings_observable_remove_MapChanged,
};

DEFINE_IINSPECTABLE( settings_iterable, IIterable_IKeyValuePair_HSTRING_IInspectable, struct settings,
                     IPropertySet_iface )

static HRESULT WINAPI settings_iterable_First( IIterable_IKeyValuePair_HSTRING_IInspectable *iface,
        IIterator_IKeyValuePair_HSTRING_IInspectable **value )
{
    struct settings *impl = impl_from_IIterable_IKeyValuePair_HSTRING_IInspectable( iface );
    IIterable_IKeyValuePair_HSTRING_IInspectable *iterable;
    IPropertySet *copy;
    HRESULT hr;

    if (!value) return E_POINTER;
    if (FAILED(hr = settings_snapshot( impl, &copy ))) return hr;
    if (SUCCEEDED(hr = IPropertySet_QueryInterface( copy, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable,
                                                    (void **)&iterable )))
    {
        hr = IIterable_IKeyValuePair_HSTRING_IInspectable_First( iterable, value );
        IIterable_IKeyValuePair_HSTRING_IInspectable_Release( iterable );
    }
    IPropertySet_Release( copy );
    return hr;
}

static const struct IIterable_IKeyValuePair_HSTRING_IInspectableVtbl settings_iterable_vtbl =
{
    settings_iterable_QueryInterface,
    settings_iterable_AddRef,
    settings_iterable_Release,
    settings_iterable_GetIids,
    settings_iterable_GetRuntimeClassName,
    settings_iterable_GetTrustLevel,
    settings_iterable_First,
};

static HRESULT settings_create_class( IPropertySet *inner, const WCHAR *class_name, IPropertySet **out );

HRESULT settings_create( IPropertySet *inner, IPropertySet **out )
{
    HRESULT hr = settings_create_class( inner, L"Windows.Storage.ApplicationDataContainerSettings", out );
    if (SUCCEEDED(hr)) CONTAINING_RECORD( *out, struct settings, IPropertySet_iface )->container = TRUE;
    return hr;
}

static HRESULT settings_create_class( IPropertySet *inner, const WCHAR *class_name, IPropertySet **out )
{
    struct settings *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPropertySet_iface.lpVtbl = &settings_vtbl;
    impl->IMap_HSTRING_IInspectable_iface.lpVtbl = &settings_map_vtbl;
    impl->IObservableMap_HSTRING_IInspectable_iface.lpVtbl = &settings_observable_vtbl;
    impl->IIterable_IKeyValuePair_HSTRING_IInspectable_iface.lpVtbl = &settings_iterable_vtbl;
    impl->ref = 1;
    impl->class_name = class_name;

    if (FAILED(hr = IPropertySet_QueryInterface( inner, &IID_IMap_HSTRING_IInspectable,
                                                 (void **)&impl->inner_map )) ||
        FAILED(hr = IPropertySet_QueryInterface( inner, &IID_IObservableMap_HSTRING_IInspectable,
                                                 (void **)&impl->inner_observable )) ||
        FAILED(hr = IPropertySet_QueryInterface( inner, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable,
                                                 (void **)&impl->inner_iterable )))
    {
        if (impl->inner_observable) IObservableMap_HSTRING_IInspectable_Release( impl->inner_observable );
        if (impl->inner_map) IMap_HSTRING_IInspectable_Release( impl->inner_map );
        free( impl );
        return hr;
    }
    IPropertySet_AddRef( (impl->inner = inner) );

    TRACE( "created settings %p over property set %p.\n", impl, inner );
    *out = &impl->IPropertySet_iface;
    return S_OK;
}

/* Windows.Storage.ApplicationDataCompositeValue: a group of settings stored under one key, which
 * Shapr3D uses to save its sign-in session. It is a property set with the settings semantics - a
 * missing key reads as null - so it is the same wrapper over a fresh PropertySet, under its own
 * class name. Like the rest of ApplicationData here, it lives in memory only. */
static HRESULT WINAPI composite_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface;
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI composite_factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI composite_factory_Release( IActivationFactory *iface ) { return 1; }

static HRESULT WINAPI composite_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI composite_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI composite_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI composite_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    static const WCHAR property_set[] = L"Windows.Foundation.Collections.PropertySet";
    HSTRING_HEADER header;
    IInspectable *inspectable;
    IPropertySet *inner, *composite;
    HSTRING name;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    *instance = NULL;
    if (FAILED(hr = WindowsCreateStringReference( property_set, ARRAY_SIZE(property_set) - 1, &header, &name )) ||
        FAILED(hr = RoActivateInstance( name, &inspectable )))
        return hr;
    hr = IInspectable_QueryInterface( inspectable, &IID_IPropertySet, (void **)&inner );
    IInspectable_Release( inspectable );
    if (FAILED(hr)) return hr;

    hr = settings_create_class( inner, L"Windows.Storage.ApplicationDataCompositeValue", &composite );
    IPropertySet_Release( inner );
    if (SUCCEEDED(hr)) *instance = (IInspectable *)composite;
    return hr;
}

static const struct IActivationFactoryVtbl composite_factory_vtbl =
{
    composite_factory_QueryInterface,
    composite_factory_AddRef,
    composite_factory_Release,
    composite_factory_GetIids,
    composite_factory_GetRuntimeClassName,
    composite_factory_GetTrustLevel,
    composite_factory_ActivateInstance,
};

static IActivationFactory composite_factory = { &composite_factory_vtbl };
IActivationFactory *composite_value_factory = &composite_factory;

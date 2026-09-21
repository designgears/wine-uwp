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

static HRESULT WINAPI settings_GetRuntimeClassName( IPropertySet *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Storage.ApplicationDataContainerSettings";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
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
    return IMap_HSTRING_IInspectable_GetView( impl->inner_map, view );
}

static HRESULT WINAPI settings_map_Insert( IMap_HSTRING_IInspectable *iface, HSTRING key,
                                           IInspectable *value, boolean *replaced )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    TRACE( "key %s, value %p.\n", debugstr_hstring( key ), value );
    return IMap_HSTRING_IInspectable_Insert( impl->inner_map, key, value, replaced );
}

static HRESULT WINAPI settings_map_Remove( IMap_HSTRING_IInspectable *iface, HSTRING key )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    return IMap_HSTRING_IInspectable_Remove( impl->inner_map, key );
}

static HRESULT WINAPI settings_map_Clear( IMap_HSTRING_IInspectable *iface )
{
    struct settings *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    return IMap_HSTRING_IInspectable_Clear( impl->inner_map );
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
    return IIterable_IKeyValuePair_HSTRING_IInspectable_First( impl->inner_iterable, value );
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

HRESULT settings_create( IPropertySet *inner, IPropertySet **out )
{
    struct settings *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPropertySet_iface.lpVtbl = &settings_vtbl;
    impl->IMap_HSTRING_IInspectable_iface.lpVtbl = &settings_map_vtbl;
    impl->IObservableMap_HSTRING_IInspectable_iface.lpVtbl = &settings_observable_vtbl;
    impl->IIterable_IKeyValuePair_HSTRING_IInspectable_iface.lpVtbl = &settings_iterable_vtbl;
    impl->ref = 1;

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

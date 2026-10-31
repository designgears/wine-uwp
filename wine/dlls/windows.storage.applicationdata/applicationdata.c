/* WinRT Windows.Storage.ApplicationData ApplicationData Implementation
 *
 * Copyright (C) 2023 Mohamad Al-Jaf
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

WINE_DEFAULT_DEBUG_CHANNEL(data);

struct application_data_statics
{
    IActivationFactory IActivationFactory_iface;
    IApplicationDataStatics IApplicationDataStatics_iface;
    LONG ref;
};

static inline struct application_data_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct application_data_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct application_data_statics *impl = impl_from_IActivationFactory( iface );

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

    if (IsEqualGUID( iid, &IID_IApplicationDataStatics ))
    {
        *out = &impl->IApplicationDataStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct application_data_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct application_data_statics *impl = impl_from_IActivationFactory( iface );
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
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
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

struct application_data
{
    IApplicationData IApplicationData_iface;
    IApplicationData2 IApplicationData2_iface;
    IApplicationData3 IApplicationData3_iface;
    LONG ref;
};

static inline struct application_data *impl_from_IApplicationData( IApplicationData *iface )
{
    return CONTAINING_RECORD( iface, struct application_data, IApplicationData_iface );
}

static HRESULT WINAPI application_data_QueryInterface( IApplicationData *iface, REFIID iid, void **out )
{
    struct application_data *impl = impl_from_IApplicationData( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IApplicationData ))
    {
        *out = &impl->IApplicationData_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IApplicationData2 ))
    {
        *out = &impl->IApplicationData2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IApplicationData3 ))
    {
        *out = &impl->IApplicationData3_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI application_data_AddRef( IApplicationData *iface )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI application_data_Release( IApplicationData *iface )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI application_data_GetIids( IApplicationData *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_GetRuntimeClassName( IApplicationData *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_GetTrustLevel( IApplicationData *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_get_Version( IApplicationData *iface, UINT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_SetVersionAsync( IApplicationData *iface, UINT32 version, IApplicationDataSetVersionHandler *handler,
                                                        IAsyncAction **operation )
{
    FIXME( "iface %p, version %d, handler %p, operation %p stub!\n", iface, version, handler, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_ClearAllAsync( IApplicationData *iface, IAsyncAction **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_ClearAsync( IApplicationData *iface, ApplicationDataLocality locality, IAsyncAction **operation )
{
    FIXME( "iface %p, %d locality, operation %p stub!\n", iface, locality, operation );
    return E_NOTIMPL;
}

/* ApplicationDataContainer. The settings live in a PropertySet for the lifetime of the process:
 * an app reads and writes them freely, they just do not survive a restart yet.
 *
 * Child containers are the same: a container owns its children by name, so CreateContainer with
 * the same name hands back the same object and the values written through it. Shapr3D creates
 * "userPreferences" from App's constructor, and E_NOTIMPL there became a NotImplementedException
 * that killed it before the first frame. Container names compare case-insensitively, as on
 * Windows, where they are registry keys. */
struct child_container
{
    HSTRING name;
    IApplicationDataContainer *container;
};

struct data_container
{
    IApplicationDataContainer IApplicationDataContainer_iface;
    IPropertySet *values;
    ApplicationDataLocality locality;
    HSTRING name;
    LONG ref;

    CRITICAL_SECTION cs;
    struct child_container *children;
    UINT32 child_count, child_capacity;
};

static HRESULT create_data_container( ApplicationDataLocality locality, const WCHAR *name, UINT32 name_len,
                                      IApplicationDataContainer **out );

static inline struct data_container *impl_from_IApplicationDataContainer( IApplicationDataContainer *iface )
{
    return CONTAINING_RECORD( iface, struct data_container, IApplicationDataContainer_iface );
}

static HRESULT WINAPI data_container_QueryInterface( IApplicationDataContainer *iface, REFIID iid, void **out )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IApplicationDataContainer ))
    {
        *out = &impl->IApplicationDataContainer_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_container_AddRef( IApplicationDataContainer *iface )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI data_container_Release( IApplicationDataContainer *iface )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        UINT32 i;

        for (i = 0; i < impl->child_count; i++)
        {
            WindowsDeleteString( impl->children[i].name );
            IApplicationDataContainer_Release( impl->children[i].container );
        }
        free( impl->children );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        WindowsDeleteString( impl->name );
        if (impl->values) IPropertySet_Release( impl->values );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI data_container_GetIids( IApplicationDataContainer *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_container_GetRuntimeClassName( IApplicationDataContainer *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_ApplicationDataContainer,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_ApplicationDataContainer) - 1,
                                class_name );
}

static HRESULT WINAPI data_container_GetTrustLevel( IApplicationDataContainer *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI data_container_get_Name( IApplicationDataContainer *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IApplicationDataContainer( iface )->name, value );
}

static HRESULT WINAPI data_container_get_Locality( IApplicationDataContainer *iface, ApplicationDataLocality *value )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->locality;
    return S_OK;
}

static HRESULT WINAPI data_container_get_Values( IApplicationDataContainer *iface, IPropertySet **value )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    IPropertySet_AddRef( impl->values );
    *value = impl->values;
    return S_OK;
}

/* Containers is a snapshot: Windows hands back a view of the children at the time of the call,
 * so a container created afterwards does not appear in a view taken before it. */
struct container_view
{
    IMapView_HSTRING_ApplicationDataContainer IMapView_HSTRING_ApplicationDataContainer_iface;
    IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    LONG ref;
    UINT32 count;
    struct child_container items[];
};

struct container_iterator
{
    IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    LONG ref;
    struct container_view *view;
    UINT32 index;
};

struct container_pair
{
    IKeyValuePair_HSTRING_ApplicationDataContainer IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    LONG ref;
    HSTRING key;
    IApplicationDataContainer *value;
};

static inline struct container_pair *impl_from_IKeyValuePair_HSTRING_ApplicationDataContainer(
        IKeyValuePair_HSTRING_ApplicationDataContainer *iface )
{
    return CONTAINING_RECORD( iface, struct container_pair, IKeyValuePair_HSTRING_ApplicationDataContainer_iface );
}

static HRESULT WINAPI container_pair_QueryInterface( IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                     REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_ApplicationDataContainer ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI container_pair_AddRef( IKeyValuePair_HSTRING_ApplicationDataContainer *iface )
{
    return InterlockedIncrement( &impl_from_IKeyValuePair_HSTRING_ApplicationDataContainer( iface )->ref );
}

static ULONG WINAPI container_pair_Release( IKeyValuePair_HSTRING_ApplicationDataContainer *iface )
{
    struct container_pair *impl = impl_from_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->key );
        IApplicationDataContainer_Release( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI container_pair_GetIids( IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                              ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI container_pair_GetRuntimeClassName( IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                          HSTRING *name )
{
    static const WCHAR class_name[] =
        L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Storage.ApplicationDataContainer>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI container_pair_GetTrustLevel( IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                    TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI container_pair_get_Key( IKeyValuePair_HSTRING_ApplicationDataContainer *iface, HSTRING *key )
{
    if (!key) return E_POINTER;
    return WindowsDuplicateString( impl_from_IKeyValuePair_HSTRING_ApplicationDataContainer( iface )->key, key );
}

static HRESULT WINAPI container_pair_get_Value( IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                IApplicationDataContainer **value )
{
    struct container_pair *impl = impl_from_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );

    if (!value) return E_POINTER;
    IApplicationDataContainer_AddRef( (*value = impl->value) );
    return S_OK;
}

static const struct IKeyValuePair_HSTRING_ApplicationDataContainerVtbl container_pair_vtbl =
{
    container_pair_QueryInterface,
    container_pair_AddRef,
    container_pair_Release,
    /* IInspectable methods */
    container_pair_GetIids,
    container_pair_GetRuntimeClassName,
    container_pair_GetTrustLevel,
    /* IKeyValuePair<HSTRING, ApplicationDataContainer> methods */
    container_pair_get_Key,
    container_pair_get_Value,
};

static HRESULT container_pair_create( const struct child_container *child,
                                      IKeyValuePair_HSTRING_ApplicationDataContainer **out )
{
    struct container_pair *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IKeyValuePair_HSTRING_ApplicationDataContainer_iface.lpVtbl = &container_pair_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( child->name, &impl->key )))
    {
        free( impl );
        return hr;
    }
    IApplicationDataContainer_AddRef( (impl->value = child->container) );
    *out = &impl->IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    return S_OK;
}

static inline struct container_iterator *impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer(
        IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface )
{
    return CONTAINING_RECORD( iface, struct container_iterator,
                              IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer_iface );
}

static HRESULT WINAPI container_iterator_QueryInterface( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                         REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI container_iterator_AddRef( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer( iface )->ref );
}

static ULONG WINAPI container_iterator_Release( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface )
{
    struct container_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IMapView_HSTRING_ApplicationDataContainer_Release( &impl->view->IMapView_HSTRING_ApplicationDataContainer_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI container_iterator_GetIids( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                  ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI container_iterator_GetRuntimeClassName( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                              HSTRING *name )
{
    static const WCHAR class_name[] =
        L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Storage.ApplicationDataContainer>>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI container_iterator_GetTrustLevel( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                        TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI container_iterator_get_Current( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                      IKeyValuePair_HSTRING_ApplicationDataContainer **value )
{
    struct container_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );

    if (!value) return E_POINTER;
    *value = NULL;
    if (impl->index >= impl->view->count) return E_BOUNDS;
    return container_pair_create( &impl->view->items[impl->index], value );
}

static HRESULT WINAPI container_iterator_get_HasCurrent( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                         boolean *value )
{
    struct container_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );

    if (!value) return E_POINTER;
    *value = impl->index < impl->view->count;
    return S_OK;
}

static HRESULT WINAPI container_iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                   boolean *value )
{
    struct container_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );

    if (!value) return E_POINTER;
    if (impl->index < impl->view->count) impl->index++;
    *value = impl->index < impl->view->count;
    return S_OK;
}

static HRESULT WINAPI container_iterator_GetMany( IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                  UINT32 size, IKeyValuePair_HSTRING_ApplicationDataContainer **items,
                                                  UINT32 *count )
{
    struct container_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );
    UINT32 i;
    HRESULT hr;

    if (!count) return E_POINTER;
    for (i = 0; i < size && impl->index + i < impl->view->count; i++)
    {
        if (FAILED(hr = container_pair_create( &impl->view->items[impl->index + i], &items[i] )))
        {
            while (i--) IKeyValuePair_HSTRING_ApplicationDataContainer_Release( items[i] );
            *count = 0;
            return hr;
        }
    }
    impl->index += i;
    *count = i;
    return S_OK;
}

static const struct IIterator_IKeyValuePair_HSTRING_ApplicationDataContainerVtbl container_iterator_vtbl =
{
    container_iterator_QueryInterface,
    container_iterator_AddRef,
    container_iterator_Release,
    /* IInspectable methods */
    container_iterator_GetIids,
    container_iterator_GetRuntimeClassName,
    container_iterator_GetTrustLevel,
    /* IIterator<IKeyValuePair<HSTRING, ApplicationDataContainer>> methods */
    container_iterator_get_Current,
    container_iterator_get_HasCurrent,
    container_iterator_MoveNext,
    container_iterator_GetMany,
};

static inline struct container_view *impl_from_IMapView_HSTRING_ApplicationDataContainer(
        IMapView_HSTRING_ApplicationDataContainer *iface )
{
    return CONTAINING_RECORD( iface, struct container_view, IMapView_HSTRING_ApplicationDataContainer_iface );
}

static HRESULT WINAPI container_view_QueryInterface( IMapView_HSTRING_ApplicationDataContainer *iface,
                                                     REFIID iid, void **out )
{
    struct container_view *impl = impl_from_IMapView_HSTRING_ApplicationDataContainer( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMapView_HSTRING_ApplicationDataContainer ))
        *out = &impl->IMapView_HSTRING_ApplicationDataContainer_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer ))
        *out = &impl->IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI container_view_AddRef( IMapView_HSTRING_ApplicationDataContainer *iface )
{
    return InterlockedIncrement( &impl_from_IMapView_HSTRING_ApplicationDataContainer( iface )->ref );
}

static ULONG WINAPI container_view_Release( IMapView_HSTRING_ApplicationDataContainer *iface )
{
    struct container_view *impl = impl_from_IMapView_HSTRING_ApplicationDataContainer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++)
        {
            WindowsDeleteString( impl->items[i].name );
            IApplicationDataContainer_Release( impl->items[i].container );
        }
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI container_view_GetIids( IMapView_HSTRING_ApplicationDataContainer *iface,
                                              ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI container_view_GetRuntimeClassName( IMapView_HSTRING_ApplicationDataContainer *iface,
                                                          HSTRING *name )
{
    static const WCHAR class_name[] =
        L"Windows.Foundation.Collections.IMapView`2<String, Windows.Storage.ApplicationDataContainer>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI container_view_GetTrustLevel( IMapView_HSTRING_ApplicationDataContainer *iface,
                                                    TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static const struct child_container *find_child( const struct child_container *items, UINT32 count, HSTRING name )
{
    const WCHAR *str = WindowsGetStringRawBuffer( name, NULL );
    UINT32 i;

    for (i = 0; i < count; i++)
        if (!wcsicmp( WindowsGetStringRawBuffer( items[i].name, NULL ), str )) return &items[i];
    return NULL;
}

static BOOL array_reserve_children( struct data_container *impl )
{
    UINT32 capacity = max( 4, impl->child_capacity * 2 );
    struct child_container *children;

    if (!(children = realloc( impl->children, capacity * sizeof(*children) ))) return FALSE;
    impl->children = children;
    impl->child_capacity = capacity;
    return TRUE;
}

static HRESULT WINAPI container_view_Lookup( IMapView_HSTRING_ApplicationDataContainer *iface, HSTRING key,
                                             IApplicationDataContainer **value )
{
    struct container_view *impl = impl_from_IMapView_HSTRING_ApplicationDataContainer( iface );
    const struct child_container *child;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!(child = find_child( impl->items, impl->count, key ))) return E_BOUNDS;
    IApplicationDataContainer_AddRef( (*value = child->container) );
    return S_OK;
}

static HRESULT WINAPI container_view_get_Size( IMapView_HSTRING_ApplicationDataContainer *iface, UINT32 *size )
{
    if (!size) return E_POINTER;
    *size = impl_from_IMapView_HSTRING_ApplicationDataContainer( iface )->count;
    return S_OK;
}

static HRESULT WINAPI container_view_HasKey( IMapView_HSTRING_ApplicationDataContainer *iface, HSTRING key,
                                             boolean *found )
{
    struct container_view *impl = impl_from_IMapView_HSTRING_ApplicationDataContainer( iface );

    if (!found) return E_POINTER;
    *found = !!find_child( impl->items, impl->count, key );
    return S_OK;
}

/* Split may legitimately return two nulls; callers treat that as "not splittable". */
static HRESULT WINAPI container_view_Split( IMapView_HSTRING_ApplicationDataContainer *iface,
                                            IMapView_HSTRING_ApplicationDataContainer **first,
                                            IMapView_HSTRING_ApplicationDataContainer **second )
{
    if (!first || !second) return E_POINTER;
    *first = *second = NULL;
    return S_OK;
}

static const struct IMapView_HSTRING_ApplicationDataContainerVtbl container_view_vtbl =
{
    container_view_QueryInterface,
    container_view_AddRef,
    container_view_Release,
    /* IInspectable methods */
    container_view_GetIids,
    container_view_GetRuntimeClassName,
    container_view_GetTrustLevel,
    /* IMapView<HSTRING, ApplicationDataContainer> methods */
    container_view_Lookup,
    container_view_get_Size,
    container_view_HasKey,
    container_view_Split,
};

DEFINE_IINSPECTABLE( container_view_iterable, IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer,
                     struct container_view, IMapView_HSTRING_ApplicationDataContainer_iface )

static HRESULT WINAPI container_view_iterable_First( IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                                     IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer **value )
{
    struct container_view *impl = impl_from_IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer( iface );
    struct container_iterator *iter;

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer_iface.lpVtbl = &container_iterator_vtbl;
    iter->ref = 1;
    iter->view = impl;
    IMapView_HSTRING_ApplicationDataContainer_AddRef( &impl->IMapView_HSTRING_ApplicationDataContainer_iface );
    *value = &iter->IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    return S_OK;
}

static const struct IIterable_IKeyValuePair_HSTRING_ApplicationDataContainerVtbl container_view_iterable_vtbl =
{
    container_view_iterable_QueryInterface,
    container_view_iterable_AddRef,
    container_view_iterable_Release,
    /* IInspectable methods */
    container_view_iterable_GetIids,
    container_view_iterable_GetRuntimeClassName,
    container_view_iterable_GetTrustLevel,
    /* IIterable<IKeyValuePair<HSTRING, ApplicationDataContainer>> methods */
    container_view_iterable_First,
};

static HRESULT WINAPI data_container_get_Containers( IApplicationDataContainer *iface,
        IMapView_HSTRING_ApplicationDataContainer **value )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );
    struct container_view *view;
    UINT32 i;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    EnterCriticalSection( &impl->cs );
    if (!(view = calloc( 1, offsetof( struct container_view, items[impl->child_count] ) )))
    {
        LeaveCriticalSection( &impl->cs );
        return E_OUTOFMEMORY;
    }
    view->IMapView_HSTRING_ApplicationDataContainer_iface.lpVtbl = &container_view_vtbl;
    view->IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer_iface.lpVtbl = &container_view_iterable_vtbl;
    view->ref = 1;
    for (i = 0; i < impl->child_count; i++)
    {
        /* duplicating an HSTRING that is not a reference only bumps its refcount, and cannot fail */
        WindowsDuplicateString( impl->children[i].name, &view->items[i].name );
        IApplicationDataContainer_AddRef( (view->items[i].container = impl->children[i].container) );
    }
    view->count = impl->child_count;
    LeaveCriticalSection( &impl->cs );

    *value = &view->IMapView_HSTRING_ApplicationDataContainer_iface;
    return S_OK;
}

static HRESULT WINAPI data_container_CreateContainer( IApplicationDataContainer *iface, HSTRING name,
        ApplicationDataCreateDisposition disposition, IApplicationDataContainer **container )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );
    const struct child_container *child;
    UINT32 name_len;
    const WCHAR *str = WindowsGetStringRawBuffer( name, &name_len );
    HRESULT hr = S_OK;

    TRACE( "iface %p, name %s, disposition %d, container %p.\n", iface, debugstr_hstring( name ),
           disposition, container );

    if (!container) return E_POINTER;
    *container = NULL;
    /* the documented limit on a container name, which is a registry key name on Windows */
    if (!name_len || name_len > 255) return E_INVALIDARG;

    EnterCriticalSection( &impl->cs );
    if ((child = find_child( impl->children, impl->child_count, name )))
        IApplicationDataContainer_AddRef( (*container = child->container) );
    else if (disposition == ApplicationDataCreateDisposition_Existing)
        hr = HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    else if (impl->child_count == impl->child_capacity &&
             !array_reserve_children( impl ))
        hr = E_OUTOFMEMORY;
    else if (SUCCEEDED(hr = create_data_container( impl->locality, str, name_len, container )))
    {
        struct child_container *slot = &impl->children[impl->child_count];

        if (FAILED(hr = WindowsCreateString( str, name_len, &slot->name )))
        {
            IApplicationDataContainer_Release( *container );
            *container = NULL;
        }
        else
        {
            IApplicationDataContainer_AddRef( (slot->container = *container) );
            impl->child_count++;
        }
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI data_container_DeleteContainer( IApplicationDataContainer *iface, HSTRING name )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );
    const struct child_container *child;
    UINT32 i;

    TRACE( "iface %p, name %s.\n", iface, debugstr_hstring( name ) );

    EnterCriticalSection( &impl->cs );
    if ((child = find_child( impl->children, impl->child_count, name )))
    {
        i = child - impl->children;
        WindowsDeleteString( impl->children[i].name );
        IApplicationDataContainer_Release( impl->children[i].container );
        memmove( &impl->children[i], &impl->children[i + 1],
                 (impl->child_count - i - 1) * sizeof(*impl->children) );
        impl->child_count--;
    }
    LeaveCriticalSection( &impl->cs );
    /* deleting a container that does not exist is not an error */
    return S_OK;
}

static const struct IApplicationDataContainerVtbl data_container_vtbl =
{
    data_container_QueryInterface,
    data_container_AddRef,
    data_container_Release,
    /* IInspectable methods */
    data_container_GetIids,
    data_container_GetRuntimeClassName,
    data_container_GetTrustLevel,
    /* IApplicationDataContainer methods */
    data_container_get_Name,
    data_container_get_Locality,
    data_container_get_Values,
    data_container_get_Containers,
    data_container_CreateContainer,
    data_container_DeleteContainer,
};

static HRESULT create_data_container( ApplicationDataLocality locality, const WCHAR *name, UINT32 name_len,
                                      IApplicationDataContainer **out )
{
    struct data_container *impl;
    IInspectable *inspectable;
    IPropertySet *values;
    HSTRING_HEADER header;
    HSTRING class_name;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IApplicationDataContainer_iface.lpVtbl = &data_container_vtbl;
    impl->locality = locality;
    impl->ref = 1;

    if (FAILED(hr = WindowsCreateString( name, name_len, &impl->name )) ||
        FAILED(hr = WindowsCreateStringReference( RuntimeClass_Windows_Foundation_Collections_PropertySet,
            ARRAY_SIZE(RuntimeClass_Windows_Foundation_Collections_PropertySet) - 1, &header, &class_name )) ||
        FAILED(hr = RoActivateInstance( class_name, &inspectable )))
    {
        WindowsDeleteString( impl->name );
        free( impl );
        return hr;
    }
    hr = IInspectable_QueryInterface( inspectable, &IID_IPropertySet, (void **)&values );
    IInspectable_Release( inspectable );
    if (SUCCEEDED(hr))
    {
        hr = settings_create( values, &impl->values );
        IPropertySet_Release( values );
    }
    if (FAILED(hr))
    {
        WindowsDeleteString( impl->name );
        free( impl );
        return hr;
    }

    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": data_container.cs");
    *out = &impl->IApplicationDataContainer_iface;
    return S_OK;
}

/* Persistence. Windows keeps a package's settings in a registry hive under
 * %LOCALAPPDATA%\Packages\<family>\Settings; here each locality's tree is written to a file of our
 * own in that folder, wine-local.dat or wine-roaming.dat, whenever a container's values change,
 * and read back when the root container is first handed out. Without it an app forgets everything
 * at exit - Shapr3D keeps its sign-in session in LocalSettings, so every start meant signing in
 * again.
 *
 * The format is ours and little-endian: "WUSD", a version, then the root container. A container
 * is its name, its values and its children; a value is its key, a type and a payload. The types
 * are PropertyType's, plus COMPOSITE for an ApplicationDataCompositeValue, whose payload is its
 * own list of values. Values of other kinds are skipped with a FIXME. */
static IApplicationDataContainer *local_settings, *roaming_settings;
static CRITICAL_SECTION persist_cs;
static CRITICAL_SECTION_DEBUG persist_cs_debug =
{
    0, 0, &persist_cs,
    { &persist_cs_debug.ProcessLocksList, &persist_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": persist_cs") }
};
static CRITICAL_SECTION persist_cs = { &persist_cs_debug, -1, 0, 0, 0, 0 };
static LONG loading;

#define PERSIST_MAGIC     0x44535557 /* 'WUSD' */
#define PERSIST_VERSION   1
#define PERSIST_COMPOSITE 0xffff

struct buffer
{
    BYTE *data;
    SIZE_T size, capacity;
    BOOL failed;
};

static void put( struct buffer *buf, const void *data, SIZE_T size )
{
    if (buf->failed) return;
    if (buf->size + size > buf->capacity)
    {
        SIZE_T capacity = max( buf->capacity * 2, buf->size + size + 256 );
        BYTE *grown = realloc( buf->data, capacity );
        if (!grown) { buf->failed = TRUE; return; }
        buf->data = grown;
        buf->capacity = capacity;
    }
    memcpy( buf->data + buf->size, data, size );
    buf->size += size;
}
static void put_u32( struct buffer *buf, UINT32 value ) { put( buf, &value, sizeof(value) ); }
static void put_str( struct buffer *buf, const WCHAR *str, UINT32 len )
{
    put_u32( buf, len );
    put( buf, str, len * sizeof(WCHAR) );
}
static void put_hstr( struct buffer *buf, HSTRING str )
{
    UINT32 len;
    const WCHAR *raw = WindowsGetStringRawBuffer( str, &len );
    put_str( buf, raw, len );
}

static BOOL is_composite( IInspectable *value )
{
    HSTRING name;
    BOOL ret;

    if (FAILED(IInspectable_GetRuntimeClassName( value, &name ))) return FALSE;
    ret = !wcscmp( WindowsGetStringRawBuffer( name, NULL ), L"Windows.Storage.ApplicationDataCompositeValue" );
    WindowsDeleteString( name );
    return ret;
}

static void put_values( struct buffer *buf, IInspectable *set );

/* The type and payload of one value; returns FALSE for a kind that is not persisted. */
static BOOL put_value( struct buffer *buf, IInspectable *value )
{
    IPropertyValue *pv;
    PropertyType type;
    union { BYTE u8; INT16 i16; UINT16 u16; INT32 i32; UINT32 u32; INT64 i64; UINT64 u64; FLOAT f;
            DOUBLE d; WCHAR c; boolean b; DateTime dt; TimeSpan ts; GUID guid; } v;
    UINT32 size = 0;
    HRESULT hr = E_FAIL;

    if (FAILED(IInspectable_QueryInterface( value, &IID_IPropertyValue, (void **)&pv )))
    {
        if (!is_composite( value )) return FALSE;
        put_u32( buf, PERSIST_COMPOSITE );
        put_values( buf, value );
        return TRUE;
    }
    if (FAILED(IPropertyValue_get_Type( pv, &type ))) { IPropertyValue_Release( pv ); return FALSE; }

    switch (type)
    {
    case PropertyType_UInt8:    hr = IPropertyValue_GetUInt8( pv, &v.u8 ); size = sizeof(v.u8); break;
    case PropertyType_Int16:    hr = IPropertyValue_GetInt16( pv, &v.i16 ); size = sizeof(v.i16); break;
    case PropertyType_UInt16:   hr = IPropertyValue_GetUInt16( pv, &v.u16 ); size = sizeof(v.u16); break;
    case PropertyType_Int32:    hr = IPropertyValue_GetInt32( pv, &v.i32 ); size = sizeof(v.i32); break;
    case PropertyType_UInt32:   hr = IPropertyValue_GetUInt32( pv, &v.u32 ); size = sizeof(v.u32); break;
    case PropertyType_Int64:    hr = IPropertyValue_GetInt64( pv, &v.i64 ); size = sizeof(v.i64); break;
    case PropertyType_UInt64:   hr = IPropertyValue_GetUInt64( pv, &v.u64 ); size = sizeof(v.u64); break;
    case PropertyType_Single:   hr = IPropertyValue_GetSingle( pv, &v.f ); size = sizeof(v.f); break;
    case PropertyType_Double:   hr = IPropertyValue_GetDouble( pv, &v.d ); size = sizeof(v.d); break;
    case PropertyType_Char16:   hr = IPropertyValue_GetChar16( pv, &v.c ); size = sizeof(v.c); break;
    case PropertyType_Boolean:  hr = IPropertyValue_GetBoolean( pv, &v.b ); size = sizeof(v.b); break;
    case PropertyType_DateTime: hr = IPropertyValue_GetDateTime( pv, &v.dt ); size = sizeof(v.dt); break;
    case PropertyType_TimeSpan: hr = IPropertyValue_GetTimeSpan( pv, &v.ts ); size = sizeof(v.ts); break;
    case PropertyType_Guid:     hr = IPropertyValue_GetGuid( pv, &v.guid ); size = sizeof(v.guid); break;
    case PropertyType_String:
    {
        HSTRING str;
        if (SUCCEEDED(hr = IPropertyValue_GetString( pv, &str )))
        {
            put_u32( buf, type );
            put_hstr( buf, str );
            WindowsDeleteString( str );
        }
        IPropertyValue_Release( pv );
        return SUCCEEDED(hr);
    }
    case PropertyType_UInt8Array:
    {
        BYTE *bytes;
        UINT32 count;
        if (SUCCEEDED(hr = IPropertyValue_GetUInt8Array( pv, &count, &bytes )))
        {
            put_u32( buf, type );
            put_u32( buf, count );
            put( buf, bytes, count );
            CoTaskMemFree( bytes );
        }
        IPropertyValue_Release( pv );
        return SUCCEEDED(hr);
    }
    case PropertyType_StringArray:
    {
        HSTRING *strs;
        UINT32 count, i;
        if (SUCCEEDED(hr = IPropertyValue_GetStringArray( pv, &count, &strs )))
        {
            put_u32( buf, type );
            put_u32( buf, count );
            for (i = 0; i < count; i++)
            {
                put_hstr( buf, strs[i] );
                WindowsDeleteString( strs[i] );
            }
            CoTaskMemFree( strs );
        }
        IPropertyValue_Release( pv );
        return SUCCEEDED(hr);
    }
    default:
        FIXME( "not persisting a value of type %d\n", type );
        break;
    }
    IPropertyValue_Release( pv );
    if (FAILED(hr) || !size) return FALSE;
    put_u32( buf, type );
    put_u32( buf, size );
    put( buf, &v, size );
    return TRUE;
}

/* The values of a set, as a count then key/value pairs. The count is patched once the pairs are
 * written, since values of unsupported kinds are left out. */
static void put_values( struct buffer *buf, IInspectable *set )
{
    IIterable_IKeyValuePair_HSTRING_IInspectable *iterable;
    IIterator_IKeyValuePair_HSTRING_IInspectable *iter;
    IKeyValuePair_HSTRING_IInspectable *pair;
    SIZE_T count_at = buf->size;
    UINT32 count = 0;
    boolean has;

    put_u32( buf, 0 );
    if (FAILED(IInspectable_QueryInterface( set, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable,
                                            (void **)&iterable )))
        return;
    if (SUCCEEDED(IIterable_IKeyValuePair_HSTRING_IInspectable_First( iterable, &iter )))
    {
        while (SUCCEEDED(IIterator_IKeyValuePair_HSTRING_IInspectable_get_HasCurrent( iter, &has )) && has)
        {
            if (SUCCEEDED(IIterator_IKeyValuePair_HSTRING_IInspectable_get_Current( iter, &pair )))
            {
                HSTRING key;
                IInspectable *value;

                if (SUCCEEDED(IKeyValuePair_HSTRING_IInspectable_get_Key( pair, &key )))
                {
                    if (SUCCEEDED(IKeyValuePair_HSTRING_IInspectable_get_Value( pair, &value )) && value)
                    {
                        SIZE_T start = buf->size;
                        put_hstr( buf, key );
                        if (put_value( buf, value )) count++;
                        else buf->size = start;
                        IInspectable_Release( value );
                    }
                    WindowsDeleteString( key );
                }
                IKeyValuePair_HSTRING_IInspectable_Release( pair );
            }
            if (FAILED(IIterator_IKeyValuePair_HSTRING_IInspectable_MoveNext( iter, &has ))) break;
        }
        IIterator_IKeyValuePair_HSTRING_IInspectable_Release( iter );
    }
    IIterable_IKeyValuePair_HSTRING_IInspectable_Release( iterable );
    if (!buf->failed) memcpy( buf->data + count_at, &count, sizeof(count) );
}

static void put_container( struct buffer *buf, IApplicationDataContainer *iface )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( iface );
    UINT32 i;

    put_hstr( buf, impl->name );
    put_values( buf, (IInspectable *)impl->values );
    EnterCriticalSection( &impl->cs );
    put_u32( buf, impl->child_count );
    for (i = 0; i < impl->child_count; i++) put_container( buf, impl->children[i].container );
    LeaveCriticalSection( &impl->cs );
}

static BOOL settings_file( ApplicationDataLocality locality, WCHAR *path, SIZE_T size )
{
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 family_len = ARRAY_SIZE(family);
    SIZE_T len;

    if (GetCurrentPackageFamilyName( &family_len, family )) return FALSE;
    if (!GetEnvironmentVariableW( L"LOCALAPPDATA", path, size )) return FALSE;
    len = wcslen( path );
    if (swprintf( path + len, size - len, L"\\Packages\\%s", family ) < 0) return FALSE;
    CreateDirectoryW( path, NULL );
    len = wcslen( path );
    if (swprintf( path + len, size - len, L"\\Settings" ) < 0) return FALSE;
    CreateDirectoryW( path, NULL );
    len = wcslen( path );
    return swprintf( path + len, size - len, L"\\wine-%s.dat",
                     locality == ApplicationDataLocality_Roaming ? L"roaming" : L"local" ) >= 0;
}

static void save_root( IApplicationDataContainer *root, ApplicationDataLocality locality )
{
    struct buffer buf = { 0 };
    WCHAR path[MAX_PATH], temp[MAX_PATH + 4];
    HANDLE file;
    DWORD written;

    if (!root || !settings_file( locality, path, ARRAY_SIZE(path) )) return;
    put_u32( &buf, PERSIST_MAGIC );
    put_u32( &buf, PERSIST_VERSION );
    put_container( &buf, root );
    if (buf.failed) { ERR( "out of memory saving the settings\n" ); free( buf.data ); return; }

    /* written aside and moved over, so a crash mid-write cannot lose what was there */
    swprintf( temp, ARRAY_SIZE(temp), L"%s.new", path );
    file = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        ERR( "cannot write %s, error %lu\n", debugstr_w( temp ), GetLastError() );
        free( buf.data );
        return;
    }
    if (!WriteFile( file, buf.data, buf.size, &written, NULL ) || written != buf.size)
    {
        ERR( "cannot write %s, error %lu\n", debugstr_w( temp ), GetLastError() );
        CloseHandle( file );
        DeleteFileW( temp );
        free( buf.data );
        return;
    }
    CloseHandle( file );
    if (!MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING ))
        ERR( "cannot replace %s, error %lu\n", debugstr_w( path ), GetLastError() );
    TRACE( "saved %Iu bytes to %s\n", buf.size, debugstr_w( path ) );
    free( buf.data );
}

void settings_changed( void )
{
    if (loading) return;
    EnterCriticalSection( &persist_cs );
    save_root( local_settings, ApplicationDataLocality_Local );
    save_root( roaming_settings, ApplicationDataLocality_Roaming );
    LeaveCriticalSection( &persist_cs );
}

struct reader
{
    const BYTE *data;
    SIZE_T size, pos;
    BOOL failed;
};

static const void *get( struct reader *r, SIZE_T size )
{
    const void *p;
    if (r->failed || size > r->size - r->pos) { r->failed = TRUE; return NULL; }
    p = r->data + r->pos;
    r->pos += size;
    return p;
}
static UINT32 get_u32( struct reader *r )
{
    const UINT32 *p = get( r, sizeof(*p) );
    UINT32 v = 0;
    if (p) memcpy( &v, p, sizeof(v) );
    return v;
}
static HRESULT get_hstr( struct reader *r, HSTRING *out )
{
    UINT32 len = get_u32( r );
    const WCHAR *str;

    *out = NULL;
    if (len > r->size || !(str = get( r, (SIZE_T)len * sizeof(WCHAR) ))) return E_FAIL;
    return WindowsCreateString( str, len, out );
}

static HRESULT get_values( struct reader *r, IPropertySet *set, IPropertyValueStatics *statics );

static HRESULT get_value( struct reader *r, IPropertyValueStatics *statics, IInspectable **out )
{
    UINT32 type = get_u32( r ), size, count, i;
    const void *p;
    HRESULT hr;

    *out = NULL;
    if (r->failed) return E_FAIL;
    switch (type)
    {
    case PERSIST_COMPOSITE:
    {
        IPropertySet *set;
        if (FAILED(hr = IActivationFactory_ActivateInstance( composite_value_factory, out ))) return hr;
        if (FAILED(hr = IInspectable_QueryInterface( *out, &IID_IPropertySet, (void **)&set ))) return hr;
        hr = get_values( r, set, statics );
        IPropertySet_Release( set );
        return hr;
    }
    case PropertyType_String:
    {
        HSTRING str;
        if (FAILED(hr = get_hstr( r, &str ))) return hr;
        hr = IPropertyValueStatics_CreateString( statics, str, out );
        WindowsDeleteString( str );
        return hr;
    }
    case PropertyType_UInt8Array:
        count = get_u32( r );
        if (!(p = get( r, count ))) return E_FAIL;
        return IPropertyValueStatics_CreateUInt8Array( statics, count, (BYTE *)p, out );
    case PropertyType_StringArray:
    {
        HSTRING *strs;
        count = get_u32( r );
        if (count > r->size || !(strs = calloc( max( count, 1 ), sizeof(*strs) ))) return E_FAIL;
        for (i = 0, hr = S_OK; i < count && SUCCEEDED(hr); i++) hr = get_hstr( r, &strs[i] );
        if (SUCCEEDED(hr)) hr = IPropertyValueStatics_CreateStringArray( statics, count, strs, out );
        for (i = 0; i < count; i++) WindowsDeleteString( strs[i] );
        free( strs );
        return hr;
    }
    }

    size = get_u32( r );
    if (!(p = get( r, size ))) return E_FAIL;
#define SCALAR(kind, ctype) \
    case PropertyType_##kind: { ctype v; if (size != sizeof(v)) return E_FAIL; memcpy( &v, p, sizeof(v) ); \
                                return IPropertyValueStatics_Create##kind( statics, v, out ); }
    switch (type)
    {
    SCALAR(UInt8, BYTE) SCALAR(Int16, INT16) SCALAR(UInt16, UINT16) SCALAR(Int32, INT32)
    SCALAR(UInt32, UINT32) SCALAR(Int64, INT64) SCALAR(UInt64, UINT64) SCALAR(Single, FLOAT)
    SCALAR(Double, DOUBLE) SCALAR(Char16, WCHAR) SCALAR(Boolean, boolean) SCALAR(DateTime, DateTime)
    SCALAR(TimeSpan, TimeSpan) SCALAR(Guid, GUID)
    }
#undef SCALAR
    return E_FAIL;
}

static HRESULT get_values( struct reader *r, IPropertySet *set, IPropertyValueStatics *statics )
{
    IMap_HSTRING_IInspectable *map;
    UINT32 count = get_u32( r ), i;
    HRESULT hr;

    if (r->failed) return E_FAIL;
    if (FAILED(hr = IPropertySet_QueryInterface( set, &IID_IMap_HSTRING_IInspectable, (void **)&map ))) return hr;
    for (i = 0; i < count && SUCCEEDED(hr); i++)
    {
        IInspectable *value;
        HSTRING key;
        boolean replaced;

        if (FAILED(hr = get_hstr( r, &key ))) break;
        if (SUCCEEDED(hr = get_value( r, statics, &value )))
        {
            hr = IMap_HSTRING_IInspectable_Insert( map, key, value, &replaced );
            IInspectable_Release( value );
        }
        WindowsDeleteString( key );
    }
    IMap_HSTRING_IInspectable_Release( map );
    return hr;
}

static HRESULT get_container( struct reader *r, IApplicationDataContainer *container,
                              IPropertyValueStatics *statics )
{
    struct data_container *impl = impl_from_IApplicationDataContainer( container );
    UINT32 count, i;
    HSTRING name;
    HRESULT hr;

    if (FAILED(hr = get_hstr( r, &name ))) return hr;   /* the root's is empty; a child's was used to create it */
    WindowsDeleteString( name );
    if (FAILED(hr = get_values( r, impl->values, statics ))) return hr;
    count = get_u32( r );
    for (i = 0; i < count && !r->failed && SUCCEEDED(hr); i++)
    {
        IApplicationDataContainer *child;
        SIZE_T pos = r->pos;

        /* peek at the child's name to create it, then let get_container read it again */
        if (FAILED(hr = get_hstr( r, &name ))) break;
        r->pos = pos;
        hr = IApplicationDataContainer_CreateContainer( container, name, ApplicationDataCreateDisposition_Always, &child );
        WindowsDeleteString( name );
        if (FAILED(hr)) break;
        hr = get_container( r, child, statics );
        IApplicationDataContainer_Release( child );
    }
    return r->failed ? E_FAIL : hr;
}

static void load_root( IApplicationDataContainer *root, ApplicationDataLocality locality )
{
    static const WCHAR property_value[] = L"Windows.Foundation.PropertyValue";
    IPropertyValueStatics *statics;
    struct reader r = { 0 };
    WCHAR path[MAX_PATH];
    HSTRING_HEADER header;
    LARGE_INTEGER size;
    HSTRING name;
    HANDLE file;
    DWORD read;
    BYTE *data;
    HRESULT hr;

    if (!settings_file( locality, path, ARRAY_SIZE(path) )) return;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return;   /* nothing saved yet */
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > 64 * 1024 * 1024 ||
        !(data = malloc( size.QuadPart ? size.QuadPart : 1 )))
    {
        CloseHandle( file );
        return;
    }
    if (!ReadFile( file, data, size.QuadPart, &read, NULL ) || read != size.QuadPart)
    {
        CloseHandle( file );
        free( data );
        return;
    }
    CloseHandle( file );

    r.data = data;
    r.size = size.QuadPart;
    if (get_u32( &r ) != PERSIST_MAGIC || get_u32( &r ) != PERSIST_VERSION)
    {
        WARN( "%s is not a settings file of ours\n", debugstr_w( path ) );
        free( data );
        return;
    }
    if (FAILED(hr = WindowsCreateStringReference( property_value, ARRAY_SIZE(property_value) - 1, &header, &name )) ||
        FAILED(hr = RoGetActivationFactory( name, &IID_IPropertyValueStatics, (void **)&statics )))
    {
        ERR( "no PropertyValue statics, hr %#lx\n", hr );
        free( data );
        return;
    }
    InterlockedIncrement( &loading );
    hr = get_container( &r, root, statics );
    InterlockedDecrement( &loading );
    IPropertyValueStatics_Release( statics );
    if (FAILED(hr)) ERR( "%s is damaged, hr %#lx; keeping what was read\n", debugstr_w( path ), hr );
    else TRACE( "loaded %s\n", debugstr_w( path ) );
    free( data );
}

/* A value stored into a settings map is copied, as on Windows: the container never calls back into the
 * caller's object. Keeping the caller's IPropertyValue made every later save call into it - for a .NET
 * app that is a managed CCW - and Shapr3D's crash handler, saving a flag from a graphics thread in the
 * middle of a crash, died inside mrt100 there instead of writing its report. The copy goes through the
 * persistence format itself, so what is kept is exactly what would be saved and reloaded. */
HRESULT copy_setting_value( IInspectable *value, IInspectable **out )
{
    static const WCHAR property_value[] = L"Windows.Foundation.PropertyValue";
    IPropertyValueStatics *statics;
    struct buffer buf = { 0 };
    struct reader r = { 0 };
    HSTRING_HEADER header;
    HSTRING name;
    HRESULT hr;

    *out = NULL;
    /* our own composite values keep their object: their own inserts are copied the same way */
    if (!value || is_composite( value ) || !put_value( &buf, value ) || buf.failed)
    {
        free( buf.data );
        if ((*out = value)) IInspectable_AddRef( value );
        return S_OK;
    }
    if (FAILED(hr = WindowsCreateStringReference( property_value, ARRAY_SIZE(property_value) - 1, &header, &name )) ||
        FAILED(hr = RoGetActivationFactory( name, &IID_IPropertyValueStatics, (void **)&statics )))
    {
        free( buf.data );
        return hr;
    }
    r.data = buf.data;
    r.size = buf.size;
    hr = get_value( &r, statics, out );
    IPropertyValueStatics_Release( statics );
    free( buf.data );
    return hr;
}

/* The root containers are process-wide, like the folders: every ApplicationData.Current hands back
 * the same LocalSettings, so what one caller writes another reads. Their Name is empty on Windows. */
static HRESULT get_root_container( IApplicationDataContainer **root, ApplicationDataLocality locality,
                                   IApplicationDataContainer **value )
{
    HRESULT hr;

    if (!value) return E_POINTER;
    if (!*root)
    {
        IApplicationDataContainer *container;
        if (FAILED(hr = create_data_container( locality, L"", 0, &container ))) return hr;
        /* filled before it is published, so no caller sees a half-loaded tree */
        EnterCriticalSection( &persist_cs );
        if (!*root)
        {
            load_root( container, locality );
            *root = container;
        }
        else IApplicationDataContainer_Release( container );
        LeaveCriticalSection( &persist_cs );
    }
    IApplicationDataContainer_AddRef( *root );
    *value = *root;
    return S_OK;
}

static HRESULT WINAPI application_data_get_LocalSettings( IApplicationData *iface, IApplicationDataContainer **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_root_container( &local_settings, ApplicationDataLocality_Local, value );
}

/* Nothing roams here - there is no account to roam to - but the container itself is an ordinary
 * settings store, so an app writing to it gets its values back, as it would offline on Windows. */
static HRESULT WINAPI application_data_get_RoamingSettings( IApplicationData *iface, IApplicationDataContainer **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_root_container( &roaming_settings, ApplicationDataLocality_Roaming, value );
}

/* The per-package data folders live under %LOCALAPPDATA%\Packages\<family>, the layout every
 * packaged app already sees through its own redirected paths; windows.storage.dll builds the
 * StorageFolder comes from windows.storage.dll, where the class lives; it is resolved at runtime
 * because Wine's build cannot make an import library for a module with a dotted name. */
static HRESULT storage_folder_create( const WCHAR *path, IStorageFolder **out )
{
    static HRESULT (WINAPI *p___wine_storage_folder_create)( const WCHAR *, IStorageFolder ** );
    static HMODULE storage;

    if (!p___wine_storage_folder_create)
    {
        if (!storage && !(storage = LoadLibraryW( L"windows.storage.dll" ))) return E_NOINTERFACE;
        if (!(p___wine_storage_folder_create = (void *)GetProcAddress( storage, "__wine_storage_folder_create" )))
            return E_NOINTERFACE;
    }
    return p___wine_storage_folder_create( path, out );
}
static HRESULT application_data_folder( const WCHAR *state, IStorageFolder **value )
{
    WCHAR path[MAX_PATH], family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 family_len = ARRAY_SIZE(family);
    LONG err;

    if (!value) return E_POINTER;
    *value = NULL;

    if ((err = GetCurrentPackageFamilyName( &family_len, family ))) return HRESULT_FROM_WIN32( err );
    if (!GetEnvironmentVariableW( L"LOCALAPPDATA", path, ARRAY_SIZE(path) )) return E_FAIL;
    if (swprintf( path + wcslen( path ), ARRAY_SIZE(path) - wcslen( path ), L"\\Packages\\%s\\%s",
                  family, state ) < 0)
        return E_FAIL;

    if (!CreateDirectoryW( path, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        /* Packages and Packages\<family> may not exist yet either (a fresh prefix has no Packages) */
        WCHAR *sep = path + wcslen( path ) - wcslen( state ) - wcslen( family ) - 10;
        while ((sep = wcschr( sep + 1, '\\' )))
        {
            *sep = 0;
            CreateDirectoryW( path, NULL );
            *sep = '\\';
        }
        if (!CreateDirectoryW( path, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS)
            return HRESULT_FROM_WIN32( GetLastError() );
    }
    return storage_folder_create( path, value );
}

static HRESULT WINAPI application_data_get_LocalFolder( IApplicationData *iface, IStorageFolder **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return application_data_folder( L"LocalState", value );
}

static HRESULT WINAPI application_data_get_RoamingFolder( IApplicationData *iface, IStorageFolder **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return application_data_folder( L"RoamingState", value );
}

static HRESULT WINAPI application_data_get_TemporaryFolder( IApplicationData *iface, IStorageFolder **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return application_data_folder( L"TempState", value );
}

static HRESULT WINAPI application_data_add_DataChanged( IApplicationData *iface, ITypedEventHandler_ApplicationData_IInspectable *handler,
                                                        EventRegistrationToken *token )
{
    static LONG next_token;

    /* DataChanged fires when roamed data arrives from another device, which never happens here.
     * Registering has to succeed anyway: C++/CX and .NET both turn a refused registration into a
     * throw, and the event simply never firing is the truthful answer. */
    FIXME( "iface %p, handler %p, token %p: the event never fires.\n", iface, handler, token );
    if (!handler || !token) return E_POINTER;
    token->value = InterlockedIncrement( &next_token );
    return S_OK;
}

static HRESULT WINAPI application_data_remove_DataChanged( IApplicationData *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI application_data_SignalDataChanged( IApplicationData *iface )
{
    FIXME( "iface %p: no DataChanged handlers are ever invoked.\n", iface );
    return S_OK;
}

static HRESULT WINAPI application_data_get_RoamingStorageQuota( IApplicationData *iface, UINT64 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct IApplicationDataVtbl application_data_vtbl =
{
    application_data_QueryInterface,
    application_data_AddRef,
    application_data_Release,
    /* IInspectable methods */
    application_data_GetIids,
    application_data_GetRuntimeClassName,
    application_data_GetTrustLevel,
    /* IApplicationData methods */
    application_data_get_Version,
    application_data_SetVersionAsync,
    application_data_ClearAllAsync,
    application_data_ClearAsync,
    application_data_get_LocalSettings,
    application_data_get_RoamingSettings,
    application_data_get_LocalFolder,
    application_data_get_RoamingFolder,
    application_data_get_TemporaryFolder,
    application_data_add_DataChanged,
    application_data_remove_DataChanged,
    application_data_SignalDataChanged,
    application_data_get_RoamingStorageQuota,
};

DEFINE_IINSPECTABLE( application_data2, IApplicationData2, struct application_data, IApplicationData_iface )

static HRESULT WINAPI application_data2_get_LocalCacheFolder( IApplicationData2 *iface, IStorageFolder **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return application_data_folder( L"LocalCache", value );
}

static const struct IApplicationData2Vtbl application_data2_vtbl =
{
    application_data2_QueryInterface,
    application_data2_AddRef,
    application_data2_Release,
    /* IInspectable methods */
    application_data2_GetIids,
    application_data2_GetRuntimeClassName,
    application_data2_GetTrustLevel,
    /* IApplicationData2 methods */
    application_data2_get_LocalCacheFolder,
};

/* IApplicationData3. Solitaire QIs for this from its startup path, calls GetPublisherCacheFolder
 * (slot 6) and dereferences the result **without checking the HRESULT** - so refusing the
 * interface left it reading through a NULL and taking an access violation in its own code, right
 * after "Solitaire::Startup START". */
DEFINE_IINSPECTABLE( application_data3, IApplicationData3, struct application_data, IApplicationData_iface )

/* A publisher cache folder is shared between apps from the same publisher. This prefix has one
 * user and no publisher store, so give each named folder its own directory under the package's
 * app data - it keeps the per-name isolation the API promises without inventing a sharing
 * relationship we cannot honour. */
static HRESULT WINAPI application_data3_GetPublisherCacheFolder( IApplicationData3 *iface, HSTRING name,
                                                                 IStorageFolder **value )
{
    const WCHAR *folder = WindowsGetStringRawBuffer( name, NULL );
    WCHAR state[MAX_PATH];

    TRACE( "iface %p, name %s, value %p.\n", iface, debugstr_hstring( name ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!folder || !*folder) return E_INVALIDARG;
    /* a name with a separator in it would escape the package's own storage */
    if (wcspbrk( folder, L"\\/:" )) return E_INVALIDARG;

    if (swprintf( state, ARRAY_SIZE(state), L"PublisherCache\\%s", folder ) < 0) return E_FAIL;
    return application_data_folder( state, value );
}

static HRESULT WINAPI application_data3_ClearPublisherCacheFolderAsync( IApplicationData3 *iface, HSTRING name,
                                                                        IAsyncAction **action )
{
    FIXME( "iface %p, name %s, action %p: not clearing anything.\n", iface, debugstr_hstring( name ), action );
    if (action) *action = NULL;
    return E_NOTIMPL;
}

/* Windows returns null here unless the app declares the sharedUserCertificates-style capability
 * that grants a shared local folder; null is the documented answer for everyone else, not a
 * refusal. */
static HRESULT WINAPI application_data3_get_SharedLocalFolder( IApplicationData3 *iface, IStorageFolder **value )
{
    TRACE( "iface %p, value %p: this prefix has no shared local folder.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static const struct IApplicationData3Vtbl application_data3_vtbl =
{
    application_data3_QueryInterface,
    application_data3_AddRef,
    application_data3_Release,
    /* IInspectable methods */
    application_data3_GetIids,
    application_data3_GetRuntimeClassName,
    application_data3_GetTrustLevel,
    /* IApplicationData3 methods */
    application_data3_GetPublisherCacheFolder,
    application_data3_ClearPublisherCacheFolderAsync,
    application_data3_get_SharedLocalFolder,
};

DEFINE_IINSPECTABLE( application_data_statics, IApplicationDataStatics, struct application_data_statics, IActivationFactory_iface )

static HRESULT WINAPI application_data_statics_get_Current( IApplicationDataStatics *iface, IApplicationData **value )
{
    struct application_data *impl;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IApplicationData_iface.lpVtbl = &application_data_vtbl;
    impl->IApplicationData2_iface.lpVtbl = &application_data2_vtbl;
    impl->IApplicationData3_iface.lpVtbl = &application_data3_vtbl;
    impl->ref = 1;

    *value = &impl->IApplicationData_iface;
    TRACE( "created IApplicationData %p.\n", *value );
    return S_OK;
}

static const struct IApplicationDataStaticsVtbl application_data_statics_vtbl =
{
    application_data_statics_QueryInterface,
    application_data_statics_AddRef,
    application_data_statics_Release,
    /* IInspectable methods */
    application_data_statics_GetIids,
    application_data_statics_GetRuntimeClassName,
    application_data_statics_GetTrustLevel,
    /* IApplicationDataStatics methods */
    application_data_statics_get_Current,
};

static struct application_data_statics application_data_statics =
{
    {&factory_vtbl},
    {&application_data_statics_vtbl},
    1,
};

IActivationFactory *application_data_factory = &application_data_statics.IActivationFactory_iface;

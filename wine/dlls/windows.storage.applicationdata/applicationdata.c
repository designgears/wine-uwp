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
 * an app reads and writes them freely, they just do not survive a restart yet. */
struct data_container
{
    IApplicationDataContainer IApplicationDataContainer_iface;
    IPropertySet *values;
    ApplicationDataLocality locality;
    LONG ref;
};

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
    return WindowsCreateString( L"", 0, value );
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

static HRESULT WINAPI data_container_get_Containers( IApplicationDataContainer *iface,
        IMapView_HSTRING_ApplicationDataContainer **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_container_CreateContainer( IApplicationDataContainer *iface, HSTRING name,
        ApplicationDataCreateDisposition disposition, IApplicationDataContainer **container )
{
    FIXME( "iface %p, name %s, disposition %d, container %p stub!\n", iface,
           debugstr_hstring( name ), disposition, container );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_container_DeleteContainer( IApplicationDataContainer *iface, HSTRING name )
{
    FIXME( "iface %p, name %s stub!\n", iface, debugstr_hstring( name ) );
    return E_NOTIMPL;
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

static HRESULT create_data_container( ApplicationDataLocality locality, IApplicationDataContainer **out )
{
    struct data_container *impl;
    IInspectable *inspectable;
    IPropertySet *values;
    HSTRING_HEADER header;
    HSTRING name;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IApplicationDataContainer_iface.lpVtbl = &data_container_vtbl;
    impl->locality = locality;
    impl->ref = 1;

    if (FAILED(hr = WindowsCreateStringReference( RuntimeClass_Windows_Foundation_Collections_PropertySet,
            ARRAY_SIZE(RuntimeClass_Windows_Foundation_Collections_PropertySet) - 1, &header, &name )) ||
        FAILED(hr = RoActivateInstance( name, &inspectable )))
    {
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
        free( impl );
        return hr;
    }

    *out = &impl->IApplicationDataContainer_iface;
    return S_OK;
}

static HRESULT WINAPI application_data_get_LocalSettings( IApplicationData *iface, IApplicationDataContainer **value )
{
    static IApplicationDataContainer *local_settings;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!local_settings)
    {
        IApplicationDataContainer *container;
        if (FAILED(hr = create_data_container( ApplicationDataLocality_Local, &container ))) return hr;
        if (InterlockedCompareExchangePointer( (void **)&local_settings, container, NULL ))
            IApplicationDataContainer_Release( container );
    }
    IApplicationDataContainer_AddRef( local_settings );
    *value = local_settings;
    return S_OK;
}

static HRESULT WINAPI application_data_get_RoamingSettings( IApplicationData *iface, IApplicationDataContainer **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
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
        /* the Packages\<family> level may not exist yet either */
        WCHAR *sep = wcsrchr( path, '\\' );
        *sep = 0;
        if (!CreateDirectoryW( path, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS)
            return HRESULT_FROM_WIN32( GetLastError() );
        *sep = '\\';
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
    FIXME( "iface %p, handler %p, token %p stub!\n", iface, handler, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_remove_DataChanged( IApplicationData *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data_SignalDataChanged( IApplicationData *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
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

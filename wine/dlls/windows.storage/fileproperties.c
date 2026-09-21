/*
 * Windows.Storage.FileProperties - the properties a shell item exposes about a file.
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

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/*
 * There is no tag parser here and no property system behind it, so the music properties are what
 * can be known from the file itself: the title is the file name without its extension, which is
 * what Windows also falls back to for an untagged file. Everything else is reported empty rather
 * than invented. That is enough for a library view to list a track and play it; it is not enough
 * to group by album, and an app that sorts by artist will see one empty group.
 */

/* ---------------------------------------------------------------- MusicProperties */

struct music_properties
{
    IMusicPropertiesInternal IMusicPropertiesInternal_iface;
    LONG ref;

    HSTRING title;
    HSTRING album;
    HSTRING artist;
    HSTRING album_artist;
    HSTRING subtitle;
    HSTRING publisher;
    UINT32 track_number;
    UINT32 rating;
    UINT32 year;
};

static inline struct music_properties *impl_from_IMusicPropertiesInternal( IMusicPropertiesInternal *iface )
{
    return CONTAINING_RECORD( iface, struct music_properties, IMusicPropertiesInternal_iface );
}

static HRESULT WINAPI music_props_QueryInterface( IMusicPropertiesInternal *iface, REFIID iid, void **out )
{
    struct music_properties *impl = impl_from_IMusicPropertiesInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMusicPropertiesInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IMusicPropertiesInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI music_props_AddRef( IMusicPropertiesInternal *iface )
{
    return InterlockedIncrement( &impl_from_IMusicPropertiesInternal( iface )->ref );
}

static ULONG WINAPI music_props_Release( IMusicPropertiesInternal *iface )
{
    struct music_properties *impl = impl_from_IMusicPropertiesInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->title );
        WindowsDeleteString( impl->album );
        WindowsDeleteString( impl->artist );
        WindowsDeleteString( impl->album_artist );
        WindowsDeleteString( impl->subtitle );
        WindowsDeleteString( impl->publisher );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI music_props_GetIids( IMusicPropertiesInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI music_props_GetRuntimeClassName( IMusicPropertiesInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Storage.FileProperties.MusicProperties",
                                ARRAY_SIZE(L"Windows.Storage.FileProperties.MusicProperties") - 1,
                                class_name );
}

static HRESULT WINAPI music_props_GetTrustLevel( IMusicPropertiesInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

#define DEFINE_MUSIC_STRING( name, field )                                                         \
    static HRESULT WINAPI music_props_get_##name( IMusicPropertiesInternal *iface, HSTRING *value ) \
    {                                                                                              \
        struct music_properties *impl = impl_from_IMusicPropertiesInternal( iface );               \
        TRACE( "iface %p, value %p.\n", iface, value );                                            \
        if (!value) return E_POINTER;                                                              \
        return WindowsDuplicateString( impl->field, value );                                       \
    }                                                                                              \
    static HRESULT WINAPI music_props_put_##name( IMusicPropertiesInternal *iface, HSTRING value )  \
    {                                                                                              \
        struct music_properties *impl = impl_from_IMusicPropertiesInternal( iface );               \
        HSTRING copy;                                                                              \
        HRESULT hr;                                                                                \
        TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );                        \
        if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;                        \
        WindowsDeleteString( impl->field );                                                        \
        impl->field = copy;                                                                        \
        return S_OK;                                                                               \
    }

DEFINE_MUSIC_STRING( Album, album )
DEFINE_MUSIC_STRING( Artist, artist )
DEFINE_MUSIC_STRING( Title, title )
DEFINE_MUSIC_STRING( AlbumArtist, album_artist )
DEFINE_MUSIC_STRING( Subtitle, subtitle )
DEFINE_MUSIC_STRING( Publisher, publisher )

#define DEFINE_MUSIC_UINT32( name, field )                                                         \
    static HRESULT WINAPI music_props_get_##name( IMusicPropertiesInternal *iface, UINT32 *value )  \
    {                                                                                              \
        struct music_properties *impl = impl_from_IMusicPropertiesInternal( iface );               \
        TRACE( "iface %p, value %p.\n", iface, value );                                            \
        if (!value) return E_POINTER;                                                              \
        *value = impl->field;                                                                      \
        return S_OK;                                                                               \
    }                                                                                              \
    static HRESULT WINAPI music_props_put_##name( IMusicPropertiesInternal *iface, UINT32 value )   \
    {                                                                                              \
        struct music_properties *impl = impl_from_IMusicPropertiesInternal( iface );               \
        TRACE( "iface %p, value %u.\n", iface, value );                                            \
        impl->field = value;                                                                       \
        return S_OK;                                                                               \
    }

DEFINE_MUSIC_UINT32( TrackNumber, track_number )
DEFINE_MUSIC_UINT32( Rating, rating )
DEFINE_MUSIC_UINT32( Year, year )

/* Duration and bitrate would need the file decoded; report them as unknown rather than guess. A
 * zero duration is what Windows reports for a file it has not analysed yet. */
static HRESULT WINAPI music_props_get_Duration( IMusicPropertiesInternal *iface, TimeSpan *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    value->Duration = 0;
    return S_OK;
}

static HRESULT WINAPI music_props_get_Bitrate( IMusicPropertiesInternal *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

/* The multi-valued properties are all empty. They have to be real empty vectors and not NULL: a
 * caller enumerates them without checking. */
static HRESULT WINAPI music_props_get_string_vector( IMusicPropertiesInternal *iface, IVector_HSTRING **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return empty_hstring_vector_create( value );
}

static const struct IMusicPropertiesInternalVtbl music_props_vtbl =
{
    /* IUnknown methods */
    music_props_QueryInterface,
    music_props_AddRef,
    music_props_Release,
    /* IInspectable methods */
    music_props_GetIids,
    music_props_GetRuntimeClassName,
    music_props_GetTrustLevel,
    /* IMusicProperties methods */
    music_props_get_Album,
    music_props_put_Album,
    music_props_get_Artist,
    music_props_put_Artist,
    music_props_get_string_vector,   /* Genre */
    music_props_get_TrackNumber,
    music_props_put_TrackNumber,
    music_props_get_Title,
    music_props_put_Title,
    music_props_get_Rating,
    music_props_put_Rating,
    music_props_get_Duration,
    music_props_get_Bitrate,
    music_props_get_AlbumArtist,
    music_props_put_AlbumArtist,
    music_props_get_string_vector,   /* Composers */
    music_props_get_string_vector,   /* Conductors */
    music_props_get_Subtitle,
    music_props_put_Subtitle,
    music_props_get_string_vector,   /* Producers */
    music_props_get_Publisher,
    music_props_put_Publisher,
    music_props_get_string_vector,   /* Writers */
    music_props_get_Year,
    music_props_put_Year,
};

/* The title is the file name without its extension. */
static HRESULT music_properties_create( const WCHAR *path, struct music_properties **out )
{
    struct music_properties *impl;
    const WCHAR *name, *ext;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMusicPropertiesInternal_iface.lpVtbl = &music_props_vtbl;
    impl->ref = 1;

    name = wcsrchr( path, '\\' );
    name = name ? name + 1 : path;
    ext = wcsrchr( name, '.' );
    if (FAILED(hr = WindowsCreateString( name, ext ? (UINT32)(ext - name) : wcslen( name ), &impl->title )))
    {
        free( impl );
        return hr;
    }

    TRACE( "%s -> title %s.\n", debugstr_w(path), debugstr_hstring(impl->title) );
    *out = impl;
    return S_OK;
}

/* ---------------------------------------------------------------- IAsyncOperation<MusicProperties>
 *
 * The properties are known as soon as the path is, so this is a completed operation: put_Completed
 * runs the handler straight away on the caller's thread.
 */

struct async_music_properties
{
    IAsyncOperationMusicPropertiesInternal IAsyncOperationMusicPropertiesInternal_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    struct music_properties *result;
};

static inline struct async_music_properties *impl_from_IAsyncOperationMusicPropertiesInternal(
        IAsyncOperationMusicPropertiesInternal *iface )
{
    return CONTAINING_RECORD( iface, struct async_music_properties,
                              IAsyncOperationMusicPropertiesInternal_iface );
}

static HRESULT WINAPI async_music_QueryInterface( IAsyncOperationMusicPropertiesInternal *iface,
                                                  REFIID iid, void **out )
{
    struct async_music_properties *impl = impl_from_IAsyncOperationMusicPropertiesInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationMusicPropertiesInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperationMusicPropertiesInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI async_music_AddRef( IAsyncOperationMusicPropertiesInternal *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperationMusicPropertiesInternal( iface )->ref );
}

static ULONG WINAPI async_music_Release( IAsyncOperationMusicPropertiesInternal *iface )
{
    struct async_music_properties *impl = impl_from_IAsyncOperationMusicPropertiesInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->result) IMusicPropertiesInternal_Release( &impl->result->IMusicPropertiesInternal_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_music_GetIids( IAsyncOperationMusicPropertiesInternal *iface,
                                           ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_music_GetRuntimeClassName( IAsyncOperationMusicPropertiesInternal *iface,
                                                       HSTRING *class_name )
{
    return WindowsCreateString(
        L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.FileProperties.MusicProperties>",
        ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.FileProperties.MusicProperties>") - 1,
        class_name );
}

static HRESULT WINAPI async_music_GetTrustLevel( IAsyncOperationMusicPropertiesInternal *iface,
                                                 TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* AsyncOperationCompletedHandler<T>::Invoke is slot 3: a delegate has no IInspectable. */
static HRESULT WINAPI async_music_put_Completed( IAsyncOperationMusicPropertiesInternal *iface,
                                                 IUnknown *handler )
{
    HRESULT (WINAPI *invoke)( IUnknown *, void *, INT32 );

    TRACE( "iface %p, handler %p.\n", iface, handler );

    if (!handler) return S_OK;
    IUnknown_AddRef( handler );
    invoke = (void *)(*(void ***)handler)[3];
    invoke( handler, iface, Completed );
    IUnknown_Release( handler );
    return S_OK;
}

static HRESULT WINAPI async_music_get_Completed( IAsyncOperationMusicPropertiesInternal *iface,
                                                 IUnknown **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    return S_OK;
}

static HRESULT WINAPI async_music_GetResults( IAsyncOperationMusicPropertiesInternal *iface,
                                              IMusicPropertiesInternal **results )
{
    struct async_music_properties *impl = impl_from_IAsyncOperationMusicPropertiesInternal( iface );

    TRACE( "iface %p, results %p.\n", iface, results );

    if (!results) return E_POINTER;
    IMusicPropertiesInternal_AddRef( (*results = &impl->result->IMusicPropertiesInternal_iface) );
    return S_OK;
}

static const struct IAsyncOperationMusicPropertiesInternalVtbl async_music_vtbl =
{
    async_music_QueryInterface,
    async_music_AddRef,
    async_music_Release,
    async_music_GetIids,
    async_music_GetRuntimeClassName,
    async_music_GetTrustLevel,
    async_music_put_Completed,
    async_music_get_Completed,
    async_music_GetResults,
};

DEFINE_IINSPECTABLE_( async_music_info, IAsyncInfo, struct async_music_properties,
                      impl_from_IAsyncInfo_music, IAsyncInfo_iface,
                      &impl->IAsyncOperationMusicPropertiesInternal_iface )

static HRESULT WINAPI async_music_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI async_music_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    if (!status) return E_POINTER;
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI async_music_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code )
{
    if (!code) return E_POINTER;
    *code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_music_info_Cancel( IAsyncInfo *iface ) { return S_OK; }
static HRESULT WINAPI async_music_info_Close( IAsyncInfo *iface ) { return S_OK; }

static const struct IAsyncInfoVtbl async_music_info_vtbl =
{
    async_music_info_QueryInterface,
    async_music_info_AddRef,
    async_music_info_Release,
    async_music_info_GetIids,
    async_music_info_GetRuntimeClassName,
    async_music_info_GetTrustLevel,
    async_music_info_get_Id,
    async_music_info_get_Status,
    async_music_info_get_ErrorCode,
    async_music_info_Cancel,
    async_music_info_Close,
};


/* ---------------------------------------------------------------- the property map
 *
 * RetrievePropertiesAsync is the generic property-system path. There is no property store behind a
 * file here, so the map is empty: Lookup reports E_BOUNDS and HasKey is FALSE for everything. That
 * is the same answer Windows gives for a property it has no value for, and it is the honest one -
 * inventing a duration or an artist would put wrong data in a library view.
 */

struct property_map
{
    IPropertyMapInternal IPropertyMapInternal_iface;
    LONG ref;
};

static inline struct property_map *impl_from_IPropertyMapInternal( IPropertyMapInternal *iface )
{
    return CONTAINING_RECORD( iface, struct property_map, IPropertyMapInternal_iface );
}

static HRESULT WINAPI property_map_QueryInterface( IPropertyMapInternal *iface, REFIID iid, void **out )
{
    struct property_map *impl = impl_from_IPropertyMapInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IPropertyMapInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IPropertyMapInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI property_map_AddRef( IPropertyMapInternal *iface )
{
    return InterlockedIncrement( &impl_from_IPropertyMapInternal( iface )->ref );
}

static ULONG WINAPI property_map_Release( IPropertyMapInternal *iface )
{
    struct property_map *impl = impl_from_IPropertyMapInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI property_map_GetIids( IPropertyMapInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI property_map_GetRuntimeClassName( IPropertyMapInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IMap`2<String, Object>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IMap`2<String, Object>") - 1,
                                class_name );
}

static HRESULT WINAPI property_map_GetTrustLevel( IPropertyMapInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI property_map_Lookup( IPropertyMapInternal *iface, HSTRING key, IInspectable **value )
{
    TRACE( "iface %p, key %s, value %p: no value known.\n", iface, debugstr_hstring( key ), value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_BOUNDS;
}

static HRESULT WINAPI property_map_get_Size( IPropertyMapInternal *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI property_map_HasKey( IPropertyMapInternal *iface, HSTRING key, boolean *value )
{
    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI property_map_GetView( IPropertyMapInternal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

/* The map is a read-only answer about a file, so writing to it changes nothing on disk. Say so
 * rather than accept a value that would silently vanish. */
static HRESULT WINAPI property_map_Insert( IPropertyMapInternal *iface, HSTRING key, IInspectable *value,
                                           boolean *replaced )
{
    FIXME( "iface %p, key %s, value %p, replaced %p stub!\n", iface, debugstr_hstring( key ), value, replaced );
    return E_NOTIMPL;
}

static HRESULT WINAPI property_map_Remove( IPropertyMapInternal *iface, HSTRING key )
{
    FIXME( "iface %p, key %s stub!\n", iface, debugstr_hstring( key ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI property_map_Clear( IPropertyMapInternal *iface )
{
    TRACE( "iface %p: already empty.\n", iface );
    return S_OK;
}

static const struct IPropertyMapInternalVtbl property_map_vtbl =
{
    property_map_QueryInterface,
    property_map_AddRef,
    property_map_Release,
    property_map_GetIids,
    property_map_GetRuntimeClassName,
    property_map_GetTrustLevel,
    property_map_Lookup,
    property_map_get_Size,
    property_map_HasKey,
    property_map_GetView,
    property_map_Insert,
    property_map_Remove,
    property_map_Clear,
};

/* ---------------------------------------------------------------- IAsyncOperation<IMap<...>> */

struct async_property_map
{
    IAsyncOperationPropertyMapInternal IAsyncOperationPropertyMapInternal_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    struct property_map *result;
};

static inline struct async_property_map *impl_from_IAsyncOperationPropertyMapInternal(
        IAsyncOperationPropertyMapInternal *iface )
{
    return CONTAINING_RECORD( iface, struct async_property_map, IAsyncOperationPropertyMapInternal_iface );
}

static HRESULT WINAPI async_map_QueryInterface( IAsyncOperationPropertyMapInternal *iface, REFIID iid, void **out )
{
    struct async_property_map *impl = impl_from_IAsyncOperationPropertyMapInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationPropertyMapInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperationPropertyMapInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI async_map_AddRef( IAsyncOperationPropertyMapInternal *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperationPropertyMapInternal( iface )->ref );
}

static ULONG WINAPI async_map_Release( IAsyncOperationPropertyMapInternal *iface )
{
    struct async_property_map *impl = impl_from_IAsyncOperationPropertyMapInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->result) IPropertyMapInternal_Release( &impl->result->IPropertyMapInternal_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_map_GetIids( IAsyncOperationPropertyMapInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_map_GetRuntimeClassName( IAsyncOperationPropertyMapInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString(
        L"Windows.Foundation.IAsyncOperation`1<Windows.Foundation.Collections.IMap`2<String, Object>>",
        ARRAY_SIZE(L"Windows.Foundation.IAsyncOperation`1<Windows.Foundation.Collections.IMap`2<String, Object>>") - 1,
        class_name );
}

static HRESULT WINAPI async_map_GetTrustLevel( IAsyncOperationPropertyMapInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_map_put_Completed( IAsyncOperationPropertyMapInternal *iface, IUnknown *handler )
{
    HRESULT (WINAPI *invoke)( IUnknown *, void *, INT32 );

    TRACE( "iface %p, handler %p.\n", iface, handler );

    if (!handler) return S_OK;
    IUnknown_AddRef( handler );
    invoke = (void *)(*(void ***)handler)[3];
    invoke( handler, iface, Completed );
    IUnknown_Release( handler );
    return S_OK;
}

static HRESULT WINAPI async_map_get_Completed( IAsyncOperationPropertyMapInternal *iface, IUnknown **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    return S_OK;
}

static HRESULT WINAPI async_map_GetResults( IAsyncOperationPropertyMapInternal *iface,
                                            IPropertyMapInternal **results )
{
    struct async_property_map *impl = impl_from_IAsyncOperationPropertyMapInternal( iface );

    TRACE( "iface %p, results %p.\n", iface, results );

    if (!results) return E_POINTER;
    IPropertyMapInternal_AddRef( (*results = &impl->result->IPropertyMapInternal_iface) );
    return S_OK;
}

static const struct IAsyncOperationPropertyMapInternalVtbl async_map_vtbl =
{
    async_map_QueryInterface,
    async_map_AddRef,
    async_map_Release,
    async_map_GetIids,
    async_map_GetRuntimeClassName,
    async_map_GetTrustLevel,
    async_map_put_Completed,
    async_map_get_Completed,
    async_map_GetResults,
};

DEFINE_IINSPECTABLE_( async_map_info, IAsyncInfo, struct async_property_map,
                      impl_from_IAsyncInfo_map, IAsyncInfo_iface,
                      &impl->IAsyncOperationPropertyMapInternal_iface )

static HRESULT WINAPI async_map_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI async_map_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    if (!status) return E_POINTER;
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI async_map_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code )
{
    if (!code) return E_POINTER;
    *code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_map_info_Cancel( IAsyncInfo *iface ) { return S_OK; }
static HRESULT WINAPI async_map_info_Close( IAsyncInfo *iface ) { return S_OK; }

static const struct IAsyncInfoVtbl async_map_info_vtbl =
{
    async_map_info_QueryInterface,
    async_map_info_AddRef,
    async_map_info_Release,
    async_map_info_GetIids,
    async_map_info_GetRuntimeClassName,
    async_map_info_GetTrustLevel,
    async_map_info_get_Id,
    async_map_info_get_Status,
    async_map_info_get_ErrorCode,
    async_map_info_Cancel,
    async_map_info_Close,
};

HRESULT WINAPI __wine_property_map_async_create( IAsyncOperationPropertyMapInternal **out )
{
    struct async_property_map *async;
    struct property_map *map;

    if (!(map = calloc( 1, sizeof(*map) ))) return E_OUTOFMEMORY;
    map->IPropertyMapInternal_iface.lpVtbl = &property_map_vtbl;
    map->ref = 1;

    if (!(async = calloc( 1, sizeof(*async) )))
    {
        free( map );
        return E_OUTOFMEMORY;
    }
    async->IAsyncOperationPropertyMapInternal_iface.lpVtbl = &async_map_vtbl;
    async->IAsyncInfo_iface.lpVtbl = &async_map_info_vtbl;
    async->ref = 1;
    async->result = map;

    *out = &async->IAsyncOperationPropertyMapInternal_iface;
    return S_OK;
}

/* ---------------------------------------------------------------- StorageItemContentProperties */

struct content_properties
{
    IStorageItemContentPropertiesInternal IStorageItemContentPropertiesInternal_iface;
    IStorageItemExtraPropertiesInternal IStorageItemExtraPropertiesInternal_iface;
    LONG ref;
    WCHAR *path;
};

static inline struct content_properties *impl_from_IStorageItemContentPropertiesInternal(
        IStorageItemContentPropertiesInternal *iface )
{
    return CONTAINING_RECORD( iface, struct content_properties,
                              IStorageItemContentPropertiesInternal_iface );
}

static HRESULT WINAPI content_props_QueryInterface( IStorageItemContentPropertiesInternal *iface,
                                                    REFIID iid, void **out )
{
    struct content_properties *impl = impl_from_IStorageItemContentPropertiesInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageItemContentPropertiesInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IStorageItemContentPropertiesInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageItemExtraPropertiesInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IStorageItemExtraPropertiesInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI content_props_AddRef( IStorageItemContentPropertiesInternal *iface )
{
    return InterlockedIncrement( &impl_from_IStorageItemContentPropertiesInternal( iface )->ref );
}

static ULONG WINAPI content_props_Release( IStorageItemContentPropertiesInternal *iface )
{
    struct content_properties *impl = impl_from_IStorageItemContentPropertiesInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        free( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI content_props_GetIids( IStorageItemContentPropertiesInternal *iface,
                                             ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI content_props_GetRuntimeClassName( IStorageItemContentPropertiesInternal *iface,
                                                         HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Storage.FileProperties.StorageItemContentProperties",
                                ARRAY_SIZE(L"Windows.Storage.FileProperties.StorageItemContentProperties") - 1,
                                class_name );
}

static HRESULT WINAPI content_props_GetTrustLevel( IStorageItemContentPropertiesInternal *iface,
                                                   TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI content_props_GetMusicPropertiesAsync( IStorageItemContentPropertiesInternal *iface,
        IAsyncOperationMusicPropertiesInternal **operation )
{
    struct content_properties *impl = impl_from_IStorageItemContentPropertiesInternal( iface );
    struct async_music_properties *async;
    struct music_properties *props;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    if (FAILED(hr = music_properties_create( impl->path, &props ))) return hr;
    if (!(async = calloc( 1, sizeof(*async) )))
    {
        IMusicPropertiesInternal_Release( &props->IMusicPropertiesInternal_iface );
        return E_OUTOFMEMORY;
    }
    async->IAsyncOperationMusicPropertiesInternal_iface.lpVtbl = &async_music_vtbl;
    async->IAsyncInfo_iface.lpVtbl = &async_music_info_vtbl;
    async->ref = 1;
    async->result = props;

    *operation = &async->IAsyncOperationMusicPropertiesInternal_iface;
    return S_OK;
}

/* Video, image and document properties would each need their own object. Nothing asks for them on
 * a music library page, and an empty one would claim the file is something it is not. */
static HRESULT WINAPI content_props_GetOtherPropertiesAsync( IStorageItemContentPropertiesInternal *iface,
        IInspectable **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static const struct IStorageItemContentPropertiesInternalVtbl content_props_vtbl =
{
    content_props_QueryInterface,
    content_props_AddRef,
    content_props_Release,
    content_props_GetIids,
    content_props_GetRuntimeClassName,
    content_props_GetTrustLevel,
    content_props_GetMusicPropertiesAsync,
    content_props_GetOtherPropertiesAsync,   /* Video */
    content_props_GetOtherPropertiesAsync,   /* Image */
    content_props_GetOtherPropertiesAsync,   /* Document */
};

DEFINE_IINSPECTABLE_( extra_props, IStorageItemExtraPropertiesInternal, struct content_properties,
                      impl_from_IStorageItemExtraPropertiesInternal, IStorageItemExtraPropertiesInternal_iface,
                      &impl->IStorageItemContentPropertiesInternal_iface )

static HRESULT WINAPI extra_props_RetrievePropertiesAsync( IStorageItemExtraPropertiesInternal *iface,
        IIterable_HSTRING *properties_to_retrieve, IAsyncOperationPropertyMapInternal **operation )
{
    TRACE( "iface %p, properties_to_retrieve %p, operation %p.\n", iface, properties_to_retrieve, operation );

    if (!operation) return E_POINTER;
    return __wine_property_map_async_create( operation );
}

/* Writing properties would mean writing tags back to the file. */
static HRESULT WINAPI extra_props_SavePropertiesAsync( IStorageItemExtraPropertiesInternal *iface,
        IInspectable *properties_to_save, IInspectable **operation )
{
    FIXME( "iface %p, properties_to_save %p, operation %p stub!\n", iface, properties_to_save, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI extra_props_SavePropertiesAsyncOverloadDefault( IStorageItemExtraPropertiesInternal *iface,
        IInspectable **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static const struct IStorageItemExtraPropertiesInternalVtbl extra_props_vtbl =
{
    /* IUnknown methods */
    extra_props_QueryInterface,
    extra_props_AddRef,
    extra_props_Release,
    /* IInspectable methods */
    extra_props_GetIids,
    extra_props_GetRuntimeClassName,
    extra_props_GetTrustLevel,
    /* IStorageItemExtraProperties methods */
    extra_props_RetrievePropertiesAsync,
    extra_props_SavePropertiesAsync,
    extra_props_SavePropertiesAsyncOverloadDefault,
};

HRESULT content_properties_create( const WCHAR *path, IStorageItemContentPropertiesInternal **out )
{
    struct content_properties *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStorageItemContentPropertiesInternal_iface.lpVtbl = &content_props_vtbl;
    impl->IStorageItemExtraPropertiesInternal_iface.lpVtbl = &extra_props_vtbl;
    impl->ref = 1;
    if (!(impl->path = wcsdup( path )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }

    *out = &impl->IStorageItemContentPropertiesInternal_iface;
    return S_OK;
}

/*
 * Windows.Media.Core.MediaSource and Windows.Media.Playback.MediaPlaybackItem
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

/*
 * A MediaSource on Windows owns a demuxed pipeline. Here it is a handle onto a file: the player
 * opens the file itself with a Media Foundation source reader, so all a source has to do is say
 * which file it is. That is what IWineMediaPath is for - there is no public interface that
 * answers the question, and MediaPlaybackItem has to forward it too, because an app hands the
 * player an item (or a list of them), not the source.
 */

/* ---------------------------------------------------------------- MediaSource */

struct media_source
{
    IMediaSource2Internal IMediaSource2Internal_iface;
    IWineMediaPath IWineMediaPath_iface;
    LONG ref;
    HSTRING path;
    /* A source made from a binder has no path of its own: the binder only learns the file when
     * the player asks for it, which is the whole point of a binder. Snapshotting the path at
     * creation time left the source permanently empty. */
    IMediaBinderInternal *binder;
};

static inline struct media_source *impl_from_IMediaSource2Internal( IMediaSource2Internal *iface )
{
    return CONTAINING_RECORD( iface, struct media_source, IMediaSource2Internal_iface );
}

static HRESULT WINAPI media_source_QueryInterface( IMediaSource2Internal *iface, REFIID iid, void **out )
{
    struct media_source *impl = impl_from_IMediaSource2Internal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaSource2Internal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaSource2Internal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWineMediaPath ))
    {
        IInspectable_AddRef( (*out = &impl->IWineMediaPath_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI media_source_AddRef( IMediaSource2Internal *iface )
{
    return InterlockedIncrement( &impl_from_IMediaSource2Internal( iface )->ref );
}

static ULONG WINAPI media_source_Release( IMediaSource2Internal *iface )
{
    struct media_source *impl = impl_from_IMediaSource2Internal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->binder) IMediaBinderInternal_Release( impl->binder );
        WindowsDeleteString( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI media_source_GetIids( IMediaSource2Internal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_GetRuntimeClassName( IMediaSource2Internal *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_MediaSource,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_MediaSource) - 1, class_name );
}

static HRESULT WINAPI media_source_GetTrustLevel( IMediaSource2Internal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* The source is opened by the player when it plays, so it is never "open" on its own and the
 * open-completed event never fires - but registering has to succeed. */
static HRESULT WINAPI media_source_add_OpenOperationCompleted( IMediaSource2Internal *iface, IInspectable *handler,
                                                               EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p: never fires.\n", iface, handler, token );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI media_source_remove_OpenOperationCompleted( IMediaSource2Internal *iface,
                                                                  EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI media_source_get_CustomProperties( IMediaSource2Internal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

/* The duration is not known until the file is opened, and opening it here would mean decoding
 * twice. Report zero, which is what a source that has not been opened reports. */
static HRESULT WINAPI media_source_get_Duration( IMediaSource2Internal *iface, IInspectable **value )
{
    TRACE( "iface %p, value %p: not opened yet.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI media_source_get_IsOpen( IMediaSource2Internal *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI media_source_get_ExternalTimedTextSources( IMediaSource2Internal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_get_ExternalTimedMetadataTracks( IMediaSource2Internal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static const struct IMediaSource2InternalVtbl media_source_vtbl =
{
    /* IUnknown methods */
    media_source_QueryInterface,
    media_source_AddRef,
    media_source_Release,
    /* IInspectable methods */
    media_source_GetIids,
    media_source_GetRuntimeClassName,
    media_source_GetTrustLevel,
    /* IMediaSource2Internal methods */
    media_source_add_OpenOperationCompleted,
    media_source_remove_OpenOperationCompleted,
    media_source_get_CustomProperties,
    media_source_get_Duration,
    media_source_get_IsOpen,
    media_source_get_ExternalTimedTextSources,
    media_source_get_ExternalTimedMetadataTracks,
};

DEFINE_IINSPECTABLE_( media_source_path, IWineMediaPath, struct media_source,
                      impl_from_IWineMediaPath_source, IWineMediaPath_iface,
                      &impl->IMediaSource2Internal_iface )

static HRESULT WINAPI media_source_path_get_Path( IWineMediaPath *iface, HSTRING *value )
{
    struct media_source *impl = impl_from_IWineMediaPath_source( iface );
    IWineMediaPath *binder_path;
    HRESULT hr;

    TRACE( "iface %p, value %p -> %s.\n", iface, value, debugstr_hstring( impl->path ) );

    if (!value) return E_POINTER;
    if (impl->path) return WindowsDuplicateString( impl->path, value );

    if (impl->binder &&
        SUCCEEDED(IMediaBinderInternal_QueryInterface( impl->binder, &IID_IWineMediaPath,
                                                       (void **)&binder_path )))
    {
        hr = IWineMediaPath_get_Path( binder_path, value );
        IWineMediaPath_Release( binder_path );
        return hr;
    }

    *value = NULL;
    return E_PENDING;
}

static const struct IWineMediaPathVtbl media_source_path_vtbl =
{
    media_source_path_QueryInterface,
    media_source_path_AddRef,
    media_source_path_Release,
    media_source_path_GetIids,
    media_source_path_GetRuntimeClassName,
    media_source_path_GetTrustLevel,
    media_source_path_get_Path,
};

static HRESULT media_source_create( HSTRING path, IMediaSource2Internal **out )
{
    struct media_source *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaSource2Internal_iface.lpVtbl = &media_source_vtbl;
    impl->IWineMediaPath_iface.lpVtbl = &media_source_path_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( path, &impl->path )))
    {
        free( impl );
        return hr;
    }

    TRACE( "created MediaSource %p for %s.\n", impl, debugstr_hstring( path ) );
    *out = &impl->IMediaSource2Internal_iface;
    return S_OK;
}

/* ---------------------------------------------------------------- MediaPlaybackItem */

struct playback_item
{
    IMediaPlaybackItemInternal IMediaPlaybackItemInternal_iface;
    IWineMediaPath IWineMediaPath_iface;
    LONG ref;
    IMediaSource2Internal *source;
};

static inline struct playback_item *impl_from_IMediaPlaybackItemInternal( IMediaPlaybackItemInternal *iface )
{
    return CONTAINING_RECORD( iface, struct playback_item, IMediaPlaybackItemInternal_iface );
}

static HRESULT WINAPI playback_item_QueryInterface( IMediaPlaybackItemInternal *iface, REFIID iid, void **out )
{
    struct playback_item *impl = impl_from_IMediaPlaybackItemInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaPlaybackItemInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaPlaybackItemInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWineMediaPath ))
    {
        IInspectable_AddRef( (*out = &impl->IWineMediaPath_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI playback_item_AddRef( IMediaPlaybackItemInternal *iface )
{
    return InterlockedIncrement( &impl_from_IMediaPlaybackItemInternal( iface )->ref );
}

static ULONG WINAPI playback_item_Release( IMediaPlaybackItemInternal *iface )
{
    struct playback_item *impl = impl_from_IMediaPlaybackItemInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->source) IMediaSource2Internal_Release( impl->source );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI playback_item_GetIids( IMediaPlaybackItemInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_item_GetRuntimeClassName( IMediaPlaybackItemInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Playback_MediaPlaybackItem,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Playback_MediaPlaybackItem) - 1,
                                class_name );
}

static HRESULT WINAPI playback_item_GetTrustLevel( IMediaPlaybackItemInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* Track lists come from a demuxed source; nothing is demuxed until the player opens the file, so
 * these never change and the registrations never fire. */
static HRESULT WINAPI playback_item_add_TracksChanged( IMediaPlaybackItemInternal *iface, IInspectable *handler,
                                                       EventRegistrationToken *token )
{
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI playback_item_remove_TracksChanged( IMediaPlaybackItemInternal *iface,
                                                          EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI playback_item_get_Source( IMediaPlaybackItemInternal *iface, IMediaSource2Internal **value )
{
    struct playback_item *impl = impl_from_IMediaPlaybackItemInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->source)) IMediaSource2Internal_AddRef( *value );
    return S_OK;
}

/* The track lists come from demuxing, which happens in the player when it opens the file, so an
 * item never has any. They have to be empty collections rather than a refusal: returning
 * E_NOTIMPL here threw straight out of the app's open-file path. */
static HRESULT WINAPI playback_item_get_AudioTracks( IMediaPlaybackItemInternal *iface, IInspectable **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return audio_track_list_create( (IInspectable *)iface, value );
}

static HRESULT WINAPI playback_item_get_VideoTracks( IMediaPlaybackItemInternal *iface, IInspectable **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return video_track_list_create( (IInspectable *)iface, value );
}

static HRESULT WINAPI playback_item_get_MetadataTracks( IMediaPlaybackItemInternal *iface, IInspectable **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return metadata_track_list_create( (IInspectable *)iface, value );
}

static const struct IMediaPlaybackItemInternalVtbl playback_item_vtbl =
{
    /* IUnknown methods */
    playback_item_QueryInterface,
    playback_item_AddRef,
    playback_item_Release,
    /* IInspectable methods */
    playback_item_GetIids,
    playback_item_GetRuntimeClassName,
    playback_item_GetTrustLevel,
    /* IMediaPlaybackItemInternal methods */
    playback_item_add_TracksChanged,      /* Audio */
    playback_item_remove_TracksChanged,
    playback_item_add_TracksChanged,      /* Video */
    playback_item_remove_TracksChanged,
    playback_item_add_TracksChanged,      /* TimedMetadata */
    playback_item_remove_TracksChanged,
    playback_item_get_Source,
    playback_item_get_AudioTracks,
    playback_item_get_VideoTracks,
    playback_item_get_MetadataTracks,
};

DEFINE_IINSPECTABLE_( playback_item_path, IWineMediaPath, struct playback_item,
                      impl_from_IWineMediaPath_item, IWineMediaPath_iface,
                      &impl->IMediaPlaybackItemInternal_iface )

static HRESULT WINAPI playback_item_path_get_Path( IWineMediaPath *iface, HSTRING *value )
{
    struct playback_item *impl = impl_from_IWineMediaPath_item( iface );
    IWineMediaPath *source_path;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!impl->source) return E_FAIL;
    if (FAILED(hr = IMediaSource2Internal_QueryInterface( impl->source, &IID_IWineMediaPath, (void **)&source_path )))
        return hr;
    hr = IWineMediaPath_get_Path( source_path, value );
    IWineMediaPath_Release( source_path );
    return hr;
}

static const struct IWineMediaPathVtbl playback_item_path_vtbl =
{
    playback_item_path_QueryInterface,
    playback_item_path_AddRef,
    playback_item_path_Release,
    playback_item_path_GetIids,
    playback_item_path_GetRuntimeClassName,
    playback_item_path_GetTrustLevel,
    playback_item_path_get_Path,
};

/* A TypedEventHandler<A*, B*> is a delegate: three IUnknown slots and Invoke. We only ever call
 * the pointer the app registered, never ask anyone for this interface, so it needs no IID. */
typedef struct IWineEventHandler IWineEventHandler;
struct IWineEventHandlerVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)( IWineEventHandler *, REFIID, void ** );
    ULONG (STDMETHODCALLTYPE *AddRef)( IWineEventHandler * );
    ULONG (STDMETHODCALLTYPE *Release)( IWineEventHandler * );
    HRESULT (STDMETHODCALLTYPE *Invoke)( IWineEventHandler *, void *sender, void *args );
};
struct IWineEventHandler { const struct IWineEventHandlerVtbl *lpVtbl; };

/* ---------------------------------------------------------------- MediaBinder
 *
 * A binder is a promise to produce content later: the app sets a token, the player raises Binding
 * when it needs the content, and the handler calls SetSource on the args. Nothing here raises
 * Binding - the player opens a file directly - so a binder created from one carries the source it
 * was given and answers IWineMediaPath from it, which is what makes a bound source playable.
 */
struct media_binder
{
    IMediaBinderInternal IMediaBinderInternal_iface;
    IWineMediaPath IWineMediaPath_iface;
    LONG ref;
    HSTRING token;
    IMediaSource2Internal *source;

    /* Binding is the only channel through which the app names the file, so the handler has to be
     * kept and actually called. resolving guards against re-entering the handler from the
     * get_Path the handler's own setters can provoke. */
    IWineEventHandler *handler;
    HSTRING path;
    BOOL resolving;
    CRITICAL_SECTION cs;
};

static HRESULT binding_args_create( struct media_binder *binder, IMediaBindingEventArgsInternal **out );

static inline struct media_binder *impl_from_IMediaBinderInternal( IMediaBinderInternal *iface )
{
    return CONTAINING_RECORD( iface, struct media_binder, IMediaBinderInternal_iface );
}

static HRESULT WINAPI binder_QueryInterface( IMediaBinderInternal *iface, REFIID iid, void **out )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaBinderInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaBinderInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWineMediaPath ))
    {
        IInspectable_AddRef( (*out = &impl->IWineMediaPath_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI binder_AddRef( IMediaBinderInternal *iface )
{
    return InterlockedIncrement( &impl_from_IMediaBinderInternal( iface )->ref );
}

static ULONG WINAPI binder_Release( IMediaBinderInternal *iface )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->source) IMediaSource2Internal_Release( impl->source );
        if (impl->handler) impl->handler->lpVtbl->Release( impl->handler );
        WindowsDeleteString( impl->path );
        WindowsDeleteString( impl->token );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI binder_GetIids( IMediaBinderInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI binder_GetRuntimeClassName( IMediaBinderInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_MediaBinder,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_MediaBinder) - 1, class_name );
}

static HRESULT WINAPI binder_GetTrustLevel( IMediaBinderInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI binder_add_Binding( IMediaBinderInternal *iface, IInspectable *handler,
                                          EventRegistrationToken *token )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    EnterCriticalSection( &impl->cs );
    if (impl->handler) impl->handler->lpVtbl->Release( impl->handler );
    if ((impl->handler = (IWineEventHandler *)handler)) impl->handler->lpVtbl->AddRef( impl->handler );
    LeaveCriticalSection( &impl->cs );

    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI binder_remove_Binding( IMediaBinderInternal *iface, EventRegistrationToken token )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );

    EnterCriticalSection( &impl->cs );
    if (impl->handler) impl->handler->lpVtbl->Release( impl->handler );
    impl->handler = NULL;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

/* Raise Binding and wait for the answer. The handler may set the content inline, or take a
 * deferral and set it from another thread; both end with impl->path set, so poll for that rather
 * than trying to model the deferral's lifetime. */
static HRESULT binder_resolve( struct media_binder *impl, HSTRING *value )
{
    IMediaBindingEventArgsInternal *args;
    IWineEventHandler *handler;
    unsigned int i;
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (impl->path || impl->resolving || !impl->handler)
    {
        hr = impl->path ? WindowsDuplicateString( impl->path, value ) : E_PENDING;
        LeaveCriticalSection( &impl->cs );
        return hr;
    }
    impl->resolving = TRUE;
    handler = impl->handler;
    handler->lpVtbl->AddRef( handler );
    LeaveCriticalSection( &impl->cs );

    if (SUCCEEDED(hr = binding_args_create( impl, &args )))
    {
        TRACE( "binder %p: raising Binding.\n", impl );
        hr = handler->lpVtbl->Invoke( handler, &impl->IMediaBinderInternal_iface, args );
        if (FAILED(hr)) WARN( "binder %p: the Binding handler failed, hr %#lx.\n", impl, hr );

        /* A deferred answer arrives on another thread; five seconds is far longer than opening a
         * local file takes and still bounded, so a handler that never answers cannot wedge us. */
        for (i = 0; i < 500; i++)
        {
            EnterCriticalSection( &impl->cs );
            hr = impl->path ? S_OK : E_PENDING;
            LeaveCriticalSection( &impl->cs );
            if (hr == S_OK) break;
            Sleep( 10 );
        }
        IMediaBindingEventArgsInternal_Release( args );
    }
    handler->lpVtbl->Release( handler );

    EnterCriticalSection( &impl->cs );
    impl->resolving = FALSE;
    if (impl->path)
    {
        TRACE( "binder %p: bound to %s.\n", impl, debugstr_hstring( impl->path ) );
        hr = WindowsDuplicateString( impl->path, value );
    }
    else
    {
        WARN( "binder %p: Binding did not name any content.\n", impl );
        hr = E_PENDING;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI binder_get_Token( IMediaBinderInternal *iface, HSTRING *value )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->token, value );
}

static HRESULT WINAPI binder_put_Token( IMediaBinderInternal *iface, HSTRING value )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->token );
    impl->token = copy;
    return S_OK;
}

static HRESULT WINAPI binder_get_Source( IMediaBinderInternal *iface, IMediaSource2Internal **value )
{
    struct media_binder *impl = impl_from_IMediaBinderInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->source)) IMediaSource2Internal_AddRef( *value );
    return S_OK;
}

static const struct IMediaBinderInternalVtbl binder_vtbl =
{
    /* IUnknown methods */
    binder_QueryInterface,
    binder_AddRef,
    binder_Release,
    /* IInspectable methods */
    binder_GetIids,
    binder_GetRuntimeClassName,
    binder_GetTrustLevel,
    /* IMediaBinder methods */
    binder_add_Binding,
    binder_remove_Binding,
    binder_get_Token,
    binder_put_Token,
    binder_get_Source,
};

DEFINE_IINSPECTABLE_( binder_path, IWineMediaPath, struct media_binder,
                      impl_from_IWineMediaPath_binder, IWineMediaPath_iface,
                      &impl->IMediaBinderInternal_iface )

static HRESULT WINAPI binder_path_get_Path( IWineMediaPath *iface, HSTRING *value )
{
    struct media_binder *impl = impl_from_IWineMediaPath_binder( iface );
    IWineMediaPath *source_path;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    if (SUCCEEDED(hr = binder_resolve( impl, value ))) return hr;

    if (!impl->source) return hr;
    if (FAILED(hr = IMediaSource2Internal_QueryInterface( impl->source, &IID_IWineMediaPath,
                                                          (void **)&source_path )))
        return hr;
    hr = IWineMediaPath_get_Path( source_path, value );
    IWineMediaPath_Release( source_path );
    return hr;
}

static const struct IWineMediaPathVtbl binder_path_vtbl =
{
    binder_path_QueryInterface,
    binder_path_AddRef,
    binder_path_Release,
    binder_path_GetIids,
    binder_path_GetRuntimeClassName,
    binder_path_GetTrustLevel,
    binder_path_get_Path,
};


/* ---------------------------------------------------------- MediaBindingEventArgs
 *
 * What the app receives when Binding fires, and the only place it ever names the picked file.
 * Every setter lands in the binder's path; the player then opens that file exactly as it would
 * have opened one passed to MediaSource.CreateFromStorageFile.
 */
struct binding_args
{
    IMediaBindingEventArgsInternal IMediaBindingEventArgsInternal_iface;
    IMediaBindingEventArgs2Internal IMediaBindingEventArgs2Internal_iface;
    IDeferral IDeferral_iface;
    LONG ref;
    struct media_binder *binder;
};

static inline struct binding_args *impl_from_IMediaBindingEventArgsInternal( IMediaBindingEventArgsInternal *iface )
{
    return CONTAINING_RECORD( iface, struct binding_args, IMediaBindingEventArgsInternal_iface );
}

static HRESULT WINAPI binding_args_QueryInterface( IMediaBindingEventArgsInternal *iface, REFIID iid, void **out )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgsInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaBindingEventArgsInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaBindingEventArgsInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaBindingEventArgs2Internal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaBindingEventArgs2Internal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI binding_args_AddRef( IMediaBindingEventArgsInternal *iface )
{
    return InterlockedIncrement( &impl_from_IMediaBindingEventArgsInternal( iface )->ref );
}

static ULONG WINAPI binding_args_Release( IMediaBindingEventArgsInternal *iface )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgsInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IMediaBinderInternal_Release( &impl->binder->IMediaBinderInternal_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI binding_args_GetIids( IMediaBindingEventArgsInternal *iface, ULONG *c, IID **i )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI binding_args_GetRuntimeClassName( IMediaBindingEventArgsInternal *iface, HSTRING *name )
{
    static const WCHAR *cls = L"Windows.Media.Core.MediaBindingEventArgs";
    return WindowsCreateString( cls, wcslen( cls ), name );
}

static HRESULT WINAPI binding_args_GetTrustLevel( IMediaBindingEventArgsInternal *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

/* Binding is never cancelled here, but registering has to succeed: an app that treats the
 * subscription as part of answering the event would otherwise never answer it. */
static HRESULT WINAPI binding_args_add_Canceled( IMediaBindingEventArgsInternal *iface,
                                                 IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p: Canceled never fires.\n", iface, handler );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI binding_args_remove_Canceled( IMediaBindingEventArgsInternal *iface,
                                                    EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI binding_args_get_MediaBinder( IMediaBindingEventArgsInternal *iface,
                                                    IMediaBinderInternal **value )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IMediaBinderInternal_AddRef( (*value = &impl->binder->IMediaBinderInternal_iface) );
    return S_OK;
}

/* Store whatever the handler resolved to. binder_resolve is polling for exactly this. */
static void binding_args_set_path( struct binding_args *impl, HSTRING path )
{
    HSTRING copy = NULL;

    if (FAILED(WindowsDuplicateString( path, &copy ))) return;

    EnterCriticalSection( &impl->binder->cs );
    WindowsDeleteString( impl->binder->path );
    impl->binder->path = copy;
    LeaveCriticalSection( &impl->binder->cs );

    TRACE( "binder %p bound to %s.\n", impl->binder, debugstr_hstring( copy ) );
}

static HRESULT WINAPI binding_args_SetUri( IMediaBindingEventArgsInternal *iface, IInspectable *uri )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgsInternal( iface );
    IUriRuntimeClass *uri_iface;
    HSTRING raw = NULL;
    HRESULT hr;

    TRACE( "iface %p, uri %p.\n", iface, uri );

    if (!uri) return E_INVALIDARG;
    if (FAILED(hr = IInspectable_QueryInterface( uri, &IID_IUriRuntimeClass, (void **)&uri_iface )))
        return hr;
    hr = IUriRuntimeClass_get_RawUri( uri_iface, &raw );
    IUriRuntimeClass_Release( uri_iface );
    if (FAILED(hr)) return hr;

    binding_args_set_path( impl, raw );
    WindowsDeleteString( raw );
    return S_OK;
}

static HRESULT WINAPI binding_args_SetStorageFile( IMediaBindingEventArgs2Internal *iface, IInspectable *file );

/* A stream reference still knows which item it came from, so ask it for the path rather than
 * reading the stream: the player opens files itself. */
static HRESULT binding_args_set_from_item( struct binding_args *impl, IInspectable *obj )
{
    IStorageItem *item;
    HSTRING path = NULL;
    HRESULT hr;

    if (!obj) return E_INVALIDARG;
    if (FAILED(hr = IInspectable_QueryInterface( obj, &IID_IStorageItem, (void **)&item ))) return hr;
    hr = IStorageItem_get_Path( item, &path );
    IStorageItem_Release( item );
    if (FAILED(hr)) return hr;

    binding_args_set_path( impl, path );
    WindowsDeleteString( path );
    return S_OK;
}

static HRESULT WINAPI binding_args_SetStream( IMediaBindingEventArgsInternal *iface,
                                              IInspectable *stream, HSTRING content_type )
{
    FIXME( "iface %p, stream %p, content_type %s: the player opens files, not streams.\n",
           iface, stream, debugstr_hstring( content_type ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI binding_args_SetStreamReference( IMediaBindingEventArgsInternal *iface,
                                                       IInspectable *stream, HSTRING content_type )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgsInternal( iface );

    TRACE( "iface %p, stream %p, content_type %s.\n", iface, stream, debugstr_hstring( content_type ) );

    return binding_args_set_from_item( impl, stream );
}

/* The deferral is the args themselves: the handler completes it when it has answered, and
 * binder_resolve is already waiting on the path rather than on this object. */
static inline struct binding_args *impl_from_IDeferral( IDeferral *iface )
{
    return CONTAINING_RECORD( iface, struct binding_args, IDeferral_iface );
}

static HRESULT WINAPI binding_deferral_QueryInterface( IDeferral *iface, REFIID iid, void **out )
{
    struct binding_args *impl = impl_from_IDeferral( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDeferral ))
    {
        IInspectable_AddRef( (*out = &impl->IDeferral_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI binding_deferral_AddRef( IDeferral *iface )
{
    return binding_args_AddRef( &impl_from_IDeferral( iface )->IMediaBindingEventArgsInternal_iface );
}

static ULONG WINAPI binding_deferral_Release( IDeferral *iface )
{
    return binding_args_Release( &impl_from_IDeferral( iface )->IMediaBindingEventArgsInternal_iface );
}

static HRESULT WINAPI binding_deferral_GetIids( IDeferral *iface, ULONG *c, IID **i )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI binding_deferral_GetRuntimeClassName( IDeferral *iface, HSTRING *name )
{
    static const WCHAR *cls = L"Windows.Foundation.Deferral";
    return WindowsCreateString( cls, wcslen( cls ), name );
}

static HRESULT WINAPI binding_deferral_GetTrustLevel( IDeferral *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI binding_deferral_Complete( IDeferral *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static const struct IDeferralVtbl binding_deferral_vtbl =
{
    binding_deferral_QueryInterface,
    binding_deferral_AddRef,
    binding_deferral_Release,
    binding_deferral_GetIids,
    binding_deferral_GetRuntimeClassName,
    binding_deferral_GetTrustLevel,
    binding_deferral_Complete,
};

static HRESULT WINAPI binding_args_GetDeferral( IMediaBindingEventArgsInternal *iface, IDeferral **value )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgsInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IDeferral_AddRef( (*value = &impl->IDeferral_iface) );
    return S_OK;
}

static const struct IMediaBindingEventArgsInternalVtbl binding_args_vtbl =
{
    /* IUnknown methods */
    binding_args_QueryInterface,
    binding_args_AddRef,
    binding_args_Release,
    /* IInspectable methods */
    binding_args_GetIids,
    binding_args_GetRuntimeClassName,
    binding_args_GetTrustLevel,
    /* IMediaBindingEventArgs methods */
    binding_args_add_Canceled,
    binding_args_remove_Canceled,
    binding_args_get_MediaBinder,
    binding_args_GetDeferral,
    binding_args_SetUri,
    binding_args_SetStream,
    binding_args_SetStreamReference,
};

DEFINE_IINSPECTABLE_( binding_args2, IMediaBindingEventArgs2Internal, struct binding_args,
                      impl_from_IMediaBindingEventArgs2Internal, IMediaBindingEventArgs2Internal_iface,
                      &impl->IMediaBindingEventArgsInternal_iface )

static HRESULT WINAPI binding_args_SetAdaptiveMediaSource( IMediaBindingEventArgs2Internal *iface,
                                                           IInspectable *source )
{
    FIXME( "iface %p, source %p: adaptive sources are not supported.\n", iface, source );
    return E_NOTIMPL;
}

static HRESULT WINAPI binding_args_SetStorageFile( IMediaBindingEventArgs2Internal *iface, IInspectable *file )
{
    struct binding_args *impl = impl_from_IMediaBindingEventArgs2Internal( iface );

    TRACE( "iface %p, file %p.\n", iface, file );

    return binding_args_set_from_item( impl, file );
}

static const struct IMediaBindingEventArgs2InternalVtbl binding_args2_vtbl =
{
    binding_args2_QueryInterface,
    binding_args2_AddRef,
    binding_args2_Release,
    binding_args2_GetIids,
    binding_args2_GetRuntimeClassName,
    binding_args2_GetTrustLevel,
    binding_args_SetAdaptiveMediaSource,
    binding_args_SetStorageFile,
};

static HRESULT binding_args_create( struct media_binder *binder, IMediaBindingEventArgsInternal **out )
{
    struct binding_args *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaBindingEventArgsInternal_iface.lpVtbl = &binding_args_vtbl;
    impl->IMediaBindingEventArgs2Internal_iface.lpVtbl = &binding_args2_vtbl;
    impl->IDeferral_iface.lpVtbl = &binding_deferral_vtbl;
    impl->ref = 1;
    IMediaBinderInternal_AddRef( (impl->binder = binder, &binder->IMediaBinderInternal_iface) );

    *out = &impl->IMediaBindingEventArgsInternal_iface;
    return S_OK;
}

/* ---------------------------------------------------------------- activation factories */

struct media_source_statics
{
    IActivationFactory IActivationFactory_iface;
    IMediaSourceStaticsInternal IMediaSourceStaticsInternal_iface;
    IMediaSourceStatics2Internal IMediaSourceStatics2Internal_iface;
    LONG ref;
};

static inline struct media_source_statics *impl_from_IActivationFactory_ms( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct media_source_statics, IActivationFactory_iface );
}

static HRESULT WINAPI media_source_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct media_source_statics *impl = impl_from_IActivationFactory_ms( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaSourceStaticsInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaSourceStaticsInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaSourceStatics2Internal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaSourceStatics2Internal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI media_source_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory_ms( iface )->ref );
}

static ULONG WINAPI media_source_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory_ms( iface )->ref );
}

static HRESULT WINAPI media_source_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_MediaSource,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_MediaSource) - 1, class_name );
}

static HRESULT WINAPI media_source_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* MediaSource has no default constructor on Windows either. */
    if (instance) *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl media_source_factory_vtbl =
{
    media_source_factory_QueryInterface,
    media_source_factory_AddRef,
    media_source_factory_Release,
    media_source_factory_GetIids,
    media_source_factory_GetRuntimeClassName,
    media_source_factory_GetTrustLevel,
    media_source_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( media_source_statics, IMediaSourceStaticsInternal, struct media_source_statics,
                     IActivationFactory_iface )

static HRESULT WINAPI media_source_statics_CreateFromStorageFile( IMediaSourceStaticsInternal *iface,
        IInspectable *file, IMediaSource2Internal **value )
{
    IStorageItem *item = NULL;
    HSTRING path = NULL;
    HRESULT hr;

    TRACE( "iface %p, file %p, value %p.\n", iface, file, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!file) return E_INVALIDARG;

    if (FAILED(hr = IInspectable_QueryInterface( file, &IID_IStorageItem, (void **)&item ))) return hr;
    hr = IStorageItem_get_Path( item, &path );
    IStorageItem_Release( item );
    if (FAILED(hr)) return hr;

    hr = media_source_create( path, value );
    WindowsDeleteString( path );
    return hr;
}

static HRESULT WINAPI media_source_statics_CreateFromUri( IMediaSourceStaticsInternal *iface,
        IInspectable *uri, IMediaSource2Internal **value )
{
    HSTRING raw = NULL;
    HRESULT hr;

    TRACE( "iface %p, uri %p, value %p.\n", iface, uri, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!uri) return E_INVALIDARG;

    if (FAILED(hr = IUriRuntimeClass_get_RawUri( (IUriRuntimeClass *)uri, &raw ))) return hr;
    hr = media_source_create( raw, value );
    WindowsDeleteString( raw );
    return hr;
}

/* The other factories take sources Wine cannot open from a path - an adaptive stream, a
 * MediaStreamSource driven by the app, a raw byte stream. */
static HRESULT WINAPI media_source_statics_CreateUnsupported( IMediaSourceStaticsInternal *iface,
        IInspectable *source, IMediaSource2Internal **value )
{
    FIXME( "iface %p, source %p, value %p: only files and URIs are supported.\n", iface, source, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

/* CreateFromStream and CreateFromStreamReference take a content type as well; the extra argument
 * is ignored, but the thunk has to have the right shape to sit in the vtable. */
static HRESULT WINAPI media_source_statics_CreateUnsupportedWithType( IMediaSourceStaticsInternal *iface,
        IInspectable *source, HSTRING content_type, IMediaSource2Internal **value )
{
    FIXME( "iface %p, source %p, content_type %s, value %p: only files and URIs are supported.\n",
           iface, source, debugstr_hstring( content_type ), value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static const struct IMediaSourceStaticsInternalVtbl media_source_statics_vtbl =
{
    /* IUnknown methods */
    media_source_statics_QueryInterface,
    media_source_statics_AddRef,
    media_source_statics_Release,
    /* IInspectable methods */
    media_source_statics_GetIids,
    media_source_statics_GetRuntimeClassName,
    media_source_statics_GetTrustLevel,
    /* IMediaSourceStaticsInternal methods */
    media_source_statics_CreateUnsupported,   /* CreateFromAdaptiveMediaSource */
    media_source_statics_CreateUnsupported,   /* CreateFromMediaStreamSource */
    media_source_statics_CreateUnsupported,   /* CreateFromMseStreamSource */
    media_source_statics_CreateUnsupported,   /* CreateFromIMediaSource */
    media_source_statics_CreateFromStorageFile,
    media_source_statics_CreateUnsupportedWithType,   /* CreateFromStream */
    media_source_statics_CreateUnsupportedWithType,   /* CreateFromStreamReference */
    media_source_statics_CreateFromUri,
};

DEFINE_IINSPECTABLE_( media_source_statics2, IMediaSourceStatics2Internal, struct media_source_statics,
                      impl_from_IMediaSourceStatics2Internal, IMediaSourceStatics2Internal_iface,
                      &impl->IActivationFactory_iface )

/* A source made from a binder holds the binder, not a path: the binder does not know which file
 * it is until Binding fires, and Binding does not fire until the player asks. Reading the path
 * here instead left every bound source empty for the rest of its life. */
static HRESULT WINAPI media_source_statics2_CreateFromMediaBinder( IMediaSourceStatics2Internal *iface,
        IMediaBinderInternal *binder, IMediaSource2Internal **value )
{
    struct media_source *impl;
    HRESULT hr;

    TRACE( "iface %p, binder %p, value %p.\n", iface, binder, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!binder) return E_INVALIDARG;

    if (FAILED(hr = media_source_create( NULL, value ))) return hr;
    impl = impl_from_IMediaSource2Internal( *value );
    WindowsDeleteString( impl->path );
    impl->path = NULL;
    IMediaBinderInternal_AddRef( (impl->binder = binder) );
    return S_OK;
}

static const struct IMediaSourceStatics2InternalVtbl media_source_statics2_vtbl =
{
    media_source_statics2_QueryInterface,
    media_source_statics2_AddRef,
    media_source_statics2_Release,
    media_source_statics2_GetIids,
    media_source_statics2_GetRuntimeClassName,
    media_source_statics2_GetTrustLevel,
    media_source_statics2_CreateFromMediaBinder,
};

static struct media_source_statics media_source_statics =
{
    {&media_source_factory_vtbl},
    {&media_source_statics_vtbl},
    {&media_source_statics2_vtbl},
    1,
};

/* MediaBinder's own factory: the class has a default constructor. */
struct binder_statics { IActivationFactory IActivationFactory_iface; LONG ref; };

static inline struct binder_statics *impl_from_IActivationFactory_binder( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct binder_statics, IActivationFactory_iface );
}

static HRESULT WINAPI binder_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct binder_statics *impl = impl_from_IActivationFactory_binder( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI binder_factory_AddRef( IActivationFactory *iface )
{ return InterlockedIncrement( &impl_from_IActivationFactory_binder( iface )->ref ); }
static ULONG WINAPI binder_factory_Release( IActivationFactory *iface )
{ return InterlockedDecrement( &impl_from_IActivationFactory_binder( iface )->ref ); }
static HRESULT WINAPI binder_factory_GetIids( IActivationFactory *iface, ULONG *c, IID **i )
{ FIXME( "stub!\n" ); return E_NOTIMPL; }
static HRESULT WINAPI binder_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_MediaBinder,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_MediaBinder) - 1, name );
}
static HRESULT WINAPI binder_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{ if (level) *level = BaseTrust; return S_OK; }

static HRESULT WINAPI binder_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct media_binder *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaBinderInternal_iface.lpVtbl = &binder_vtbl;
    impl->IWineMediaPath_iface.lpVtbl = &binder_path_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, 0 );

    *instance = (IInspectable *)&impl->IMediaBinderInternal_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl binder_factory_vtbl =
{
    binder_factory_QueryInterface,
    binder_factory_AddRef,
    binder_factory_Release,
    binder_factory_GetIids,
    binder_factory_GetRuntimeClassName,
    binder_factory_GetTrustLevel,
    binder_factory_ActivateInstance,
};

static struct binder_statics binder_statics = { {&binder_factory_vtbl}, 1 };

IActivationFactory *media_binder_factory = &binder_statics.IActivationFactory_iface;

IActivationFactory *media_source_factory = &media_source_statics.IActivationFactory_iface;

/* MediaPlaybackItem's factory takes the source the item wraps. */
struct playback_item_statics
{
    IActivationFactory IActivationFactory_iface;
    IMediaPlaybackItemFactoryInternal IMediaPlaybackItemFactoryInternal_iface;
    LONG ref;
};

static inline struct playback_item_statics *impl_from_IActivationFactory_pi( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct playback_item_statics, IActivationFactory_iface );
}

static HRESULT WINAPI playback_item_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct playback_item_statics *impl = impl_from_IActivationFactory_pi( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlaybackItemFactoryInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IMediaPlaybackItemFactoryInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI playback_item_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory_pi( iface )->ref );
}

static ULONG WINAPI playback_item_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory_pi( iface )->ref );
}

static HRESULT WINAPI playback_item_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_item_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Playback_MediaPlaybackItem,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Playback_MediaPlaybackItem) - 1,
                                class_name );
}

static HRESULT WINAPI playback_item_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_item_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* An item without a source is meaningless; Create is the only constructor. */
    if (instance) *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl playback_item_factory_vtbl =
{
    playback_item_factory_QueryInterface,
    playback_item_factory_AddRef,
    playback_item_factory_Release,
    playback_item_factory_GetIids,
    playback_item_factory_GetRuntimeClassName,
    playback_item_factory_GetTrustLevel,
    playback_item_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( playback_item_statics, IMediaPlaybackItemFactoryInternal, struct playback_item_statics,
                     IActivationFactory_iface )

static HRESULT WINAPI playback_item_statics_Create( IMediaPlaybackItemFactoryInternal *iface,
        IMediaSource2Internal *source, IMediaPlaybackItemInternal **value )
{
    struct playback_item *impl;

    TRACE( "iface %p, source %p, value %p.\n", iface, source, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!source) return E_INVALIDARG;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaPlaybackItemInternal_iface.lpVtbl = &playback_item_vtbl;
    impl->IWineMediaPath_iface.lpVtbl = &playback_item_path_vtbl;
    impl->ref = 1;
    IMediaSource2Internal_AddRef( (impl->source = source) );

    *value = &impl->IMediaPlaybackItemInternal_iface;
    return S_OK;
}

static const struct IMediaPlaybackItemFactoryInternalVtbl playback_item_statics_vtbl =
{
    playback_item_statics_QueryInterface,
    playback_item_statics_AddRef,
    playback_item_statics_Release,
    playback_item_statics_GetIids,
    playback_item_statics_GetRuntimeClassName,
    playback_item_statics_GetTrustLevel,
    playback_item_statics_Create,
};

static struct playback_item_statics playback_item_statics =
{
    {&playback_item_factory_vtbl},
    {&playback_item_statics_vtbl},
    1,
};

IActivationFactory *playback_item_factory = &playback_item_statics.IActivationFactory_iface;

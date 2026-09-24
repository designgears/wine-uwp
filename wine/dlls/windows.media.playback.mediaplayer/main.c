/* WinRT Windows.Media.Playback.MediaPlayer Implementation
 *
 * Copyright (C) 2025 Mohamad Al-Jaf
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

#include "initguid.h"
#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mediaplayer);

struct media_player
{
    IMediaPlayer IMediaPlayer_iface;
    IMediaPlayer2 IMediaPlayer2_iface;
    IMediaPlayer3 IMediaPlayer3_iface;
    IMediaPlayerSource2 IMediaPlayerSource2_iface;
    IMediaPlayer5 IMediaPlayer5_iface;
    IClosable IClosable_iface;
    LONG ref;

    ISystemMediaTransportControls *controls;
    IUnknown *command_manager;
    IUnknown *break_manager;
    HWND window;

    /* Wine has no audio session categories or per-player device routing; the app only needs these
     * to round-trip, and MediaPlayer is the class through which XAML apps set them at startup. */
    MediaPlayerAudioCategory audio_category;
    MediaPlayerAudioDeviceType audio_device_type;
    boolean frame_server_enabled;
    boolean real_time_playback;
    DOUBLE audio_balance;
    StereoscopicVideoRenderMode stereo_mode;
    TimeSpan timeline_offset;
    IMediaPlaybackSession *session;
    IMediaPlaybackSource *source;
    struct playback *playback;

    /* Handlers for MediaOpened and the session's PlaybackStateChanged. An app's transport is
     * disabled until it is told the media is open, so these are the events that have to be real
     * rather than a token that never fires. */
    IUnknown *media_opened[8];
    unsigned int media_opened_count;

    /* Player state an app sets and reads back before anything is playing. Wine decodes nothing, so
     * the read-only ones below report a stopped, unseekable player. */
    boolean auto_play;
    boolean looping;
    boolean muted;
    DOUBLE volume;
    DOUBLE rate;
    TimeSpan position;
};

static inline struct media_player *impl_from_IMediaPlayer( IMediaPlayer *iface )
{
    return CONTAINING_RECORD( iface, struct media_player, IMediaPlayer_iface );
}

/*
 * A private MediaPlayer interface, {9d47a108-81a6-43f9-b40d-6403552df734}. It is in no .winmd and
 * unnamed in HKCR\Interface, but the shipping implementation has it: Windows.Media.Playback.
 * ProxyStub.dll says 19 vtable entries (13 methods), and the 19-entry vtable of MediaPlayerImpl in
 * Windows.Media.Playback.MediaPlayer.dll names every one of them through its public PDB. Media
 * Player's generated x:Bind code casts to it, and refusing the QueryInterface raises an
 * InvalidCastException out of PropertyChanged_NavigationViewSelectedItem.
 *
 * Slot 6 is an event registration, not a getter - which is worth recording, because guessing it was
 * a getter and writing NULL through its first argument corrupted the caller's handler and killed
 * the process.
 */
static const GUID IID_IMediaPlayerPrivate =
    {0x9d47a108,0x81a6,0x43f9,{0xb4,0x0d,0x64,0x03,0x55,0x2d,0xf7,0x34}};

struct media_player_private
{
    const void **vtbl;
    LONG ref;
};

static HRESULT WINAPI mpp_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct media_player_private *impl = iface;

    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMediaPlayerPrivate ))
    {
        InterlockedIncrement( &impl->ref );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI mpp_AddRef( void *iface )
{
    return InterlockedIncrement( &((struct media_player_private *)iface)->ref );
}
static ULONG WINAPI mpp_Release( void *iface )
{
    struct media_player_private *impl = iface;
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}
static HRESULT WINAPI mpp_GetIids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI mpp_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Media.Playback.MediaPlayer", 33, name );
}
static HRESULT WINAPI mpp_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
/* The three event pairs. Nothing here ever raises them; registering has to succeed. */
static HRESULT WINAPI mpp_add_event( void *iface, void *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}
static HRESULT WINAPI mpp_remove_event( void *iface, EventRegistrationToken token )
{
    return S_OK;
}
static HRESULT WINAPI mpp_get_CastingRenderLocation( void *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}
static HRESULT WINAPI mpp_get_VideoSwapchainHandle( void *iface, HANDLE *value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}
static HRESULT WINAPI mpp_get_CurrentMediaPlaybackItem( void *iface, void **value )
{
    /* Nothing is playing. CMediaPlayerSubtitleManager::AttachMediaPlayer, the one caller that can
     * be read, checks this for NULL and carries on. */
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}
static HRESULT WINAPI mpp_get_AudioSessionId( void *iface, GUID *value )
{
    if (!value) return E_POINTER;
    memset( value, 0, sizeof(*value) );
    return S_OK;
}
static HRESULT WINAPI mpp_get_TelemetryCorrelationId( void *iface, GUID *value )
{
    if (!value) return E_POINTER;
    memset( value, 0, sizeof(*value) );
    return S_OK;
}
static HRESULT WINAPI mpp_put_TelemetryCorrelationId( void *iface, GUID value )
{
    return S_OK;
}
static HRESULT WINAPI mpp_DisableVisibilityHandling( void *iface )
{
    return S_OK;
}
static const void *media_player_private_vtbl[] =
{
    mpp_QueryInterface, mpp_AddRef, mpp_Release,
    mpp_GetIids, mpp_GetRuntimeClassName, mpp_GetTrustLevel,
    mpp_add_event, mpp_remove_event,                 /* CastingRenderLocationChanged */
    mpp_add_event, mpp_remove_event,                 /* TimelineControllerChanged */
    mpp_add_event, mpp_remove_event,                 /* IsLoopingEnabledChanged */
    mpp_get_CastingRenderLocation,
    mpp_get_VideoSwapchainHandle,
    mpp_get_CurrentMediaPlaybackItem,
    mpp_get_AudioSessionId,
    mpp_get_TelemetryCorrelationId,
    mpp_put_TelemetryCorrelationId,
    mpp_DisableVisibilityHandling,
};

static HRESULT create_media_player_private( void **out )
{
    struct media_player_private *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->vtbl = media_player_private_vtbl;
    impl->ref = 1;
    *out = impl;
    return S_OK;
}

static HRESULT WINAPI media_player_QueryInterface( IMediaPlayer *iface, REFIID iid, void **out )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaPlayer ))
    {
        *out = &impl->IMediaPlayer_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlayerPrivate ))
        return create_media_player_private( out );

    if (IsEqualGUID( iid, &IID_IMediaPlayer2 ))
    {
        *out = &impl->IMediaPlayer2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlayerSource2 ))
    {
        *out = &impl->IMediaPlayerSource2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlayer3 ))
    {
        *out = &impl->IMediaPlayer3_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        *out = &impl->IClosable_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlayer5 ))
    {
        *out = &impl->IMediaPlayer5_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI media_player_AddRef( IMediaPlayer *iface )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI media_player_Release( IMediaPlayer *iface )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        playback_destroy( impl->playback );
        ISystemMediaTransportControls_Release( impl->controls );
        DestroyWindow( impl->window );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI media_player_GetIids( IMediaPlayer *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player_GetRuntimeClassName( IMediaPlayer *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player_GetTrustLevel( IMediaPlayer *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player_get_AutoPlay( IMediaPlayer *iface, boolean *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->auto_play;
    return S_OK;
}

static HRESULT WINAPI media_player_put_AutoPlay( IMediaPlayer *iface, boolean value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p\n", iface );
    impl->auto_play = value;
    return S_OK;
}

static HRESULT WINAPI media_player_get_NaturalDuration( IMediaPlayer *iface, TimeSpan *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    value->Duration = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_get_Position( IMediaPlayer *iface, TimeSpan *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->position;
    return S_OK;
}

static HRESULT WINAPI media_player_put_Position( IMediaPlayer *iface, TimeSpan value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p\n", iface );
    impl->position = value;
    return S_OK;
}

static HRESULT WINAPI media_player_get_BufferingProgress( IMediaPlayer *iface, DOUBLE *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0.0;
    return S_OK;
}

static HRESULT WINAPI media_player_get_CurrentState( IMediaPlayer *iface, MediaPlayerState *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = MediaPlayerState_Closed;
    return S_OK;
}

static HRESULT WINAPI media_player_get_CanSeek( IMediaPlayer *iface, boolean *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI media_player_get_CanPause( IMediaPlayer *iface, boolean *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI media_player_get_IsLoopingEnabled( IMediaPlayer *iface, boolean *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->looping;
    return S_OK;
}

static HRESULT WINAPI media_player_put_IsLoopingEnabled( IMediaPlayer *iface, boolean value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p\n", iface );
    impl->looping = value;
    return S_OK;
}

static HRESULT WINAPI media_player_get_IsProtected( IMediaPlayer *iface, boolean *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI media_player_get_IsMuted( IMediaPlayer *iface, boolean *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->muted;
    return S_OK;
}

static HRESULT WINAPI media_player_put_IsMuted( IMediaPlayer *iface, boolean value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p\n", iface );
    impl->muted = value;
    return S_OK;
}

static HRESULT WINAPI media_player_get_PlaybackRate( IMediaPlayer *iface, DOUBLE *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->rate;
    return S_OK;
}

static HRESULT WINAPI media_player_put_PlaybackRate( IMediaPlayer *iface, DOUBLE value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p\n", iface );
    impl->rate = value;
    return S_OK;
}

static HRESULT WINAPI media_player_get_Volume( IMediaPlayer *iface, DOUBLE *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->volume;
    return S_OK;
}

static HRESULT WINAPI media_player_put_Volume( IMediaPlayer *iface, DOUBLE value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    TRACE( "iface %p\n", iface );
    impl->volume = value;
    return S_OK;
}

static HRESULT WINAPI media_player_get_PlaybackMediaMarkers( IMediaPlayer *iface, IPlaybackMediaMarkerSequence **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player_add_MediaOpened( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );

    TRACE( "iface %p, value %p, token %p.\n", iface, value, token );

    if (value && impl->media_opened_count < ARRAY_SIZE(impl->media_opened))
    {
        IUnknown_AddRef( (IUnknown *)value );
        impl->media_opened[impl->media_opened_count++] = (IUnknown *)value;
    }
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_MediaOpened( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_MediaEnded( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_MediaEnded( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_MediaFailed( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_MediaPlayerFailedEventArgs *value,
                                                    EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (token) token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_MediaFailed( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_CurrentStateChanged( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value,
                                                            EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (token) token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_CurrentStateChanged( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_PlaybackMediaMarkerReached( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_PlaybackMediaMarkerReachedEventArgs *value,
                                                                   EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (token) token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_PlaybackMediaMarkerReached( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_MediaPlayerRateChanged( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_MediaPlayerRateChangedEventArgs *value,
                                                               EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (token) token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_MediaPlayerRateChanged( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_VolumeChanged( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_VolumeChanged( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_SeekCompleted( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_SeekCompleted( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_BufferingStarted( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_BufferingStarted( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player_add_BufferingEnded( IMediaPlayer *iface, ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player_remove_BufferingEnded( IMediaPlayer *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT media_player_resolve_source( struct media_player *impl );

static HRESULT WINAPI media_player_Play( IMediaPlayer *iface )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );

    TRACE( "iface %p.\n", iface );

    if (!impl->playback) return E_FAIL;
    /* Resolve now: by the time an app calls Play its list has the item in it. */
    media_player_resolve_source( impl );
    return playback_play( impl->playback );
}

static HRESULT WINAPI media_player_Pause( IMediaPlayer *iface )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );

    TRACE( "iface %p.\n", iface );

    if (!impl->playback) return E_FAIL;
    return playback_pause( impl->playback );
}

/* A Uri arrives as file:///C:/path or as a plain path; the reader takes either, but only after
 * the URI escaping is undone, so hand it the unescaped form. */
static HRESULT media_player_source_from_uri( IUriRuntimeClass *uri, WCHAR **out )
{
    HSTRING raw = NULL;
    const WCHAR *str;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = IUriRuntimeClass_get_RawUri( uri, &raw ))) return hr;
    if (!(str = WindowsGetStringRawBuffer( raw, NULL )))
    {
        WindowsDeleteString( raw );
        return E_INVALIDARG;
    }
    if (!(*out = wcsdup( str ))) hr = E_OUTOFMEMORY;
    WindowsDeleteString( raw );
    return hr;
}

static HRESULT WINAPI media_player_SetUriSource( IMediaPlayer *iface, IUriRuntimeClass *value )
{
    struct media_player *impl = impl_from_IMediaPlayer( iface );
    WCHAR *path;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!impl->playback) return E_FAIL;
    if (FAILED(hr = media_player_source_from_uri( value, &path ))) return hr;

    hr = playback_set_path( impl->playback, path );
    free( path );
    if (SUCCEEDED(hr) && impl->auto_play) hr = playback_play( impl->playback );
    return hr;
}

static const struct IMediaPlayerVtbl media_player_vtbl =
{
    /* IUnknown methods */
    media_player_QueryInterface,
    media_player_AddRef,
    media_player_Release,
    /* IInspectable methods */
    media_player_GetIids,
    media_player_GetRuntimeClassName,
    media_player_GetTrustLevel,
    /* IMediaPlayer methods */
    media_player_get_AutoPlay,
    media_player_put_AutoPlay,
    media_player_get_NaturalDuration,
    media_player_get_Position,
    media_player_put_Position,
    media_player_get_BufferingProgress,
    media_player_get_CurrentState,
    media_player_get_CanSeek,
    media_player_get_CanPause,
    media_player_get_IsLoopingEnabled,
    media_player_put_IsLoopingEnabled,
    media_player_get_IsProtected,
    media_player_get_IsMuted,
    media_player_put_IsMuted,
    media_player_get_PlaybackRate,
    media_player_put_PlaybackRate,
    media_player_get_Volume,
    media_player_put_Volume,
    media_player_get_PlaybackMediaMarkers,
    media_player_add_MediaOpened,
    media_player_remove_MediaOpened,
    media_player_add_MediaEnded,
    media_player_remove_MediaEnded,
    media_player_add_MediaFailed,
    media_player_remove_MediaFailed,
    media_player_add_CurrentStateChanged,
    media_player_remove_CurrentStateChanged,
    media_player_add_PlaybackMediaMarkerReached,
    media_player_remove_PlaybackMediaMarkerReached,
    media_player_add_MediaPlayerRateChanged,
    media_player_remove_MediaPlayerRateChanged,
    media_player_add_VolumeChanged,
    media_player_remove_VolumeChanged,
    media_player_add_SeekCompleted,
    media_player_remove_SeekCompleted,
    media_player_add_BufferingStarted,
    media_player_remove_BufferingStarted,
    media_player_add_BufferingEnded,
    media_player_remove_BufferingEnded,
    media_player_Play,
    media_player_Pause,
    media_player_SetUriSource,
};

DEFINE_IINSPECTABLE( media_player2, IMediaPlayer2, struct media_player, IMediaPlayer_iface )

static HRESULT WINAPI media_player2_get_SystemMediaTransportControls( IMediaPlayer2 *iface, ISystemMediaTransportControls **value )
{
    struct media_player *impl = impl_from_IMediaPlayer2( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    *value = impl->controls;
    ISystemMediaTransportControls_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI media_player2_get_AudioCategory( IMediaPlayer2 *iface, MediaPlayerAudioCategory *value )
{
    struct media_player *impl = impl_from_IMediaPlayer2( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->audio_category;
    return S_OK;
}

static HRESULT WINAPI media_player2_put_AudioCategory( IMediaPlayer2 *iface, MediaPlayerAudioCategory value )
{
    struct media_player *impl = impl_from_IMediaPlayer2( iface );

    TRACE( "iface %p, value %#x\n", iface, value );

    impl->audio_category = value;
    return S_OK;
}

static HRESULT WINAPI media_player2_get_AudioDeviceType( IMediaPlayer2 *iface, MediaPlayerAudioDeviceType *value )
{
    struct media_player *impl = impl_from_IMediaPlayer2( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->audio_device_type;
    return S_OK;
}

static HRESULT WINAPI media_player2_put_AudioDeviceType( IMediaPlayer2 *iface, MediaPlayerAudioDeviceType value )
{
    struct media_player *impl = impl_from_IMediaPlayer2( iface );

    TRACE( "iface %p, value %#x\n", iface, value );

    impl->audio_device_type = value;
    return S_OK;
}

static const struct IMediaPlayer2Vtbl media_player2_vtbl =
{
    /* IUnknown methods */
    media_player2_QueryInterface,
    media_player2_AddRef,
    media_player2_Release,
    /* IInspectable methods */
    media_player2_GetIids,
    media_player2_GetRuntimeClassName,
    media_player2_GetTrustLevel,
    /* IMediaPlayer2 methods */
    media_player2_get_SystemMediaTransportControls,
    media_player2_get_AudioCategory,
    media_player2_put_AudioCategory,
    media_player2_get_AudioDeviceType,
    media_player2_put_AudioDeviceType,
};

/*
 * MediaPlaybackSession - the player's view of what is playing. Wine decodes nothing, so this
 * reports a stopped session that can neither seek nor pause, and never raises an event.
 */

struct playback_session
{
    IMediaPlaybackSession IMediaPlaybackSession_iface;
    IMediaPlaybackSession2 IMediaPlaybackSession2_iface;
    LONG ref;

    boolean mirroring;
    IMediaPlaybackSphericalVideoProjection *projection;

    IMediaPlayer *player;
    TimeSpan position;
    DOUBLE playback_rate;
    Rect source_rect;
    StereoscopicVideoPackingMode packing_mode;
};

static inline struct playback_session *impl_from_IMediaPlaybackSession( IMediaPlaybackSession *iface )
{
    return CONTAINING_RECORD( iface, struct playback_session, IMediaPlaybackSession_iface );
}

static HRESULT WINAPI playback_session_QueryInterface( IMediaPlaybackSession *iface, REFIID iid, void **out )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaPlaybackSession ))
    {
        *out = &impl->IMediaPlaybackSession_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IMediaPlaybackSession2 ))
    {
        *out = &impl->IMediaPlaybackSession2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI playback_session_AddRef( IMediaPlaybackSession *iface )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI playback_session_Release( IMediaPlaybackSession *iface )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI playback_session_GetIids( IMediaPlaybackSession *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_session_GetRuntimeClassName( IMediaPlaybackSession *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Media_Playback_MediaPlaybackSession,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Playback_MediaPlaybackSession) - 1, class_name );
}

static HRESULT WINAPI playback_session_GetTrustLevel( IMediaPlaybackSession *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI playback_session_add_PlaybackStateChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_PlaybackStateChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_PlaybackRateChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_PlaybackRateChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_SeekCompleted( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_SeekCompleted( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_BufferingStarted( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_BufferingStarted( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_BufferingEnded( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_BufferingEnded( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_BufferingProgressChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_BufferingProgressChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_DownloadProgressChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_DownloadProgressChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_NaturalDurationChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_NaturalDurationChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_PositionChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_PositionChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_add_NaturalVideoSizeChanged( IMediaPlaybackSession *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_remove_NaturalVideoSizeChanged( IMediaPlaybackSession *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session_get_MediaPlayer( IMediaPlaybackSession *iface, IMediaPlayer **value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    IMediaPlayer_AddRef( (*value = impl->player) );
    return S_OK;
}

static HRESULT WINAPI playback_session_get_NaturalDuration( IMediaPlaybackSession *iface, TimeSpan *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    struct media_player *player = impl->player ? impl_from_IMediaPlayer( impl->player ) : NULL;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    value->Duration = player && player->playback ? playback_duration( player->playback ) : 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_Position( IMediaPlaybackSession *iface, TimeSpan *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->position;
    return S_OK;
}

static HRESULT WINAPI playback_session_put_Position( IMediaPlaybackSession *iface, TimeSpan value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %I64d\n", iface, value.Duration );
    impl->position = value;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_PlaybackState( IMediaPlaybackSession *iface, MediaPlaybackState *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    struct media_player *player = impl->player ? impl_from_IMediaPlayer( impl->player ) : NULL;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    if (!player || !player->playback) *value = MediaPlaybackState_None;
    else if (playback_is_playing( player->playback )) *value = MediaPlaybackState_Playing;
    else if (playback_duration( player->playback )) *value = MediaPlaybackState_Paused;
    else *value = MediaPlaybackState_None;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_CanSeek( IMediaPlaybackSession *iface, boolean *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_CanPause( IMediaPlaybackSession *iface, boolean *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_IsProtected( IMediaPlaybackSession *iface, boolean *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_PlaybackRate( IMediaPlaybackSession *iface, DOUBLE *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->playback_rate;
    return S_OK;
}

static HRESULT WINAPI playback_session_put_PlaybackRate( IMediaPlaybackSession *iface, DOUBLE value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %f\n", iface, value );
    impl->playback_rate = value;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_BufferingProgress( IMediaPlaybackSession *iface, DOUBLE *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0.0;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_DownloadProgress( IMediaPlaybackSession *iface, DOUBLE *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0.0;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_NaturalVideoHeight( IMediaPlaybackSession *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_NaturalVideoWidth( IMediaPlaybackSession *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_NormalizedSourceRect( IMediaPlaybackSession *iface, Rect *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->source_rect;
    return S_OK;
}

static HRESULT WINAPI playback_session_put_NormalizedSourceRect( IMediaPlaybackSession *iface, Rect value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value (%f,%f)-(%f,%f)\n", iface, value.X, value.Y, value.Width, value.Height );
    impl->source_rect = value;
    return S_OK;
}

static HRESULT WINAPI playback_session_get_StereoscopicVideoPackingMode( IMediaPlaybackSession *iface,
                                                                         StereoscopicVideoPackingMode *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->packing_mode;
    return S_OK;
}

static HRESULT WINAPI playback_session_put_StereoscopicVideoPackingMode( IMediaPlaybackSession *iface,
                                                                         StereoscopicVideoPackingMode value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession( iface );
    TRACE( "iface %p, value %d\n", iface, value );
    impl->packing_mode = value;
    return S_OK;
}

static const struct IMediaPlaybackSessionVtbl playback_session_vtbl =
{
    playback_session_QueryInterface,
    playback_session_AddRef,
    playback_session_Release,
    /* IInspectable methods */
    playback_session_GetIids,
    playback_session_GetRuntimeClassName,
    playback_session_GetTrustLevel,
    /* IMediaPlaybackSession methods */
    playback_session_add_PlaybackStateChanged,
    playback_session_remove_PlaybackStateChanged,
    playback_session_add_PlaybackRateChanged,
    playback_session_remove_PlaybackRateChanged,
    playback_session_add_SeekCompleted,
    playback_session_remove_SeekCompleted,
    playback_session_add_BufferingStarted,
    playback_session_remove_BufferingStarted,
    playback_session_add_BufferingEnded,
    playback_session_remove_BufferingEnded,
    playback_session_add_BufferingProgressChanged,
    playback_session_remove_BufferingProgressChanged,
    playback_session_add_DownloadProgressChanged,
    playback_session_remove_DownloadProgressChanged,
    playback_session_add_NaturalDurationChanged,
    playback_session_remove_NaturalDurationChanged,
    playback_session_add_PositionChanged,
    playback_session_remove_PositionChanged,
    playback_session_add_NaturalVideoSizeChanged,
    playback_session_remove_NaturalVideoSizeChanged,
    playback_session_get_MediaPlayer,
    playback_session_get_NaturalDuration,
    playback_session_get_Position,
    playback_session_put_Position,
    playback_session_get_PlaybackState,
    playback_session_get_CanSeek,
    playback_session_get_CanPause,
    playback_session_get_IsProtected,
    playback_session_get_PlaybackRate,
    playback_session_put_PlaybackRate,
    playback_session_get_BufferingProgress,
    playback_session_get_DownloadProgress,
    playback_session_get_NaturalVideoHeight,
    playback_session_get_NaturalVideoWidth,
    playback_session_get_NormalizedSourceRect,
    playback_session_put_NormalizedSourceRect,
    playback_session_get_StereoscopicVideoPackingMode,
    playback_session_put_StereoscopicVideoPackingMode,
};


/* The spherical projection object: an app reads it to find out whether the source is 360 video.
 * Wine never decodes one, so it stays disabled and the settings just round-trip. */
struct spherical_projection
{
    IMediaPlaybackSphericalVideoProjection IMediaPlaybackSphericalVideoProjection_iface;
    LONG ref;

    boolean enabled;
    SphericalVideoFrameFormat frame_format;
    DOUBLE horizontal_fov;
    Quaternion orientation;
    SphericalVideoProjectionMode projection_mode;
};

static inline struct spherical_projection *impl_from_IMediaPlaybackSphericalVideoProjection(
        IMediaPlaybackSphericalVideoProjection *iface )
{
    return CONTAINING_RECORD( iface, struct spherical_projection, IMediaPlaybackSphericalVideoProjection_iface );
}

static HRESULT WINAPI spherical_QueryInterface( IMediaPlaybackSphericalVideoProjection *iface, REFIID iid, void **out )
{
    struct spherical_projection *impl = impl_from_IMediaPlaybackSphericalVideoProjection( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaPlaybackSphericalVideoProjection ))
    {
        *out = &impl->IMediaPlaybackSphericalVideoProjection_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI spherical_AddRef( IMediaPlaybackSphericalVideoProjection *iface )
{
    return InterlockedIncrement( &impl_from_IMediaPlaybackSphericalVideoProjection( iface )->ref );
}

static ULONG WINAPI spherical_Release( IMediaPlaybackSphericalVideoProjection *iface )
{
    struct spherical_projection *impl = impl_from_IMediaPlaybackSphericalVideoProjection( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI spherical_GetIids( IMediaPlaybackSphericalVideoProjection *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI spherical_GetRuntimeClassName( IMediaPlaybackSphericalVideoProjection *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Media_Playback_MediaPlaybackSphericalVideoProjection,
            ARRAY_SIZE(RuntimeClass_Windows_Media_Playback_MediaPlaybackSphericalVideoProjection) - 1, class_name );
}

static HRESULT WINAPI spherical_GetTrustLevel( IMediaPlaybackSphericalVideoProjection *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

#define SPHERICAL_PROP( name, type, field )                                                        \
    static HRESULT WINAPI spherical_get_##name( IMediaPlaybackSphericalVideoProjection *iface, type *value ) \
    {                                                                                              \
        struct spherical_projection *impl = impl_from_IMediaPlaybackSphericalVideoProjection( iface ); \
        TRACE( "iface %p, value %p\n", iface, value );                                             \
        if (!value) return E_POINTER;                                                              \
        *value = impl->field;                                                                      \
        return S_OK;                                                                               \
    }                                                                                              \
    static HRESULT WINAPI spherical_put_##name( IMediaPlaybackSphericalVideoProjection *iface, type value ) \
    {                                                                                              \
        struct spherical_projection *impl = impl_from_IMediaPlaybackSphericalVideoProjection( iface ); \
        TRACE( "iface %p\n", iface );                                                              \
        impl->field = value;                                                                       \
        return S_OK;                                                                               \
    }

SPHERICAL_PROP( IsEnabled, boolean, enabled )
SPHERICAL_PROP( FrameFormat, SphericalVideoFrameFormat, frame_format )
SPHERICAL_PROP( HorizontalFieldOfViewInDegrees, DOUBLE, horizontal_fov )
SPHERICAL_PROP( ViewOrientation, Quaternion, orientation )
SPHERICAL_PROP( ProjectionMode, SphericalVideoProjectionMode, projection_mode )

static const struct IMediaPlaybackSphericalVideoProjectionVtbl spherical_vtbl =
{
    spherical_QueryInterface,
    spherical_AddRef,
    spherical_Release,
    /* IInspectable methods */
    spherical_GetIids,
    spherical_GetRuntimeClassName,
    spherical_GetTrustLevel,
    /* IMediaPlaybackSphericalVideoProjection methods */
    spherical_get_IsEnabled,
    spherical_put_IsEnabled,
    spherical_get_FrameFormat,
    spherical_put_FrameFormat,
    spherical_get_HorizontalFieldOfViewInDegrees,
    spherical_put_HorizontalFieldOfViewInDegrees,
    spherical_get_ViewOrientation,
    spherical_put_ViewOrientation,
    spherical_get_ProjectionMode,
    spherical_put_ProjectionMode,
};

static HRESULT spherical_projection_create( IMediaPlaybackSphericalVideoProjection **out )
{
    struct spherical_projection *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaPlaybackSphericalVideoProjection_iface.lpVtbl = &spherical_vtbl;
    impl->ref = 1;
    impl->horizontal_fov = 120.0;
    impl->orientation.W = 1.0f;

    *out = &impl->IMediaPlaybackSphericalVideoProjection_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( playback_session2, IMediaPlaybackSession2, struct playback_session, IMediaPlaybackSession_iface )

static HRESULT WINAPI playback_session2_add_BufferedRangesChanged( IMediaPlaybackSession2 *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session2_remove_BufferedRangesChanged( IMediaPlaybackSession2 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session2_add_PlayedRangesChanged( IMediaPlaybackSession2 *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session2_remove_PlayedRangesChanged( IMediaPlaybackSession2 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session2_add_SeekableRangesChanged( IMediaPlaybackSession2 *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session2_remove_SeekableRangesChanged( IMediaPlaybackSession2 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session2_add_SupportedPlaybackRatesChanged( IMediaPlaybackSession2 *iface,
        ITypedEventHandler_MediaPlaybackSession_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI playback_session2_remove_SupportedPlaybackRatesChanged( IMediaPlaybackSession2 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI playback_session2_get_SphericalVideoProjection( IMediaPlaybackSession2 *iface,
        IMediaPlaybackSphericalVideoProjection **value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession2( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    if (!impl->projection && FAILED(hr = spherical_projection_create( &impl->projection ))) return hr;
    IMediaPlaybackSphericalVideoProjection_AddRef( (*value = impl->projection) );
    return S_OK;
}

static HRESULT WINAPI playback_session2_get_IsMirroring( IMediaPlaybackSession2 *iface, boolean *value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession2( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->mirroring;
    return S_OK;
}

static HRESULT WINAPI playback_session2_put_IsMirroring( IMediaPlaybackSession2 *iface, boolean value )
{
    struct playback_session *impl = impl_from_IMediaPlaybackSession2( iface );
    TRACE( "iface %p, value %u\n", iface, value );
    impl->mirroring = value;
    return S_OK;
}

static HRESULT WINAPI playback_session2_GetBufferedRanges( IMediaPlaybackSession2 *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_session2_GetPlayedRanges( IMediaPlaybackSession2 *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_session2_GetSeekableRanges( IMediaPlaybackSession2 *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI playback_session2_IsSupportedPlaybackRateRange( IMediaPlaybackSession2 *iface, DOUBLE rate1,
                                                                      DOUBLE rate2, boolean *value )
{
    TRACE( "iface %p, rate1 %f, rate2 %f, value %p\n", iface, rate1, rate2, value );
    if (!value) return E_POINTER;
    /* nothing plays, so only normal speed is "supported" */
    *value = (rate1 <= 1.0 && rate2 >= 1.0);
    return S_OK;
}

static const struct IMediaPlaybackSession2Vtbl playback_session2_vtbl =
{
    playback_session2_QueryInterface,
    playback_session2_AddRef,
    playback_session2_Release,
    /* IInspectable methods */
    playback_session2_GetIids,
    playback_session2_GetRuntimeClassName,
    playback_session2_GetTrustLevel,
    /* IMediaPlaybackSession2 methods */
    playback_session2_add_BufferedRangesChanged,
    playback_session2_remove_BufferedRangesChanged,
    playback_session2_add_PlayedRangesChanged,
    playback_session2_remove_PlayedRangesChanged,
    playback_session2_add_SeekableRangesChanged,
    playback_session2_remove_SeekableRangesChanged,
    playback_session2_add_SupportedPlaybackRatesChanged,
    playback_session2_remove_SupportedPlaybackRatesChanged,
    playback_session2_get_SphericalVideoProjection,
    playback_session2_get_IsMirroring,
    playback_session2_put_IsMirroring,
    playback_session2_GetBufferedRanges,
    playback_session2_GetPlayedRanges,
    playback_session2_GetSeekableRanges,
    playback_session2_IsSupportedPlaybackRateRange,
};

static HRESULT playback_session_create( IMediaPlayer *player, IMediaPlaybackSession **out )
{
    struct playback_session *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaPlaybackSession_iface.lpVtbl = &playback_session_vtbl;
    impl->IMediaPlaybackSession2_iface.lpVtbl = &playback_session2_vtbl;
    impl->ref = 1;
    impl->player = player;
    impl->playback_rate = 1.0;
    impl->source_rect.Width = 1.0f;
    impl->source_rect.Height = 1.0f;

    *out = &impl->IMediaPlaybackSession_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( media_player_source2, IMediaPlayerSource2, struct media_player, IMediaPlayer_iface )

static HRESULT WINAPI media_player_source2_get_Source( IMediaPlayerSource2 *iface, IMediaPlaybackSource **value )
{
    struct media_player *impl = impl_from_IMediaPlayerSource2( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->source)) IMediaPlaybackSource_AddRef( *value );
    return S_OK;
}

/* Wine.Internal.IWineMediaPath, implemented by MediaSource and MediaPlaybackItem in
 * dlls/windows.media: the only way to ask a playback source which file it wraps. There is no
 * public interface for it - a MediaSource is opaque on Windows because the pipeline lives inside
 * it, whereas here the player opens the file itself. get_Path is slot 6, after IInspectable. */
DEFINE_GUID(IID_IWineMediaPath, 0x148b8ebf, 0x7e85, 0x458e, 0xb5, 0x16, 0x10, 0x38, 0xad, 0x8b, 0xf5, 0x25);

static HRESULT source_query_path( IUnknown *source, HSTRING *path )
{
    HRESULT (WINAPI *get_path)( IUnknown *, HSTRING * );
    IUnknown *wine_path;
    HRESULT hr;

    *path = NULL;
    if (FAILED(hr = IUnknown_QueryInterface( source, &IID_IWineMediaPath, (void **)&wine_path )))
        return hr;
    get_path = (void *)(*(void ***)wine_path)[6];
    hr = get_path( wine_path, path );
    IUnknown_Release( wine_path );
    return hr;
}

/* A source is a MediaSource, a MediaPlaybackItem wrapping one, or a MediaPlaybackList of items.
 * The first two answer IWineMediaPath directly; for a list, the item to play is the current one,
 * and until the list reports one there is nothing to open yet. */
/* Resolve the source to a file. This is deliberately not done when the source is set: an app
 * assigns a MediaPlaybackList to the player and *then* appends items to it, so asking at
 * assignment time finds an empty list (E_BOUNDS). Asking when playback starts finds whatever is
 * current by then. */
static HRESULT media_player_resolve_source( struct media_player *impl )
{
    HSTRING path = NULL;
    const WCHAR *str;
    HRESULT hr;

    if (!impl->playback) return E_FAIL;
    if (!impl->source) return S_OK;

    if (FAILED(hr = source_query_path( (IUnknown *)impl->source, &path )))
    {
        WARN( "source %p does not say which file it is yet, hr %#lx.\n", impl->source, hr );
        return hr;
    }

    str = WindowsGetStringRawBuffer( path, NULL );
    if (str && *str) hr = playback_set_path( impl->playback, str );
    else hr = E_FAIL;
    WindowsDeleteString( path );

    if (SUCCEEDED(hr))
    {
        unsigned int i;

        /* Learn the duration before announcing the media, so a transport that reads it as it
         * enables itself sees the real value. */
        playback_probe( impl->playback );

        for (i = 0; i < impl->media_opened_count; i++)
        {
            ITypedEventHandler_MediaPlayer_IInspectable *handler =
                (ITypedEventHandler_MediaPlayer_IInspectable *)impl->media_opened[i];
            TRACE( "reporting MediaOpened to %p.\n", handler );
            ITypedEventHandler_MediaPlayer_IInspectable_Invoke( handler, &impl->IMediaPlayer_iface, NULL );
        }
    }
    return hr;
}

/* An app assigns a playback list and fills it immediately afterwards, so the source cannot say
 * which file it holds at the moment it is set - but the player has to announce the media before
 * the app's transport will enable itself, and the app will not call Play until it does. Resolving
 * only at Play deadlocks the two. Retry briefly in the background instead: the item appears within
 * a few milliseconds in practice. */
static DWORD WINAPI media_player_resolve_thread( void *arg )
{
    struct media_player *impl = arg;
    unsigned int i;

    for (i = 0; i < 60; i++)
    {
        if (SUCCEEDED(media_player_resolve_source( impl )))
        {
            TRACE( "source resolved after %ums.\n", i * 50 );
            if (impl->auto_play) playback_play( impl->playback );
            IMediaPlayer_Release( &impl->IMediaPlayer_iface );
            return 0;
        }
        Sleep( 50 );
    }

    WARN( "the source never said which file it holds.\n" );
    IMediaPlayer_Release( &impl->IMediaPlayer_iface );
    return 0;
}

static HRESULT WINAPI media_player_source2_put_Source( IMediaPlayerSource2 *iface, IMediaPlaybackSource *value )
{
    struct media_player *impl = impl_from_IMediaPlayerSource2( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    if (value) IMediaPlaybackSource_AddRef( value );
    if (impl->source) IMediaPlaybackSource_Release( impl->source );
    impl->source = value;

    if (value && impl->playback)
    {
        HANDLE thread;

        if (SUCCEEDED(media_player_resolve_source( impl )))
        {
            if (impl->auto_play) playback_play( impl->playback );
        }
        else if ((thread = CreateThread( NULL, 0, media_player_resolve_thread,
                                         (IMediaPlayer_AddRef( &impl->IMediaPlayer_iface ), impl),
                                         0, NULL )))
        {
            CloseHandle( thread );
        }
        else IMediaPlayer_Release( &impl->IMediaPlayer_iface );
    }
    return S_OK;
}

static const struct IMediaPlayerSource2Vtbl media_player_source2_vtbl =
{
    media_player_source2_QueryInterface,
    media_player_source2_AddRef,
    media_player_source2_Release,
    /* IInspectable methods */
    media_player_source2_GetIids,
    media_player_source2_GetRuntimeClassName,
    media_player_source2_GetTrustLevel,
    /* IMediaPlayerSource2 methods */
    media_player_source2_get_Source,
    media_player_source2_put_Source,
};

DEFINE_IINSPECTABLE( media_player3, IMediaPlayer3, struct media_player, IMediaPlayer_iface )

static HRESULT WINAPI media_player3_add_IsMutedChanged( IMediaPlayer3 *iface,
        ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player3_remove_IsMutedChanged( IMediaPlayer3 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player3_add_SourceChanged( IMediaPlayer3 *iface,
        ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player3_remove_SourceChanged( IMediaPlayer3 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player3_get_AudioBalance( IMediaPlayer3 *iface, DOUBLE *value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->audio_balance;
    return S_OK;
}

static HRESULT WINAPI media_player3_put_AudioBalance( IMediaPlayer3 *iface, DOUBLE value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %f\n", iface, value );
    impl->audio_balance = value;
    return S_OK;
}

static HRESULT WINAPI media_player3_get_RealTimePlayback( IMediaPlayer3 *iface, boolean *value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->real_time_playback;
    return S_OK;
}

static HRESULT WINAPI media_player3_put_RealTimePlayback( IMediaPlayer3 *iface, boolean value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %u\n", iface, value );
    impl->real_time_playback = value;
    return S_OK;
}

static HRESULT WINAPI media_player3_get_StereoscopicVideoRenderMode( IMediaPlayer3 *iface,
                                                                     StereoscopicVideoRenderMode *value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->stereo_mode;
    return S_OK;
}

static HRESULT WINAPI media_player3_put_StereoscopicVideoRenderMode( IMediaPlayer3 *iface,
                                                                     StereoscopicVideoRenderMode value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %d\n", iface, value );
    impl->stereo_mode = value;
    return S_OK;
}

/*
 * MediaPlaybackCommandManager and its command behaviours. Both interfaces are public - the slot
 * lists come straight out of Windows.Media.winmd - but Wine's IDL declares
 * IMediaPlaybackCommandManager with no members, so these use raw vtables rather than the header's
 * empty one. Media Player asks for the command manager during startup and an E_NOTIMPL there is
 * the NotImplementedException it stops on; nothing raises these commands here, so the behaviours
 * are all disabled and the events never fire. A behaviour getter must still hand back an object:
 * the app dereferences what it gets.
 */
struct command_behavior
{
    const void **vtbl;
    LONG ref;
    IUnknown *manager;
};
struct command_manager
{
    const void **vtbl;
    LONG ref;
    IUnknown *player;
    struct command_behavior *behavior;
};

static const GUID IID_IMediaPlaybackCommandManager_local =
    {0x5acee5a6,0x5cb6,0x4a5a,{0x85,0x21,0xcc,0x86,0xb1,0xc1,0xed,0x37}};
static const GUID IID_ICommandBehavior_local =
    {0x786c1e78,0xce78,0x4a10,{0xaf,0xd6,0x84,0x3f,0xcb,0xb9,0x0c,0x2e}};

static const void *command_behavior_vtbl[12];
static const void *command_manager_vtbl[39];

static HRESULT WINAPI cb_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct command_behavior *impl = iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICommandBehavior_local ))
    {
        InterlockedIncrement( &impl->ref );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI cb_AddRef( void *iface ) { return InterlockedIncrement( &((struct command_behavior *)iface)->ref ); }
static ULONG WINAPI cb_Release( void *iface )
{
    struct command_behavior *impl = iface;
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}
static HRESULT WINAPI cb_GetIids( void *iface, ULONG *c, IID **i ) { *c = 0; *i = NULL; return S_OK; }
static HRESULT WINAPI cb_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Media.Playback.MediaPlaybackCommandManagerCommandBehavior", 62, name );
}
static HRESULT WINAPI cb_GetTrustLevel( void *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }
static HRESULT WINAPI cb_get_CommandManager( void *iface, void **value )
{
    struct command_behavior *impl = iface;
    if (!value) return E_POINTER;
    if ((*value = impl->manager)) IUnknown_AddRef( impl->manager );
    return S_OK;
}
static HRESULT WINAPI cb_get_IsEnabled( void *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI cb_get_EnablingRule( void *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}
static HRESULT WINAPI cb_put_EnablingRule( void *iface, INT32 value ) { return S_OK; }
static HRESULT WINAPI cb_add_event( void *iface, void *handler, EventRegistrationToken *token )
{
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}
static HRESULT WINAPI cb_remove_event( void *iface, EventRegistrationToken token ) { return S_OK; }

static const void *command_behavior_vtbl[12] =
{
    cb_QueryInterface, cb_AddRef, cb_Release, cb_GetIids, cb_GetRuntimeClassName, cb_GetTrustLevel,
    cb_get_CommandManager, cb_get_IsEnabled, cb_get_EnablingRule, cb_put_EnablingRule,
    cb_add_event, cb_remove_event,
};

static HRESULT WINAPI cm_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct command_manager *impl = iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaPlaybackCommandManager_local ))
    {
        InterlockedIncrement( &impl->ref );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI cm_AddRef( void *iface ) { return InterlockedIncrement( &((struct command_manager *)iface)->ref ); }
static ULONG WINAPI cm_Release( void *iface )
{
    struct command_manager *impl = iface;
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->behavior) cb_Release( impl->behavior );
        free( impl );
    }
    return ref;
}
static HRESULT WINAPI cm_GetIids( void *iface, ULONG *c, IID **i ) { *c = 0; *i = NULL; return S_OK; }
static HRESULT WINAPI cm_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Media.Playback.MediaPlaybackCommandManager", 50, name );
}
static HRESULT WINAPI cm_GetTrustLevel( void *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }
static HRESULT WINAPI cm_get_IsEnabled( void *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI cm_put_IsEnabled( void *iface, boolean value ) { return S_OK; }
static HRESULT WINAPI cm_get_MediaPlayer( void *iface, void **value )
{
    struct command_manager *impl = iface;
    if (!value) return E_POINTER;
    if ((*value = impl->player)) IUnknown_AddRef( impl->player );
    return S_OK;
}
static HRESULT WINAPI cm_get_Behavior( void *iface, void **value )
{
    struct command_manager *impl = iface;
    if (!value) return E_POINTER;
    cb_AddRef( impl->behavior );
    *value = impl->behavior;
    return S_OK;
}
static HRESULT WINAPI cm_add_event( void *iface, void *handler, EventRegistrationToken *token )
{
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}
static HRESULT WINAPI cm_remove_event( void *iface, EventRegistrationToken token ) { return S_OK; }

static const void *command_manager_vtbl[39] =
{
    cm_QueryInterface, cm_AddRef, cm_Release, cm_GetIids, cm_GetRuntimeClassName, cm_GetTrustLevel,
    cm_get_IsEnabled, cm_put_IsEnabled, cm_get_MediaPlayer,
    /* Play, Pause, Next, Previous, FastForward, Rewind, Shuffle, AutoRepeatMode, Position, Rate */
    cm_get_Behavior, cm_get_Behavior, cm_get_Behavior, cm_get_Behavior, cm_get_Behavior,
    cm_get_Behavior, cm_get_Behavior, cm_get_Behavior, cm_get_Behavior, cm_get_Behavior,
    /* the ten *Received event pairs */
    cm_add_event, cm_remove_event, cm_add_event, cm_remove_event, cm_add_event, cm_remove_event,
    cm_add_event, cm_remove_event, cm_add_event, cm_remove_event, cm_add_event, cm_remove_event,
    cm_add_event, cm_remove_event, cm_add_event, cm_remove_event, cm_add_event, cm_remove_event,
    cm_add_event, cm_remove_event,
};

static HRESULT create_command_manager( IUnknown *player, void **out )
{
    struct command_manager *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (!(impl->behavior = calloc( 1, sizeof(*impl->behavior) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->vtbl = command_manager_vtbl;
    impl->ref = 1;
    impl->player = player;
    impl->behavior->vtbl = command_behavior_vtbl;
    impl->behavior->ref = 1;
    impl->behavior->manager = (IUnknown *)impl;
    *out = impl;
    return S_OK;
}

/* MediaBreakManager, instrumented: handing one back destabilised the run, so every entry point
 * says what it was asked for. Slot list from Windows.Media.winmd. */
static const GUID IID_IMediaBreakManager_local =
    {0xa854ddb1,0xfeb4,0x4d9b,{0x9d,0x97,0x0f,0xdb,0xe5,0x8e,0x5e,0x39}};

struct break_manager { const void **vtbl; LONG ref; IMediaPlaybackSession *session; };
static const void *break_manager_vtbl[18];

static HRESULT WINAPI bm_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct break_manager *impl = iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMediaBreakManager_local ))
    {
        InterlockedIncrement( &impl->ref );
        *out = iface;
        return S_OK;
    }
    TRACE( "no interface %s.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI bm_AddRef( void *iface ) { return InterlockedIncrement( &((struct break_manager *)iface)->ref ); }
static ULONG WINAPI bm_Release( void *iface )
{
    struct break_manager *impl = iface;
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->session) IMediaPlaybackSession_Release( impl->session );
        free( impl );
    }
    return ref;
}
static HRESULT WINAPI bm_GetIids( void *iface, ULONG *c, IID **i ) { *c = 0; *i = NULL; return S_OK; }
static HRESULT WINAPI bm_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Media.Playback.MediaBreakManager", 39, name );
}
static HRESULT WINAPI bm_GetTrustLevel( void *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }
static HRESULT WINAPI bm_add_event( void *iface, void *handler, EventRegistrationToken *token )
{
    TRACE( "handler %p, token %p.\n", handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}
static HRESULT WINAPI bm_remove_event( void *iface, EventRegistrationToken token )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}
static HRESULT WINAPI bm_get_CurrentBreak( void *iface, void **value )
{
    TRACE( "iface %p.\n", iface );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}
static HRESULT WINAPI bm_get_PlaybackSession( void *iface, IMediaPlaybackSession **value )
{
    struct break_manager *impl = iface;

    TRACE( "iface %p, value %p.\n", iface, value );

    /* The player's own session, not null: the app reads this straight after asking for the break
     * manager and goes on to use it. Returning null here is what made the run unstable. */
    if (!value) return E_POINTER;
    if (impl->session) IMediaPlaybackSession_AddRef( impl->session );
    *value = impl->session;
    return S_OK;
}
static HRESULT WINAPI bm_PlayBreak( void *iface, void *media_break ) { TRACE( "iface %p.\n", iface ); return S_OK; }
static HRESULT WINAPI bm_SkipCurrentBreak( void *iface ) { TRACE( "iface %p.\n", iface ); return S_OK; }

static const void *break_manager_vtbl[18] =
{
    bm_QueryInterface, bm_AddRef, bm_Release, bm_GetIids, bm_GetRuntimeClassName, bm_GetTrustLevel,
    bm_add_event, bm_remove_event, bm_add_event, bm_remove_event,
    bm_add_event, bm_remove_event, bm_add_event, bm_remove_event,
    bm_get_CurrentBreak, bm_get_PlaybackSession, bm_PlayBreak, bm_SkipCurrentBreak,
};

static HRESULT create_break_manager( IMediaPlaybackSession *session, void **out )
{
    struct break_manager *impl;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->vtbl = break_manager_vtbl;
    impl->ref = 1;
    if ((impl->session = session)) IMediaPlaybackSession_AddRef( session );
    *out = impl;
    return S_OK;
}

static HRESULT WINAPI media_player3_get_BreakManager( IMediaPlayer3 *iface, IMediaBreakManager **value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );

    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!impl->session && FAILED(hr = playback_session_create( &impl->IMediaPlayer_iface, &impl->session )))
        return hr;
    if (!impl->break_manager && FAILED(hr = create_break_manager( impl->session, (void **)&impl->break_manager )))
        return hr;
    IUnknown_AddRef( impl->break_manager );
    *value = (IMediaBreakManager *)impl->break_manager;
    return S_OK;
}

static HRESULT WINAPI media_player3_get_CommandManager( IMediaPlayer3 *iface, IMediaPlaybackCommandManager **value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!impl->command_manager)
    {
        HRESULT hr = create_command_manager( (IUnknown *)&impl->IMediaPlayer_iface,
                                             (void **)&impl->command_manager );
        if (FAILED( hr )) return hr;
    }
    IUnknown_AddRef( impl->command_manager );
    *value = (IMediaPlaybackCommandManager *)impl->command_manager;
    return S_OK;
}

static HRESULT WINAPI media_player3_get_AudioDevice( IMediaPlayer3 *iface, IDeviceInformation **value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    /* the default endpoint, which is all Wine routes to */
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI media_player3_put_AudioDevice( IMediaPlayer3 *iface, IDeviceInformation *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI media_player3_get_TimelineController( IMediaPlayer3 *iface, IInspectable **value )
{
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI media_player3_put_TimelineController( IMediaPlayer3 *iface, IInspectable *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI media_player3_get_TimelineControllerPositionOffset( IMediaPlayer3 *iface, TimeSpan *value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %p\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->timeline_offset;
    return S_OK;
}

static HRESULT WINAPI media_player3_put_TimelineControllerPositionOffset( IMediaPlayer3 *iface, TimeSpan value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    TRACE( "iface %p, value %I64d\n", iface, value.Duration );
    impl->timeline_offset = value;
    return S_OK;
}

static HRESULT WINAPI media_player3_get_PlaybackSession( IMediaPlayer3 *iface, IMediaPlaybackSession **value )
{
    struct media_player *impl = impl_from_IMediaPlayer3( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    if (!impl->session && FAILED(hr = playback_session_create( &impl->IMediaPlayer_iface, &impl->session ))) return hr;
    IMediaPlaybackSession_AddRef( (*value = impl->session) );
    return S_OK;
}

static HRESULT WINAPI media_player3_StepForwardOneFrame( IMediaPlayer3 *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player3_StepBackwardOneFrame( IMediaPlayer3 *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player3_GetAsCastingSource( IMediaPlayer3 *iface, IInspectable **return_value )
{
    FIXME( "iface %p, return_value %p stub!\n", iface, return_value );
    return E_NOTIMPL;
}

static const struct IMediaPlayer3Vtbl media_player3_vtbl =
{
    media_player3_QueryInterface,
    media_player3_AddRef,
    media_player3_Release,
    /* IInspectable methods */
    media_player3_GetIids,
    media_player3_GetRuntimeClassName,
    media_player3_GetTrustLevel,
    /* IMediaPlayer3 methods */
    media_player3_add_IsMutedChanged,
    media_player3_remove_IsMutedChanged,
    media_player3_add_SourceChanged,
    media_player3_remove_SourceChanged,
    media_player3_get_AudioBalance,
    media_player3_put_AudioBalance,
    media_player3_get_RealTimePlayback,
    media_player3_put_RealTimePlayback,
    media_player3_get_StereoscopicVideoRenderMode,
    media_player3_put_StereoscopicVideoRenderMode,
    media_player3_get_BreakManager,
    media_player3_get_CommandManager,
    media_player3_get_AudioDevice,
    media_player3_put_AudioDevice,
    media_player3_get_TimelineController,
    media_player3_put_TimelineController,
    media_player3_get_TimelineControllerPositionOffset,
    media_player3_put_TimelineControllerPositionOffset,
    media_player3_get_PlaybackSession,
    media_player3_StepForwardOneFrame,
    media_player3_StepBackwardOneFrame,
    media_player3_GetAsCastingSource,
};

DEFINE_IINSPECTABLE( media_player5, IMediaPlayer5, struct media_player, IMediaPlayer_iface )

/* The frame-server mode: an app turns it on to pull decoded frames itself instead of letting the
 * player present them. Wine decodes nothing yet, so the flag round-trips and no frame ever arrives. */
static HRESULT WINAPI media_player5_add_VideoFrameAvailable( IMediaPlayer5 *iface,
        ITypedEventHandler_MediaPlayer_IInspectable *value, EventRegistrationToken *token )
{
    FIXME( "iface %p, value %p, token %p stub!\n", iface, value, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player5_remove_VideoFrameAvailable( IMediaPlayer5 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player5_get_IsVideoFrameServerEnabled( IMediaPlayer5 *iface, boolean *value )
{
    struct media_player *impl = impl_from_IMediaPlayer5( iface );

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->frame_server_enabled;
    return S_OK;
}

static HRESULT WINAPI media_player5_put_IsVideoFrameServerEnabled( IMediaPlayer5 *iface, boolean value )
{
    struct media_player *impl = impl_from_IMediaPlayer5( iface );

    TRACE( "iface %p, value %u\n", iface, value );

    impl->frame_server_enabled = value;
    return S_OK;
}

static HRESULT WINAPI media_player5_CopyFrameToVideoSurface( IMediaPlayer5 *iface, IDirect3DSurface *destination )
{
    FIXME( "iface %p, destination %p stub!\n", iface, destination );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player5_CopyFrameToVideoSurfaceWithTargetRectangle( IMediaPlayer5 *iface,
        IDirect3DSurface *destination, Rect target_rectangle )
{
    FIXME( "iface %p, destination %p, target_rectangle (%f,%f)-(%f,%f) stub!\n", iface, destination,
           target_rectangle.X, target_rectangle.Y, target_rectangle.Width, target_rectangle.Height );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player5_CopyFrameToStereoscopicVideoSurfaces( IMediaPlayer5 *iface,
        IDirect3DSurface *destination_left_eye, IDirect3DSurface *destination_right_eye )
{
    FIXME( "iface %p, destination_left_eye %p, destination_right_eye %p stub!\n", iface,
           destination_left_eye, destination_right_eye );
    return E_NOTIMPL;
}

static HRESULT WINAPI media_player5_add_SubtitleFrameChanged( IMediaPlayer5 *iface,
        ITypedEventHandler_MediaPlayer_IInspectable *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p stub!\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI media_player5_remove_SubtitleFrameChanged( IMediaPlayer5 *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI media_player5_RenderSubtitlesToSurface( IMediaPlayer5 *iface, IDirect3DSurface *destination,
                                                              boolean *result )
{
    FIXME( "iface %p, destination %p, result %p stub!\n", iface, destination, result );
    if (!result) return E_POINTER;
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI media_player5_RenderSubtitlesToSurfaceWithTargetRectangle( IMediaPlayer5 *iface,
        IDirect3DSurface *destination, Rect target_rectangle, boolean *result )
{
    FIXME( "iface %p, destination %p, target_rectangle (%f,%f)-(%f,%f), result %p stub!\n", iface, destination,
           target_rectangle.X, target_rectangle.Y, target_rectangle.Width, target_rectangle.Height, result );
    if (!result) return E_POINTER;
    *result = FALSE;
    return S_OK;
}

DEFINE_IINSPECTABLE( media_player_closable, IClosable, struct media_player, IMediaPlayer_iface )

/* .NET projects IClosable as IDisposable, so "using (var player = new MediaPlayer())" needs it.
 * Close stops playback; the object itself goes when its last reference does. */
static HRESULT WINAPI media_player_closable_Close( IClosable *iface )
{
    struct media_player *impl = impl_from_IClosable( iface );
    TRACE( "iface %p.\n", iface );
    return IMediaPlayer_Pause( &impl->IMediaPlayer_iface );
}

static const struct IClosableVtbl media_player_closable_vtbl =
{
    media_player_closable_QueryInterface,
    media_player_closable_AddRef,
    media_player_closable_Release,
    media_player_closable_GetIids,
    media_player_closable_GetRuntimeClassName,
    media_player_closable_GetTrustLevel,
    media_player_closable_Close,
};

static const struct IMediaPlayer5Vtbl media_player5_vtbl =
{
    media_player5_QueryInterface,
    media_player5_AddRef,
    media_player5_Release,
    /* IInspectable methods */
    media_player5_GetIids,
    media_player5_GetRuntimeClassName,
    media_player5_GetTrustLevel,
    /* IMediaPlayer5 methods */
    media_player5_add_VideoFrameAvailable,
    media_player5_remove_VideoFrameAvailable,
    media_player5_get_IsVideoFrameServerEnabled,
    media_player5_put_IsVideoFrameServerEnabled,
    media_player5_CopyFrameToVideoSurface,
    media_player5_CopyFrameToVideoSurfaceWithTargetRectangle,
    media_player5_CopyFrameToStereoscopicVideoSurfaces,
    media_player5_add_SubtitleFrameChanged,
    media_player5_remove_SubtitleFrameChanged,
    media_player5_RenderSubtitlesToSurface,
    media_player5_RenderSubtitlesToSurfaceWithTargetRectangle,
};

struct media_player_statics
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct media_player_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct media_player_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct media_player_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct media_player_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct media_player_statics *impl = impl_from_IActivationFactory( iface );
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

static HRESULT get_system_media_transport_controls( HWND *window, ISystemMediaTransportControls **controls )
{
    static const WCHAR *media_control_statics_name = L"Windows.Media.SystemMediaTransportControls";
    ISystemMediaTransportControlsInterop *media_control_interop_statics = NULL;
    IActivationFactory *factory = NULL;
    HSTRING str = NULL;
    HRESULT hr;
    HWND hwnd;

    FIXME( "shell integration not implemented.\n" );

    if (!(hwnd = CreateWindowExW( 0, L"static", NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW( NULL ), NULL ))) return HRESULT_FROM_WIN32( GetLastError() );

    if (FAILED(hr = WindowsCreateString( media_control_statics_name, wcslen( media_control_statics_name ), &str ))) goto done;
    if (SUCCEEDED(hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory )))
    {
        hr = IActivationFactory_QueryInterface( factory, &IID_ISystemMediaTransportControlsInterop, (void **)&media_control_interop_statics );
        IActivationFactory_Release( factory );
    }
    if (SUCCEEDED(hr))
    {
        hr = ISystemMediaTransportControlsInterop_GetForWindow( media_control_interop_statics, hwnd, &IID_ISystemMediaTransportControls, (void **)controls );
        ISystemMediaTransportControlsInterop_Release( media_control_interop_statics );
    }
    WindowsDeleteString( str );

done:
    if (FAILED(hr)) DestroyWindow( hwnd );
    else *window = hwnd;
    return hr;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct media_player *impl;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    *instance = NULL;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = get_system_media_transport_controls( &impl->window, &impl->controls )))
    {
        free( impl );
        return hr;
    }

    impl->IMediaPlayer_iface.lpVtbl = &media_player_vtbl;
    if (FAILED(playback_create( &impl->playback )))
        WARN( "no playback engine; the player will not produce audio.\n" );
    impl->IMediaPlayer2_iface.lpVtbl = &media_player2_vtbl;
    impl->IMediaPlayer3_iface.lpVtbl = &media_player3_vtbl;
    impl->IMediaPlayerSource2_iface.lpVtbl = &media_player_source2_vtbl;
    impl->volume = 1.0;
    impl->rate = 1.0;
    impl->IMediaPlayer5_iface.lpVtbl = &media_player5_vtbl;
    impl->IClosable_iface.lpVtbl = &media_player_closable_vtbl;
    impl->ref = 1;

    *instance = (IInspectable *)&impl->IMediaPlayer_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    /* IUnknown methods */
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

static struct media_player_statics media_player_statics =
{
    {&factory_vtbl},
    1,
};

static IActivationFactory *media_player_factory = &media_player_statics.IActivationFactory_iface;

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "class %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;

    if (!wcscmp( buffer, RuntimeClass_Windows_Media_Playback_MediaPlayer ))
        IActivationFactory_QueryInterface( media_player_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}

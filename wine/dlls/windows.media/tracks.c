/*
 * Windows.Media.Core.AudioTrack and Windows.Media.Playback.MediaPlaybackAudioTrackList
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
 * Media Player asks a freshly created MediaPlaybackItem for its AudioTracks before it will put the
 * item in a playlist, and casts the answer to MediaPlaybackAudioTrackList. Handing back an empty
 * IVectorView threw InvalidCastException inside the app, so the item was discarded and the list
 * the player had been given as its source stayed empty forever.
 *
 * Nothing here demuxes anything - the player opens the file with Media Foundation when it comes
 * to play it. The list exists so that the app can see one audio track and carry on, which is what
 * it would see for any ordinary audio file.
 */

/* IVectorView<AudioTrack> and IIterable<AudioTrack>, derived over AudioTrack's real default
 * interface IMediaTrack. windows.media.core.idl declares the default as IAudioTrack, so the
 * generated IID_IVectorView_AudioTrack does not match what the app asks for. */
struct track_list_kind
{
    const WCHAR *class_name;      /* what .NET types the RCW as */
    const WCHAR *track_class;     /* the element class name */
    GUID view_iid;                /* IVectorView<T> */
    GUID iterable_iid;            /* IIterable<T> */
    GUID observable_iid;          /* IObservableVector<T> */
    INT32 track_kind;             /* MediaTrackKind */
    BOOL single_select;           /* audio and video lists are single-select */
    BOOL has_track;               /* only the audio list carries a track */
};

static const struct track_list_kind audio_kind =
{
    L"Windows.Media.Playback.MediaPlaybackAudioTrackList", L"Windows.Media.Core.AudioTrack",
    {0xe71ffa4a,0x14a7,0x5068,{0x89,0x1c,0x6e,0x71,0x7a,0xf8,0xbf,0x8d}},
    {0xb3be50a8,0x9856,0x5656,{0xab,0x6d,0xcd,0x58,0xf9,0xde,0x0a,0x4f}},
    {0xa72dc363,0x78d9,0x5aaa,{0xba,0x50,0xb5,0x2a,0xd7,0x65,0x68,0xde}},
    0 /* MediaTrackKind_Audio */, TRUE, TRUE,
};

/* A music file has no video and no timed metadata, so these lists are genuinely empty - but they
 * still have to be the right class, or the app's cast throws exactly as it did for audio. */
static const struct track_list_kind video_kind =
{
    L"Windows.Media.Playback.MediaPlaybackVideoTrackList", L"Windows.Media.Core.VideoTrack",
    {0x200c6224,0x0b43,0x54fb,{0x99,0x5f,0xfd,0x86,0x88,0xbf,0x24,0xde}},
    {0x84413442,0xa2e3,0x5e0f,{0x93,0x6d,0xbc,0x40,0xb0,0xfb,0x2d,0xcd}},
    {0xadf2598a,0x5dc2,0x54bd,{0xbc,0x23,0x0e,0x5d,0xf0,0x08,0xa4,0xa9}},
    1 /* MediaTrackKind_Video */, TRUE, FALSE,
};

static const struct track_list_kind metadata_kind =
{
    L"Windows.Media.Playback.MediaPlaybackTimedMetadataTrackList",
    L"Windows.Media.Core.TimedMetadataTrack",
    {0x9fd7f588,0x6a85,0x516f,{0xbd,0xcb,0xdc,0xb2,0xca,0xc1,0x1f,0x2e}},
    {0x74d8d19e,0x63e3,0x53e1,{0x9b,0x4f,0x83,0x93,0xeb,0xaf,0x75,0x7e}},
    {0x1bce7131,0x4d00,0x5fa5,{0xac,0x6b,0x51,0x2d,0xce,0xdc,0x37,0x39}},
    2 /* MediaTrackKind_TimedMetadata */, FALSE, FALSE,
};

/* ---------------------------------------------------------------- AudioTrack */

struct audio_track
{
    IMediaTrackInternal IMediaTrackInternal_iface;
    IAudioTrackInternal IAudioTrackInternal_iface;
    LONG ref;
    HSTRING label;
    const struct track_list_kind *kind;
    /* The item owns the list which owns the track, so this is deliberately not a reference. */
    IInspectable *item;
};

static inline struct audio_track *impl_from_IMediaTrackInternal( IMediaTrackInternal *iface )
{
    return CONTAINING_RECORD( iface, struct audio_track, IMediaTrackInternal_iface );
}

static HRESULT WINAPI audio_track_QueryInterface( IMediaTrackInternal *iface, REFIID iid, void **out )
{
    struct audio_track *impl = impl_from_IMediaTrackInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMediaTrackInternal ))
    {
        IInspectable_AddRef( (*out = (IInspectable *)&impl->IMediaTrackInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAudioTrackInternal ))
    {
        IInspectable_AddRef( (*out = (IInspectable *)&impl->IAudioTrackInternal_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI audio_track_AddRef( IMediaTrackInternal *iface )
{
    return InterlockedIncrement( &impl_from_IMediaTrackInternal( iface )->ref );
}

static ULONG WINAPI audio_track_Release( IMediaTrackInternal *iface )
{
    struct audio_track *impl = impl_from_IMediaTrackInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->label );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI audio_track_GetIids( IMediaTrackInternal *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI audio_track_GetRuntimeClassName( IMediaTrackInternal *iface, HSTRING *name )
{
    struct audio_track *impl = impl_from_IMediaTrackInternal( iface );
    return WindowsCreateString( impl->kind->track_class, wcslen( impl->kind->track_class ), name );
}

static HRESULT WINAPI audio_track_GetTrustLevel( IMediaTrackInternal *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI audio_track_get_Id( IMediaTrackInternal *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( L"0", 1, value );
}

/* Nothing is demuxed, so the language is genuinely unknown. An empty string is what a track with
 * no language tag reports, which is a case every player already handles. */
static HRESULT WINAPI audio_track_get_Language( IMediaTrackInternal *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI audio_track_get_TrackKind( IMediaTrackInternal *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl_from_IMediaTrackInternal( iface )->kind->track_kind;
    return S_OK;
}

static HRESULT WINAPI audio_track_put_Label( IMediaTrackInternal *iface, HSTRING value )
{
    struct audio_track *impl = impl_from_IMediaTrackInternal( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->label );
    impl->label = copy;
    return S_OK;
}

static HRESULT WINAPI audio_track_get_Label( IMediaTrackInternal *iface, HSTRING *value )
{
    struct audio_track *impl = impl_from_IMediaTrackInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->label, value );
}

static const struct IMediaTrackInternalVtbl audio_track_vtbl =
{
    /* IUnknown methods */
    audio_track_QueryInterface,
    audio_track_AddRef,
    audio_track_Release,
    /* IInspectable methods */
    audio_track_GetIids,
    audio_track_GetRuntimeClassName,
    audio_track_GetTrustLevel,
    /* IMediaTrack methods */
    audio_track_get_Id,
    audio_track_get_Language,
    audio_track_get_TrackKind,
    audio_track_put_Label,
    audio_track_get_Label,
};

DEFINE_IINSPECTABLE_( audio_track2, IAudioTrackInternal, struct audio_track,
                      impl_from_IAudioTrackInternal, IAudioTrackInternal_iface,
                      &impl->IMediaTrackInternal_iface )

/* The track never fails to open here - the player opens the file itself, and reports its own
 * failure - but the registration has to succeed. */
static HRESULT WINAPI audio_track_add_OpenFailed( IAudioTrackInternal *iface, IInspectable *handler,
                                                  EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p: OpenFailed never fires.\n", iface, handler );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI audio_track_remove_OpenFailed( IAudioTrackInternal *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI audio_track_GetEncodingProperties( IAudioTrackInternal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p: nothing is demuxed, so the encoding is not known.\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI audio_track_get_PlaybackItem( IAudioTrackInternal *iface, IInspectable **value )
{
    struct audio_track *impl = impl_from_IAudioTrackInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->item)) IInspectable_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI audio_track_get_Name( IAudioTrackInternal *iface, HSTRING *value )
{
    struct audio_track *impl = impl_from_IAudioTrackInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->label, value );
}

static HRESULT WINAPI audio_track_get_SupportInfo( IAudioTrackInternal *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p: no decoder has been consulted yet.\n", iface, value );
    if (value) *value = NULL;
    return E_NOTIMPL;
}

static const struct IAudioTrackInternalVtbl audio_track2_vtbl =
{
    audio_track2_QueryInterface,
    audio_track2_AddRef,
    audio_track2_Release,
    audio_track2_GetIids,
    audio_track2_GetRuntimeClassName,
    audio_track2_GetTrustLevel,
    audio_track_add_OpenFailed,
    audio_track_remove_OpenFailed,
    audio_track_GetEncodingProperties,
    audio_track_get_PlaybackItem,
    audio_track_get_Name,
    audio_track_get_SupportInfo,
};

static HRESULT audio_track_create( IInspectable *item, const struct track_list_kind *kind,
                                   struct audio_track **out )
{
    struct audio_track *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMediaTrackInternal_iface.lpVtbl = &audio_track_vtbl;
    impl->IAudioTrackInternal_iface.lpVtbl = &audio_track2_vtbl;
    impl->ref = 1;
    impl->item = item;
    impl->kind = kind;
    if (FAILED(hr = WindowsCreateString( L"Audio", 5, &impl->label )))
    {
        free( impl );
        return hr;
    }

    *out = impl;
    return S_OK;
}

/* --------------------------------------------- MediaPlaybackAudioTrackList
 *
 * IVectorView<AudioTrack> over the single track, plus ISingleSelectMediaTrackList so the app can
 * ask which track is playing, plus IObservableVector<AudioTrack> so it can watch for changes.
 * The vector view's four methods sit directly after IInspectable, so its vtable is hand-rolled
 * the same way the empty view's was.
 */
struct audio_track_list
{
    IInspectable IInspectable_iface;                          /* IVectorView<AudioTrack> */
    ISingleSelectMediaTrackListInternal ISingleSelectMediaTrackListInternal_iface;
    IInspectable IObservableVector_iface;                     /* IObservableVector<AudioTrack> */
    IMediaPlaybackTimedMetadataTrackListInternal IMediaPlaybackTimedMetadataTrackListInternal_iface;
    LONG ref;
    INT32 selected;
    const struct track_list_kind *kind;
    struct audio_track *track;
};

static inline struct audio_track_list *impl_from_track_list( IInspectable *iface )
{
    return CONTAINING_RECORD( iface, struct audio_track_list, IInspectable_iface );
}

static HRESULT WINAPI track_list_QueryInterface( IInspectable *iface, REFIID iid, void **out )
{
    struct audio_track_list *impl = impl_from_track_list( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &impl->kind->view_iid ) ||
        IsEqualGUID( iid, &impl->kind->iterable_iid ))
    {
        IInspectable_AddRef( (*out = &impl->IInspectable_iface) );
        return S_OK;
    }

    if (impl->kind->single_select && IsEqualGUID( iid, &IID_ISingleSelectMediaTrackListInternal ))
    {
        IInspectable_AddRef( (*out = (IInspectable *)&impl->ISingleSelectMediaTrackListInternal_iface) );
        return S_OK;
    }

    if (!impl->kind->single_select &&
        IsEqualGUID( iid, &IID_IMediaPlaybackTimedMetadataTrackListInternal ))
    {
        IInspectable_AddRef( (*out = (IInspectable *)&impl->IMediaPlaybackTimedMetadataTrackListInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &impl->kind->observable_iid ))
    {
        IInspectable_AddRef( (*out = &impl->IObservableVector_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI track_list_AddRef( IInspectable *iface )
{
    return InterlockedIncrement( &impl_from_track_list( iface )->ref );
}

static ULONG WINAPI track_list_Release( IInspectable *iface )
{
    struct audio_track_list *impl = impl_from_track_list( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->track) IMediaTrackInternal_Release( &impl->track->IMediaTrackInternal_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI track_list_GetIids( IInspectable *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

/* This is how .NET decides what type the RCW is. Naming the interface instead of the class is
 * what made the app's cast to MediaPlaybackAudioTrackList throw. */
static HRESULT WINAPI track_list_GetRuntimeClassName( IInspectable *iface, HSTRING *name )
{
    struct audio_track_list *impl = impl_from_track_list( iface );
    return WindowsCreateString( impl->kind->class_name, wcslen( impl->kind->class_name ), name );
}

static HRESULT WINAPI track_list_GetTrustLevel( IInspectable *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI track_list_GetAt( IInspectable *iface, UINT32 index, IInspectable **value )
{
    struct audio_track_list *impl = impl_from_track_list( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index || !impl->track) return E_BOUNDS;

    IMediaTrackInternal_AddRef( &impl->track->IMediaTrackInternal_iface );
    *value = (IInspectable *)&impl->track->IMediaTrackInternal_iface;
    return S_OK;
}

static HRESULT WINAPI track_list_get_Size( IInspectable *iface, UINT32 *value )
{
    struct audio_track_list *impl = impl_from_track_list( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->track ? 1 : 0;
    return S_OK;
}

static HRESULT WINAPI track_list_IndexOf( IInspectable *iface, IInspectable *item,
                                          UINT32 *index, boolean *found )
{
    struct audio_track_list *impl = impl_from_track_list( iface );

    TRACE( "iface %p, item %p, index %p, found %p.\n", iface, item, index, found );

    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = impl->track && item == (IInspectable *)&impl->track->IMediaTrackInternal_iface;
    return S_OK;
}

static HRESULT WINAPI track_list_GetMany( IInspectable *iface, UINT32 start, UINT32 count,
                                          IInspectable **items, UINT32 *written )
{
    struct audio_track_list *impl = impl_from_track_list( iface );

    TRACE( "iface %p, start %u, count %u.\n", iface, start, count );

    if (!written) return E_POINTER;
    *written = 0;
    if (start || !count || !impl->track) return S_OK;

    IMediaTrackInternal_AddRef( &impl->track->IMediaTrackInternal_iface );
    items[0] = (IInspectable *)&impl->track->IMediaTrackInternal_iface;
    *written = 1;
    return S_OK;
}

static const void *track_list_vtbl[] =
{
    track_list_QueryInterface, track_list_AddRef, track_list_Release,
    track_list_GetIids, track_list_GetRuntimeClassName, track_list_GetTrustLevel,
    track_list_GetAt, track_list_get_Size, track_list_IndexOf, track_list_GetMany,
};

/* IObservableVector<AudioTrack>: IInspectable plus add/remove_VectorChanged. The track list never
 * changes - there is one track for the life of the item - but registering has to succeed. */
static inline struct audio_track_list *impl_from_observable( IInspectable *iface )
{
    return CONTAINING_RECORD( iface, struct audio_track_list, IObservableVector_iface );
}

static HRESULT WINAPI track_observable_QueryInterface( IInspectable *iface, REFIID iid, void **out )
{
    return track_list_QueryInterface( &impl_from_observable( iface )->IInspectable_iface, iid, out );
}

static ULONG WINAPI track_observable_AddRef( IInspectable *iface )
{
    return track_list_AddRef( &impl_from_observable( iface )->IInspectable_iface );
}

static ULONG WINAPI track_observable_Release( IInspectable *iface )
{
    return track_list_Release( &impl_from_observable( iface )->IInspectable_iface );
}

static HRESULT WINAPI track_observable_GetIids( IInspectable *iface, ULONG *count, IID **iids )
{
    return track_list_GetIids( &impl_from_observable( iface )->IInspectable_iface, count, iids );
}

static HRESULT WINAPI track_observable_GetRuntimeClassName( IInspectable *iface, HSTRING *name )
{
    return track_list_GetRuntimeClassName( &impl_from_observable( iface )->IInspectable_iface, name );
}

static HRESULT WINAPI track_observable_GetTrustLevel( IInspectable *iface, TrustLevel *level )
{
    return track_list_GetTrustLevel( &impl_from_observable( iface )->IInspectable_iface, level );
}

static HRESULT WINAPI track_observable_add_VectorChanged( IInspectable *iface, IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p: the track list never changes.\n", iface, handler );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI track_observable_remove_VectorChanged( IInspectable *iface, EventRegistrationToken token )
{
    return S_OK;
}

static const void *track_observable_vtbl[] =
{
    track_observable_QueryInterface, track_observable_AddRef, track_observable_Release,
    track_observable_GetIids, track_observable_GetRuntimeClassName, track_observable_GetTrustLevel,
    track_observable_add_VectorChanged, track_observable_remove_VectorChanged,
};

DEFINE_IINSPECTABLE_( track_select, ISingleSelectMediaTrackListInternal, struct audio_track_list,
                      impl_from_ISingleSelectMediaTrackListInternal,
                      ISingleSelectMediaTrackListInternal_iface, &impl->IInspectable_iface )

static HRESULT WINAPI track_select_add_SelectedIndexChanged( ISingleSelectMediaTrackListInternal *iface,
                                                             IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p: there is only one track, so the selection never changes.\n",
           iface, handler );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI track_select_remove_SelectedIndexChanged( ISingleSelectMediaTrackListInternal *iface,
                                                                EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI track_select_put_SelectedIndex( ISingleSelectMediaTrackListInternal *iface, INT32 value )
{
    struct audio_track_list *impl = impl_from_ISingleSelectMediaTrackListInternal( iface );

    TRACE( "iface %p, value %ld.\n", iface, (long)value );

    impl->selected = value;
    return S_OK;
}

static HRESULT WINAPI track_select_get_SelectedIndex( ISingleSelectMediaTrackListInternal *iface, INT32 *value )
{
    struct audio_track_list *impl = impl_from_ISingleSelectMediaTrackListInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->selected;
    return S_OK;
}

static const struct ISingleSelectMediaTrackListInternalVtbl track_select_vtbl =
{
    track_select_QueryInterface,
    track_select_AddRef,
    track_select_Release,
    track_select_GetIids,
    track_select_GetRuntimeClassName,
    track_select_GetTrustLevel,
    track_select_add_SelectedIndexChanged,
    track_select_remove_SelectedIndexChanged,
    track_select_put_SelectedIndex,
    track_select_get_SelectedIndex,
};

DEFINE_IINSPECTABLE_( track_metadata, IMediaPlaybackTimedMetadataTrackListInternal, struct audio_track_list,
                      impl_from_IMediaPlaybackTimedMetadataTrackListInternal,
                      IMediaPlaybackTimedMetadataTrackListInternal_iface, &impl->IInspectable_iface )

static HRESULT WINAPI track_metadata_add_PresentationModeChanged( IMediaPlaybackTimedMetadataTrackListInternal *iface,
                                                                  IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p: the list is empty, so the mode never changes.\n", iface, handler );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI track_metadata_remove_PresentationModeChanged( IMediaPlaybackTimedMetadataTrackListInternal *iface,
                                                                     EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI track_metadata_GetPresentationMode( IMediaPlaybackTimedMetadataTrackListInternal *iface,
                                                          UINT32 index, INT32 *value )
{
    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );
    if (!value) return E_POINTER;
    *value = 0;   /* TimedMetadataTrackPresentationMode_Disabled */
    return S_OK;
}

static HRESULT WINAPI track_metadata_SetPresentationMode( IMediaPlaybackTimedMetadataTrackListInternal *iface,
                                                          UINT32 index, INT32 value )
{
    TRACE( "iface %p, index %u, value %ld: the list is empty.\n", iface, index, (long)value );
    return S_OK;
}

static const struct IMediaPlaybackTimedMetadataTrackListInternalVtbl track_metadata_vtbl =
{
    track_metadata_QueryInterface,
    track_metadata_AddRef,
    track_metadata_Release,
    track_metadata_GetIids,
    track_metadata_GetRuntimeClassName,
    track_metadata_GetTrustLevel,
    track_metadata_add_PresentationModeChanged,
    track_metadata_remove_PresentationModeChanged,
    track_metadata_GetPresentationMode,
    track_metadata_SetPresentationMode,
};

static HRESULT track_list_create( IInspectable *item, const struct track_list_kind *kind,
                                  IInspectable **out )
{
    struct audio_track_list *impl;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IInspectable_iface.lpVtbl = (const IInspectableVtbl *)track_list_vtbl;
    impl->IObservableVector_iface.lpVtbl = (const IInspectableVtbl *)track_observable_vtbl;
    impl->ISingleSelectMediaTrackListInternal_iface.lpVtbl = &track_select_vtbl;
    impl->IMediaPlaybackTimedMetadataTrackListInternal_iface.lpVtbl = &track_metadata_vtbl;
    impl->ref = 1;
    impl->kind = kind;
    /* An empty single-select list reports -1, which is what "nothing selected" means. */
    impl->selected = kind->has_track ? 0 : -1;

    if (kind->has_track && FAILED(hr = audio_track_create( item, kind, &impl->track )))
    {
        free( impl );
        return hr;
    }

    TRACE( "created %s %p with %u tracks.\n", debugstr_w( kind->class_name ), impl,
           impl->track ? 1 : 0 );
    *out = &impl->IInspectable_iface;
    return S_OK;
}

HRESULT audio_track_list_create( IInspectable *item, IInspectable **out )
{
    return track_list_create( item, &audio_kind, out );
}

HRESULT video_track_list_create( IInspectable *item, IInspectable **out )
{
    return track_list_create( item, &video_kind, out );
}

HRESULT metadata_track_list_create( IInspectable *item, IInspectable **out )
{
    return track_list_create( item, &metadata_kind, out );
}

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

#ifndef __WINE_WINDOWS_MEDIA_PLAYBACK_MEDIAPLAYER_PRIVATE_H
#define __WINE_WINDOWS_MEDIA_PLAYBACK_MEDIAPLAYER_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Numerics
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Media
#define WIDL_using_Windows_Devices_Enumeration
#define WIDL_using_Windows_Graphics_DirectX_Direct3D11
#define WIDL_using_Windows_Media_Playback
#include "windows.media.playback.h"
#include "systemmediatransportcontrolsinterop.h"

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )

#endif

/* Playback, in playback_engine.c. Notifications come from the engine's threads, unlocked. */
struct playback;
enum playback_event
{
    PLAYBACK_EVENT_STATE,       /* started or stopped playing */
    PLAYBACK_EVENT_POSITION,    /* the clock moved (a few times a second while playing) */
    PLAYBACK_EVENT_SEEKED,
    PLAYBACK_EVENT_ENDED,
};
typedef void (*playback_notify_cb)( void *ctx, enum playback_event event );
HRESULT playback_create( playback_notify_cb notify, void *ctx, struct playback **out );
void playback_destroy( struct playback *pb );
HRESULT playback_set_path( struct playback *pb, const WCHAR *path );
HRESULT playback_probe( struct playback *pb );
HRESULT playback_play( struct playback *pb );
HRESULT playback_pause( struct playback *pb );
HRESULT playback_seek( struct playback *pb, LONGLONG position );
void playback_set_volume( struct playback *pb, double volume, BOOL muted );
BOOL playback_is_playing( struct playback *pb );
LONGLONG playback_position( struct playback *pb );
LONGLONG playback_duration( struct playback *pb );
void playback_video_size( struct playback *pb, UINT32 *width, UINT32 *height );
void playback_set_surface_size( struct playback *pb, UINT32 width, UINT32 height );
HANDLE playback_video_surface( struct playback *pb );

/*
 * Audio playback for Windows.Media.Playback.MediaPlayer.
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

#include "mfapi.h"
#include "mfidl.h"
#include "mfreadwrite.h"
#include "mferror.h"
#include "mmsystem.h"

WINE_DEFAULT_DEBUG_CHANNEL(mediaplayer);

/*
 * MediaPlayer had no playback at all: Play() was a stub that returned E_NOTIMPL, so an app could
 * set a source and never hear anything. This decodes with a Media Foundation source reader - which
 * gets winegstreamer's decoders, so it handles whatever the installed plugins handle, not just
 * PCM - and renders through waveOut.
 *
 * waveOut rather than WASAPI on purpose: this needs one stereo/mono PCM stream with no mixing
 * policy or session management, and waveOut is the interface in Wine where that is a handful of
 * calls rather than a device-enumeration dance. The decode runs on its own thread; everything the
 * player reads back (position, state) is updated from there under the player's lock.
 */

#define PLAYBACK_CHUNKS 8

struct playback
{
    CRITICAL_SECTION cs;
    HANDLE thread;
    HANDLE stop_event;

    WCHAR *path;
    BOOL playing;
    BOOL stopping;

    /* what the player reports back */
    LONGLONG position_100ns;
    LONGLONG duration_100ns;

    /* set by the caller so state changes can be reported */
    void *player;
};

static DWORD WINAPI playback_thread( void *arg );

static void playback_stop_locked( struct playback *pb )
{
    HANDLE thread = pb->thread;

    if (!thread) return;
    pb->stopping = TRUE;
    SetEvent( pb->stop_event );
    LeaveCriticalSection( &pb->cs );
    WaitForSingleObject( thread, 5000 );
    EnterCriticalSection( &pb->cs );
    CloseHandle( thread );
    pb->thread = NULL;
    pb->stopping = FALSE;
    pb->playing = FALSE;
}

HRESULT playback_create( struct playback **out )
{
    struct playback *pb;

    if (!(pb = calloc( 1, sizeof(*pb) ))) return E_OUTOFMEMORY;
    /* No DebugInfo annotation here: InitializeCriticalSectionEx leaves DebugInfo NULL in this
     * PE build, and the usual "DebugInfo->Spare[0] = ..." idiom faults on it. */
    InitializeCriticalSectionEx( &pb->cs, 0, 0 );
    if (!(pb->stop_event = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        DeleteCriticalSection( &pb->cs );
        free( pb );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    *out = pb;
    return S_OK;
}

void playback_destroy( struct playback *pb )
{
    if (!pb) return;
    EnterCriticalSection( &pb->cs );
    playback_stop_locked( pb );
    LeaveCriticalSection( &pb->cs );
    CloseHandle( pb->stop_event );
    free( pb->path );
    DeleteCriticalSection( &pb->cs );
    free( pb );
}

LONG WINAPI GetCurrentPackagePath( UINT32 *length, WCHAR *path );

static int hex_digit( WCHAR c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL is_dot_segment( const WCHAR *seg, const WCHAR *end )
{
    return seg[0] == '.' && (end - seg == 1 || (end - seg == 2 && seg[1] == '.'));
}

/* ms-appx:///<path> (or ms-appx://<package name>/<path>) names a file in the package's install
 * directory. Media Foundation has no handler for the scheme - the reader failed every packaged
 * video with MF_E_UNSUPPORTED_SCHEME - so turn it into the file's path. Percent escapes are
 * decoded (ASCII only), and "." / ".." segments are refused so a URI cannot leave the package.
 * Returns NULL for anything that is not an ms-appx URI or does not resolve. */
static WCHAR *ms_appx_to_path( const WCHAR *uri )
{
    const WCHAR *p;
    WCHAR *out, *seg;
    UINT32 len = 0;
    SIZE_T n;

    if (wcsnicmp( uri, L"ms-appx://", 10 )) return NULL;
    if (!(p = wcschr( uri + 10, '/' ))) return NULL;   /* skip the (usually empty) host */
    while (*p == '/') p++;

    if (GetCurrentPackagePath( &len, NULL ) != ERROR_INSUFFICIENT_BUFFER) return NULL;
    if (!(out = malloc( (len + wcslen( p ) + 2) * sizeof(WCHAR) ))) return NULL;
    if (GetCurrentPackagePath( &len, out )) goto fail;

    n = wcslen( out );
    if (n && out[n - 1] != '\\') out[n++] = '\\';
    seg = out + n;
    for (; *p && *p != '?' && *p != '#'; p++)
    {
        WCHAR c = *p;
        int hi, lo;

        if (c == '%')
        {
            if ((hi = hex_digit( p[1] )) < 0 || (lo = hex_digit( p[2] )) < 0) goto fail;
            if (!(c = hi * 16 + lo) || c >= 0x80) goto fail;
            p += 2;
        }
        if (c == '/') c = '\\';
        if (c == '\\')
        {
            if (is_dot_segment( seg, out + n )) goto fail;
            out[n++] = c;
            seg = out + n;
            continue;
        }
        out[n++] = c;
    }
    if (is_dot_segment( seg, out + n )) goto fail;
    out[n] = 0;
    TRACE( "%s is %s.\n", debugstr_w(uri), debugstr_w(out) );
    return out;

fail:
    WARN( "cannot resolve %s.\n", debugstr_w(uri) );
    free( out );
    return NULL;
}


/* A source can arrive as a plain path, a file:// URI or a ms-appx URI; the reader wants a path or
 * a URL it understands. ms-appx is resolved to the packaged file here; the others pass through. */
HRESULT playback_set_path( struct playback *pb, const WCHAR *path )
{
    WCHAR *copy;

    if (!path) return E_INVALIDARG;
    if (!(copy = ms_appx_to_path( path )) && !(copy = wcsdup( path ))) return E_OUTOFMEMORY;

    EnterCriticalSection( &pb->cs );
    playback_stop_locked( pb );
    free( pb->path );
    pb->path = copy;
    pb->position_100ns = 0;
    pb->duration_100ns = 0;
    LeaveCriticalSection( &pb->cs );

    TRACE( "source set to %s.\n", debugstr_w(copy) );
    return S_OK;
}

/* Open the file just far enough to learn its duration. An app's transport stays disabled until
 * the player says the media is open and how long it is, so this has to happen when the source is
 * set - not when playback starts. */
HRESULT playback_probe( struct playback *pb )
{
    IMFSourceReader *reader = NULL;
    PROPVARIANT duration;
    WCHAR *path;
    HRESULT hr;

    EnterCriticalSection( &pb->cs );
    path = pb->path ? wcsdup( pb->path ) : NULL;
    LeaveCriticalSection( &pb->cs );
    if (!path) return MF_E_NOT_INITIALIZED;

    if (FAILED(hr = MFStartup( MF_VERSION, MFSTARTUP_FULL )))
    {
        free( path );
        return hr;
    }

    if (SUCCEEDED(hr = MFCreateSourceReaderFromURL( path, NULL, &reader )))
    {
        PropVariantInit( &duration );
        if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute( reader, MF_SOURCE_READER_MEDIASOURCE,
                                                                &MF_PD_DURATION, &duration ))
            && duration.vt == VT_UI8)
        {
            EnterCriticalSection( &pb->cs );
            pb->duration_100ns = duration.uhVal.QuadPart;
            LeaveCriticalSection( &pb->cs );
            TRACE( "%s is %I64u00ns long.\n", debugstr_w(path), duration.uhVal.QuadPart );
        }
        PropVariantClear( &duration );
        IMFSourceReader_Release( reader );
    }
    else ERR( "cannot open %s, hr %#lx.\n", debugstr_w(path), hr );

    MFShutdown();
    free( path );
    return hr;
}

HRESULT playback_play( struct playback *pb )
{
    HRESULT hr = S_OK;

    EnterCriticalSection( &pb->cs );
    if (!pb->path)
    {
        LeaveCriticalSection( &pb->cs );
        WARN( "no source set.\n" );
        return MF_E_NOT_INITIALIZED;
    }
    if (pb->thread)
    {
        LeaveCriticalSection( &pb->cs );
        TRACE( "already playing.\n" );
        return S_OK;
    }

    ResetEvent( pb->stop_event );
    pb->playing = TRUE;
    if (!(pb->thread = CreateThread( NULL, 0, playback_thread, pb, 0, NULL )))
    {
        pb->playing = FALSE;
        hr = HRESULT_FROM_WIN32( GetLastError() );
    }
    LeaveCriticalSection( &pb->cs );
    return hr;
}

HRESULT playback_pause( struct playback *pb )
{
    /* There is no resume-from-position yet, so a pause stops the stream and keeps the position;
     * a later Play restarts from there once seeking is wired up. */
    EnterCriticalSection( &pb->cs );
    playback_stop_locked( pb );
    LeaveCriticalSection( &pb->cs );
    return S_OK;
}

BOOL playback_is_playing( struct playback *pb )
{
    BOOL playing;
    EnterCriticalSection( &pb->cs );
    playing = pb->playing;
    LeaveCriticalSection( &pb->cs );
    return playing;
}

LONGLONG playback_position( struct playback *pb )
{
    LONGLONG position;
    EnterCriticalSection( &pb->cs );
    position = pb->position_100ns;
    LeaveCriticalSection( &pb->cs );
    return position;
}

LONGLONG playback_duration( struct playback *pb )
{
    LONGLONG duration;
    EnterCriticalSection( &pb->cs );
    duration = pb->duration_100ns;
    LeaveCriticalSection( &pb->cs );
    return duration;
}

/* Ask the reader for plain 16-bit PCM and let it insert whatever decoder that needs. */
static HRESULT configure_reader( IMFSourceReader *reader, WAVEFORMATEX **format )
{
    IMFMediaType *type, *native = NULL;
    UINT32 size = 0;
    HRESULT hr;

    if (FAILED(hr = MFCreateMediaType( &type ))) return hr;
    if (FAILED(hr = IMFMediaType_SetGUID( type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio ))) goto done;
    if (FAILED(hr = IMFMediaType_SetGUID( type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM ))) goto done;
    if (FAILED(hr = IMFSourceReader_SetCurrentMediaType( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                         NULL, type )))
    {
        WARN( "the reader cannot produce PCM for this file: %#lx.\n", hr );
        goto done;
    }

    if (FAILED(hr = IMFSourceReader_GetCurrentMediaType( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                         &native ))) goto done;
    hr = MFCreateWaveFormatExFromMFMediaType( native, format, &size, MFWaveFormatExConvertFlag_Normal );
    IMFMediaType_Release( native );

done:
    IMFMediaType_Release( type );
    return hr;
}

static DWORD WINAPI playback_thread( void *arg )
{
    struct playback *pb = arg;
    IMFSourceReader *reader = NULL;
    IMFAttributes *attributes = NULL;
    WAVEFORMATEX *format = NULL;
    HWAVEOUT wave = NULL;
    WAVEHDR headers[PLAYBACK_CHUNKS] = {0};
    BYTE *buffers[PLAYBACK_CHUNKS] = {0};
    unsigned int next = 0, queued = 0, i;
    WCHAR *path;
    PROPVARIANT duration;
    HRESULT hr;

    EnterCriticalSection( &pb->cs );
    path = wcsdup( pb->path );
    LeaveCriticalSection( &pb->cs );
    if (!path) return 0;

    if (FAILED(hr = MFStartup( MF_VERSION, MFSTARTUP_FULL )))
    {
        ERR( "MFStartup failed, hr %#lx.\n", hr );
        free( path );
        return 0;
    }

    /* The reader is only ever read from this thread, so no multithreaded marshalling is needed. */
    if (FAILED(hr = MFCreateAttributes( &attributes, 1 ))) goto done;
    IMFAttributes_SetUINT32( attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE );

    if (FAILED(hr = MFCreateSourceReaderFromURL( path, attributes, &reader )))
    {
        ERR( "cannot open %s, hr %#lx.\n", debugstr_w(path), hr );
        goto done;
    }
    IMFSourceReader_SetStreamSelection( reader, MF_SOURCE_READER_ALL_STREAMS, FALSE );
    IMFSourceReader_SetStreamSelection( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE );

    if (FAILED(hr = configure_reader( reader, &format ))) goto done;
    TRACE( "%s: %u ch, %lu Hz, %u bits.\n", debugstr_w(path), format->nChannels,
           format->nSamplesPerSec, format->wBitsPerSample );

    PropVariantInit( &duration );
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute( reader, MF_SOURCE_READER_MEDIASOURCE,
                                                            &MF_PD_DURATION, &duration ))
        && duration.vt == VT_UI8)
    {
        EnterCriticalSection( &pb->cs );
        pb->duration_100ns = duration.uhVal.QuadPart;
        LeaveCriticalSection( &pb->cs );
    }
    PropVariantClear( &duration );

    if ((hr = waveOutOpen( &wave, WAVE_MAPPER, format, 0, 0, CALLBACK_NULL )) != MMSYSERR_NOERROR)
    {
        ERR( "waveOutOpen failed, %#lx.\n", hr );
        hr = E_FAIL;
        goto done;
    }

    while (WaitForSingleObject( pb->stop_event, 0 ) != WAIT_OBJECT_0)
    {
        IMFSample *sample = NULL;
        IMFMediaBuffer *buffer = NULL;
        DWORD flags = 0, length = 0;
        BYTE *data;

        if (FAILED(hr = IMFSourceReader_ReadSample( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                    0, NULL, &flags, NULL, &sample )))
        {
            ERR( "ReadSample failed, hr %#lx.\n", hr );
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;   /* a gap or a format change with no data */

        if (FAILED(hr = IMFSample_ConvertToContiguousBuffer( sample, &buffer )))
        {
            IMFSample_Release( sample );
            break;
        }
        if (SUCCEEDED(IMFMediaBuffer_Lock( buffer, &data, NULL, &length )) && length)
        {
            /* Recycle a small ring of headers so the device always has work queued while the
             * next sample decodes. */
            if (queued == PLAYBACK_CHUNKS)
            {
                while (!(headers[next].dwFlags & WHDR_DONE))
                {
                    if (WaitForSingleObject( pb->stop_event, 10 ) == WAIT_OBJECT_0) break;
                }
                waveOutUnprepareHeader( wave, &headers[next], sizeof(*headers) );
                free( buffers[next] );
                buffers[next] = NULL;
                queued--;
            }

            if ((buffers[next] = malloc( length )))
            {
                memcpy( buffers[next], data, length );
                headers[next].lpData = (LPSTR)buffers[next];
                headers[next].dwBufferLength = length;
                headers[next].dwFlags = 0;
                if (waveOutPrepareHeader( wave, &headers[next], sizeof(*headers) ) == MMSYSERR_NOERROR &&
                    waveOutWrite( wave, &headers[next], sizeof(*headers) ) == MMSYSERR_NOERROR)
                {
                    queued++;
                    next = (next + 1) % PLAYBACK_CHUNKS;
                }
                else
                {
                    free( buffers[next] );
                    buffers[next] = NULL;
                }
            }

            LONGLONG timestamp = 0;
            if (SUCCEEDED(IMFSample_GetSampleTime( sample, &timestamp )))
            {
                EnterCriticalSection( &pb->cs );
                pb->position_100ns = timestamp;
                LeaveCriticalSection( &pb->cs );
            }
            IMFMediaBuffer_Unlock( buffer );
        }
        IMFMediaBuffer_Release( buffer );
        IMFSample_Release( sample );
    }

    /* Let whatever is already queued finish, unless we were asked to stop. */
    if (wave && WaitForSingleObject( pb->stop_event, 0 ) != WAIT_OBJECT_0)
    {
        for (i = 0; i < PLAYBACK_CHUNKS; i++)
        {
            unsigned int slot = (next + i) % PLAYBACK_CHUNKS;
            if (!buffers[slot]) continue;
            while (!(headers[slot].dwFlags & WHDR_DONE))
                if (WaitForSingleObject( pb->stop_event, 20 ) == WAIT_OBJECT_0) break;
        }
    }

done:
    if (wave)
    {
        waveOutReset( wave );
        for (i = 0; i < PLAYBACK_CHUNKS; i++)
        {
            if (!buffers[i]) continue;
            waveOutUnprepareHeader( wave, &headers[i], sizeof(*headers) );
            free( buffers[i] );
        }
        waveOutClose( wave );
    }
    CoTaskMemFree( format );
    if (reader) IMFSourceReader_Release( reader );
    if (attributes) IMFAttributes_Release( attributes );
    MFShutdown();
    free( path );

    EnterCriticalSection( &pb->cs );
    pb->playing = FALSE;
    LeaveCriticalSection( &pb->cs );
    TRACE( "playback finished, hr %#lx.\n", hr );
    return 0;
}

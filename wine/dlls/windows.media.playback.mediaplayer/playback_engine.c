/*
 * Playback for Windows.Media.Playback.MediaPlayer.
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
 * Playback for Windows.Media.Playback.MediaPlayer.
 *
 * The file is decoded with a Media Foundation source reader - which gets winegstreamer's decoders,
 * so this handles whatever the installed plugins handle - on its own thread, and the audio goes to
 * waveOut. waveOut rather than WASAPI on purpose: one PCM stream with no mixing policy or session
 * management is a handful of calls there.
 *
 * Playback follows a clock: the position playback started from plus the time since, on the
 * performance counter. A second thread ticks the clock for PositionChanged and notices the end.
 * Pausing stops the threads and keeps the clock's reading; playing again seeks the reader to it.
 */

#define PLAYBACK_CHUNKS 8

struct playback
{
    CRITICAL_SECTION cs;
    HANDLE stop_event;
    HANDLE threads[2];          /* audio, tick */
    LONG decoding;              /* decode threads still running */

    WCHAR *path;
    BOOL playing;
    BOOL has_audio, has_video;

    LONGLONG duration_100ns;
    LONGLONG start_position;    /* clock reading when playback (re)started, or the paused reading */
    LONGLONG start_qpc;
    LONGLONG qpc_frequency;

    double volume;
    BOOL muted;
    BOOL volume_dirty;

    playback_notify_cb notify;
    void *notify_ctx;
};

static DWORD WINAPI audio_thread( void *arg );
static DWORD WINAPI tick_thread( void *arg );

static LONGLONG qpc_now( void )
{
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    return now.QuadPart;
}

static LONGLONG clock_locked( struct playback *pb )
{
    LONGLONG position = pb->start_position;

    if (pb->playing)
        position += (qpc_now() - pb->start_qpc) * 10000000 / pb->qpc_frequency;
    if (pb->duration_100ns && position > pb->duration_100ns) position = pb->duration_100ns;
    return position;
}

static void notify( struct playback *pb, enum playback_event event )
{
    if (pb->notify) pb->notify( pb->notify_ctx, event );
}

/* Stop and join the threads. Called with the lock held; drops it while waiting. A thread that
 * stops playback from inside a notification (an app handler calling Pause) is not waited for: it
 * leaves on its own as soon as it returns to its loop. */
static void playback_stop_locked( struct playback *pb )
{
    HANDLE threads[ARRAY_SIZE(pb->threads)];
    unsigned int i;

    if (pb->playing) pb->start_position = clock_locked( pb );
    pb->playing = FALSE;
    memcpy( threads, pb->threads, sizeof(threads) );
    memset( pb->threads, 0, sizeof(pb->threads) );
    SetEvent( pb->stop_event );

    LeaveCriticalSection( &pb->cs );
    for (i = 0; i < ARRAY_SIZE(threads); i++)
    {
        if (!threads[i]) continue;
        if (GetThreadId( threads[i] ) != GetCurrentThreadId()) WaitForSingleObject( threads[i], 5000 );
        CloseHandle( threads[i] );
    }
    EnterCriticalSection( &pb->cs );
}

HRESULT playback_create( playback_notify_cb notify_cb, void *ctx, struct playback **out )
{
    struct playback *pb;
    LARGE_INTEGER frequency;

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
    QueryPerformanceFrequency( &frequency );
    pb->qpc_frequency = frequency.QuadPart;
    pb->volume = 1.0;
    pb->notify = notify_cb;
    pb->notify_ctx = ctx;
    *out = pb;
    return S_OK;
}

void playback_destroy( struct playback *pb )
{
    if (!pb) return;
    EnterCriticalSection( &pb->cs );
    pb->notify = NULL;
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
 * a URL it understands. ms-appx is resolved to the packaged file here; the others pass through.
 * Setting a path always starts over, even with the same file: the player only does it when it
 * opens a source. */
HRESULT playback_set_path( struct playback *pb, const WCHAR *path )
{
    WCHAR *copy;

    if (!path) return E_INVALIDARG;
    if (!(copy = ms_appx_to_path( path )) && !(copy = wcsdup( path ))) return E_OUTOFMEMORY;

    EnterCriticalSection( &pb->cs );
    playback_stop_locked( pb );
    free( pb->path );
    pb->path = copy;
    pb->start_position = 0;
    pb->duration_100ns = 0;
    pb->has_audio = pb->has_video = FALSE;
    LeaveCriticalSection( &pb->cs );

    TRACE( "source set to %s.\n", debugstr_w(copy) );
    return S_OK;
}

/* ---------------------------------------------------------------- control */

/* Open the file far enough to learn its duration and which streams it has. An app's transport
 * stays disabled until the player says the media is open and how long it is, so this has to happen
 * when the source is set - not when playback starts. */
HRESULT playback_probe( struct playback *pb )
{
    IMFSourceReader *reader = NULL;
    IMFMediaType *type;
    BOOL has_audio = FALSE, has_video = FALSE;
    LONGLONG duration_100ns = 0;
    PROPVARIANT duration;
    WCHAR *path;
    HRESULT hr;

    EnterCriticalSection( &pb->cs );
    path = pb->path ? wcsdup( pb->path ) : NULL;
    if (pb->duration_100ns || pb->has_audio || pb->has_video)
    {
        LeaveCriticalSection( &pb->cs );
        free( path );
        return S_OK;   /* already probed */
    }
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
            duration_100ns = duration.uhVal.QuadPart;
        PropVariantClear( &duration );

        if (SUCCEEDED(IMFSourceReader_GetNativeMediaType( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &type )))
        {
            has_audio = TRUE;
            IMFMediaType_Release( type );
        }
        if (SUCCEEDED(IMFSourceReader_GetNativeMediaType( reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &type )))
        {
            has_video = TRUE;
            IMFMediaType_Release( type );
        }
        IMFSourceReader_Release( reader );
        TRACE( "%s: %I64d x 100 ns, audio %d, video %d.\n", debugstr_w(path), duration_100ns,
               has_audio, has_video );
    }
    else ERR( "cannot open %s, hr %#lx.\n", debugstr_w(path), hr );
    MFShutdown();

    EnterCriticalSection( &pb->cs );
    if (pb->path && !wcscmp( pb->path, path ))
    {
        pb->duration_100ns = duration_100ns;
        pb->has_audio = has_audio;
        pb->has_video = has_video;
    }
    LeaveCriticalSection( &pb->cs );

    free( path );
    return hr;
}

static HRESULT playback_start_locked( struct playback *pb )
{
    HRESULT hr = S_OK;

    if (pb->duration_100ns && pb->start_position >= pb->duration_100ns) pb->start_position = 0;
    if (!pb->has_audio && !pb->has_video) pb->has_audio = TRUE;   /* not probed: try for sound */

    ResetEvent( pb->stop_event );
    pb->playing = TRUE;
    pb->start_qpc = qpc_now();
    pb->decoding = 0;
    if (pb->has_audio && (pb->threads[0] = CreateThread( NULL, 0, audio_thread, pb, 0, NULL )))
        pb->decoding++;
    if (!(pb->threads[1] = CreateThread( NULL, 0, tick_thread, pb, 0, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        playback_stop_locked( pb );
    }
    return hr;
}

HRESULT playback_play( struct playback *pb )
{
    HRESULT hr;

    EnterCriticalSection( &pb->cs );
    if (!pb->path)
    {
        LeaveCriticalSection( &pb->cs );
        WARN( "no source set.\n" );
        return MF_E_NOT_INITIALIZED;
    }
    if (pb->playing)
    {
        LeaveCriticalSection( &pb->cs );
        TRACE( "already playing.\n" );
        return S_OK;
    }
    playback_stop_locked( pb );   /* join the threads of a playback that reached the end */
    hr = playback_start_locked( pb );
    LeaveCriticalSection( &pb->cs );

    if (SUCCEEDED(hr)) notify( pb, PLAYBACK_EVENT_STATE );
    return hr;
}

HRESULT playback_pause( struct playback *pb )
{
    BOOL was_playing;

    EnterCriticalSection( &pb->cs );
    was_playing = pb->playing;
    playback_stop_locked( pb );
    LeaveCriticalSection( &pb->cs );

    if (was_playing) notify( pb, PLAYBACK_EVENT_STATE );
    return S_OK;
}

HRESULT playback_seek( struct playback *pb, LONGLONG position )
{
    HRESULT hr = S_OK;
    BOOL was_playing;

    EnterCriticalSection( &pb->cs );
    was_playing = pb->playing;
    playback_stop_locked( pb );
    if (position < 0) position = 0;
    if (pb->duration_100ns && position > pb->duration_100ns) position = pb->duration_100ns;
    pb->start_position = position;
    if (was_playing) hr = playback_start_locked( pb );
    LeaveCriticalSection( &pb->cs );

    TRACE( "seeked to %I64d.\n", position );
    notify( pb, PLAYBACK_EVENT_SEEKED );
    notify( pb, PLAYBACK_EVENT_POSITION );
    return hr;
}

void playback_set_volume( struct playback *pb, double volume, BOOL muted )
{
    EnterCriticalSection( &pb->cs );
    pb->volume = volume;
    pb->muted = muted;
    pb->volume_dirty = TRUE;
    LeaveCriticalSection( &pb->cs );
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
    position = clock_locked( pb );
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

/* ---------------------------------------------------------------- threads */

/* SetCurrentPosition's time format: GUID_NULL, 100 ns units. GUID_NULL itself lives in libuuid,
 * which this module cannot link - main.c instantiates the GUIDs it uses with initguid.h, and
 * libuuid's copies of the same ones then collide. */
static const GUID no_time_format;

static IMFSourceReader *open_reader( struct playback *pb, LONGLONG *start )
{
    IMFAttributes *attributes = NULL;
    IMFSourceReader *reader = NULL;
    PROPVARIANT position;
    WCHAR *path;
    HRESULT hr;

    EnterCriticalSection( &pb->cs );
    path = wcsdup( pb->path );
    *start = pb->start_position;
    LeaveCriticalSection( &pb->cs );
    if (!path) return NULL;

    /* The reader is only ever read from its own thread, so no multithreaded marshalling is needed. */
    if (SUCCEEDED(MFCreateAttributes( &attributes, 1 )))
        IMFAttributes_SetUINT32( attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE );
    if (FAILED(hr = MFCreateSourceReaderFromURL( path, attributes, &reader )))
        ERR( "cannot open %s, hr %#lx.\n", debugstr_w(path), hr );
    else
    {
        IMFSourceReader_SetStreamSelection( reader, MF_SOURCE_READER_ALL_STREAMS, FALSE );
        IMFSourceReader_SetStreamSelection( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE );
        if (*start)
        {
            position.vt = VT_I8;
            position.hVal.QuadPart = *start;
            if (FAILED(hr = IMFSourceReader_SetCurrentPosition( reader, &no_time_format, &position )))
                WARN( "cannot seek to %I64d, hr %#lx.\n", *start, hr );
        }
    }
    if (attributes) IMFAttributes_Release( attributes );
    free( path );
    return reader;
}

/* Ask the reader for plain 16-bit PCM and let it insert whatever decoder that needs. */
static HRESULT configure_audio( IMFSourceReader *reader, WAVEFORMATEX **format )
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

static void apply_volume( struct playback *pb, HWAVEOUT wave )
{
    DWORD level;

    EnterCriticalSection( &pb->cs );
    level = pb->muted ? 0 : (DWORD)(max( 0.0, min( pb->volume, 1.0 ) ) * 0xffff);
    pb->volume_dirty = FALSE;
    LeaveCriticalSection( &pb->cs );
    waveOutSetVolume( wave, level | (level << 16) );
}

static DWORD WINAPI audio_thread( void *arg )
{
    struct playback *pb = arg;
    IMFSourceReader *reader = NULL;
    WAVEFORMATEX *format = NULL;
    HWAVEOUT wave = NULL;
    WAVEHDR headers[PLAYBACK_CHUNKS] = {0};
    BYTE *buffers[PLAYBACK_CHUNKS] = {0};
    unsigned int next = 0, queued = 0, i;
    LONGLONG start;
    HRESULT hr;

    if (FAILED(hr = MFStartup( MF_VERSION, MFSTARTUP_FULL )))
    {
        ERR( "MFStartup failed, hr %#lx.\n", hr );
        InterlockedDecrement( &pb->decoding );
        return 0;
    }
    if (!(reader = open_reader( pb, &start ))) goto done;
    if (FAILED(hr = configure_audio( reader, &format ))) goto done;
    TRACE( "%u ch, %lu Hz, %u bits, from %I64d.\n", format->nChannels, format->nSamplesPerSec,
           format->wBitsPerSample, start );

    if ((hr = waveOutOpen( &wave, WAVE_MAPPER, format, 0, 0, CALLBACK_NULL )) != MMSYSERR_NOERROR)
    {
        ERR( "waveOutOpen failed, %#lx.\n", hr );
        wave = NULL;
        goto done;
    }
    apply_volume( pb, wave );

    while (WaitForSingleObject( pb->stop_event, 0 ) != WAIT_OBJECT_0)
    {
        IMFSample *sample = NULL;
        IMFMediaBuffer *buffer = NULL;
        DWORD flags = 0, length = 0;
        LONGLONG timestamp = 0;
        BYTE *data;

        if (pb->volume_dirty) apply_volume( pb, wave );
        if (FAILED(hr = IMFSourceReader_ReadSample( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                    0, NULL, &flags, &timestamp, &sample )))
        {
            ERR( "ReadSample failed, hr %#lx.\n", hr );
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;   /* a gap or a format change with no data */
        /* A seek lands on the key frame before the target; drop what precedes the clock. */
        if (timestamp + 400000 < start)
        {
            IMFSample_Release( sample );
            continue;
        }

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
    MFShutdown();
    InterlockedDecrement( &pb->decoding );
    TRACE( "audio finished, hr %#lx.\n", hr );
    return 0;
}

/* Raise PositionChanged a few times a second while playing, and notice the end: the decoder is
 * done and the clock has caught up with the duration. */
static DWORD WINAPI tick_thread( void *arg )
{
    struct playback *pb = arg;

    while (WaitForSingleObject( pb->stop_event, 250 ) != WAIT_OBJECT_0)
    {
        BOOL ended = FALSE;
        LONGLONG position;

        EnterCriticalSection( &pb->cs );
        position = clock_locked( pb );
        if (pb->playing && !pb->decoding && (!pb->duration_100ns || position >= pb->duration_100ns))
        {
            pb->start_position = pb->duration_100ns ? pb->duration_100ns : position;
            pb->playing = FALSE;
            ended = TRUE;
        }
        LeaveCriticalSection( &pb->cs );

        notify( pb, PLAYBACK_EVENT_POSITION );
        if (ended)
        {
            TRACE( "reached the end.\n" );
            notify( pb, PLAYBACK_EVENT_STATE );
            notify( pb, PLAYBACK_EVENT_ENDED );
            break;
        }
    }
    return 0;
}

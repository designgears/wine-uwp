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
#include "d3d11.h"
#include "dxgi1_2.h"

WINE_DEFAULT_DEBUG_CHANNEL(mediaplayer);

/*
 * Playback for Windows.Media.Playback.MediaPlayer.
 *
 * The file is decoded with Media Foundation source readers - which get winegstreamer's decoders,
 * so this handles whatever the installed plugins handle - on one thread per stream: audio goes to
 * waveOut, video is converted to RGB32 and copied into a DXGI composition swap chain that XAML's
 * MediaPlayerElement shows (see video_output below). waveOut rather than WASAPI on purpose: one
 * PCM stream with no mixing policy or session management is a handful of calls there.
 *
 * Both streams follow one clock: the position playback started from plus the time since, on the
 * performance counter. A third thread ticks the clock for PositionChanged and notices the end.
 * Pausing stops the threads and keeps the clock's reading; playing again seeks the readers to it.
 */

#define PLAYBACK_CHUNKS 8

/* The swap chain XAML's MediaPlayerPresenter shows. XAML asks the player for a composition surface
 * handle (IMediaPlayerPrivate::get_VideoSwapchainHandle); on Windows the media engine makes one
 * with DCompositionCreateSurfaceHandle and presents into it through
 * IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle. Wine's dxgi has no
 * IDXGIFactoryMedia, so this makes an ordinary composition swap chain and binds the handle to it
 * with dcomp's private wine_dcomp_bind_surface_handle. */
struct video_output
{
    SRWLOCK lock;               /* the decode thread presents, XAML's thread resizes */
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain1 *swapchain;
    ID3D11Texture2D *buffer;
    HANDLE surface;
    UINT32 frame_width, frame_height;   /* the video's natural size */
    BYTE *frame;                /* the last frame at that size, alpha forced opaque */
    UINT32 width, height;       /* the swap chain's: what XAML shows the video at */
    BYTE *scratch;              /* the frame scaled to that */
    UINT32 letterbox_width, letterbox_height;   /* where in scratch the picture last went */
};

struct playback
{
    CRITICAL_SECTION cs;
    HANDLE stop_event;
    HANDLE threads[3];          /* audio, video, tick */
    LONG decoding;              /* decode threads still running */

    WCHAR *path;
    BOOL playing;
    BOOL has_audio, has_video;
    UINT32 video_width, video_height;
    UINT32 surface_width, surface_height;   /* what XAML shows the video at, once it says */
    struct video_output *video;

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
static DWORD WINAPI video_thread( void *arg );
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

static void video_output_destroy( struct video_output *vo );

void playback_destroy( struct playback *pb )
{
    if (!pb) return;
    EnterCriticalSection( &pb->cs );
    pb->notify = NULL;
    playback_stop_locked( pb );
    LeaveCriticalSection( &pb->cs );
    video_output_destroy( pb->video );
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
 * opens a source. The video output stays - XAML holds a handle to its surface. */
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
    pb->video_width = pb->video_height = 0;
    LeaveCriticalSection( &pb->cs );

    TRACE( "source set to %s.\n", debugstr_w(copy) );
    return S_OK;
}

/* ---------------------------------------------------------------- video output */

static void video_output_destroy( struct video_output *vo )
{
    HRESULT (WINAPI *bind)( HANDLE, IUnknown * );
    HMODULE dcomp;

    if (!vo) return;
    if (vo->surface)
    {
        if ((dcomp = GetModuleHandleW( L"dcomp.dll" )) &&
            (bind = (void *)GetProcAddress( dcomp, "wine_dcomp_bind_surface_handle" )))
            bind( vo->surface, NULL );
        CloseHandle( vo->surface );
    }
    if (vo->buffer) ID3D11Texture2D_Release( vo->buffer );
    if (vo->swapchain) IDXGISwapChain1_Release( vo->swapchain );
    if (vo->context) ID3D11DeviceContext_Release( vo->context );
    if (vo->device) ID3D11Device_Release( vo->device );
    free( vo->scratch );
    free( vo->frame );
    free( vo );
}

/* Shrink with a box filter - every source pixel under an output pixel counts, so the thin grid
 * lines of a CAD viewport survive a 3:1 reduction. dst is dw x dh pixels of a dst_pitch-byte image. */
static void shrink_frame( const BYTE *src, UINT32 sw, UINT32 sh, BYTE *dst, UINT32 dst_pitch, UINT32 dw, UINT32 dh )
{
    UINT32 x, y, i, j, *x0, *x1;

    if (!(x0 = malloc( dw * 2 * sizeof(*x0) ))) return;
    x1 = x0 + dw;
    for (x = 0; x < dw; x++)
    {
        x0[x] = (UINT64)x * sw / dw;
        x1[x] = max( x0[x] + 1, (UINT32)((UINT64)(x + 1) * sw / dw) );
    }
    for (y = 0; y < dh; y++)
    {
        UINT32 y0 = (UINT64)y * sh / dh, y1 = max( y0 + 1, (UINT32)((UINT64)(y + 1) * sh / dh) );
        UINT32 *out = (UINT32 *)(dst + (SIZE_T)y * dst_pitch);

        for (x = 0; x < dw; x++)
        {
            UINT32 b = 0, g = 0, r = 0, n = (y1 - y0) * (x1[x] - x0[x]);
            for (j = y0; j < y1; j++)
            {
                const UINT32 *row = (const UINT32 *)(src + (SIZE_T)j * sw * 4);
                for (i = x0[x]; i < x1[x]; i++)
                {
                    b += row[i] & 0xff;
                    g += (row[i] >> 8) & 0xff;
                    r += (row[i] >> 16) & 0xff;
                }
            }
            out[x] = 0xff000000 | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
    free( x0 );
}

/* Grow bilinearly, in 8-bit fixed point, sampling at pixel centres. */
static void grow_frame( const BYTE *src, UINT32 sw, UINT32 sh, BYTE *dst, UINT32 dst_pitch, UINT32 dw, UINT32 dh )
{
    UINT32 x, y, *xi;
    BYTE *xf;

    if (!(xi = malloc( dw * (sizeof(*xi) + 1) ))) return;
    xf = (BYTE *)(xi + dw);
    for (x = 0; x < dw; x++)
    {
        INT64 fx = ((INT64)(2 * x + 1) * sw * 128) / dw - 128;   /* source x in 1/256 pixels */
        if (fx < 0) fx = 0;
        xi[x] = min( fx >> 8, sw - 1 );
        xf[x] = xi[x] + 1 < sw ? fx & 0xff : 0;
    }
    for (y = 0; y < dh; y++)
    {
        INT64 fy = ((INT64)(2 * y + 1) * sh * 128) / dh - 128;
        UINT32 y0, fry, *out = (UINT32 *)(dst + (SIZE_T)y * dst_pitch);
        const UINT32 *r0, *r1;

        if (fy < 0) fy = 0;
        y0 = min( fy >> 8, sh - 1 );
        fry = y0 + 1 < sh ? fy & 0xff : 0;
        r0 = (const UINT32 *)(src + (SIZE_T)y0 * sw * 4);
        r1 = fry ? r0 + sw : r0;
        for (x = 0; x < dw; x++)
        {
            UINT32 a = r0[xi[x]], b = r0[xi[x] + (xf[x] ? 1 : 0)];
            UINT32 c = r1[xi[x]], d = r1[xi[x] + (xf[x] ? 1 : 0)];
            UINT32 fx = xf[x], v = 0xff000000, shift;

            for (shift = 0; shift < 24; shift += 8)
            {
                UINT32 top = ((a >> shift) & 0xff) * (256 - fx) + ((b >> shift) & 0xff) * fx;
                UINT32 bottom = ((c >> shift) & 0xff) * (256 - fx) + ((d >> shift) & 0xff) * fx;
                v |= ((top * (256 - fry) + bottom * fry) >> 16) << shift;
            }
            out[x] = v;
        }
    }
    free( xi );
}

/* Show the last frame at the swap chain's size. The picture keeps its aspect ratio, centred on
 * black, as the Windows media engine draws it: XAML hands over the size of the whole element -
 * the whole window in full screen - and leaves the letterboxing to the engine. Called with the
 * lock held. */
static void video_output_render_locked( struct video_output *vo )
{
    const BYTE *data = vo->frame;

    if (!vo->buffer) return;   /* a resize failed to get the buffer back */
    if (vo->width != vo->frame_width || vo->height != vo->frame_height)
    {
        UINT32 dw, dh, x0, y0, i;
        BYTE *origin;

        if ((UINT64)vo->width * vo->frame_height <= (UINT64)vo->height * vo->frame_width)
        {
            dw = vo->width;
            dh = max( 1, (UINT32)((UINT64)vo->width * vo->frame_height / vo->frame_width) );
        }
        else
        {
            dh = vo->height;
            dw = max( 1, (UINT32)((UINT64)vo->height * vo->frame_width / vo->frame_height) );
        }
        x0 = (vo->width - dw) / 2;
        y0 = (vo->height - dh) / 2;
        if (dw != vo->letterbox_width || dh != vo->letterbox_height)
        {
            /* the bars only need painting when the picture's place changes */
            for (i = 0; i < vo->width * vo->height; i++) ((UINT32 *)vo->scratch)[i] = 0xff000000;
            vo->letterbox_width = dw;
            vo->letterbox_height = dh;
        }
        origin = vo->scratch + (SIZE_T)y0 * vo->width * 4 + (SIZE_T)x0 * 4;
        if (dw <= vo->frame_width && dh <= vo->frame_height)
            shrink_frame( vo->frame, vo->frame_width, vo->frame_height, origin, vo->width * 4, dw, dh );
        else
            grow_frame( vo->frame, vo->frame_width, vo->frame_height, origin, vo->width * 4, dw, dh );
        data = vo->scratch;
    }
    ID3D11DeviceContext_UpdateSubresource( vo->context, (ID3D11Resource *)vo->buffer, 0, NULL,
                                           data, vo->width * 4, 0 );
    IDXGISwapChain1_Present( vo->swapchain, 0, 0 );
}

/* Take one RGB32 frame (any pitch, negative for bottom-up) and present it. The fourth byte of
 * RGB32 is padding, often zero, and the compositor would treat the frame as transparent, so it is
 * forced opaque on the way. */
static void video_output_present( struct video_output *vo, const BYTE *scan0, LONG pitch,
                                  UINT32 width, UINT32 height )
{
    UINT32 x, y, w, h;

    AcquireSRWLockExclusive( &vo->lock );
    w = min( width, vo->frame_width );
    h = min( height, vo->frame_height );
    for (y = 0; y < h; y++)
    {
        const UINT32 *src = (const UINT32 *)(scan0 + (LONG_PTR)y * pitch);
        UINT32 *dst = (UINT32 *)(vo->frame + (SIZE_T)y * vo->frame_width * 4);
        for (x = 0; x < w; x++) dst[x] = src[x] | 0xff000000;
    }
    video_output_render_locked( vo );
    ReleaseSRWLockExclusive( &vo->lock );
}

/* XAML says how big it shows the video (IMediaPlayer4::SetSurfaceSize). On Windows the media
 * engine then renders at that size; here the swap chain is resized and frames are scaled into it,
 * since the compositor shows a swap chain's pixels one to one. */
static void video_output_resize( struct video_output *vo, UINT32 width, UINT32 height )
{
    BYTE *scratch;
    HRESULT hr;

    width = min( max( width, 1 ), 8192 );
    height = min( max( height, 1 ), 8192 );

    AcquireSRWLockExclusive( &vo->lock );
    if (width == vo->width && height == vo->height) goto done;
    if (!(scratch = realloc( vo->scratch, (SIZE_T)width * height * 4 ))) goto done;
    vo->scratch = scratch;
    vo->letterbox_width = vo->letterbox_height = 0;

    ID3D11Texture2D_Release( vo->buffer );
    vo->buffer = NULL;
    if (FAILED(hr = IDXGISwapChain1_ResizeBuffers( vo->swapchain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0 )))
        ERR( "cannot resize the swap chain to %ux%u, hr %#lx.\n", width, height, hr );
    else
    {
        vo->width = width;
        vo->height = height;
    }
    IDXGISwapChain1_GetBuffer( vo->swapchain, 0, &IID_ID3D11Texture2D, (void **)&vo->buffer );
    TRACE( "video output %p now %ux%u.\n", vo, vo->width, vo->height );
    if (vo->buffer) video_output_render_locked( vo );
done:
    ReleaseSRWLockExclusive( &vo->lock );
}

static HRESULT video_output_create( UINT32 width, UINT32 height, struct video_output **out )
{
    HRESULT (WINAPI *create_handle)( DWORD, SECURITY_ATTRIBUTES *, HANDLE * );
    HRESULT (WINAPI *bind)( HANDLE, IUnknown * );
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    IDXGIFactory2 *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgi = NULL;
    struct video_output *vo;
    HMODULE dcomp;
    UINT32 i;
    HRESULT hr;

    if (!(vo = calloc( 1, sizeof(*vo) ))) return E_OUTOFMEMORY;
    vo->width = vo->frame_width = width;
    vo->height = vo->frame_height = height;
    if (!(vo->scratch = malloc( (SIZE_T)width * height * 4 )) || !(vo->frame = malloc( (SIZE_T)width * height * 4 )))
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }
    for (i = 0; i < width * height; i++) ((UINT32 *)vo->frame)[i] = 0xff000000;

    if (FAILED(hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       NULL, 0, D3D11_SDK_VERSION, &vo->device, NULL, &vo->context )))
        goto fail;
    if (FAILED(hr = ID3D11Device_QueryInterface( vo->device, &IID_IDXGIDevice, (void **)&dxgi ))) goto fail;
    if (FAILED(hr = IDXGIDevice_GetAdapter( dxgi, &adapter ))) goto fail;
    if (FAILED(hr = IDXGIAdapter_GetParent( adapter, &IID_IDXGIFactory2, (void **)&factory ))) goto fail;

    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(hr = IDXGIFactory2_CreateSwapChainForComposition( factory, (IUnknown *)vo->device, &desc,
                                                                 NULL, &vo->swapchain )))
    {
        ERR( "cannot create a %ux%u composition swap chain, hr %#lx.\n", width, height, hr );
        goto fail;
    }
    if (FAILED(hr = IDXGISwapChain1_GetBuffer( vo->swapchain, 0, &IID_ID3D11Texture2D, (void **)&vo->buffer )))
        goto fail;

    if (!(dcomp = LoadLibraryW( L"dcomp.dll" )) ||
        !(create_handle = (void *)GetProcAddress( dcomp, "DCompositionCreateSurfaceHandle" )) ||
        !(bind = (void *)GetProcAddress( dcomp, "wine_dcomp_bind_surface_handle" )))
    {
        ERR( "dcomp cannot bind surface handles; video will not be shown.\n" );
    }
    else if (SUCCEEDED(hr = create_handle( 0x3 /* COMPOSITIONOBJECT_ALL_ACCESS */, NULL, &vo->surface )) &&
             FAILED(hr = bind( vo->surface, (IUnknown *)vo->swapchain )))
    {
        CloseHandle( vo->surface );
        vo->surface = NULL;
    }

    /* Black until the first frame, so the element shows a video area rather than nothing. */
    AcquireSRWLockExclusive( &vo->lock );
    video_output_render_locked( vo );
    ReleaseSRWLockExclusive( &vo->lock );
    TRACE( "%ux%u video output %p, surface handle %p.\n", width, height, vo, vo->surface );

    IDXGIFactory2_Release( factory );
    IDXGIAdapter_Release( adapter );
    IDXGIDevice_Release( dxgi );
    *out = vo;
    return S_OK;

fail:
    if (factory) IDXGIFactory2_Release( factory );
    if (adapter) IDXGIAdapter_Release( adapter );
    if (dxgi) IDXGIDevice_Release( dxgi );
    video_output_destroy( vo );
    return hr;
}

/* A new video for the same output: hold frames of its size, black until the first one. Until XAML
 * says how big it shows the video, the swap chain follows the frame. */
static void video_output_set_frame_size( struct video_output *vo, UINT32 width, UINT32 height )
{
    BOOL follow;
    BYTE *frame;
    UINT32 i;

    AcquireSRWLockExclusive( &vo->lock );
    if ((width != vo->frame_width || height != vo->frame_height) &&
        (frame = realloc( vo->frame, (SIZE_T)width * height * 4 )))
    {
        follow = vo->width == vo->frame_width && vo->height == vo->frame_height;
        vo->frame = frame;
        vo->frame_width = width;
        vo->frame_height = height;
        vo->letterbox_width = vo->letterbox_height = 0;
        ReleaseSRWLockExclusive( &vo->lock );
        if (follow) video_output_resize( vo, width, height );
        AcquireSRWLockExclusive( &vo->lock );
    }
    for (i = 0; i < vo->frame_width * vo->frame_height; i++) ((UINT32 *)vo->frame)[i] = 0xff000000;
    video_output_render_locked( vo );
    ReleaseSRWLockExclusive( &vo->lock );
}

/* What get_VideoSwapchainHandle hands XAML, which closes it when it is done: a duplicate. */
HANDLE playback_video_surface( struct playback *pb )
{
    HANDLE handle = NULL;

    EnterCriticalSection( &pb->cs );
    if (pb->video && pb->video->surface)
        DuplicateHandle( GetCurrentProcess(), pb->video->surface, GetCurrentProcess(), &handle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS );
    LeaveCriticalSection( &pb->cs );
    return handle;
}

void playback_set_surface_size( struct playback *pb, UINT32 width, UINT32 height )
{
    struct video_output *video;

    EnterCriticalSection( &pb->cs );
    pb->surface_width = width;
    pb->surface_height = height;
    video = pb->video;
    LeaveCriticalSection( &pb->cs );
    /* The output lives as long as the playback, which outlives the player's callers. */
    if (video && width && height) video_output_resize( video, width, height );
}

void playback_video_size( struct playback *pb, UINT32 *width, UINT32 *height )
{
    EnterCriticalSection( &pb->cs );
    *width = pb->video_width;
    *height = pb->video_height;
    LeaveCriticalSection( &pb->cs );
}

/* ---------------------------------------------------------------- control */

/* Open the file far enough to learn its duration, which streams it has and how big the picture
 * is, and make the video output. An app's transport stays disabled until the player says the media
 * is open and how long it is, and XAML shows no video until the session reports a natural size, so
 * this has to happen when the source is set - not when playback starts. */
HRESULT playback_probe( struct playback *pb )
{
    struct video_output *video = NULL;
    IMFSourceReader *reader = NULL;
    IMFMediaType *type;
    UINT32 width = 0, height = 0;
    BOOL has_audio = FALSE, has_video = FALSE, created = FALSE;
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
            MFVideoArea area;
            UINT64 size;

            has_video = TRUE;
            if (SUCCEEDED(IMFMediaType_GetUINT64( type, &MF_MT_FRAME_SIZE, &size )))
            {
                width = size >> 32;
                height = (UINT32)size;
            }
            /* A coded size is padded (1088 for 1080p); the aperture is the picture. */
            if (SUCCEEDED(IMFMediaType_GetBlob( type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area,
                                                sizeof(area), NULL )) && area.Area.cx && area.Area.cy)
            {
                width = area.Area.cx;
                height = area.Area.cy;
            }
            IMFMediaType_Release( type );
        }
        IMFSourceReader_Release( reader );
        TRACE( "%s: %I64d x 100 ns, audio %d, video %d %ux%u.\n", debugstr_w(path), duration_100ns,
               has_audio, has_video, width, height );
    }
    else ERR( "cannot open %s, hr %#lx.\n", debugstr_w(path), hr );
    MFShutdown();

    EnterCriticalSection( &pb->cs );
    video = pb->video;
    LeaveCriticalSection( &pb->cs );
    /* One output per player: a later video only changes the frame it holds. Probing is serialised
     * by the player, and no decode thread runs while a new file is being probed. */
    if (has_video && width && height)
    {
        if (video) video_output_set_frame_size( video, width, height );
        else if (FAILED(video_output_create( width, height, &video ))) video = NULL;
        else created = TRUE;
    }

    EnterCriticalSection( &pb->cs );
    if (pb->path && !wcscmp( pb->path, path ))
    {
        pb->duration_100ns = duration_100ns;
        pb->has_audio = has_audio;
        pb->has_video = has_video;
        pb->video_width = width;
        pb->video_height = height;
        if (created)
        {
            pb->video = video;
            created = FALSE;
            if (pb->surface_width && pb->surface_height)
                video_output_resize( pb->video, pb->surface_width, pb->surface_height );
        }
    }
    LeaveCriticalSection( &pb->cs );

    if (created) video_output_destroy( video );   /* the path changed while probing */
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
    if (pb->has_video && (pb->threads[1] = CreateThread( NULL, 0, video_thread, pb, 0, NULL )))
        pb->decoding++;
    if (!(pb->threads[2] = CreateThread( NULL, 0, tick_thread, pb, 0, NULL )))
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

static IMFSourceReader *open_reader( struct playback *pb, BOOL video, LONGLONG *start )
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

    /* The reader is only ever read from its own thread, so no multithreaded marshalling is needed.
     * Video processing lets the reader convert the decoder's YUV into the RGB32 we copy out. */
    if (SUCCEEDED(MFCreateAttributes( &attributes, 1 )))
        IMFAttributes_SetUINT32( attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, video );
    if (FAILED(hr = MFCreateSourceReaderFromURL( path, attributes, &reader )))
        ERR( "cannot open %s, hr %#lx.\n", debugstr_w(path), hr );
    else
    {
        IMFSourceReader_SetStreamSelection( reader, MF_SOURCE_READER_ALL_STREAMS, FALSE );
        IMFSourceReader_SetStreamSelection( reader, video ? MF_SOURCE_READER_FIRST_VIDEO_STREAM
                                                          : MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE );
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

/* Sleep until the clock reaches the timestamp. FALSE if playback was stopped meanwhile. */
static BOOL wait_for_clock( struct playback *pb, LONGLONG timestamp )
{
    for (;;)
    {
        LONGLONG ahead = timestamp - playback_position( pb );
        DWORD ms;

        if (ahead <= 0) return TRUE;
        ms = min( ahead / 10000 + 1, 50 );
        if (WaitForSingleObject( pb->stop_event, ms ) == WAIT_OBJECT_0) return FALSE;
    }
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
    if (!(reader = open_reader( pb, FALSE, &start ))) goto done;
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

static HRESULT configure_video( IMFSourceReader *reader, UINT32 *width, UINT32 *height, LONG *stride )
{
    static const GUID *subtypes[] = { &MFVideoFormat_RGB32, &MFVideoFormat_ARGB32 };
    IMFMediaType *type, *current;
    unsigned int i;
    UINT64 size;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(subtypes); i++)
    {
        if (FAILED(hr = MFCreateMediaType( &type ))) return hr;
        IMFMediaType_SetGUID( type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video );
        IMFMediaType_SetGUID( type, &MF_MT_SUBTYPE, subtypes[i] );
        hr = IMFSourceReader_SetCurrentMediaType( reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type );
        IMFMediaType_Release( type );
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr))
    {
        ERR( "the reader cannot produce RGB32 for this file: %#lx.\n", hr );
        return hr;
    }

    if (FAILED(hr = IMFSourceReader_GetCurrentMediaType( reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current )))
        return hr;
    if (SUCCEEDED(hr = IMFMediaType_GetUINT64( current, &MF_MT_FRAME_SIZE, &size )))
    {
        *width = size >> 32;
        *height = (UINT32)size;
        /* The reader reported 1440 for a 1440-pixel-wide RGB32 frame: the width, not the row's
         * 5760 bytes. Anything shorter than 4 bytes a pixel cannot be right. Not yet traced to
         * its source in winegstreamer. */
        if (FAILED(IMFMediaType_GetUINT32( current, &MF_MT_DEFAULT_STRIDE, (UINT32 *)stride )) ||
            abs( *stride ) < *width * 4)
            *stride = *stride < 0 ? -(LONG)(*width * 4) : (LONG)(*width * 4);
    }
    IMFMediaType_Release( current );
    return hr;
}

static DWORD WINAPI video_thread( void *arg )
{
    struct playback *pb = arg;
    IMFSourceReader *reader = NULL;
    struct video_output *vo;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    BOOL first = TRUE;
    LONGLONG start;
    HRESULT hr;

    EnterCriticalSection( &pb->cs );
    vo = pb->video;   /* stays for the playback's lifetime */
    LeaveCriticalSection( &pb->cs );

    if (!vo || FAILED(hr = MFStartup( MF_VERSION, MFSTARTUP_FULL )))
    {
        InterlockedDecrement( &pb->decoding );
        return 0;
    }
    if (!(reader = open_reader( pb, TRUE, &start ))) goto done;
    if (FAILED(hr = configure_video( reader, &width, &height, &stride ))) goto done;
    TRACE( "%ux%u RGB32, stride %ld, from %I64d.\n", width, height, stride, start );

    while (WaitForSingleObject( pb->stop_event, 0 ) != WAIT_OBJECT_0)
    {
        IMFSample *sample = NULL;
        IMFMediaBuffer *buffer = NULL;
        IMF2DBuffer *buffer2d;
        LONGLONG timestamp = 0;
        DWORD flags = 0, length;
        BYTE *data, *scan0;
        LONG pitch;

        if (FAILED(hr = IMFSourceReader_ReadSample( reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                    0, NULL, &flags, &timestamp, &sample )))
        {
            ERR( "ReadSample failed, hr %#lx.\n", hr );
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            configure_video( reader, &width, &height, &stride );
        if (!sample) continue;

        /* Frames before a seek target are decoded only to get there. A frame that is already far
         * behind the clock is dropped too, so slow decoding loses frames rather than sync - but
         * the first one is always shown. */
        if (timestamp + 400000 < start ||
            (!first && playback_position( pb ) - timestamp > 2000000))
        {
            IMFSample_Release( sample );
            continue;
        }
        if (!wait_for_clock( pb, timestamp ))
        {
            IMFSample_Release( sample );
            break;
        }

        if (SUCCEEDED(IMFSample_GetBufferByIndex( sample, 0, &buffer )))
        {
            if (SUCCEEDED(IMFMediaBuffer_QueryInterface( buffer, &IID_IMF2DBuffer, (void **)&buffer2d )))
            {
                if (SUCCEEDED(IMF2DBuffer_Lock2D( buffer2d, &scan0, &pitch )))
                {
                    if (first) TRACE( "first frame at %I64d, 2D pitch %ld.\n", timestamp, pitch );
                    if (abs( pitch ) < width * 4) pitch = pitch < 0 ? -(LONG)(width * 4) : (LONG)(width * 4);
                    video_output_present( vo, scan0, pitch, width, height );
                    IMF2DBuffer_Unlock2D( buffer2d );
                }
                IMF2DBuffer_Release( buffer2d );
            }
            else if (SUCCEEDED(IMFMediaBuffer_Lock( buffer, &data, NULL, &length )))
            {
                /* A negative stride is a bottom-up image: the first row is the last in memory. */
                scan0 = stride < 0 ? data + (SIZE_T)(height - 1) * -stride : data;
                if (length >= (SIZE_T)abs( stride ) * height)
                    video_output_present( vo, scan0, stride, width, height );
                IMFMediaBuffer_Unlock( buffer );
            }
            IMFMediaBuffer_Release( buffer );
            first = FALSE;
        }
        IMFSample_Release( sample );
    }

done:
    if (reader) IMFSourceReader_Release( reader );
    MFShutdown();
    InterlockedDecrement( &pb->decoding );
    TRACE( "video finished, hr %#lx.\n", hr );
    return 0;
}

/* Raise PositionChanged a few times a second while playing, and notice the end: both decoders
 * are done and the clock has caught up with the duration. */
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

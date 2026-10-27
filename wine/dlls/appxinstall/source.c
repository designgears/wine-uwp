/*
 * appxinstall: a package's bytes, from a file or an http(s) URL
 *
 * A URL is read with range requests: a zip's directory and a few members cost a few requests and a sequential read
 * streams the whole thing, with nothing written to disk. Reads go through 8 MiB blocks. A sequential read keeps the
 * next PREFETCH blocks in flight, each its own range request on a pool thread: one request at a time, started only
 * when the unpacker asked for it, got ~35 MB/s out of a CDN that serves a parallel client several times that.
 *
 * Copyright 2026 wine-uwp contributors
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

#include <stdlib.h>
#include <string.h>

#include "appxinstall_private.h"
#include "winhttp.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxinstall);

#define BLOCK (8u << 20)
#define PREFETCH 8

/* one block of a URL: fetched by a pool thread, kept until another block takes its place (direct-mapped) */
struct block
{
    struct source *source;
    UINT64 index;                   /* which block; ~0 when empty */
    BYTE *data;
    ULONG size;
    HRESULT hr;
    BOOL busy;                      /* a fetch is running */
    HANDLE done;                    /* set while not busy */
};

struct source
{
    const struct appx_install_params *params;
    UINT64 size;
    /* a file */
    HANDLE file;
    /* a URL */
    HINTERNET session, connection;
    WCHAR *host, *path;
    INTERNET_PORT port;
    BOOL secure;
    struct block blocks[PREFETCH];
    UINT64 last;                    /* the block read last, to tell a sequential read */
    /* a window onto another source */
    struct source *parent;
    UINT64 base;
};

BOOL is_url( const WCHAR *path )
{
    return !wcsnicmp( path, L"http://", 7 ) || !wcsnicmp( path, L"https://", 8 );
}

static BOOL connection_failure( DWORD error )
{
    switch (error)
    {
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_TIMEOUT:
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        return TRUE;
    }
    return FALSE;
}

/* sleeps ms, waking early for a cancel; FALSE if cancelled */
static BOOL backoff( const struct appx_install_params *params, DWORD ms )
{
    for (; ms; ms -= min( ms, 100 ))
    {
        if (params->cancel && *params->cancel) return FALSE;
        Sleep( min( ms, 100 ) );
    }
    return !(params->cancel && *params->cancel);
}

/* Sends method on the source's URL with an optional byte range. A connection that fails is retried for about two
 * minutes (2..32 s apart): a network that drops for a moment (Wi-Fi, a VPN coming up) used to fail whole installs
 * at the first request. An HTTP status is an answer, not an outage, and is returned to the caller. */
static HRESULT http_send( struct source *source, const WCHAR *method, UINT64 start, UINT64 end, HINTERNET *out,
                          DWORD *status )
{
    WCHAR range[64];
    DWORD delay = 2000, size;
    HINTERNET request;

    for (;;)
    {
        DWORD error = 0;

        request = WinHttpOpenRequest( source->connection, method, source->path, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, source->secure ? WINHTTP_FLAG_SECURE : 0 );
        if (!request) return HRESULT_FROM_WIN32( GetLastError() );
        WinHttpSetTimeouts( request, 60000, 60000, 60000, 60000 );
        if (end) swprintf( range, ARRAY_SIZE(range), L"Range: bytes=%I64u-%I64u\r\n", start, end - 1 );
        if (WinHttpSendRequest( request, end ? range : WINHTTP_NO_ADDITIONAL_HEADERS, -1, NULL, 0, 0, 0 ) &&
            WinHttpReceiveResponse( request, NULL ))
        {
            size = sizeof(*status);
            WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX );
            *out = request;
            return S_OK;
        }
        error = GetLastError();
        WinHttpCloseHandle( request );
        if (!connection_failure( error ) || delay > 32000) return HRESULT_FROM_WIN32( error );
        report( source->params, APPX_EVENT_MESSAGE, 0, 0, L"network: error %lu; retrying in %lu s", error, delay / 1000 );
        if (!backoff( source->params, delay )) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        delay *= 2;
    }
}

static HRESULT http_open( struct source *source, const WCHAR *url )
{
    URL_COMPONENTS uc = { sizeof(uc) };
    WCHAR header[64];
    HINTERNET request;
    DWORD status = 0, size;
    HRESULT hr;

    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = ~0u;
    if (!WinHttpCrackUrl( url, 0, 0, &uc )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!(source->host = calloc( uc.dwHostNameLength + 1, sizeof(WCHAR) )) ||
        !(source->path = calloc( uc.dwUrlPathLength + uc.dwExtraInfoLength + 1, sizeof(WCHAR) )))
        return E_OUTOFMEMORY;
    memcpy( source->host, uc.lpszHostName, uc.dwHostNameLength * sizeof(WCHAR) );
    memcpy( source->path, uc.lpszUrlPath, (uc.dwUrlPathLength + uc.dwExtraInfoLength) * sizeof(WCHAR) );
    source->port = uc.nPort;
    source->secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    if (!(source->session = WinHttpOpen( L"wine-uwp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0 )) ||
        !(source->connection = WinHttpConnect( source->session, source->host, source->port, 0 )))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (FAILED(hr = http_send( source, L"HEAD", 0, 0, &request, &status ))) return hr;
    if (status != 200)
    {
        WinHttpCloseHandle( request );
        ERR( "%s: HTTP %lu\n", debugstr_w(url), status );
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    }
    size = sizeof(header);
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_ACCEPT_RANGES, WINHTTP_HEADER_NAME_BY_INDEX, header, &size,
                              WINHTTP_NO_HEADER_INDEX ) || wcsicmp( header, L"bytes" ))
    {
        WinHttpCloseHandle( request );
        ERR( "%s: the server does not support partial downloads\n", debugstr_w(url) );
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    }
    size = sizeof(header);
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, header, &size,
                              WINHTTP_NO_HEADER_INDEX ))
    {
        WinHttpCloseHandle( request );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    WinHttpCloseHandle( request );
    source->size = _wcstoui64( header, NULL, 10 );
    return S_OK;
}

/* reads [start, end) from the server in 256 KiB pieces, retrying a broken transfer five times; between pieces it
 * waits out a pause, and resumes with a new range request at the next byte (an open response left idle while
 * paused times out at the CDN) */
static HRESULT http_fetch( struct source *source, UINT64 start, UINT64 end, BYTE *out )
{
    UINT64 done = 0, want = end - start;
    unsigned int attempts = 0;
    HINTERNET request;
    DWORD status, got;
    HRESULT hr;

    while (done < want)
    {
        BOOL broken = FALSE;

        if (!wait_while_paused( source->params )) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        if (FAILED(hr = http_send( source, L"GET", start + done, end, &request, &status ))) return hr;
        if (status != 206 && !(status == 200 && !start && end == source->size))
        {
            WinHttpCloseHandle( request );
            ERR( "range %I64u-%I64u: HTTP %lu\n", start + done, end, status );
            return HRESULT_FROM_WIN32( ERROR_WINHTTP_INVALID_SERVER_RESPONSE );
        }
        while (done < want)
        {
            if (source->params->pause && *source->params->pause) break;
            if (source->params->cancel && *source->params->cancel) break;
            if (!WinHttpReadData( request, out + done, min( want - done, 256u << 10 ), &got )) { broken = TRUE; break; }
            if (!got) { broken = TRUE; break; }
            done += got;
        }
        WinHttpCloseHandle( request );
        if (source->params->cancel && *source->params->cancel) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        if (broken && done < want)
        {
            if (++attempts == 5) return HRESULT_FROM_WIN32( ERROR_WINHTTP_CONNECTION_ERROR );
            if (!backoff( source->params, 2000 * attempts )) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        }
    }
    return S_OK;
}

HRESULT source_open( const WCHAR *path, const struct appx_install_params *params, struct source **out )
{
    struct source *source;
    unsigned int i;
    LARGE_INTEGER size;
    HRESULT hr;

    if (!(source = calloc( 1, sizeof(*source) ))) return E_OUTOFMEMORY;
    source->params = params;
    source->file = INVALID_HANDLE_VALUE;
    source->last = ~(UINT64)1;
    for (i = 0; i < PREFETCH; i++) source->blocks[i].index = ~(UINT64)0;
    if (is_url( path ))
    {
        report( params, APPX_EVENT_MESSAGE, 0, 0, L"downloading %s", path );
        if (FAILED(hr = http_open( source, path )))
        {
            source_close( source );
            return hr;
        }
    }
    else
    {
        source->file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
        if (source->file == INVALID_HANDLE_VALUE || !GetFileSizeEx( source->file, &size ))
        {
            hr = HRESULT_FROM_WIN32( GetLastError() );
            source_close( source );
            return hr;
        }
        source->size = size.QuadPart;
    }
    *out = source;
    return S_OK;
}

HRESULT source_open_range( struct source *parent, UINT64 offset, UINT64 size, struct source **out )
{
    struct source *source;

    if (offset > parent->size || size > parent->size - offset) return HRESULT_FROM_WIN32( ERROR_HANDLE_EOF );
    if (!(source = calloc( 1, sizeof(*source) ))) return E_OUTOFMEMORY;
    source->params = parent->params;
    source->file = INVALID_HANDLE_VALUE;
    source->parent = parent;
    source->base = offset;
    source->size = size;
    *out = source;
    return S_OK;
}

UINT64 source_size( const struct source *source )
{
    return source->size;
}

static void CALLBACK block_fetch( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct block *block = context;
    struct source *source = block->source;
    UINT64 start = block->index * BLOCK, end = min( start + BLOCK, source->size );

    block->size = end - start;
    block->hr = http_fetch( source, start, end, block->data );
    block->busy = FALSE;
    SetEvent( block->done );
}

/* starts fetching block index into its slot, unless it is there or another fetch holds the slot */
static HRESULT block_start( struct source *source, UINT64 index, BOOL wait )
{
    struct block *block = &source->blocks[index % PREFETCH];

    if (index * BLOCK >= source->size) return S_OK;
    if (block->index == index && (block->busy || SUCCEEDED(block->hr))) return S_OK;
    if (block->busy)
    {
        if (!wait) return S_OK;
        WaitForSingleObject( block->done, INFINITE );
    }
    if (!block->data && !(block->data = malloc( BLOCK ))) return E_OUTOFMEMORY;
    if (!block->done && !(block->done = CreateEventW( NULL, TRUE, TRUE, NULL ))) return HRESULT_FROM_WIN32( GetLastError() );
    block->source = source;
    block->index = index;
    block->hr = E_PENDING;
    block->busy = TRUE;
    ResetEvent( block->done );
    if (!TrySubmitThreadpoolCallback( block_fetch, block, NULL ))
    {
        block_fetch( NULL, block );
        return S_OK;
    }
    return S_OK;
}

/* block index, fetched; a sequential read also starts the ones after it */
static HRESULT block_get( struct source *source, UINT64 index, struct block **out )
{
    struct block *block = &source->blocks[index % PREFETCH];
    BOOL sequential = index == source->last || index == source->last + 1;
    HRESULT hr;
    UINT64 i;

    if (FAILED(hr = block_start( source, index, TRUE ))) return hr;
    if (sequential)
        for (i = 1; i < PREFETCH; i++) block_start( source, index + i, FALSE );
    source->last = index;
    WaitForSingleObject( block->done, INFINITE );
    if (FAILED(hr = block->hr))
    {
        block->index = ~(UINT64)0;
        return hr;
    }
    *out = block;
    return S_OK;
}

HRESULT source_read( struct source *source, UINT64 offset, void *buffer, ULONG size )
{
    BYTE *out = buffer;
    HRESULT hr;

    if (offset > source->size || size > source->size - offset) return HRESULT_FROM_WIN32( ERROR_HANDLE_EOF );
    if (source->parent) return source_read( source->parent, source->base + offset, buffer, size );

    if (source->file != INVALID_HANDLE_VALUE)
    {
        OVERLAPPED ov = { 0 };
        DWORD got;

        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        if (!ReadFile( source->file, buffer, size, &got, &ov )) return HRESULT_FROM_WIN32( GetLastError() );
        return got == size ? S_OK : HRESULT_FROM_WIN32( ERROR_HANDLE_EOF );
    }

    while (size)
    {
        UINT64 index = offset / BLOCK;
        struct block *block;
        ULONG n;

        if (FAILED(hr = block_get( source, index, &block ))) return hr;
        n = min( size, block->size - (offset - index * BLOCK) );
        memcpy( out, block->data + (offset - index * BLOCK), n );
        out += n;
        offset += n;
        size -= n;
    }
    return S_OK;
}

void source_close( struct source *source )
{
    unsigned int i;

    if (!source) return;
    for (i = 0; i < PREFETCH; i++)   /* a prefetch still running reads the connection and its buffer */
        if (source->blocks[i].done)
        {
            WaitForSingleObject( source->blocks[i].done, INFINITE );
            CloseHandle( source->blocks[i].done );
        }
    for (i = 0; i < PREFETCH; i++) free( source->blocks[i].data );
    if (source->file != INVALID_HANDLE_VALUE) CloseHandle( source->file );
    if (source->connection) WinHttpCloseHandle( source->connection );
    if (source->session) WinHttpCloseHandle( source->session );
    free( source->host );
    free( source->path );
    free( source );
}

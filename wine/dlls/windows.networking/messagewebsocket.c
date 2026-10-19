/* WinRT Windows.Networking.Sockets.MessageWebSocket Implementation
 *
 * Minimal client WebSocket (ws/wss): ConnectAsync, SetRequestHeader,
 * OutputStream writes, MessageReceived / Closed.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define WIDL_using_Wine_Internal
#include "private.h"

#include "winsock2.h"
#include "ws2tcpip.h"
#define SECURITY_WIN32
#include "sspi.h"
#include "schannel.h"
#include "robuffer.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsock);

#define MAX_WS_HEADERS 32
#define MAX_WS_HANDLERS 8
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xa

static const GUID IID_IDataReader_local = {0xe2b50029, 0xb4c1, 0x4314, {0xa4,0xb8, 0xfb,0x81,0x3a,0x2f,0x27,0x5e}};

struct ws_header { HSTRING name; HSTRING value; };

struct message_websocket
{
    IMessageWebSocket IMessageWebSocket_iface;
    IWebSocket IWebSocket_iface;
    IClosable IClosable_iface;
    IMessageWebSocketControl IMessageWebSocketControl_iface;
    IWebSocketControl IWebSocketControl_iface;
    IWebSocketInformation IWebSocketInformation_iface;
    IOutputStream IOutputStream_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    CRITICAL_SECTION write_cs;

    SOCKET sock;
    BOOL connected, secure, closing;

    CredHandle cred;
    CtxtHandle ctx;
    BOOL have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;

    BYTE *enc_buf;
    SIZE_T enc_size, enc_len;
    BYTE *dec_buf;
    SIZE_T dec_size, dec_len, dec_off;

    struct ws_header headers[MAX_WS_HEADERS];
    UINT header_count;

    SocketMessageType message_type;
    UINT32 max_message_size;
    UINT32 outbound_buffer_size;

    ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs *closed_handlers[MAX_WS_HANDLERS];
    ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs *msg_handlers[MAX_WS_HANDLERS];
    INT64 next_token;

    HANDLE recv_thread;
    UINT16 close_code;
    HSTRING close_reason;
    HSTRING protocol;
};

struct ws_op
{
    IUnknown IUnknown_iface;
    LONG ref;
    IUriRuntimeClass *uri;
    IBuffer *buffer;
};

struct msg_args
{
    IMessageWebSocketMessageReceivedEventArgs IMessageWebSocketMessageReceivedEventArgs_iface;
    LONG ref;
    SocketMessageType type;
    BYTE *data;
    UINT32 size;
};

struct closed_args
{
    IWebSocketClosedEventArgs IWebSocketClosedEventArgs_iface;
    LONG ref;
    UINT16 code;
    HSTRING reason;
};

struct mem_stream
{
    IInputStream IInputStream_iface;
    LONG ref;
    BYTE *data;
    UINT32 size, pos;
};

struct data_reader
{
    IInspectable IInspectable_iface;
    LONG ref;
    BYTE *data;
    UINT32 size, pos;
    UINT32 unicode_encoding;
};

static inline struct message_websocket *impl_from_IMessageWebSocket( IMessageWebSocket *iface )
{
    return CONTAINING_RECORD( iface, struct message_websocket, IMessageWebSocket_iface );
}
static inline struct message_websocket *impl_from_IWebSocket( IWebSocket *iface )
{
    return CONTAINING_RECORD( iface, struct message_websocket, IWebSocket_iface );
}
static inline struct ws_op *impl_from_ws_op( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct ws_op, IUnknown_iface );
}

static HRESULT hresult_from_wsa( int err )
{
    return HRESULT_FROM_WIN32( err ? err : WSAGetLastError() );
}

static HRESULT sock_send_all( struct message_websocket *impl, const BYTE *buf, SIZE_T len )
{
    while (len)
    {
        int ret = send( impl->sock, (const char *)buf, (int)min( len, 0x10000 ), 0 );
        if (ret <= 0) return hresult_from_wsa( 0 );
        buf += ret;
        len -= ret;
    }
    return S_OK;
}

static HRESULT sock_fill( struct message_websocket *impl )
{
    int ret;
    if (impl->enc_len + 4096 > impl->enc_size)
    {
        SIZE_T size = impl->enc_size ? impl->enc_size * 2 : 16384;
        BYTE *buf = realloc( impl->enc_buf, size );
        if (!buf) return E_OUTOFMEMORY;
        impl->enc_buf = buf;
        impl->enc_size = size;
    }
    ret = recv( impl->sock, (char *)impl->enc_buf + impl->enc_len, (int)(impl->enc_size - impl->enc_len), 0 );
    if (!ret) return S_FALSE;
    if (ret < 0) return hresult_from_wsa( 0 );
    impl->enc_len += ret;
    return S_OK;
}

static HRESULT dec_append( struct message_websocket *impl, const BYTE *data, SIZE_T len )
{
    if (impl->dec_off)
    {
        memmove( impl->dec_buf, impl->dec_buf + impl->dec_off, impl->dec_len );
        impl->dec_off = 0;
    }
    if (impl->dec_len + len > impl->dec_size)
    {
        SIZE_T size = max( impl->dec_len + len, (SIZE_T)16384 );
        BYTE *buf = realloc( impl->dec_buf, size );
        if (!buf) return E_OUTOFMEMORY;
        impl->dec_buf = buf;
        impl->dec_size = size;
    }
    memcpy( impl->dec_buf + impl->dec_len, data, len );
    impl->dec_len += len;
    return S_OK;
}

static HRESULT tls_handshake( struct message_websocket *impl, const WCHAR *hostname )
{
    static const DWORD isc_req_flags = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_USE_SESSION_KEY | ISC_REQ_CONFIDENTIALITY |
                                       ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_STREAM |
                                       ISC_REQ_MANUAL_CRED_VALIDATION;
    SecBuffer out_buf = {0, SECBUFFER_TOKEN, NULL}, in_bufs[2] = {{0, SECBUFFER_TOKEN}, {0, SECBUFFER_EMPTY}};
    SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, &out_buf}, in_desc = {SECBUFFER_VERSION, 2, in_bufs};
    SCHANNEL_CRED cred = {.dwVersion = SCHANNEL_CRED_VERSION, .dwFlags = SCH_CRED_NO_DEFAULT_CREDS |
                                                                         SCH_CRED_MANUAL_CRED_VALIDATION};
    SECURITY_STATUS status;
    CtxtHandle ctx = {0};
    HRESULT hr;

    status = AcquireCredentialsHandleW( NULL, (WCHAR *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &cred,
                                        NULL, NULL, &impl->cred, NULL );
    if (status != SEC_E_OK) return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
    impl->have_cred = TRUE;

    status = InitializeSecurityContextW( &impl->cred, NULL, (WCHAR *)hostname, isc_req_flags, 0, 0, NULL, 0,
                                         &ctx, &out_desc, &(ULONG){0}, NULL );
    while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE)
    {
        if (out_buf.cbBuffer)
        {
            if (FAILED(hr = sock_send_all( impl, out_buf.pvBuffer, out_buf.cbBuffer )))
            {
                DeleteSecurityContext( &ctx );
                return hr;
            }
            FreeContextBuffer( out_buf.pvBuffer );
            out_buf.pvBuffer = NULL;
            out_buf.cbBuffer = 0;
        }
        if (status == SEC_I_CONTINUE_NEEDED)
        {
            SIZE_T extra = in_bufs[1].cbBuffer;
            memmove( impl->enc_buf, impl->enc_buf + impl->enc_len - extra, extra );
            impl->enc_len = extra;
        }
        if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE)
        {
            DeleteSecurityContext( &ctx );
            return FAILED(hr) ? hr : HRESULT_FROM_WIN32( WSAECONNRESET );
        }
        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[0].pvBuffer = impl->enc_buf;
        in_bufs[0].cbBuffer = (ULONG)impl->enc_len;
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        in_bufs[1].pvBuffer = NULL;
        in_bufs[1].cbBuffer = 0;
        status = InitializeSecurityContextW( &impl->cred, &ctx, (WCHAR *)hostname, isc_req_flags, 0, 0,
                                             &in_desc, 0, NULL, &out_desc, &(ULONG){0}, NULL );
    }
    if (status != SEC_E_OK)
    {
        DeleteSecurityContext( &ctx );
        return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
    }
    if (QueryContextAttributesW( &ctx, SECPKG_ATTR_STREAM_SIZES, &impl->sizes ) != SEC_E_OK)
    {
        DeleteSecurityContext( &ctx );
        return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
    }
    if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer)
    {
        SIZE_T extra = in_bufs[1].cbBuffer;
        memmove( impl->enc_buf, impl->enc_buf + impl->enc_len - extra, extra );
        impl->enc_len = extra;
    }
    else impl->enc_len = 0;
    impl->ctx = ctx;
    impl->have_ctx = TRUE;
    impl->secure = TRUE;
    TRACE( "TLS ok for %s.\n", debugstr_w(hostname) );
    return S_OK;
}

static HRESULT tls_send( struct message_websocket *impl, const BYTE *data, SIZE_T len )
{
    BYTE *buf;
    HRESULT hr = S_OK;
    if (!(buf = malloc( impl->sizes.cbHeader + impl->sizes.cbMaximumMessage + impl->sizes.cbTrailer )))
        return E_OUTOFMEMORY;
    while (len)
    {
        SIZE_T chunk = min( len, (SIZE_T)impl->sizes.cbMaximumMessage );
        SecBuffer bufs[4] =
        {
            {impl->sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf},
            {(ULONG)chunk, SECBUFFER_DATA, buf + impl->sizes.cbHeader},
            {impl->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf + impl->sizes.cbHeader + chunk},
            {0, SECBUFFER_EMPTY, NULL},
        };
        SecBufferDesc desc = {SECBUFFER_VERSION, ARRAY_SIZE(bufs), bufs};
        SECURITY_STATUS status;
        memcpy( bufs[1].pvBuffer, data, chunk );
        if ((status = EncryptMessage( &impl->ctx, 0, &desc, 0 )) != SEC_E_OK) { hr = HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR ); break; }
        if (FAILED(hr = sock_send_all( impl, buf, bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer ))) break;
        data += chunk;
        len -= chunk;
    }
    free( buf );
    return hr;
}

static HRESULT tls_fill_plaintext( struct message_websocket *impl )
{
    while (!impl->dec_len)
    {
        SecBuffer bufs[4];
        SecBufferDesc desc = {SECBUFFER_VERSION, ARRAY_SIZE(bufs), bufs};
        SECURITY_STATUS status;
        unsigned int i;
        HRESULT hr;
        if (!impl->enc_len)
            if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE) return hr;
        memset( bufs, 0, sizeof(bufs) );
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = impl->enc_buf;
        bufs[0].cbBuffer = (ULONG)impl->enc_len;
        status = DecryptMessage( &impl->ctx, &desc, 0, NULL );
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE) return hr;
            continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) return S_FALSE;
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
        for (i = 0; i < ARRAY_SIZE(bufs); i++)
            if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer)
                if (FAILED(hr = dec_append( impl, bufs[i].pvBuffer, bufs[i].cbBuffer ))) return hr;
        impl->enc_len = 0;
        for (i = 0; i < ARRAY_SIZE(bufs); i++)
            if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer)
            {
                memmove( impl->enc_buf, bufs[i].pvBuffer, bufs[i].cbBuffer );
                impl->enc_len = bufs[i].cbBuffer;
                break;
            }
    }
    return S_OK;
}

static HRESULT transport_read( struct message_websocket *impl, BYTE *out, UINT32 count, UINT32 *read )
{
    HRESULT hr;
    *read = 0;
    if (!count) return S_OK;
    if (impl->secure)
    {
        if (FAILED(hr = tls_fill_plaintext( impl )) || hr == S_FALSE) return hr;
        *read = min( count, (UINT32)impl->dec_len );
        memcpy( out, impl->dec_buf + impl->dec_off, *read );
        impl->dec_off += *read;
        impl->dec_len -= *read;
        if (!impl->dec_len) impl->dec_off = 0;
        return S_OK;
    }
    if (!impl->enc_len)
        if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE) return hr;
    *read = min( count, (UINT32)impl->enc_len );
    memcpy( out, impl->enc_buf, *read );
    memmove( impl->enc_buf, impl->enc_buf + *read, impl->enc_len - *read );
    impl->enc_len -= *read;
    return S_OK;
}

static HRESULT transport_read_exact( struct message_websocket *impl, BYTE *out, UINT32 count )
{
    UINT32 got = 0;
    while (got < count)
    {
        UINT32 n = 0;
        HRESULT hr = transport_read( impl, out + got, count - got, &n );
        if (FAILED(hr)) return hr;
        if (!n) return HRESULT_FROM_WIN32( WSAECONNRESET );
        got += n;
    }
    return S_OK;
}

static HRESULT transport_send( struct message_websocket *impl, const BYTE *data, SIZE_T len )
{
    if (impl->secure) return tls_send( impl, data, len );
    return sock_send_all( impl, data, len );
}

static void socket_disconnect( struct message_websocket *impl )
{
    if (impl->have_ctx) DeleteSecurityContext( &impl->ctx );
    if (impl->have_cred) FreeCredentialsHandle( &impl->cred );
    impl->have_ctx = impl->have_cred = impl->secure = impl->connected = FALSE;
    if (impl->sock != INVALID_SOCKET)
    {
        shutdown( impl->sock, SD_BOTH );
        closesocket( impl->sock );
        impl->sock = INVALID_SOCKET;
    }
    free( impl->enc_buf );
    free( impl->dec_buf );
    impl->enc_buf = impl->dec_buf = NULL;
    impl->enc_size = impl->enc_len = impl->dec_size = impl->dec_len = impl->dec_off = 0;
}

static HRESULT buffer_bytes( IBuffer *buffer, BYTE **bytes )
{
    IBufferByteAccess *access;
    HRESULT hr;
    if (FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access ))) return hr;
    hr = IBufferByteAccess_Buffer( access, bytes );
    IBufferByteAccess_Release( access );
    return hr;
}

static void base64_encode( const BYTE *in, SIZE_T len, char *out )
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    SIZE_T i, j = 0;
    for (i = 0; i + 2 < len; i += 3)
    {
        out[j++] = tbl[in[i] >> 2];
        out[j++] = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[j++] = tbl[((in[i + 1] & 0xf) << 2) | (in[i + 2] >> 6)];
        out[j++] = tbl[in[i + 2] & 0x3f];
    }
    if (i < len)
    {
        out[j++] = tbl[in[i] >> 2];
        if (i + 1 < len)
        {
            out[j++] = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            out[j++] = tbl[(in[i + 1] & 0xf) << 2];
            out[j++] = '=';
        }
        else
        {
            out[j++] = tbl[(in[i] & 3) << 4];
            out[j++] = '=';
            out[j++] = '=';
        }
    }
    out[j] = 0;
}

static void random_bytes( BYTE *buf, SIZE_T len )
{
    SIZE_T i;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter( &qpc );
    srand( (unsigned)(qpc.QuadPart ^ GetCurrentProcessId() ^ GetTickCount()) );
    for (i = 0; i < len; i++) buf[i] = (BYTE)(rand() & 0xff);
}

static HRESULT ws_send_frame( struct message_websocket *impl, BYTE opcode, const BYTE *payload, SIZE_T len )
{
    BYTE hdr[14], mask[4], *framed;
    SIZE_T hdr_len = 2, i;
    HRESULT hr;

    hdr[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) hdr[1] = 0x80 | (BYTE)len;
    else if (len <= 0xffff)
    {
        hdr[1] = 0x80 | 126;
        hdr[2] = (BYTE)(len >> 8);
        hdr[3] = (BYTE)len;
        hdr_len = 4;
    }
    else
    {
        hdr[1] = 0x80 | 127;
        memset( hdr + 2, 0, 6 );
        hdr[8] = (BYTE)(len >> 24);
        hdr[9] = (BYTE)(len >> 16);
        hdr[10] = (BYTE)(len >> 8);
        hdr[11] = (BYTE)len;
        hdr_len = 10;
    }
    random_bytes( mask, 4 );
    memcpy( hdr + hdr_len, mask, 4 );
    hdr_len += 4;
    if (!(framed = malloc( hdr_len + len ))) return E_OUTOFMEMORY;
    memcpy( framed, hdr, hdr_len );
    for (i = 0; i < len; i++) framed[hdr_len + i] = payload[i] ^ mask[i & 3];
    hr = transport_send( impl, framed, hdr_len + len );
    free( framed );
    return hr;
}

static HRESULT ws_read_frame( struct message_websocket *impl, BYTE *opcode, BYTE **payload, SIZE_T *payload_len, BOOL *fin )
{
    BYTE hdr[2], ext[8], mask[4];
    BOOL masked;
    UINT64 len;
    SIZE_T i;
    HRESULT hr;

    if (FAILED(hr = transport_read_exact( impl, hdr, 2 ))) return hr;
    *fin = !!(hdr[0] & 0x80);
    *opcode = hdr[0] & 0x0f;
    masked = !!(hdr[1] & 0x80);
    len = hdr[1] & 0x7f;
    if (len == 126)
    {
        if (FAILED(hr = transport_read_exact( impl, ext, 2 ))) return hr;
        len = ((UINT64)ext[0] << 8) | ext[1];
    }
    else if (len == 127)
    {
        if (FAILED(hr = transport_read_exact( impl, ext, 8 ))) return hr;
        len = 0;
        for (i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > 16 * 1024 * 1024) return HRESULT_FROM_WIN32( ERROR_BAD_LENGTH );
    if (masked && FAILED(hr = transport_read_exact( impl, mask, 4 ))) return hr;
    if (!(*payload = malloc( (SIZE_T)len ? (SIZE_T)len : 1 ))) return E_OUTOFMEMORY;
    if (len && FAILED(hr = transport_read_exact( impl, *payload, (UINT32)len )))
    {
        free( *payload );
        *payload = NULL;
        return hr;
    }
    if (masked)
        for (i = 0; i < (SIZE_T)len; i++) (*payload)[i] ^= mask[i & 3];
    *payload_len = (SIZE_T)len;
    return S_OK;
}

static HRESULT WINAPI ws_op_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI ws_op_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_ws_op( iface )->ref );
}
static ULONG WINAPI ws_op_Release( IUnknown *iface )
{
    struct ws_op *impl = impl_from_ws_op( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->uri) IUriRuntimeClass_Release( impl->uri );
        if (impl->buffer) IBuffer_Release( impl->buffer );
        free( impl );
    }
    return ref;
}
static const IUnknownVtbl ws_op_vtbl = { ws_op_QueryInterface, ws_op_AddRef, ws_op_Release };

static HRESULT ws_op_create( struct ws_op **out )
{
    struct ws_op *op;
    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->IUnknown_iface.lpVtbl = &ws_op_vtbl;
    op->ref = 1;
    *out = op;
    return S_OK;
}

/* ---- closed event args ---- */

static HRESULT WINAPI closed_args_QI( IWebSocketClosedEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IWebSocketClosedEventArgs ))
    {
        *out = iface;
        IWebSocketClosedEventArgs_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI closed_args_AddRef( IWebSocketClosedEventArgs *iface )
{
    return InterlockedIncrement( &CONTAINING_RECORD( iface, struct closed_args, IWebSocketClosedEventArgs_iface )->ref );
}
static ULONG WINAPI closed_args_Release( IWebSocketClosedEventArgs *iface )
{
    struct closed_args *impl = CONTAINING_RECORD( iface, struct closed_args, IWebSocketClosedEventArgs_iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) { WindowsDeleteString( impl->reason ); free( impl ); }
    return ref;
}
static HRESULT WINAPI closed_args_GetIids( IWebSocketClosedEventArgs *iface, ULONG *c, IID **i ) { return E_NOTIMPL; }
static HRESULT WINAPI closed_args_GetRuntimeClassName( IWebSocketClosedEventArgs *iface, HSTRING *n )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_Sockets_WebSocketClosedEventArgs,
                                ARRAY_SIZE(RuntimeClass_Windows_Networking_Sockets_WebSocketClosedEventArgs) - 1, n );
}
static HRESULT WINAPI closed_args_GetTrustLevel( IWebSocketClosedEventArgs *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }
static HRESULT WINAPI closed_args_get_Code( IWebSocketClosedEventArgs *iface, UINT16 *value )
{
    *value = CONTAINING_RECORD( iface, struct closed_args, IWebSocketClosedEventArgs_iface )->code;
    return S_OK;
}
static HRESULT WINAPI closed_args_get_Reason( IWebSocketClosedEventArgs *iface, HSTRING *value )
{
    return WindowsDuplicateString( CONTAINING_RECORD( iface, struct closed_args, IWebSocketClosedEventArgs_iface )->reason, value );
}
static const IWebSocketClosedEventArgsVtbl closed_args_vtbl =
{
    closed_args_QI, closed_args_AddRef, closed_args_Release,
    closed_args_GetIids, closed_args_GetRuntimeClassName, closed_args_GetTrustLevel,
    closed_args_get_Code, closed_args_get_Reason,
};

static HRESULT closed_args_create( UINT16 code, HSTRING reason, IWebSocketClosedEventArgs **out )
{
    struct closed_args *impl;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebSocketClosedEventArgs_iface.lpVtbl = &closed_args_vtbl;
    impl->ref = 1;
    impl->code = code;
    WindowsDuplicateString( reason, &impl->reason );
    *out = &impl->IWebSocketClosedEventArgs_iface;
    return S_OK;
}


/* ---- memory input stream ---- */

static HRESULT WINAPI mem_stream_QI( IInputStream *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IInputStream ))
    {
        *out = iface; IInputStream_AddRef( iface ); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI mem_stream_AddRef( IInputStream *iface )
{
    return InterlockedIncrement( &CONTAINING_RECORD( iface, struct mem_stream, IInputStream_iface )->ref );
}
static ULONG WINAPI mem_stream_Release( IInputStream *iface )
{
    struct mem_stream *impl = CONTAINING_RECORD( iface, struct mem_stream, IInputStream_iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) { free( impl->data ); free( impl ); }
    return ref;
}
static HRESULT WINAPI mem_stream_GetIids( IInputStream *iface, ULONG *c, IID **i ) { return E_NOTIMPL; }
static HRESULT WINAPI mem_stream_GetRuntimeClassName( IInputStream *iface, HSTRING *n )
{ return WindowsCreateString( L"Windows.Storage.Streams.IInputStream", 35, n ); }
static HRESULT WINAPI mem_stream_GetTrustLevel( IInputStream *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }

static HRESULT mem_read_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct mem_stream *impl = CONTAINING_RECORD( (IInputStream *)invoker, struct mem_stream, IInputStream_iface );
    struct ws_op *op = impl_from_ws_op( param );
    UINT32 capacity = 0, n;
    BYTE *bytes;
    HRESULT hr;
    if (!called_async) return STATUS_PENDING;
    if (FAILED(hr = buffer_bytes( op->buffer, &bytes ))) return hr;
    if (FAILED(hr = IBuffer_get_Capacity( op->buffer, &capacity ))) return hr;
    n = min( capacity, impl->size - impl->pos );
    memcpy( bytes, impl->data + impl->pos, n );
    impl->pos += n;
    IBuffer_put_Length( op->buffer, n );
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)op->buffer;
    IBuffer_AddRef( op->buffer );
    return S_OK;
}

static HRESULT WINAPI mem_stream_ReadAsync( IInputStream *iface, IBuffer *buffer, UINT32 count,
                                            InputStreamOptions options,
                                            IAsyncOperationWithProgress_IBuffer_UINT32 **operation )
{
    struct ws_op *op;
    HRESULT hr;
    if (!buffer || !operation) return E_POINTER;
    if (FAILED(hr = ws_op_create( &op ))) return hr;
    IBuffer_AddRef( (op->buffer = buffer) );
    hr = async_read_create( (IUnknown *)iface, &op->IUnknown_iface, mem_read_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}

static const IInputStreamVtbl mem_stream_vtbl =
{
    mem_stream_QI, mem_stream_AddRef, mem_stream_Release,
    mem_stream_GetIids, mem_stream_GetRuntimeClassName, mem_stream_GetTrustLevel,
    mem_stream_ReadAsync,
};

static HRESULT mem_stream_create( const BYTE *data, UINT32 size, IInputStream **out )
{
    struct mem_stream *impl;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (size && !(impl->data = malloc( size ))) { free( impl ); return E_OUTOFMEMORY; }
    if (size) memcpy( impl->data, data, size );
    impl->IInputStream_iface.lpVtbl = &mem_stream_vtbl;
    impl->ref = 1;
    impl->size = size;
    *out = &impl->IInputStream_iface;
    return S_OK;
}

/* ---- minimal already-loaded DataReader ---- */

static HRESULT WINAPI dr_QI( IInspectable *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDataReader_local ))
    {
        *out = iface; IInspectable_AddRef( iface ); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI dr_AddRef( IInspectable *iface )
{
    return InterlockedIncrement( &CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface )->ref );
}
static ULONG WINAPI dr_Release( IInspectable *iface )
{
    struct data_reader *impl = CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) { free( impl->data ); free( impl ); }
    return ref;
}
static HRESULT WINAPI dr_GetIids( IInspectable *iface, ULONG *c, IID **i ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_GetRuntimeClassName( IInspectable *iface, HSTRING *n )
{ return WindowsCreateString( L"Windows.Storage.Streams.DataReader", 34, n ); }
static HRESULT WINAPI dr_GetTrustLevel( IInspectable *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }

static HRESULT WINAPI dr_get_Unconsumed( IInspectable *iface, UINT32 *v )
{
    struct data_reader *impl = CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface );
    *v = impl->size - impl->pos; return S_OK;
}
static HRESULT WINAPI dr_get_Unicode( IInspectable *iface, UINT32 *v )
{
    *v = CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface )->unicode_encoding; return S_OK;
}
static HRESULT WINAPI dr_put_Unicode( IInspectable *iface, UINT32 v )
{
    CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface )->unicode_encoding = v; return S_OK;
}
static HRESULT WINAPI dr_get_ByteOrder( IInspectable *iface, UINT32 *v ) { *v = 0; return S_OK; }
static HRESULT WINAPI dr_put_ByteOrder( IInspectable *iface, UINT32 v ) { return S_OK; }
static HRESULT WINAPI dr_get_ISO( IInspectable *iface, UINT32 *v ) { *v = 0; return S_OK; }
static HRESULT WINAPI dr_put_ISO( IInspectable *iface, UINT32 v ) { return S_OK; }
static HRESULT WINAPI dr_ReadByte( IInspectable *iface, BYTE *v )
{
    struct data_reader *impl = CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface );
    if (impl->pos >= impl->size) return E_BOUNDS;
    *v = impl->data[impl->pos++]; return S_OK;
}
static HRESULT WINAPI dr_ReadBytes( IInspectable *iface, UINT32 size, BYTE *v )
{
    struct data_reader *impl = CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface );
    if (impl->pos + size > impl->size) return E_BOUNDS;
    memcpy( v, impl->data + impl->pos, size ); impl->pos += size; return S_OK;
}
static HRESULT WINAPI dr_ReadBuffer( IInspectable *iface, UINT32 length, IBuffer **buffer ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadBoolean( IInspectable *iface, boolean *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadGuid( IInspectable *iface, GUID *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadInt16( IInspectable *iface, INT16 *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadInt32( IInspectable *iface, INT32 *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadInt64( IInspectable *iface, INT64 *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadUInt16( IInspectable *iface, UINT16 *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadUInt32( IInspectable *iface, UINT32 *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadUInt64( IInspectable *iface, UINT64 *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadSingle( IInspectable *iface, FLOAT *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadDouble( IInspectable *iface, DOUBLE *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadDateTime( IInspectable *iface, DateTime *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadTimeSpan( IInspectable *iface, TimeSpan *value ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_ReadString( IInspectable *iface, UINT32 code_unit_count, HSTRING *value )
{
    struct data_reader *impl = CONTAINING_RECORD( iface, struct data_reader, IInspectable_iface );
    int wlen;
    WCHAR *wbuf;
    HRESULT hr;
    if (impl->unicode_encoding != 0) return E_NOTIMPL;
    if (impl->pos + code_unit_count > impl->size) return E_BOUNDS;
    wlen = MultiByteToWideChar( CP_UTF8, 0, (char *)impl->data + impl->pos, code_unit_count, NULL, 0 );
    if (!wlen && code_unit_count) return HRESULT_FROM_WIN32( GetLastError() );
    if (!(wbuf = malloc( (wlen + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, 0, (char *)impl->data + impl->pos, code_unit_count, wbuf, wlen );
    hr = WindowsCreateString( wbuf, wlen, value );
    free( wbuf );
    if (SUCCEEDED(hr)) impl->pos += code_unit_count;
    return hr;
}
static HRESULT WINAPI dr_LoadAsync( IInspectable *iface, UINT32 count, IInspectable **op )
{ *op = NULL; return E_NOTIMPL; }
static HRESULT WINAPI dr_DetachBuffer( IInspectable *iface, IBuffer **b ) { return E_NOTIMPL; }
static HRESULT WINAPI dr_DetachStream( IInspectable *iface, IInputStream **s ) { return E_NOTIMPL; }

/* Manual IDataReader vtable as IInspectable* slots after IInspectable methods. */
static const void *data_reader_vtbl[] =
{
    dr_QI, dr_AddRef, dr_Release, dr_GetIids, dr_GetRuntimeClassName, dr_GetTrustLevel,
    dr_get_Unconsumed, dr_get_Unicode, dr_put_Unicode, dr_get_ByteOrder, dr_put_ByteOrder,
    dr_get_ISO, dr_put_ISO, dr_ReadByte, dr_ReadBytes, dr_ReadBuffer,
    dr_ReadBoolean, dr_ReadGuid, dr_ReadInt16, dr_ReadInt32, dr_ReadInt64,
    dr_ReadUInt16, dr_ReadUInt32, dr_ReadUInt64, dr_ReadSingle, dr_ReadDouble,
    dr_ReadString, dr_ReadDateTime, dr_ReadTimeSpan, dr_LoadAsync, dr_DetachBuffer, dr_DetachStream,
};

static HRESULT data_reader_create( const BYTE *data, UINT32 size, IInspectable **out )
{
    struct data_reader *impl;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (size && !(impl->data = malloc( size ))) { free( impl ); return E_OUTOFMEMORY; }
    if (size) memcpy( impl->data, data, size );
    impl->IInspectable_iface.lpVtbl = (const IInspectableVtbl *)data_reader_vtbl;
    impl->ref = 1;
    impl->size = size;
    *out = &impl->IInspectable_iface;
    return S_OK;
}

/* ---- message received args ---- */

static HRESULT WINAPI msg_args_QI( IMessageWebSocketMessageReceivedEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMessageWebSocketMessageReceivedEventArgs ))
    {
        *out = iface; IMessageWebSocketMessageReceivedEventArgs_AddRef( iface ); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI msg_args_AddRef( IMessageWebSocketMessageReceivedEventArgs *iface )
{
    return InterlockedIncrement( &CONTAINING_RECORD( iface, struct msg_args, IMessageWebSocketMessageReceivedEventArgs_iface )->ref );
}
static ULONG WINAPI msg_args_Release( IMessageWebSocketMessageReceivedEventArgs *iface )
{
    struct msg_args *impl = CONTAINING_RECORD( iface, struct msg_args, IMessageWebSocketMessageReceivedEventArgs_iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) { free( impl->data ); free( impl ); }
    return ref;
}
static HRESULT WINAPI msg_args_GetIids( IMessageWebSocketMessageReceivedEventArgs *iface, ULONG *c, IID **i ) { return E_NOTIMPL; }
static HRESULT WINAPI msg_args_GetRuntimeClassName( IMessageWebSocketMessageReceivedEventArgs *iface, HSTRING *n )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_Sockets_MessageWebSocketMessageReceivedEventArgs,
                                ARRAY_SIZE(RuntimeClass_Windows_Networking_Sockets_MessageWebSocketMessageReceivedEventArgs) - 1, n );
}
static HRESULT WINAPI msg_args_GetTrustLevel( IMessageWebSocketMessageReceivedEventArgs *iface, TrustLevel *l )
{ *l = BaseTrust; return S_OK; }
static HRESULT WINAPI msg_args_get_MessageType( IMessageWebSocketMessageReceivedEventArgs *iface, SocketMessageType *v )
{
    *v = CONTAINING_RECORD( iface, struct msg_args, IMessageWebSocketMessageReceivedEventArgs_iface )->type;
    return S_OK;
}
static HRESULT WINAPI msg_args_GetDataReader( IMessageWebSocketMessageReceivedEventArgs *iface, IInspectable **out )
{
    struct msg_args *impl = CONTAINING_RECORD( iface, struct msg_args, IMessageWebSocketMessageReceivedEventArgs_iface );
    return data_reader_create( impl->data, impl->size, out );
}
static HRESULT WINAPI msg_args_GetDataStream( IMessageWebSocketMessageReceivedEventArgs *iface, IInputStream **out )
{
    struct msg_args *impl = CONTAINING_RECORD( iface, struct msg_args, IMessageWebSocketMessageReceivedEventArgs_iface );
    return mem_stream_create( impl->data, impl->size, out );
}
static const IMessageWebSocketMessageReceivedEventArgsVtbl msg_args_vtbl =
{
    msg_args_QI, msg_args_AddRef, msg_args_Release,
    msg_args_GetIids, msg_args_GetRuntimeClassName, msg_args_GetTrustLevel,
    msg_args_get_MessageType, msg_args_GetDataReader, msg_args_GetDataStream,
};

static HRESULT msg_args_create( SocketMessageType type, BYTE *data, UINT32 size,
                                IMessageWebSocketMessageReceivedEventArgs **out )
{
    struct msg_args *impl;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMessageWebSocketMessageReceivedEventArgs_iface.lpVtbl = &msg_args_vtbl;
    impl->ref = 1;
    impl->type = type;
    impl->data = data; /* takes ownership */
    impl->size = size;
    *out = &impl->IMessageWebSocketMessageReceivedEventArgs_iface;
    return S_OK;
}

static void fire_closed( struct message_websocket *impl, UINT16 code, HSTRING reason )
{
    IWebSocketClosedEventArgs *args = NULL;
    ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs *handlers[MAX_WS_HANDLERS];
    unsigned int i, n = 0;

    EnterCriticalSection( &impl->cs );
    if (impl->closing) { LeaveCriticalSection( &impl->cs ); return; }
    impl->closing = TRUE;
    impl->close_code = code;
    for (i = 0; i < MAX_WS_HANDLERS; i++)
        if (impl->closed_handlers[i])
        {
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_AddRef( (handlers[n++] = impl->closed_handlers[i]) );
        }
    LeaveCriticalSection( &impl->cs );

    if (SUCCEEDED(closed_args_create( code, reason, &args )))
    {
        for (i = 0; i < n; i++)
        {
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_Invoke( handlers[i], &impl->IWebSocket_iface, args );
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_Release( handlers[i] );
        }
        IWebSocketClosedEventArgs_Release( args );
    }
    else
        for (i = 0; i < n; i++)
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_Release( handlers[i] );

    EnterCriticalSection( &impl->write_cs );
    EnterCriticalSection( &impl->cs );
    socket_disconnect( impl );
    LeaveCriticalSection( &impl->cs );
    LeaveCriticalSection( &impl->write_cs );
}

static void fire_message( struct message_websocket *impl, SocketMessageType type, BYTE *data, UINT32 size )
{
    IMessageWebSocketMessageReceivedEventArgs *args = NULL;
    ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs *handlers[MAX_WS_HANDLERS];
    unsigned int i, n = 0;
    BYTE *copy;

    if (!(copy = malloc( size ? size : 1 ))) { free( data ); return; }
    if (size) memcpy( copy, data, size );
    free( data );

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < MAX_WS_HANDLERS; i++)
        if (impl->msg_handlers[i])
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_AddRef(
                (handlers[n++] = impl->msg_handlers[i]) );
    LeaveCriticalSection( &impl->cs );

    if (SUCCEEDED(msg_args_create( type, copy, size, &args )))
    {
        for (i = 0; i < n; i++)
        {
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_Invoke(
                handlers[i], &impl->IMessageWebSocket_iface, args );
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_Release( handlers[i] );
        }
        IMessageWebSocketMessageReceivedEventArgs_Release( args );
    }
    else
    {
        free( copy );
        for (i = 0; i < n; i++)
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_Release( handlers[i] );
    }
}

static DWORD WINAPI recv_thread_proc( void *arg )
{
    struct message_websocket *impl = arg;
    BYTE *acc = NULL;
    SIZE_T acc_len = 0;
    BYTE acc_opcode = 0;

    TRACE( "recv thread %p.\n", impl );
    for (;;)
    {
        BYTE opcode = 0, *payload = NULL;
        SIZE_T payload_len = 0;
        BOOL fin = FALSE;
        HRESULT hr;

        EnterCriticalSection( &impl->cs );
        if (!impl->connected || impl->closing) { LeaveCriticalSection( &impl->cs ); break; }
        LeaveCriticalSection( &impl->cs );

        hr = ws_read_frame( impl, &opcode, &payload, &payload_len, &fin );
        if (FAILED(hr))
        {
            free( payload );
            fire_closed( impl, 1006, NULL );
            break;
        }
        if (opcode == WS_OP_PING)
        {
            EnterCriticalSection( &impl->write_cs );
            ws_send_frame( impl, WS_OP_PONG, payload, payload_len );
            LeaveCriticalSection( &impl->write_cs );
            free( payload );
            continue;
        }
        if (opcode == WS_OP_PONG) { free( payload ); continue; }
        if (opcode == WS_OP_CLOSE)
        {
            UINT16 code = 1000;
            if (payload_len >= 2) code = ((UINT16)payload[0] << 8) | payload[1];
            free( payload ); free( acc ); acc = NULL;
            fire_closed( impl, code, NULL );
            break;
        }
        if (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY)
        {
            free( acc );
            acc = payload; acc_len = payload_len; acc_opcode = opcode; payload = NULL;
        }
        else if (opcode == WS_OP_CONTINUATION)
        {
            BYTE *nacc = realloc( acc, acc_len + payload_len );
            if (!nacc) { free( payload ); free( acc ); acc = NULL; fire_closed( impl, 1002, NULL ); break; }
            memcpy( nacc + acc_len, payload, payload_len );
            acc = nacc; acc_len += payload_len; free( payload ); payload = NULL;
        }
        else { free( payload ); continue; }

        if (fin && acc)
        {
            SocketMessageType type = (acc_opcode == WS_OP_TEXT) ? SocketMessageType_Utf8 : SocketMessageType_Binary;
            fire_message( impl, type, acc, (UINT32)acc_len );
            acc = NULL; acc_len = 0;
        }
    }
    free( acc );
    TRACE( "recv thread exit %p.\n", impl );
    IMessageWebSocket_Release( &impl->IMessageWebSocket_iface );
    return 0;
}


/* ---- connect ---- */

static HRESULT http_upgrade( struct message_websocket *impl, const WCHAR *host, INT32 port,
                             const WCHAR *path, const WCHAR *query, BOOL secure )
{
    char key_raw[16], key_b64[32], req[8192], resp[4096];
    char host_a[256], path_a[2048];
    SIZE_T req_len, resp_len = 0;
    UINT i;
    HRESULT hr;
    const char *status;

    random_bytes( (BYTE *)key_raw, 16 );
    base64_encode( (BYTE *)key_raw, 16, key_b64 );

    WideCharToMultiByte( CP_UTF8, 0, host, -1, host_a, sizeof(host_a), NULL, NULL );
    if (path && *path)
        WideCharToMultiByte( CP_UTF8, 0, path, -1, path_a, sizeof(path_a), NULL, NULL );
    else
        strcpy( path_a, "/" );
    if (query && *query)
    {
        SIZE_T n = strlen( path_a );
        if (n + 1 < sizeof(path_a))
        {
            path_a[n++] = '?';
            WideCharToMultiByte( CP_UTF8, 0, query + (*query == '?' ? 1 : 0), -1,
                                 path_a + n, (int)(sizeof(path_a) - n), NULL, NULL );
        }
    }

    req_len = snprintf( req, sizeof(req),
                        "GET %s HTTP/1.1\r\nHost: %s", path_a, host_a );
    if ((secure && port != 443) || (!secure && port != 80))
        req_len += snprintf( req + req_len, sizeof(req) - req_len, ":%d", port );
    req_len += snprintf( req + req_len, sizeof(req) - req_len,
                         "\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\n"
                         "Sec-WebSocket-Version: 13\r\n", key_b64 );

    for (i = 0; i < impl->header_count; i++)
    {
        char name_a[128], value_a[2048];
        WideCharToMultiByte( CP_UTF8, 0, WindowsGetStringRawBuffer( impl->headers[i].name, NULL ), -1,
                             name_a, sizeof(name_a), NULL, NULL );
        WideCharToMultiByte( CP_UTF8, 0, WindowsGetStringRawBuffer( impl->headers[i].value, NULL ), -1,
                             value_a, sizeof(value_a), NULL, NULL );
        req_len += snprintf( req + req_len, sizeof(req) - req_len, "%s: %s\r\n", name_a, value_a );
    }
    req_len += snprintf( req + req_len, sizeof(req) - req_len, "\r\n" );
    if (req_len >= sizeof(req)) return E_INVALIDARG;

    TRACE( "WS upgrade request (%Iu bytes) to %s.\n", req_len, debugstr_a(host_a) );
    if (FAILED(hr = transport_send( impl, (BYTE *)req, req_len ))) return hr;

    /* Read until end of HTTP headers. */
    while (resp_len < sizeof(resp) - 1)
    {
        UINT32 n = 0;
        hr = transport_read( impl, (BYTE *)resp + resp_len, (UINT32)(sizeof(resp) - 1 - resp_len), &n );
        if (FAILED(hr) || !n) return FAILED(hr) ? hr : HRESULT_FROM_WIN32( WSAECONNRESET );
        resp_len += n;
        resp[resp_len] = 0;
        if (strstr( resp, "\r\n\r\n" )) break;
    }
    status = resp;
    if (strncmp( status, "HTTP/1.", 7 )) return HRESULT_FROM_WIN32( ERROR_PROTOCOL_UNREACHABLE );
    while (*status && *status != ' ') status++;
    while (*status == ' ') status++;
    if (strncmp( status, "101", 3 ))
    {
        WARN( "WS upgrade failed: %.64s\n", resp );
        return HRESULT_FROM_WIN32( ERROR_PROTOCOL_UNREACHABLE );
    }
    TRACE( "WS upgrade 101 ok.\n" );
    return S_OK;
}

static HRESULT connect_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct message_websocket *impl = impl_from_IWebSocket( (IWebSocket *)invoker );
    struct ws_op *op = impl_from_ws_op( param );
    HSTRING scheme = NULL, host = NULL, path = NULL, query = NULL;
    const WCHAR *scheme_w, *host_w, *path_w, *query_w;
    INT32 port = 0;
    BOOL secure = FALSE;
    ADDRINFOW hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP};
    ADDRINFOW *info = NULL, *ai;
    WCHAR port_str[16];
    HRESULT hr;

    if (!called_async) return STATUS_PENDING;

    if (!op->uri) return E_INVALIDARG;
    IUriRuntimeClass_get_SchemeName( op->uri, &scheme );
    IUriRuntimeClass_get_Host( op->uri, &host );
    IUriRuntimeClass_get_Port( op->uri, &port );
    IUriRuntimeClass_get_Path( op->uri, &path );
    IUriRuntimeClass_get_Query( op->uri, &query );

    scheme_w = WindowsGetStringRawBuffer( scheme, NULL );
    host_w = WindowsGetStringRawBuffer( host, NULL );
    path_w = WindowsGetStringRawBuffer( path, NULL );
    query_w = WindowsGetStringRawBuffer( query, NULL );

    if (!host_w || !*host_w) { hr = E_INVALIDARG; goto done; }
    if (scheme_w && (!wcsicmp( scheme_w, L"wss" ) || !wcsicmp( scheme_w, L"https" )))
    {
        secure = TRUE;
        if (!port) port = 443;
    }
    else
    {
        if (!port) port = 80;
    }

    swprintf( port_str, ARRAY_SIZE(port_str), L"%d", port );
    TRACE( "MessageWebSocket connect %s://%s:%s%s.\n",
           debugstr_w(scheme_w), debugstr_w(host_w), debugstr_w(port_str), debugstr_w(path_w) );

    if (GetAddrInfoW( host_w, port_str, &hints, &info ))
    {
        hr = hresult_from_wsa( 0 );
        goto done;
    }

    EnterCriticalSection( &impl->write_cs );
    EnterCriticalSection( &impl->cs );
    if (impl->recv_thread)
    {
        HANDLE t = impl->recv_thread;
        impl->closing = TRUE;
        socket_disconnect( impl );
        LeaveCriticalSection( &impl->cs );
        LeaveCriticalSection( &impl->write_cs );
        WaitForSingleObject( t, 5000 );
        CloseHandle( t );
        EnterCriticalSection( &impl->write_cs );
        EnterCriticalSection( &impl->cs );
        impl->recv_thread = NULL;
        impl->closing = FALSE;
    }
    else socket_disconnect( impl );

    hr = HRESULT_FROM_WIN32( WSAEHOSTUNREACH );
    for (ai = info; ai; ai = ai->ai_next)
    {
        SOCKET s = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
        if (s == INVALID_SOCKET) continue;
        if (!connect( s, ai->ai_addr, (int)ai->ai_addrlen ))
        {
            impl->sock = s;
            impl->connected = TRUE;
            hr = S_OK;
            break;
        }
        hr = hresult_from_wsa( 0 );
        closesocket( s );
    }
    FreeAddrInfoW( info );

    if (SUCCEEDED(hr))
    {
        int one = 1;
        setsockopt( impl->sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one) );
        if (secure && FAILED(hr = tls_handshake( impl, host_w )))
            socket_disconnect( impl );
    }
    if (SUCCEEDED(hr))
        hr = http_upgrade( impl, host_w, port, path_w, query_w, secure );

    if (SUCCEEDED(hr))
    {
        impl->closing = FALSE;
        IMessageWebSocket_AddRef( &impl->IMessageWebSocket_iface );
        impl->recv_thread = CreateThread( NULL, 0, recv_thread_proc, impl, 0, NULL );
        if (!impl->recv_thread)
        {
            IMessageWebSocket_Release( &impl->IMessageWebSocket_iface );
            socket_disconnect( impl );
            hr = HRESULT_FROM_WIN32( GetLastError() );
        }
    }
    LeaveCriticalSection( &impl->cs );
    LeaveCriticalSection( &impl->write_cs );

done:
    WindowsDeleteString( scheme );
    WindowsDeleteString( host );
    WindowsDeleteString( path );
    WindowsDeleteString( query );
    TRACE( "connect returning %#lx.\n", hr );
    return hr;
}

/* ---- OutputStream ---- */

DEFINE_IINSPECTABLE( ws_output, IOutputStream, struct message_websocket, IMessageWebSocket_iface )

static HRESULT write_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct message_websocket *impl = CONTAINING_RECORD( (IOutputStream *)invoker, struct message_websocket, IOutputStream_iface );
    struct ws_op *op = impl_from_ws_op( param );
    UINT32 length = 0;
    BYTE *bytes;
    BYTE opcode;
    HRESULT hr;

    if (!called_async) return STATUS_PENDING;
    if (FAILED(hr = buffer_bytes( op->buffer, &bytes ))) return hr;
    if (FAILED(hr = IBuffer_get_Length( op->buffer, &length ))) return hr;

    opcode = (impl->message_type == SocketMessageType_Utf8) ? WS_OP_TEXT : WS_OP_BINARY;
    EnterCriticalSection( &impl->write_cs );
    if (!impl->connected) hr = HRESULT_FROM_WIN32( WSAENOTCONN );
    else hr = ws_send_frame( impl, opcode, bytes, length );
    LeaveCriticalSection( &impl->write_cs );
    if (FAILED(hr)) return hr;
    result->vt = VT_UI4;
    result->ulVal = length;
    return S_OK;
}

static HRESULT flush_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_BOOL;
    result->boolVal = VARIANT_TRUE;
    return S_OK;
}

static HRESULT WINAPI ws_output_WriteAsync( IOutputStream *iface, IBuffer *buffer,
                                            IAsyncOperationWithProgress_UINT32_UINT32 **operation )
{
    struct ws_op *op;
    HRESULT hr;
    if (!buffer || !operation) return E_POINTER;
    if (FAILED(hr = ws_op_create( &op ))) return hr;
    IBuffer_AddRef( (op->buffer = buffer) );
    hr = async_write_create( (IUnknown *)iface, &op->IUnknown_iface, write_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}
static HRESULT WINAPI ws_output_FlushAsync( IOutputStream *iface, IAsyncOperation_boolean **operation )
{
    return async_operation_boolean_create( (IUnknown *)iface, NULL, flush_callback, operation );
}
static const IOutputStreamVtbl ws_output_vtbl =
{
    ws_output_QueryInterface, ws_output_AddRef, ws_output_Release,
    ws_output_GetIids, ws_output_GetRuntimeClassName, ws_output_GetTrustLevel,
    ws_output_WriteAsync, ws_output_FlushAsync,
};


/* ---- Control ---- */

DEFINE_IINSPECTABLE( ws_msg_control, IMessageWebSocketControl, struct message_websocket, IMessageWebSocket_iface )
DEFINE_IINSPECTABLE( ws_control, IWebSocketControl, struct message_websocket, IMessageWebSocket_iface )

static HRESULT WINAPI ws_msg_control_get_MaxMessageSize( IMessageWebSocketControl *iface, UINT32 *value )
{
    *value = CONTAINING_RECORD( iface, struct message_websocket, IMessageWebSocketControl_iface )->max_message_size;
    return S_OK;
}
static HRESULT WINAPI ws_msg_control_put_MaxMessageSize( IMessageWebSocketControl *iface, UINT32 value )
{
    CONTAINING_RECORD( iface, struct message_websocket, IMessageWebSocketControl_iface )->max_message_size = value;
    return S_OK;
}
static HRESULT WINAPI ws_msg_control_get_MessageType( IMessageWebSocketControl *iface, SocketMessageType *value )
{
    *value = CONTAINING_RECORD( iface, struct message_websocket, IMessageWebSocketControl_iface )->message_type;
    return S_OK;
}
static HRESULT WINAPI ws_msg_control_put_MessageType( IMessageWebSocketControl *iface, SocketMessageType value )
{
    TRACE( "MessageType %d.\n", value );
    CONTAINING_RECORD( iface, struct message_websocket, IMessageWebSocketControl_iface )->message_type = value;
    return S_OK;
}
static const IMessageWebSocketControlVtbl ws_msg_control_vtbl =
{
    ws_msg_control_QueryInterface, ws_msg_control_AddRef, ws_msg_control_Release,
    ws_msg_control_GetIids, ws_msg_control_GetRuntimeClassName, ws_msg_control_GetTrustLevel,
    ws_msg_control_get_MaxMessageSize, ws_msg_control_put_MaxMessageSize,
    ws_msg_control_get_MessageType, ws_msg_control_put_MessageType,
};

static HRESULT WINAPI ws_control_get_OutboundBufferSizeInBytes( IWebSocketControl *iface, UINT32 *value )
{
    *value = CONTAINING_RECORD( iface, struct message_websocket, IWebSocketControl_iface )->outbound_buffer_size;
    return S_OK;
}
static HRESULT WINAPI ws_control_put_OutboundBufferSizeInBytes( IWebSocketControl *iface, UINT32 value )
{
    CONTAINING_RECORD( iface, struct message_websocket, IWebSocketControl_iface )->outbound_buffer_size = value;
    return S_OK;
}
static HRESULT WINAPI ws_control_get_ServerCredential( IWebSocketControl *iface, IInspectable **value )
{ *value = NULL; return S_OK; }
static HRESULT WINAPI ws_control_put_ServerCredential( IWebSocketControl *iface, IInspectable *value )
{ FIXME( "ServerCredential stub.\n" ); return S_OK; }
static HRESULT WINAPI ws_control_get_ProxyCredential( IWebSocketControl *iface, IInspectable **value )
{ *value = NULL; return S_OK; }
static HRESULT WINAPI ws_control_put_ProxyCredential( IWebSocketControl *iface, IInspectable *value )
{ FIXME( "ProxyCredential stub.\n" ); return S_OK; }
static HRESULT WINAPI ws_control_get_SupportedProtocols( IWebSocketControl *iface, IVector_HSTRING **value )
{
    FIXME( "SupportedProtocols stub returning NULL.\n" );
    *value = NULL;
    return S_OK;
}
static const IWebSocketControlVtbl ws_control_vtbl =
{
    ws_control_QueryInterface, ws_control_AddRef, ws_control_Release,
    ws_control_GetIids, ws_control_GetRuntimeClassName, ws_control_GetTrustLevel,
    ws_control_get_OutboundBufferSizeInBytes, ws_control_put_OutboundBufferSizeInBytes,
    ws_control_get_ServerCredential, ws_control_put_ServerCredential,
    ws_control_get_ProxyCredential, ws_control_put_ProxyCredential,
    ws_control_get_SupportedProtocols,
};

/* ---- Information ---- */

DEFINE_IINSPECTABLE( ws_info, IWebSocketInformation, struct message_websocket, IMessageWebSocket_iface )

static HRESULT WINAPI ws_info_get_LocalAddress( IWebSocketInformation *iface, IHostName **value )
{ *value = NULL; return S_OK; }
static HRESULT WINAPI ws_info_get_BandwidthStatistics( IWebSocketInformation *iface, BandwidthStatistics *value )
{
    memset( value, 0, sizeof(*value) );
    return S_OK;
}
static HRESULT WINAPI ws_info_get_Protocol( IWebSocketInformation *iface, HSTRING *value )
{
    return WindowsDuplicateString( CONTAINING_RECORD( iface, struct message_websocket, IWebSocketInformation_iface )->protocol, value );
}
static const IWebSocketInformationVtbl ws_info_vtbl =
{
    ws_info_QueryInterface, ws_info_AddRef, ws_info_Release,
    ws_info_GetIids, ws_info_GetRuntimeClassName, ws_info_GetTrustLevel,
    ws_info_get_LocalAddress, ws_info_get_BandwidthStatistics, ws_info_get_Protocol,
};

/* ---- Closable ---- */

DEFINE_IINSPECTABLE( ws_closable, IClosable, struct message_websocket, IMessageWebSocket_iface )

static HRESULT WINAPI ws_closable_Close( IClosable *iface )
{
    struct message_websocket *impl = CONTAINING_RECORD( iface, struct message_websocket, IClosable_iface );
    TRACE( "Close.\n" );
    EnterCriticalSection( &impl->write_cs );
    if (impl->connected) ws_send_frame( impl, WS_OP_CLOSE, (const BYTE *)"\x03\xe8", 2 ); /* 1000 */
    LeaveCriticalSection( &impl->write_cs );
    fire_closed( impl, 1000, NULL );
    return S_OK;
}
static const IClosableVtbl ws_closable_vtbl =
{
    ws_closable_QueryInterface, ws_closable_AddRef, ws_closable_Release,
    ws_closable_GetIids, ws_closable_GetRuntimeClassName, ws_closable_GetTrustLevel,
    ws_closable_Close,
};

/* ---- IWebSocket ---- */

DEFINE_IINSPECTABLE_( websocket, IWebSocket, struct message_websocket, websocket_impl_from, IWebSocket_iface, &impl->IMessageWebSocket_iface )

static HRESULT WINAPI websocket_get_OutputStream( IWebSocket *iface, IOutputStream **value )
{
    struct message_websocket *impl = impl_from_IWebSocket( iface );
    if (!value) return E_POINTER;
    IOutputStream_AddRef( (*value = &impl->IOutputStream_iface) );
    return S_OK;
}
static HRESULT WINAPI websocket_ConnectAsync( IWebSocket *iface, IUriRuntimeClass *uri, IAsyncAction **operation )
{
    struct ws_op *op;
    HRESULT hr;
    TRACE( "ConnectAsync uri %p.\n", uri );
    if (!operation) return E_POINTER;
    if (FAILED(hr = ws_op_create( &op ))) return hr;
    if ((op->uri = uri)) IUriRuntimeClass_AddRef( uri );
    hr = async_action_param_create( (IUnknown *)iface, &op->IUnknown_iface, connect_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}
static HRESULT WINAPI websocket_SetRequestHeader( IWebSocket *iface, HSTRING name, HSTRING value )
{
    struct message_websocket *impl = impl_from_IWebSocket( iface );
    UINT i;
    TRACE( "header %s = %s.\n", debugstr_hstring(name), debugstr_hstring(value) );
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->header_count; i++)
    {
        INT32 ord = 1;
        WindowsCompareStringOrdinal( impl->headers[i].name, name, &ord );
        if (!ord)
        {
            WindowsDeleteString( impl->headers[i].value );
            WindowsDuplicateString( value, &impl->headers[i].value );
            LeaveCriticalSection( &impl->cs );
            return S_OK;
        }
    }
    if (impl->header_count >= MAX_WS_HEADERS) { LeaveCriticalSection( &impl->cs ); return E_BOUNDS; }
    WindowsDuplicateString( name, &impl->headers[impl->header_count].name );
    WindowsDuplicateString( value, &impl->headers[impl->header_count].value );
    impl->header_count++;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}
static HRESULT WINAPI websocket_add_Closed( IWebSocket *iface,
        ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs *handler, EventRegistrationToken *token )
{
    struct message_websocket *impl = impl_from_IWebSocket( iface );
    UINT i;
    if (!handler || !token) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < MAX_WS_HANDLERS; i++)
        if (!impl->closed_handlers[i])
        {
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_AddRef( (impl->closed_handlers[i] = handler) );
            token->value = ++impl->next_token;
            LeaveCriticalSection( &impl->cs );
            return S_OK;
        }
    LeaveCriticalSection( &impl->cs );
    return E_BOUNDS;
}
static HRESULT WINAPI websocket_remove_Closed( IWebSocket *iface, EventRegistrationToken token )
{
    struct message_websocket *impl = impl_from_IWebSocket( iface );
    UINT i;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < MAX_WS_HANDLERS; i++)
        if (impl->closed_handlers[i])
        {
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs *h = impl->closed_handlers[i];
            impl->closed_handlers[i] = NULL;
            LeaveCriticalSection( &impl->cs );
            ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_Release( h );
            return S_OK;
        }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}
static HRESULT WINAPI websocket_CloseWithStatus( IWebSocket *iface, UINT16 code, HSTRING reason )
{
    struct message_websocket *impl = impl_from_IWebSocket( iface );
    BYTE payload[2] = { (BYTE)(code >> 8), (BYTE)code };
    TRACE( "CloseWithStatus %u.\n", code );
    EnterCriticalSection( &impl->write_cs );
    if (impl->connected) ws_send_frame( impl, WS_OP_CLOSE, payload, 2 );
    LeaveCriticalSection( &impl->write_cs );
    fire_closed( impl, code, reason );
    return S_OK;
}
static const IWebSocketVtbl websocket_vtbl =
{
    websocket_QueryInterface, websocket_AddRef, websocket_Release,
    websocket_GetIids, websocket_GetRuntimeClassName, websocket_GetTrustLevel,
    websocket_get_OutputStream, websocket_ConnectAsync, websocket_SetRequestHeader,
    websocket_add_Closed, websocket_remove_Closed, websocket_CloseWithStatus,
};

/* ---- IMessageWebSocket ---- */

static HRESULT WINAPI mws_QueryInterface( IMessageWebSocket *iface, REFIID iid, void **out )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    TRACE( "iid %s.\n", debugstr_guid(iid) );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMessageWebSocket ))
    {
        *out = &impl->IMessageWebSocket_iface;
    }
    else if (IsEqualGUID( iid, &IID_IWebSocket ))
        *out = &impl->IWebSocket_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else if (IsEqualGUID( iid, &IID_IMessageWebSocketControl ))
        *out = &impl->IMessageWebSocketControl_iface;
    else if (IsEqualGUID( iid, &IID_IWebSocketControl ))
        *out = &impl->IWebSocketControl_iface;
    else if (IsEqualGUID( iid, &IID_IWebSocketInformation ))
        *out = &impl->IWebSocketInformation_iface;
    else if (IsEqualGUID( iid, &IID_IOutputStream ))
        *out = &impl->IOutputStream_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}
static ULONG WINAPI mws_AddRef( IMessageWebSocket *iface )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "refcount %lu.\n", ref );
    return ref;
}
static ULONG WINAPI mws_Release( IMessageWebSocket *iface )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "refcount %lu.\n", ref );
    if (!ref)
    {
        UINT i;
        HANDLE t;
        EnterCriticalSection( &impl->cs );
        impl->closing = TRUE;
        t = impl->recv_thread;
        impl->recv_thread = NULL;
        socket_disconnect( impl );
        LeaveCriticalSection( &impl->cs );
        if (t)
        {
            WaitForSingleObject( t, 2000 );
            CloseHandle( t );
            /* recv thread held an extra ref on the object */
        }
        for (i = 0; i < impl->header_count; i++)
        {
            WindowsDeleteString( impl->headers[i].name );
            WindowsDeleteString( impl->headers[i].value );
        }
        for (i = 0; i < MAX_WS_HANDLERS; i++)
        {
            if (impl->closed_handlers[i])
                ITypedEventHandler_IWebSocket_WebSocketClosedEventArgs_Release( impl->closed_handlers[i] );
            if (impl->msg_handlers[i])
                ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_Release( impl->msg_handlers[i] );
        }
        WindowsDeleteString( impl->close_reason );
        WindowsDeleteString( impl->protocol );
        DeleteCriticalSection( &impl->cs );
        DeleteCriticalSection( &impl->write_cs );
        free( impl );
    }
    return ref;
}
static HRESULT WINAPI mws_GetIids( IMessageWebSocket *iface, ULONG *c, IID **i ) { return E_NOTIMPL; }
static HRESULT WINAPI mws_GetRuntimeClassName( IMessageWebSocket *iface, HSTRING *n )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_Sockets_MessageWebSocket,
                                ARRAY_SIZE(RuntimeClass_Windows_Networking_Sockets_MessageWebSocket) - 1, n );
}
static HRESULT WINAPI mws_GetTrustLevel( IMessageWebSocket *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }
static HRESULT WINAPI mws_get_Control( IMessageWebSocket *iface, IMessageWebSocketControl **value )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    if (!value) return E_POINTER;
    IMessageWebSocketControl_AddRef( (*value = &impl->IMessageWebSocketControl_iface) );
    return S_OK;
}
static HRESULT WINAPI mws_get_Information( IMessageWebSocket *iface, IWebSocketInformation **value )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    if (!value) return E_POINTER;
    IWebSocketInformation_AddRef( (*value = &impl->IWebSocketInformation_iface) );
    return S_OK;
}
static HRESULT WINAPI mws_add_MessageReceived( IMessageWebSocket *iface,
        ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs *handler,
        EventRegistrationToken *token )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    UINT i;
    if (!handler || !token) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < MAX_WS_HANDLERS; i++)
        if (!impl->msg_handlers[i])
        {
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_AddRef(
                (impl->msg_handlers[i] = handler) );
            token->value = ++impl->next_token;
            LeaveCriticalSection( &impl->cs );
            return S_OK;
        }
    LeaveCriticalSection( &impl->cs );
    return E_BOUNDS;
}
static HRESULT WINAPI mws_remove_MessageReceived( IMessageWebSocket *iface, EventRegistrationToken token )
{
    struct message_websocket *impl = impl_from_IMessageWebSocket( iface );
    UINT i;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < MAX_WS_HANDLERS; i++)
        if (impl->msg_handlers[i])
        {
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs *h = impl->msg_handlers[i];
            impl->msg_handlers[i] = NULL;
            LeaveCriticalSection( &impl->cs );
            ITypedEventHandler_MessageWebSocket_MessageWebSocketMessageReceivedEventArgs_Release( h );
            return S_OK;
        }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}
static const IMessageWebSocketVtbl mws_vtbl =
{
    mws_QueryInterface, mws_AddRef, mws_Release,
    mws_GetIids, mws_GetRuntimeClassName, mws_GetTrustLevel,
    mws_get_Control, mws_get_Information, mws_add_MessageReceived, mws_remove_MessageReceived,
};

/* ---- factory ---- */

struct mws_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};
static inline struct mws_factory *impl_from_factory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct mws_factory, IActivationFactory_iface );
}
static HRESULT WINAPI factory_QI( IActivationFactory *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface; IActivationFactory_AddRef( iface ); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{ return InterlockedIncrement( &impl_from_factory( iface )->ref ); }
static ULONG WINAPI factory_Release( IActivationFactory *iface )
{ return InterlockedDecrement( &impl_from_factory( iface )->ref ); }
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *c, IID **i ) { return E_NOTIMPL; }
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *n )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_Sockets_MessageWebSocket,
                                ARRAY_SIZE(RuntimeClass_Windows_Networking_Sockets_MessageWebSocket) - 1, n );
}
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *l ) { *l = BaseTrust; return S_OK; }
static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct message_websocket *impl;
    WSADATA data;
    TRACE( "ActivateInstance MessageWebSocket.\n" );
    if (!instance) return E_POINTER;
    WSAStartup( MAKEWORD(2, 2), &data );
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMessageWebSocket_iface.lpVtbl = &mws_vtbl;
    impl->IWebSocket_iface.lpVtbl = &websocket_vtbl;
    impl->IClosable_iface.lpVtbl = &ws_closable_vtbl;
    impl->IMessageWebSocketControl_iface.lpVtbl = &ws_msg_control_vtbl;
    impl->IWebSocketControl_iface.lpVtbl = &ws_control_vtbl;
    impl->IWebSocketInformation_iface.lpVtbl = &ws_info_vtbl;
    impl->IOutputStream_iface.lpVtbl = &ws_output_vtbl;
    impl->ref = 1;
    impl->sock = INVALID_SOCKET;
    impl->message_type = SocketMessageType_Binary;
    impl->max_message_size = 0xffffffff;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    InitializeCriticalSectionEx( &impl->write_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    *instance = (IInspectable *)&impl->IMessageWebSocket_iface;
    return S_OK;
}
static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QI, factory_AddRef, factory_Release,
    factory_GetIids, factory_GetRuntimeClassName, factory_GetTrustLevel,
    factory_ActivateInstance,
};
static struct mws_factory mws_factory = { {&factory_vtbl}, 1 };
IActivationFactory *message_websocket_factory = &mws_factory.IActivationFactory_iface;

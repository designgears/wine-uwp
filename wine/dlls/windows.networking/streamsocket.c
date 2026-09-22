/* WinRT Windows.Networking.Sockets.StreamSocket Implementation
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

struct stream_socket
{
    IStreamSocket IStreamSocket_iface;
    IClosable IClosable_iface;
    IStreamSocketControl IStreamSocketControl_iface;
    IStreamSocketInformation IStreamSocketInformation_iface;
    IInputStream IInputStream_iface;
    IOutputStream IOutputStream_iface;
    LONG ref;

    /* A read blocks in recv() for as long as the peer takes to answer, so it must not
     * hold a lock the writer needs - an HTTP client posts its read before sending the
     * request, and would deadlock against its own peer. */
    CRITICAL_SECTION cs;        /* connect/close and the receive path */
    CRITICAL_SECTION write_cs;  /* the send path */


    SOCKET sock;
    BOOL connected;
    BOOL secure;

    IHostName *remote_host;
    HSTRING remote_service;

    CredHandle cred;
    CtxtHandle ctx;
    BOOL have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;

    BYTE *enc_buf;      /* raw bytes read from the socket, not yet decrypted */
    SIZE_T enc_size, enc_len;
    BYTE *dec_buf;      /* plaintext waiting to be handed to the caller */
    SIZE_T dec_size, dec_len, dec_off;

    /* control knobs; kept so the getters answer what the app set */
    boolean no_delay, keep_alive;
    UINT32 outbound_buffer_size;
    SocketQualityOfService qos;
    BYTE hop_limit;
    SocketProtectionLevel protection_level;
};

/* The parameters of one pending operation. The async framework takes an IUnknown,
 * so wrap them in a trivial one rather than stashing them on the socket. */
struct socket_op
{
    IUnknown IUnknown_iface;
    LONG ref;

    IHostName *host;
    HSTRING service;
    SocketProtectionLevel level;

    IBuffer *buffer;
    UINT32 count;
};

static inline struct socket_op *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct socket_op, IUnknown_iface );
}

static HRESULT WINAPI socket_op_QueryInterface( IUnknown *iface, REFIID iid, void **out )
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

static ULONG WINAPI socket_op_AddRef( IUnknown *iface )
{
    struct socket_op *impl = impl_from_IUnknown( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI socket_op_Release( IUnknown *iface )
{
    struct socket_op *impl = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->host) IHostName_Release( impl->host );
        if (impl->buffer) IBuffer_Release( impl->buffer );
        WindowsDeleteString( impl->service );
        free( impl );
    }
    return ref;
}

static const struct IUnknownVtbl socket_op_vtbl = { socket_op_QueryInterface, socket_op_AddRef, socket_op_Release };

static HRESULT socket_op_create( struct socket_op **out )
{
    struct socket_op *op;

    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->IUnknown_iface.lpVtbl = &socket_op_vtbl;
    op->ref = 1;
    *out = op;
    return S_OK;
}

/*
 * Plain socket helpers.
 */

static HRESULT hresult_from_wsa( int err )
{
    return HRESULT_FROM_WIN32( err ? err : WSAGetLastError() );
}

static HRESULT sock_send_all( struct stream_socket *impl, const BYTE *buf, SIZE_T len )
{
    while (len)
    {
        int ret = send( impl->sock, (const char *)buf, min( len, 0x10000 ), 0 );
        if (ret <= 0) return hresult_from_wsa( 0 );
        buf += ret;
        len -= ret;
    }
    return S_OK;
}

/* Read more raw bytes into enc_buf. Returns S_FALSE at end of stream. */
static HRESULT sock_fill( struct stream_socket *impl )
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

    ret = recv( impl->sock, (char *)impl->enc_buf + impl->enc_len, impl->enc_size - impl->enc_len, 0 );
    TRACE( "recv returned %d (buffered %Iu).\n", ret, impl->enc_len );
    if (!ret) return S_FALSE;
    if (ret < 0) return hresult_from_wsa( 0 );
    impl->enc_len += ret;
    return S_OK;
}

static HRESULT dec_append( struct stream_socket *impl, const BYTE *data, SIZE_T len )
{
    if (impl->dec_off)
    {
        memmove( impl->dec_buf, impl->dec_buf + impl->dec_off, impl->dec_len );
        impl->dec_off = 0;
    }
    if (impl->dec_len + len > impl->dec_size)
    {
        SIZE_T size = max( impl->dec_len + len, 16384 );
        BYTE *buf = realloc( impl->dec_buf, size );
        if (!buf) return E_OUTOFMEMORY;
        impl->dec_buf = buf;
        impl->dec_size = size;
    }
    memcpy( impl->dec_buf + impl->dec_len, data, len );
    impl->dec_len += len;
    return S_OK;
}

/*
 * TLS.
 */

static HRESULT tls_handshake( struct stream_socket *impl, const WCHAR *hostname )
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
    if (status != SEC_E_OK)
    {
        WARN( "AcquireCredentialsHandle failed %#lx.\n", status );
        return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
    }
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
            /* the previous call consumed everything but in_bufs[1] */
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
        in_bufs[0].cbBuffer = impl->enc_len;
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        in_bufs[1].pvBuffer = NULL;
        in_bufs[1].cbBuffer = 0;

        status = InitializeSecurityContextW( &impl->cred, &ctx, (WCHAR *)hostname, isc_req_flags, 0, 0,
                                             &in_desc, 0, NULL, &out_desc, &(ULONG){0}, NULL );
        TRACE( "InitializeSecurityContext returned %#lx.\n", status );
    }

    if (status != SEC_E_OK)
    {
        WARN( "TLS handshake failed %#lx.\n", status );
        DeleteSecurityContext( &ctx );
        return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
    }

    if (QueryContextAttributesW( &ctx, SECPKG_ATTR_STREAM_SIZES, &impl->sizes ) != SEC_E_OK)
    {
        DeleteSecurityContext( &ctx );
        return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
    }

    TRACE( "handshake done, in_bufs[1] type %lu size %lu, enc_len %Iu.\n",
           in_bufs[1].BufferType, in_bufs[1].cbBuffer, impl->enc_len );
    /* anything past the last handshake message is application data */
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
    TRACE( "TLS session established.\n" );
    return S_OK;
}

static HRESULT tls_send( struct stream_socket *impl, const BYTE *data, SIZE_T len )
{
    BYTE *buf;
    HRESULT hr = S_OK;

    if (!(buf = malloc( impl->sizes.cbHeader + impl->sizes.cbMaximumMessage + impl->sizes.cbTrailer )))
        return E_OUTOFMEMORY;

    while (len)
    {
        SIZE_T chunk = min( len, impl->sizes.cbMaximumMessage );
        SecBuffer bufs[4] =
        {
            {impl->sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf},
            {chunk, SECBUFFER_DATA, buf + impl->sizes.cbHeader},
            {impl->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf + impl->sizes.cbHeader + chunk},
            {0, SECBUFFER_EMPTY, NULL},
        };
        SecBufferDesc desc = {SECBUFFER_VERSION, ARRAY_SIZE(bufs), bufs};
        SECURITY_STATUS status;

        memcpy( bufs[1].pvBuffer, data, chunk );
        if ((status = EncryptMessage( &impl->ctx, 0, &desc, 0 )) != SEC_E_OK)
        {
            WARN( "EncryptMessage failed %#lx.\n", status );
            hr = HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
            break;
        }
        if (FAILED(hr = sock_send_all( impl, buf, bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer ))) break;
        data += chunk;
        len -= chunk;
    }

    free( buf );
    return hr;
}

/* Decrypt whatever is buffered, reading more if needed. S_FALSE at end of stream. */
static HRESULT tls_fill_plaintext( struct stream_socket *impl )
{
    while (!impl->dec_len)
    {
        SecBuffer bufs[4];
        SecBufferDesc desc = {SECBUFFER_VERSION, ARRAY_SIZE(bufs), bufs};
        SECURITY_STATUS status;
        unsigned int i;
        HRESULT hr;

        if (!impl->enc_len)
        {
            if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE) return hr;
        }

        memset( bufs, 0, sizeof(bufs) );
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = impl->enc_buf;
        bufs[0].cbBuffer = impl->enc_len;

        status = DecryptMessage( &impl->ctx, &desc, 0, NULL );
        TRACE( "DecryptMessage on %Iu bytes returned %#lx.\n", impl->enc_len, status );
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE) return hr;
            continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) return S_FALSE;
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE)
        {
            WARN( "DecryptMessage failed %#lx.\n", status );
            return HRESULT_FROM_WIN32( ERROR_INTERNAL_ERROR );
        }

        for (i = 0; i < ARRAY_SIZE(bufs); i++)
        {
            if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer)
                if (FAILED(hr = dec_append( impl, bufs[i].pvBuffer, bufs[i].cbBuffer ))) return hr;
        }

        impl->enc_len = 0;
        for (i = 0; i < ARRAY_SIZE(bufs); i++)
        {
            if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer)
            {
                memmove( impl->enc_buf, bufs[i].pvBuffer, bufs[i].cbBuffer );
                impl->enc_len = bufs[i].cbBuffer;
                break;
            }
        }
    }
    return S_OK;
}

/* Hand the caller up to count bytes. S_FALSE at end of stream. */
static HRESULT socket_read( struct stream_socket *impl, BYTE *out, UINT32 count, UINT32 *read )
{
    HRESULT hr;

    *read = 0;
    if (!count) return S_OK;

    if (impl->secure)
    {
        if (FAILED(hr = tls_fill_plaintext( impl )) || hr == S_FALSE) return hr;
        *read = min( count, impl->dec_len );
        memcpy( out, impl->dec_buf + impl->dec_off, *read );
        impl->dec_off += *read;
        impl->dec_len -= *read;
        if (!impl->dec_len) impl->dec_off = 0;
        return S_OK;
    }

    if (!impl->enc_len)
    {
        if (FAILED(hr = sock_fill( impl )) || hr == S_FALSE) return hr;
    }
    *read = min( count, impl->enc_len );
    memcpy( out, impl->enc_buf, *read );
    memmove( impl->enc_buf, impl->enc_buf + *read, impl->enc_len - *read );
    impl->enc_len -= *read;
    return S_OK;
}

/* Close() and a reconnect both need the receive lock, which a blocked recv() holds for as
 * long as the peer stays silent. Shut the socket down first, without any lock, so that
 * recv() returns and the lock becomes available. */
static void socket_wake_reader( struct stream_socket *impl )
{
    SOCKET sock = impl->sock;
    if (sock != INVALID_SOCKET) shutdown( sock, SD_BOTH );
}

static void socket_disconnect( struct stream_socket *impl )
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

/*
 * IBuffer access.
 */

static HRESULT buffer_bytes( IBuffer *buffer, BYTE **bytes )
{
    IBufferByteAccess *access;
    HRESULT hr;

    if (FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access ))) return hr;
    hr = IBufferByteAccess_Buffer( access, bytes );
    IBufferByteAccess_Release( access );
    return hr;
}

/*
 * The async callbacks.
 */

static inline struct stream_socket *impl_from_IStreamSocket( IStreamSocket *iface )
{
    return CONTAINING_RECORD( iface, struct stream_socket, IStreamSocket_iface );
}

static HRESULT WINAPI connect_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct stream_socket *impl = impl_from_IStreamSocket( (IStreamSocket *)invoker );
    struct socket_op *op = impl_from_IUnknown( param );
    ADDRINFOW hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP};
    ADDRINFOW *info = NULL, *ai;
    const WCHAR *host = NULL, *service;
    HSTRING canonical = NULL;
    HRESULT hr;

    /* run the blocking work on a threadpool thread, not the caller's */
    if (!called_async) return STATUS_PENDING;

    if (op->host && SUCCEEDED(IHostName_get_CanonicalName( op->host, &canonical )))
        host = WindowsGetStringRawBuffer( canonical, NULL );
    service = WindowsGetStringRawBuffer( op->service, NULL );

    TRACE( "socket %p connecting to %s:%s, protection level %d.\n", impl, debugstr_w(host), debugstr_w(service), op->level );

    if (!host || !*host)
    {
        WindowsDeleteString( canonical );
        return E_INVALIDARG;
    }

    if (GetAddrInfoW( host, service && *service ? service : L"0", &hints, &info ))
    {
        hr = hresult_from_wsa( 0 );
        WARN( "GetAddrInfoW(%s) failed, hr %#lx.\n", debugstr_w(host), hr );
        WindowsDeleteString( canonical );
        return hr;
    }

    socket_wake_reader( impl );
    EnterCriticalSection( &impl->write_cs );
    EnterCriticalSection( &impl->cs );
    socket_disconnect( impl );

    hr = HRESULT_FROM_WIN32( WSAEHOSTUNREACH );
    for (ai = info; ai; ai = ai->ai_next)
    {
        SOCKET s = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
        if (s == INVALID_SOCKET) continue;
        if (!connect( s, ai->ai_addr, ai->ai_addrlen ))
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
        (void)0;
        setsockopt( impl->sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one) );
        impl->protection_level = op->level;
        if (op->level != SocketProtectionLevel_PlainSocket)
        {
            if (FAILED(hr = tls_handshake( impl, host ))) socket_disconnect( impl );
        }
    }

    if (SUCCEEDED(hr))
    {
        if (impl->remote_host) IHostName_Release( impl->remote_host );
        if ((impl->remote_host = op->host)) IHostName_AddRef( op->host );
        WindowsDeleteString( impl->remote_service );
        impl->remote_service = NULL;
        WindowsDuplicateString( op->service, &impl->remote_service );
    }

    LeaveCriticalSection( &impl->cs );
    LeaveCriticalSection( &impl->write_cs );
    WindowsDeleteString( canonical );

    TRACE( "connect returning %#lx.\n", hr );
    return hr;
}

static HRESULT WINAPI read_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct stream_socket *impl = CONTAINING_RECORD( (IInputStream *)invoker, struct stream_socket, IInputStream_iface );
    struct socket_op *op = impl_from_IUnknown( param );
    UINT32 read = 0, capacity = 0;
    BYTE *bytes;
    HRESULT hr;

    /* run the blocking work on a threadpool thread, not the caller's */
    if (!called_async) return STATUS_PENDING;

    if (FAILED(hr = buffer_bytes( op->buffer, &bytes ))) return hr;
    if (FAILED(hr = IBuffer_get_Capacity( op->buffer, &capacity ))) return hr;

    EnterCriticalSection( &impl->cs );
    if (!impl->connected) hr = HRESULT_FROM_WIN32( WSAENOTCONN );
    else hr = socket_read( impl, bytes, min( op->count, capacity ), &read );
    LeaveCriticalSection( &impl->cs );

    if (FAILED(hr)) return hr;
    if (FAILED(hr = IBuffer_put_Length( op->buffer, read ))) return hr;

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)op->buffer;
    IBuffer_AddRef( op->buffer );
    TRACE( "read %u of %u bytes (buffer capacity %u).\n", read, op->count, capacity );
    return S_OK;
}

static HRESULT WINAPI write_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct stream_socket *impl = CONTAINING_RECORD( (IOutputStream *)invoker, struct stream_socket, IOutputStream_iface );
    struct socket_op *op = impl_from_IUnknown( param );
    UINT32 length = 0;
    BYTE *bytes;
    HRESULT hr;

    /* run the blocking work on a threadpool thread, not the caller's */
    if (!called_async) return STATUS_PENDING;

    if (FAILED(hr = buffer_bytes( op->buffer, &bytes ))) return hr;
    if (FAILED(hr = IBuffer_get_Length( op->buffer, &length ))) return hr;

    EnterCriticalSection( &impl->write_cs );
    if (!impl->connected) hr = HRESULT_FROM_WIN32( WSAENOTCONN );
    else if (impl->secure) hr = tls_send( impl, bytes, length );
    else hr = sock_send_all( impl, bytes, length );
    LeaveCriticalSection( &impl->write_cs );

    if (FAILED(hr)) return hr;

    result->vt = VT_UI4;
    result->ulVal = length;
    TRACE( "wrote %u bytes.\n", length );
    return S_OK;
}

static HRESULT WINAPI flush_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_BOOL;
    result->boolVal = VARIANT_TRUE;
    return S_OK;
}

/*
 * IInputStream.
 */

DEFINE_IINSPECTABLE( input_stream, IInputStream, struct stream_socket, IStreamSocket_iface )

static HRESULT WINAPI input_stream_ReadAsync( IInputStream *iface, IBuffer *buffer, UINT32 count,
                                              InputStreamOptions options,
                                              IAsyncOperationWithProgress_IBuffer_UINT32 **operation )
{
    struct stream_socket *impl = impl_from_IInputStream( iface );
    struct socket_op *op;
    HRESULT hr;

    TRACE( "iface %p, buffer %p, count %u, options %#x, operation %p.\n", iface, buffer, count, options, operation );

    if (!buffer || !operation) return E_POINTER;
    if (FAILED(hr = socket_op_create( &op ))) return hr;
    IBuffer_AddRef( (op->buffer = buffer) );
    op->count = count;

    hr = async_read_create( (IUnknown *)iface, &op->IUnknown_iface, read_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}

static const struct IInputStreamVtbl input_stream_vtbl =
{
    input_stream_QueryInterface,
    input_stream_AddRef,
    input_stream_Release,
    /* IInspectable methods */
    input_stream_GetIids,
    input_stream_GetRuntimeClassName,
    input_stream_GetTrustLevel,
    /* IInputStream methods */
    input_stream_ReadAsync,
};

/*
 * IOutputStream.
 */

DEFINE_IINSPECTABLE( output_stream, IOutputStream, struct stream_socket, IStreamSocket_iface )

static HRESULT WINAPI output_stream_WriteAsync( IOutputStream *iface, IBuffer *buffer,
                                                IAsyncOperationWithProgress_UINT32_UINT32 **operation )
{
    struct stream_socket *impl = impl_from_IOutputStream( iface );
    struct socket_op *op;
    HRESULT hr;

    TRACE( "iface %p, buffer %p, operation %p.\n", iface, buffer, operation );

    if (!buffer || !operation) return E_POINTER;
    if (FAILED(hr = socket_op_create( &op ))) return hr;
    IBuffer_AddRef( (op->buffer = buffer) );

    hr = async_write_create( (IUnknown *)iface, &op->IUnknown_iface, write_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}

static HRESULT WINAPI output_stream_FlushAsync( IOutputStream *iface, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return async_operation_boolean_create( (IUnknown *)iface, NULL, flush_callback, operation );
}

static const struct IOutputStreamVtbl output_stream_vtbl =
{
    output_stream_QueryInterface,
    output_stream_AddRef,
    output_stream_Release,
    /* IInspectable methods */
    output_stream_GetIids,
    output_stream_GetRuntimeClassName,
    output_stream_GetTrustLevel,
    /* IOutputStream methods */
    output_stream_WriteAsync,
    output_stream_FlushAsync,
};

/*
 * IStreamSocketControl.
 */

DEFINE_IINSPECTABLE( socket_control, IStreamSocketControl, struct stream_socket, IStreamSocket_iface )

static HRESULT WINAPI socket_control_get_NoDelay( IStreamSocketControl *iface, boolean *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    *value = impl->no_delay;
    return S_OK;
}

static HRESULT WINAPI socket_control_put_NoDelay( IStreamSocketControl *iface, boolean value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    int opt = !!value;
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->no_delay = value;
    if (impl->sock != INVALID_SOCKET) setsockopt( impl->sock, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt) );
    return S_OK;
}

static HRESULT WINAPI socket_control_get_KeepAlive( IStreamSocketControl *iface, boolean *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    *value = impl->keep_alive;
    return S_OK;
}

static HRESULT WINAPI socket_control_put_KeepAlive( IStreamSocketControl *iface, boolean value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    int opt = !!value;
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->keep_alive = value;
    if (impl->sock != INVALID_SOCKET) setsockopt( impl->sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&opt, sizeof(opt) );
    return S_OK;
}

static HRESULT WINAPI socket_control_get_OutboundBufferSizeInBytes( IStreamSocketControl *iface, UINT32 *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    *value = impl->outbound_buffer_size;
    return S_OK;
}

static HRESULT WINAPI socket_control_put_OutboundBufferSizeInBytes( IStreamSocketControl *iface, UINT32 value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    TRACE( "iface %p, value %u.\n", iface, value );
    impl->outbound_buffer_size = value;
    if (impl->sock != INVALID_SOCKET) setsockopt( impl->sock, SOL_SOCKET, SO_SNDBUF, (char *)&value, sizeof(value) );
    return S_OK;
}

static HRESULT WINAPI socket_control_get_QualityOfService( IStreamSocketControl *iface, SocketQualityOfService *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    *value = impl->qos;
    return S_OK;
}

static HRESULT WINAPI socket_control_put_QualityOfService( IStreamSocketControl *iface, SocketQualityOfService value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->qos = value;
    return S_OK;
}

static HRESULT WINAPI socket_control_get_OutboundUnicastHopLimit( IStreamSocketControl *iface, BYTE *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    *value = impl->hop_limit;
    return S_OK;
}

static HRESULT WINAPI socket_control_put_OutboundUnicastHopLimit( IStreamSocketControl *iface, BYTE value )
{
    struct stream_socket *impl = impl_from_IStreamSocketControl( iface );
    TRACE( "iface %p, value %u.\n", iface, value );
    impl->hop_limit = value;
    return S_OK;
}

static const struct IStreamSocketControlVtbl socket_control_vtbl =
{
    socket_control_QueryInterface,
    socket_control_AddRef,
    socket_control_Release,
    /* IInspectable methods */
    socket_control_GetIids,
    socket_control_GetRuntimeClassName,
    socket_control_GetTrustLevel,
    /* IStreamSocketControl methods */
    socket_control_get_NoDelay,
    socket_control_put_NoDelay,
    socket_control_get_KeepAlive,
    socket_control_put_KeepAlive,
    socket_control_get_OutboundBufferSizeInBytes,
    socket_control_put_OutboundBufferSizeInBytes,
    socket_control_get_QualityOfService,
    socket_control_put_QualityOfService,
    socket_control_get_OutboundUnicastHopLimit,
    socket_control_put_OutboundUnicastHopLimit,
};

/*
 * IStreamSocketInformation.
 */

DEFINE_IINSPECTABLE( socket_info, IStreamSocketInformation, struct stream_socket, IStreamSocket_iface )

static HRESULT socket_port_string( SOCKET sock, BOOL local, HSTRING *value )
{
    struct sockaddr_storage addr = {0};
    int len = sizeof(addr);
    WCHAR buf[16];
    USHORT port = 0;

    if (sock == INVALID_SOCKET) return WindowsCreateString( L"", 0, value );
    if (local ? getsockname( sock, (struct sockaddr *)&addr, &len ) : getpeername( sock, (struct sockaddr *)&addr, &len ))
        return WindowsCreateString( L"", 0, value );

    if (addr.ss_family == AF_INET) port = ntohs( ((struct sockaddr_in *)&addr)->sin_port );
    else if (addr.ss_family == AF_INET6) port = ntohs( ((struct sockaddr_in6 *)&addr)->sin6_port );

    swprintf( buf, ARRAY_SIZE(buf), L"%u", port );
    return WindowsCreateString( buf, wcslen( buf ), value );
}

static HRESULT WINAPI socket_info_get_LocalAddress( IStreamSocketInformation *iface, IHostName **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI socket_info_get_LocalPort( IStreamSocketInformation *iface, HSTRING *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketInformation( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return socket_port_string( impl->sock, TRUE, value );
}

static HRESULT WINAPI socket_info_get_RemoteHostName( IStreamSocketInformation *iface, IHostName **value )
{
    struct stream_socket *impl = impl_from_IStreamSocketInformation( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if ((*value = impl->remote_host)) IHostName_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI socket_info_get_RemoteAddress( IStreamSocketInformation *iface, IHostName **value )
{
    return socket_info_get_RemoteHostName( iface, value );
}

static HRESULT WINAPI socket_info_get_RemoteServiceName( IStreamSocketInformation *iface, HSTRING *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketInformation( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl->remote_service, value );
}

static HRESULT WINAPI socket_info_get_RemotePort( IStreamSocketInformation *iface, HSTRING *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketInformation( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return socket_port_string( impl->sock, FALSE, value );
}

static HRESULT WINAPI socket_info_get_RoundTripTimeStatistics( IStreamSocketInformation *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI socket_info_get_BandwidthStatistics( IStreamSocketInformation *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI socket_info_get_ProtectionLevel( IStreamSocketInformation *iface, SocketProtectionLevel *value )
{
    struct stream_socket *impl = impl_from_IStreamSocketInformation( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->protection_level;
    return S_OK;
}

static HRESULT WINAPI socket_info_get_SessionKey( IStreamSocketInformation *iface, IBuffer **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static const struct IStreamSocketInformationVtbl socket_info_vtbl =
{
    socket_info_QueryInterface,
    socket_info_AddRef,
    socket_info_Release,
    /* IInspectable methods */
    socket_info_GetIids,
    socket_info_GetRuntimeClassName,
    socket_info_GetTrustLevel,
    /* IStreamSocketInformation methods */
    socket_info_get_LocalAddress,
    socket_info_get_LocalPort,
    socket_info_get_RemoteHostName,
    socket_info_get_RemoteAddress,
    socket_info_get_RemoteServiceName,
    socket_info_get_RemotePort,
    socket_info_get_RoundTripTimeStatistics,
    socket_info_get_BandwidthStatistics,
    socket_info_get_ProtectionLevel,
    socket_info_get_SessionKey,
};

/*
 * IClosable.
 */

DEFINE_IINSPECTABLE( closable, IClosable, struct stream_socket, IStreamSocket_iface )

static HRESULT WINAPI closable_Close( IClosable *iface )
{
    struct stream_socket *impl = impl_from_IClosable( iface );

    TRACE( "iface %p.\n", iface );

    socket_wake_reader( impl );
    EnterCriticalSection( &impl->write_cs );
    EnterCriticalSection( &impl->cs );
    socket_disconnect( impl );
    LeaveCriticalSection( &impl->cs );
    LeaveCriticalSection( &impl->write_cs );
    return S_OK;
}

static const struct IClosableVtbl closable_vtbl =
{
    closable_QueryInterface,
    closable_AddRef,
    closable_Release,
    /* IInspectable methods */
    closable_GetIids,
    closable_GetRuntimeClassName,
    closable_GetTrustLevel,
    /* IClosable methods */
    closable_Close,
};

/*
 * IStreamSocket.
 */

static HRESULT WINAPI stream_socket_QueryInterface( IStreamSocket *iface, REFIID iid, void **out )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStreamSocket ))
    {
        IInspectable_AddRef( (*out = &impl->IStreamSocket_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IInspectable_AddRef( (*out = &impl->IClosable_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStreamSocketControl ))
    {
        IInspectable_AddRef( (*out = &impl->IStreamSocketControl_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStreamSocketInformation ))
    {
        IInspectable_AddRef( (*out = &impl->IStreamSocketInformation_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IInputStream ))
    {
        IInspectable_AddRef( (*out = &impl->IInputStream_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IOutputStream ))
    {
        IInspectable_AddRef( (*out = &impl->IOutputStream_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_socket_AddRef( IStreamSocket *iface )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI stream_socket_Release( IStreamSocket *iface )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        socket_wake_reader( impl );
        socket_disconnect( impl );
        if (impl->remote_host) IHostName_Release( impl->remote_host );
        WindowsDeleteString( impl->remote_service );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        impl->write_cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->write_cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI stream_socket_GetIids( IStreamSocket *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_socket_GetRuntimeClassName( IStreamSocket *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Networking.Sockets.StreamSocket";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI stream_socket_GetTrustLevel( IStreamSocket *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI stream_socket_get_Control( IStreamSocket *iface, IStreamSocketControl **value )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    IStreamSocketControl_AddRef( &impl->IStreamSocketControl_iface );
    *value = &impl->IStreamSocketControl_iface;
    return S_OK;
}

static HRESULT WINAPI stream_socket_get_Information( IStreamSocket *iface, IStreamSocketInformation **value )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    IStreamSocketInformation_AddRef( &impl->IStreamSocketInformation_iface );
    *value = &impl->IStreamSocketInformation_iface;
    return S_OK;
}

static HRESULT WINAPI stream_socket_get_InputStream( IStreamSocket *iface, IInputStream **value )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    IInputStream_AddRef( (*value = &impl->IInputStream_iface) );
    return S_OK;
}

static HRESULT WINAPI stream_socket_get_OutputStream( IStreamSocket *iface, IOutputStream **value )
{
    struct stream_socket *impl = impl_from_IStreamSocket( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    IOutputStream_AddRef( (*value = &impl->IOutputStream_iface) );
    return S_OK;
}

static HRESULT start_connect( IStreamSocket *iface, IHostName *host, HSTRING service,
                              SocketProtectionLevel level, IAsyncAction **operation )
{
    struct socket_op *op;
    HRESULT hr;

    if (!operation) return E_POINTER;
    if (FAILED(hr = socket_op_create( &op ))) return hr;
    if ((op->host = host)) IHostName_AddRef( host );
    WindowsDuplicateString( service, &op->service );
    op->level = level;

    hr = async_action_param_create( (IUnknown *)iface, &op->IUnknown_iface, connect_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}

static HRESULT endpoint_pair_parts( IEndpointPair *pair, IHostName **host, HSTRING *service )
{
    HRESULT hr;

    *host = NULL;
    *service = NULL;
    if (!pair) return E_INVALIDARG;
    if (FAILED(hr = IEndpointPair_get_RemoteHostName( pair, host ))) return hr;
    return IEndpointPair_get_RemoteServiceName( pair, service );
}

static HRESULT WINAPI stream_socket_ConnectAsync( IStreamSocket *iface, IEndpointPair *pair, IAsyncAction **operation )
{
    IHostName *host;
    HSTRING service;
    HRESULT hr;

    TRACE( "iface %p, pair %p, operation %p.\n", iface, pair, operation );

    if (FAILED(hr = endpoint_pair_parts( pair, &host, &service ))) return hr;
    hr = start_connect( iface, host, service, SocketProtectionLevel_PlainSocket, operation );
    if (host) IHostName_Release( host );
    WindowsDeleteString( service );
    return hr;
}

static HRESULT WINAPI stream_socket_ConnectWithHostNameAsync( IStreamSocket *iface, IHostName *host, HSTRING service,
                                                              IAsyncAction **operation )
{
    TRACE( "iface %p, host %p, service %s, operation %p.\n", iface, host, debugstr_hstring( service ), operation );
    return start_connect( iface, host, service, SocketProtectionLevel_PlainSocket, operation );
}

static HRESULT WINAPI stream_socket_ConnectWithEndpointPairAndProtectionLevelAsync( IStreamSocket *iface,
        IEndpointPair *pair, SocketProtectionLevel level, IAsyncAction **operation )
{
    IHostName *host;
    HSTRING service;
    HRESULT hr;

    TRACE( "iface %p, pair %p, level %d, operation %p.\n", iface, pair, level, operation );

    if (FAILED(hr = endpoint_pair_parts( pair, &host, &service ))) return hr;
    hr = start_connect( iface, host, service, level, operation );
    if (host) IHostName_Release( host );
    WindowsDeleteString( service );
    return hr;
}

static HRESULT WINAPI stream_socket_ConnectWithProtectionLevelAsync( IStreamSocket *iface, IHostName *host,
        HSTRING service, SocketProtectionLevel level, IAsyncAction **operation )
{
    TRACE( "iface %p, host %p, service %s, level %d, operation %p.\n", iface, host,
           debugstr_hstring( service ), level, operation );
    return start_connect( iface, host, service, level, operation );
}

static HRESULT WINAPI upgrade_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct stream_socket *impl = impl_from_IStreamSocket( (IStreamSocket *)invoker );
    struct socket_op *op = impl_from_IUnknown( param );
    HSTRING canonical = NULL;
    HRESULT hr;

    /* run the blocking work on a threadpool thread, not the caller's */
    if (!called_async) return STATUS_PENDING;

    if (op->host) IHostName_get_CanonicalName( op->host, &canonical );

    EnterCriticalSection( &impl->cs );
    if (!impl->connected) hr = HRESULT_FROM_WIN32( WSAENOTCONN );
    else if (impl->secure) hr = S_OK;
    else
    {
        impl->protection_level = op->level;
        hr = tls_handshake( impl, WindowsGetStringRawBuffer( canonical, NULL ) );
    }
    LeaveCriticalSection( &impl->cs );

    WindowsDeleteString( canonical );
    return hr;
}

static HRESULT WINAPI stream_socket_UpgradeToSslAsync( IStreamSocket *iface, SocketProtectionLevel level,
                                                       IHostName *host, IAsyncAction **operation )
{
    struct socket_op *op;
    HRESULT hr;

    TRACE( "iface %p, level %d, host %p, operation %p.\n", iface, level, host, operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = socket_op_create( &op ))) return hr;
    if ((op->host = host)) IHostName_AddRef( host );
    op->level = level;

    hr = async_action_param_create( (IUnknown *)iface, &op->IUnknown_iface, upgrade_callback, operation );
    IUnknown_Release( &op->IUnknown_iface );
    return hr;
}

static const struct IStreamSocketVtbl stream_socket_vtbl =
{
    stream_socket_QueryInterface,
    stream_socket_AddRef,
    stream_socket_Release,
    /* IInspectable methods */
    stream_socket_GetIids,
    stream_socket_GetRuntimeClassName,
    stream_socket_GetTrustLevel,
    /* IStreamSocket methods */
    stream_socket_get_Control,
    stream_socket_get_Information,
    stream_socket_get_InputStream,
    stream_socket_get_OutputStream,
    stream_socket_ConnectAsync,
    stream_socket_ConnectWithHostNameAsync,
    stream_socket_ConnectWithEndpointPairAndProtectionLevelAsync,
    stream_socket_ConnectWithProtectionLevelAsync,
    stream_socket_UpgradeToSslAsync,
};

/*
 * The activation factory.
 */

struct stream_socket_statics
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct stream_socket_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct stream_socket_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct stream_socket_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct stream_socket_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct stream_socket_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Networking.Sockets.StreamSocket";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct stream_socket *impl;
    WSADATA data;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    WSAStartup( MAKEWORD(2, 2), &data );

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStreamSocket_iface.lpVtbl = &stream_socket_vtbl;
    impl->IClosable_iface.lpVtbl = &closable_vtbl;
    impl->IStreamSocketControl_iface.lpVtbl = &socket_control_vtbl;
    impl->IStreamSocketInformation_iface.lpVtbl = &socket_info_vtbl;
    impl->IInputStream_iface.lpVtbl = &input_stream_vtbl;
    impl->IOutputStream_iface.lpVtbl = &output_stream_vtbl;
    impl->ref = 1;
    impl->sock = INVALID_SOCKET;
    impl->no_delay = TRUE;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": stream_socket.cs");
    InitializeCriticalSectionEx( &impl->write_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->write_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": stream_socket.write_cs");

    *instance = (IInspectable *)&impl->IStreamSocket_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
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

static struct stream_socket_statics stream_socket_statics = { {&factory_vtbl}, 1 };

IActivationFactory *stream_socket_factory = &stream_socket_statics.IActivationFactory_iface;

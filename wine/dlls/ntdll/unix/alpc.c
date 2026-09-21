/*
 * Advanced Local Procedure Call
 *
 * Copyright 2026 Zhiyi Zhang for CodeWeavers
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

#if 0
#pragma makedep unix
#endif

#include "ntstatus.h"
#include "wine/debug.h"
#include "wine/server.h"
#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(alpc);



/* same layout rules as ntdll's AlpcGetMessageAttribute, which lives on the PE side */
static SIZE_T attr_header_size( ULONG flags )
{
    static const struct { ULONG attribute; SIZE_T size; } sizes[] =
    {
        { ALPC_MESSAGE_SECURITY_ATTRIBUTE, sizeof(ALPC_SECURITY_ATTR) },
        { ALPC_MESSAGE_VIEW_ATTRIBUTE, sizeof(ALPC_VIEW_ATTR) },
        { ALPC_MESSAGE_CONTEXT_ATTRIBUTE, sizeof(ALPC_CONTEXT_ATTR) },
        { ALPC_MESSAGE_HANDLE_ATTRIBUTE, sizeof(ALPC_HANDLE_ATTR) },
        { ALPC_MESSAGE_TOKEN_ATTRIBUTE, sizeof(ALPC_TOKEN_ATTR) },
        { ALPC_MESSAGE_DIRECT_ATTRIBUTE, sizeof(ALPC_DIRECT_ATTR) },
        { ALPC_MESSAGE_WORK_ON_BEHALF_ATTRIBUTE, sizeof(ALPC_WORK_ON_BEHALF_ATTR) },
    };
    SIZE_T size = sizeof(ALPC_MESSAGE_ATTRIBUTES);
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(sizes); i++) if (flags & sizes[i].attribute) size += sizes[i].size;
    return size;
}

static void *get_attribute( ALPC_MESSAGE_ATTRIBUTES *attrs, ULONG flag )
{
    if (!attrs || (flag & attrs->AllocatedAttributes & ALPC_MESSAGE_ATTRIBUTE_ALL) != flag) return NULL;
    return (unsigned char *)attrs + attr_header_size( attrs->AllocatedAttributes & ~(flag | (flag - 1)) );
}

/* copy a received message into the caller's buffers */
static NTSTATUS fill_received_message( ALPC_PORT_MESSAGE *msg, SIZE_T *msg_size, ALPC_MESSAGE_ATTRIBUTES *attrs,
                                       const struct alpc_receive_reply *reply, const void *data )
{
    SIZE_T total = sizeof(*msg) + reply->size;

    if (msg_size && *msg_size && *msg_size < total)
    {
        if (msg_size) *msg_size = total;
        return STATUS_BUFFER_TOO_SMALL;
    }
    memset( msg, 0, sizeof(*msg) );
    msg->DataLength  = reply->size;
    msg->TotalLength = total;
    msg->Type        = reply->type;
    msg->ClientId.UniqueProcess = ULongToHandle( reply->pid );
    msg->ClientId.UniqueThread  = ULongToHandle( reply->tid );
    msg->MessageId   = reply->msg_id;
    memcpy( msg + 1, data, reply->size );
    TRACE( "received type %u len %u id %u from %04x.%04x\n", msg->Type, msg->DataLength, (unsigned int)msg->MessageId,
           (unsigned int)reply->pid, (unsigned int)reply->tid );
    if (msg_size) *msg_size = total;

    if (attrs)
    {
        attrs->ValidAttributes = 0;
        if (attrs->AllocatedAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE)
        {
            ALPC_CONTEXT_ATTR *ctx = get_attribute( attrs, ALPC_MESSAGE_CONTEXT_ATTRIBUTE );
            ctx->PortContext    = (void *)(ULONG_PTR)reply->port_context;
            ctx->MessageContext = (void *)(ULONG_PTR)reply->context;
            ctx->Sequence       = reply->sequence;
            ctx->MessageId      = reply->msg_id;
            ctx->CallbackId     = 0;
            attrs->ValidAttributes |= ALPC_MESSAGE_CONTEXT_ATTRIBUTE;
        }
        if ((attrs->AllocatedAttributes & ALPC_MESSAGE_HANDLE_ATTRIBUTE) && reply->msg_handle)
        {
            ALPC_HANDLE_ATTR *h = get_attribute( attrs, ALPC_MESSAGE_HANDLE_ATTRIBUTE );
            h->Flags         = 0;
            h->Handle        = wine_server_ptr_handle( reply->msg_handle );
            h->ObjectType    = 0;
            h->DesiredAccess = 0;
            attrs->ValidAttributes |= ALPC_MESSAGE_HANDLE_ATTRIBUTE;
        }
    }
    return STATUS_SUCCESS;
}

static BOOL last_receive_refused;  /* set by receive_message when the port's connection was refused */

/* receive a message, waiting on the port until one arrives */
static NTSTATUS receive_message( HANDLE port, unsigned int reply_id, ALPC_PORT_MESSAGE *msg, SIZE_T *msg_size,
                                 ALPC_MESSAGE_ATTRIBUTES *attrs, LARGE_INTEGER *timeout )
{
    SIZE_T buffer_size = (msg_size && *msg_size > sizeof(*msg)) ? *msg_size - sizeof(*msg) : 65535;
    void *buffer;
    unsigned int status;

    if (!(buffer = malloc( buffer_size ))) return STATUS_NO_MEMORY;
    for (;;)
    {
        struct alpc_receive_reply info;

        SERVER_START_REQ( alpc_receive )
        {
            req->handle   = wine_server_obj_handle( port );
            req->reply_id = reply_id;
            wine_server_set_reply( req, buffer, buffer_size );
            status = wine_server_call( req );
            info = *reply;
        }
        SERVER_END_REQ;

        if (status == STATUS_PENDING)
        {
            status = NtWaitForSingleObject( port, FALSE, timeout );
            if (status == STATUS_TIMEOUT || status == STATUS_USER_APC || status == STATUS_ALERTED) break;
            if (status) break;
            continue;
        }
        if (!status)
        {
            last_receive_refused = info.refused;
            status = fill_received_message( msg, msg_size, attrs, &info, buffer );
        }
        else if (status == STATUS_BUFFER_TOO_SMALL && msg_size) *msg_size = sizeof(*msg) + info.size;
        break;
    }
    free( buffer );
    return status;
}

/* extract the transferred handle and message context from the caller's send attributes */
static void get_send_attributes( ALPC_MESSAGE_ATTRIBUTES *attrs, obj_handle_t *handle, unsigned int *access, client_ptr_t *context )
{
    *handle = 0; *access = 0; *context = 0;
    if (!attrs) return;
    if (attrs->ValidAttributes & ALPC_MESSAGE_HANDLE_ATTRIBUTE)
    {
        ALPC_HANDLE_ATTR *h = get_attribute( attrs, ALPC_MESSAGE_HANDLE_ATTRIBUTE );
        if (h && h->Handle && !(h->Flags & 0x00010000 /* ALPC_HANDLEFLG_INDIRECT */))
        {
            *handle = wine_server_obj_handle( h->Handle );
            *access = h->DesiredAccess;
        }
        else if (h && (h->Flags & 0x00010000)) FIXME( "indirect handle attribute not supported\n" );
    }
    if (attrs->ValidAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE)
    {
        ALPC_CONTEXT_ATTR *ctx = get_attribute( attrs, ALPC_MESSAGE_CONTEXT_ATTRIBUTE );
        if (ctx) *context = (client_ptr_t)(ULONG_PTR)ctx->MessageContext;
    }
    if (attrs->ValidAttributes & ~(ALPC_MESSAGE_HANDLE_ATTRIBUTE | ALPC_MESSAGE_CONTEXT_ATTRIBUTE |
                                   ALPC_MESSAGE_SECURITY_ATTRIBUTE | ALPC_MESSAGE_WORK_ON_BEHALF_ATTRIBUTE))
        FIXME( "unsupported send attributes %#x\n", (unsigned int)attrs->ValidAttributes );
}

/******************************************************************************
 *              NtAlpcAcceptConnectPort  (NTDLL.@)
 */
NTSTATUS WINAPI NtAlpcAcceptConnectPort( HANDLE *communication_port, HANDLE connection_port,
                                         ULONG flags, OBJECT_ATTRIBUTES *attr, ALPC_PORT_ATTRIBUTES *port_attr,
                                         void *port_context, ALPC_PORT_MESSAGE *connection_msg,
                                         ALPC_MESSAGE_ATTRIBUTES *msg_attr, BOOLEAN accept )
{
    unsigned int status;

    TRACE( "%p, %p, %#x, %p, %p, %p, %p, %p, %d\n", communication_port, connection_port, (int)flags, attr,
           port_attr, port_context, connection_msg, msg_attr, accept );

    if (!connection_msg) return STATUS_INVALID_PARAMETER;
    if (communication_port) *communication_port = NULL;

    SERVER_START_REQ( alpc_accept_connect )
    {
        req->handle  = wine_server_obj_handle( connection_port );
        req->msg_id  = connection_msg->MessageId;
        req->accept  = accept;
        req->flags   = port_attr ? port_attr->Flags : 0;
        req->context = (client_ptr_t)(ULONG_PTR)port_context;
        if (connection_msg->DataLength) wine_server_add_data( req, connection_msg + 1, connection_msg->DataLength );
        if (!(status = wine_server_call( req )) && communication_port)
            *communication_port = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return status;
}

/******************************************************************************
 *              NtAlpcConnectPort  (NTDLL.@)
 */
NTSTATUS WINAPI NtAlpcConnectPort( HANDLE *port_handle, UNICODE_STRING *port_name,
                                   OBJECT_ATTRIBUTES *obj_attr, ALPC_PORT_ATTRIBUTES *port_attr,
                                   DWORD flags, PSID required_server_sid,
                                   ALPC_PORT_MESSAGE *connect_msg, SIZE_T *connect_msg_size,
                                   ALPC_MESSAGE_ATTRIBUTES *send_msg_attr,
                                   ALPC_MESSAGE_ATTRIBUTES *recv_msg_attr, LARGE_INTEGER *timeout )
{
    OBJECT_ATTRIBUTES name_attr;
    struct object_attributes *objattr;
    data_size_t len;
    unsigned int status;
    obj_handle_t handle = 0, xfer_handle;
    unsigned int xfer_access, msg_id = 0;
    client_ptr_t context;
    HANDLE port;

    TRACE( "%p, %s, %p, %p, %#x, %p, %p, %p, %p, %p, %p\n", port_handle, debugstr_us( port_name ), obj_attr,
           port_attr, (int)flags, required_server_sid, connect_msg, connect_msg_size, send_msg_attr,
           recv_msg_attr, timeout );

    if (!port_handle || !port_name) return STATUS_INVALID_PARAMETER;
    *port_handle = NULL;

    InitializeObjectAttributes( &name_attr, port_name, 0, NULL, NULL );
    if ((status = wine_server_alloc_object_attributes( &name_attr, &objattr, &len ))) return status;
    get_send_attributes( send_msg_attr, &xfer_handle, &xfer_access, &context );

    SERVER_START_REQ( alpc_connect_port )
    {
        req->flags         = port_attr ? port_attr->Flags : 0;
        req->handle        = xfer_handle;
        req->handle_access = xfer_access;
        wine_server_add_data( req, objattr, len );
        if (connect_msg && connect_msg->DataLength) wine_server_add_data( req, connect_msg + 1, connect_msg->DataLength );
        if (!(status = wine_server_call( req )))
        {
            handle = reply->handle;
            msg_id = reply->msg_id;
        }
    }
    SERVER_END_REQ;
    free( objattr );
    if (status)
    {
        WARN( "connect to %s failed, status %#x\n", debugstr_us( port_name ), status );
        return status;
    }
    port = wine_server_ptr_handle( handle );

    /* wait for the server's accept / refuse */
    {
        char buffer[sizeof(ALPC_PORT_MESSAGE) + 65535];
        ALPC_PORT_MESSAGE *reply_msg = (ALPC_PORT_MESSAGE *)buffer;
        SIZE_T size = sizeof(buffer);

        status = receive_message( port, msg_id, reply_msg, &size, recv_msg_attr, timeout );
        if (!status && connect_msg && connect_msg_size)
        {
            SIZE_T copy = min( *connect_msg_size, size );
            memcpy( connect_msg, reply_msg, copy );
            *connect_msg_size = copy;
        }
        if (!status && (reply_msg->Type == ALPC_MESSAGE_TYPE_PORT_CLOSED || last_receive_refused))
            status = STATUS_PORT_CONNECTION_REFUSED;
    }
    if (status)
    {
        NtClose( port );
        return status == STATUS_TIMEOUT ? STATUS_TIMEOUT : status;
    }
    *port_handle = port;
    TRACE( "connected %p to %s\n", port, debugstr_us( port_name ) );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtAlpcCreatePort( HANDLE *port_handle, OBJECT_ATTRIBUTES *attr, ALPC_PORT_ATTRIBUTES *port_attr )
{
    struct object_attributes *objattr = NULL;
    unsigned int status;
    data_size_t len = 0;

    TRACE( "%p, %p, %p.\n", port_handle, attr, port_attr );

    if (!port_handle) return STATUS_ACCESS_VIOLATION;

    *port_handle = NULL;

    if (port_attr)
        TRACE( "port attributes: flags %#x qos (length %#x impersonation_level %d tracking_mode %d "
               "effective only %d) max_msg_length %#lx memory_bandwidth %#lx max_pool_usage %#lx "
               "max_section_size %#lx max_view_size %#lx max_total_section_size %#lx.\n",
               port_attr->Flags, port_attr->SecurityQos.Length, port_attr->SecurityQos.ImpersonationLevel,
               port_attr->SecurityQos.ContextTrackingMode, port_attr->SecurityQos.EffectiveOnly,
               port_attr->MaxMessageLength, port_attr->MemoryBandwidth, port_attr->MaxPoolUsage,
               port_attr->MaxSectionSize, port_attr->MaxViewSize, port_attr->MaxTotalSectionSize );

    if (port_attr && port_attr->Flags & 0x100000) return STATUS_INVALID_PARAMETER;

    if (attr)
    {
        if (attr->ObjectName) TRACE( "name %s.\n", debugstr_us( attr->ObjectName ) );
        if ((status = wine_server_alloc_object_attributes( attr, &objattr, &len ))) return status;
    }

    SERVER_START_REQ( alpc_create_port )
    {
        if (port_attr)
        {
            req->flags = port_attr->Flags;
            req->max_msg_len = port_attr->MaxMessageLength;
        }
        else
        {
            req->flags = 0;
            req->max_msg_len = 65535;
        }
        wine_server_add_data( req, objattr, len );
        if (!(status = wine_server_call( req )))
        {
            *port_handle = wine_server_ptr_handle( reply->handle );
            TRACE( "created %p.\n", *port_handle );
        }
        else
        {
            WARN( "status %#x.\n", status );
        }
    }
    SERVER_END_REQ;
    free( objattr );
    return status;
}


/******************************************************************************
 *              NtAlpcDisconnectPort  (NTDLL.@)
 */
NTSTATUS WINAPI NtAlpcDisconnectPort( HANDLE port_handle, ULONG flags )
{
    unsigned int status;

    TRACE( "%p, %#x\n", port_handle, (unsigned int)flags );

    SERVER_START_REQ( alpc_disconnect )
    {
        req->handle = wine_server_obj_handle( port_handle );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    return status;
}

/******************************************************************************
 *              NtAlpcSendWaitReceivePort  (NTDLL.@)
 */
NTSTATUS WINAPI NtAlpcSendWaitReceivePort( HANDLE port_handle, ULONG flags, ALPC_PORT_MESSAGE *send_msg,
                                           ALPC_MESSAGE_ATTRIBUTES *send_msg_attr, ALPC_PORT_MESSAGE *recv_msg,
                                           SIZE_T *recv_msg_size, ALPC_MESSAGE_ATTRIBUTES *recv_msg_attr,
                                           LARGE_INTEGER *timeout )
{
    unsigned int status = STATUS_SUCCESS, reply_id = 0;

    TRACE( "%p, %#x, %p, %p, %p, %p, %p, %p\n", port_handle, (unsigned int)flags, send_msg, send_msg_attr,
           recv_msg, recv_msg_size, recv_msg_attr, timeout );

    if (send_msg)
    {
        obj_handle_t xfer_handle;
        unsigned int xfer_access;
        client_ptr_t context;

        get_send_attributes( send_msg_attr, &xfer_handle, &xfer_access, &context );
        TRACE( "sending type %u len %u id %u total %u flags %#x header %s\n", send_msg->Type, send_msg->DataLength,
               (unsigned int)send_msg->MessageId, send_msg->TotalLength, (unsigned int)flags,
               wine_dbgstr_an( (const char *)send_msg, sizeof(*send_msg) + min( send_msg->DataLength, 32 ) ) );
        SERVER_START_REQ( alpc_send )
        {
            req->handle        = wine_server_obj_handle( port_handle );
            req->flags         = flags;
            req->msg_id        = send_msg->MessageId;
            req->context       = context;
            req->msg_handle    = xfer_handle;
            req->handle_access = xfer_access;
            if (send_msg->DataLength) wine_server_add_data( req, send_msg + 1, send_msg->DataLength );
            if (!(status = wine_server_call( req ))) reply_id = reply->msg_id;
        }
        SERVER_END_REQ;
        if (status)
        {
            WARN( "send on %p failed, status %#x\n", port_handle, status );
            return status;
        }
        if (!(flags & ALPC_MSGFLG_SYNC_REQUEST)) reply_id = 0;
    }

    if (recv_msg)
        status = receive_message( port_handle, reply_id, recv_msg, recv_msg_size, recv_msg_attr, timeout );

    return status;
}

/******************************************************************************
 *              NtAlpcImpersonateClientOfPort  (NTDLL.@)
 */
NTSTATUS WINAPI NtAlpcImpersonateClientOfPort( HANDLE port_handle, ALPC_PORT_MESSAGE *msg, void *reserved )
{
    HANDLE process_token, token;
    OBJECT_ATTRIBUTES attr;
    SECURITY_QUALITY_OF_SERVICE qos = { sizeof(qos), SecurityImpersonation, SECURITY_STATIC_TRACKING, FALSE };
    NTSTATUS status;

    FIXME( "%p, %p, %p: semi-stub, impersonating the current process token\n", port_handle, msg, reserved );

    /* every client is the same user on this host; impersonate our own token so that callers which
     * expect an impersonation token to be present (and query it) keep working */
    if ((status = NtOpenProcessToken( GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process_token ))) return status;
    InitializeObjectAttributes( &attr, NULL, 0, NULL, NULL );
    attr.SecurityQualityOfService = &qos;
    status = NtDuplicateToken( process_token, TOKEN_IMPERSONATE | TOKEN_QUERY, &attr, FALSE, TokenImpersonation, &token );
    NtClose( process_token );
    if (status) return status;
    status = NtSetInformationThread( GetCurrentThread(), ThreadImpersonationToken, &token, sizeof(token) );
    NtClose( token );
    return status;
}

/******************************************************************************
 *              NtAlpcConnectPortEx  (NTDLL.@)
 *
 * Windows 8+ variant taking OBJECT_ATTRIBUTES for the connection port instead of a name.
 * Used by CoreMessaging.dll to reach \BaseNamedObjects\CoreMessagingRegistrar.
 */
NTSTATUS WINAPI NtAlpcConnectPortEx( HANDLE *port_handle, OBJECT_ATTRIBUTES *connection_attr,
                                     OBJECT_ATTRIBUTES *client_attr, ALPC_PORT_ATTRIBUTES *port_attr,
                                     ULONG flags, SECURITY_DESCRIPTOR *server_sd, ALPC_PORT_MESSAGE *msg,
                                     SIZE_T *msg_len, ALPC_MESSAGE_ATTRIBUTES *out_attr,
                                     ALPC_MESSAGE_ATTRIBUTES *in_attr, LARGE_INTEGER *timeout )
{
    TRACE( "%p %p %p %p %#x %p %p %p %p %p %p\n", port_handle, connection_attr, client_attr, port_attr,
           (int)flags, server_sd, msg, msg_len, out_attr, in_attr, timeout );
    if (!connection_attr || !connection_attr->ObjectName) return STATUS_INVALID_PARAMETER;
    if (connection_attr->RootDirectory) FIXME( "root directory %p ignored\n", connection_attr->RootDirectory );
    return NtAlpcConnectPort( port_handle, connection_attr->ObjectName, client_attr, port_attr, flags,
                              server_sd, msg, msg_len, out_attr, in_attr, timeout );
}

/******************************************************************************
 *              NtAlpcQueryInformation  (NTDLL.@)
 */
NTSTATUS WINAPI NtAlpcQueryInformation( HANDLE port_handle, ULONG info_class, void *info, ULONG len, ULONG *ret_len )
{
    unsigned int status;

    TRACE( "%p %u %p %u %p\n", port_handle, (int)info_class, info, (int)len, ret_len );

    if (info_class != 0 /* AlpcBasicInformation */)
    {
        FIXME( "class %u not supported\n", (int)info_class );
        return STATUS_INVALID_INFO_CLASS;
    }
    if (len < sizeof(ALPC_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    SERVER_START_REQ( alpc_query_info )
    {
        req->handle = wine_server_obj_handle( port_handle );
        if (!(status = wine_server_call( req )))
        {
            ALPC_BASIC_INFORMATION *basic = info;
            basic->Flags       = reply->flags;
            basic->SequenceNo  = reply->sequence;
            basic->PortContext = (void *)(ULONG_PTR)reply->context;
            if (ret_len) *ret_len = sizeof(*basic);
        }
    }
    SERVER_END_REQ;
    return status;
}

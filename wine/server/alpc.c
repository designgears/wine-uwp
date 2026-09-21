/*
 * Server-side ALPC (Advanced Local Procedure Call) ports
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Model: a named connection port receives connection requests; accepting one creates a pair of
 * communication ports (server side and client side) whose queues feed each other. Messages carry an
 * id, type, sender ids, an optional transferred handle and a message context. Ports are waitable and
 * signaled while their queue is non-empty (or a pending connection has been decided).
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>

#include "windef.h"
#include "winternl.h"
#include "ntstatus.h"

#include "handle.h"
#include "thread.h"
#include "process.h"
#include "security.h"
#include "request.h"

static const WCHAR alpc_port_name[] = {'A','L','P','C',' ','P','o','r','t'};

struct type_descr alpc_port_type =
{
    { alpc_port_name, sizeof(alpc_port_name) }, /* name */
    ALPC_PORT_ALL_ACCESS,                       /* valid_access */
    {                                           /* mapping */
         STANDARD_RIGHTS_READ | ALPC_PORT_QUERY_STATE,
         DELETE | ALPC_PORT_QUERY_STATE,
         0,
         ALPC_PORT_ALL_ACCESS
    },
};

enum alpc_port_enum_type
{
    CONNECTION_PORT,
    COMMUNICATION_PORT
};

enum alpc_port_status
{
    UNINITIALIZED,   /* client port waiting for accept/refuse */
    CONNECTED,
    REFUSED,
    DISCONNECTED
};

struct alpc_msg
{
    struct list         entry;          /* entry in port queue or pending list */
    unsigned int        id;             /* message id */
    unsigned short      type;           /* ALPC_MESSAGE_TYPE_* */
    process_id_t        pid;            /* sender process id */
    thread_id_t         tid;            /* sender thread id */
    client_ptr_t        context;        /* message context of the sender */
    struct alpc_port   *reply_port;     /* sender port (for connection requests: the connecting client port) */
    struct alpc_port   *dest_port;      /* server-side communication port the message was addressed to, if any */
    struct object      *handle_obj;     /* transferred object, if any */
    unsigned int        handle_access;  /* access for the transferred handle */
    data_size_t         size;           /* data size */
    char                data[1];        /* message data */
};

struct alpc_port
{
    struct object            obj;                   /* object header */
    enum alpc_port_enum_type type;                  /* communication port or connection port */
    enum alpc_port_status    status;                /* port status */
    struct thread           *thread;                /* thread owning the port */
    unsigned int             flags;                 /* flags in port attributes */
    mem_size_t               max_msg_len;           /* max message length in port attributes */
    struct alpc_port        *peer;                  /* other end of a communication port */
    struct alpc_port        *connection;            /* connection port whose queue a server-side communication port shares */
    client_ptr_t             context;               /* port context (server side) */
    unsigned int             sequence;              /* messages received */
    struct list              queue;                 /* incoming messages */
    struct list              pending;               /* received connection requests awaiting accept */
    struct object           *sync;                  /* signaled while a message (or a connection verdict) is waiting */
};

struct alpc_port_init_data
{
    enum alpc_port_enum_type type;
    unsigned int             flags;
    mem_size_t               max_msg_len;
};

static unsigned int next_msg_id = 1;

static void alpc_port_dump( struct object *obj, int verbose );
static bool alpc_port_init( struct object *obj, const void *init_data );
static struct object *alpc_port_get_sync( struct object *obj );
static void alpc_port_destroy( struct object *obj );

static const struct object_ops alpc_port_ops =
{
    .size         = sizeof(struct alpc_port),
    .type         = &alpc_port_type,
    .dump         = alpc_port_dump,
    .init         = alpc_port_init,
    .get_sync     = alpc_port_get_sync,
    .destroy      = alpc_port_destroy
};

static void alpc_port_dump( struct object *obj, int verbose )
{
    struct alpc_port *port = (struct alpc_port *)obj;
    assert( obj->ops == &alpc_port_ops );
    fprintf( stderr, "ALPC Port type=%d status=%d peer=%p queued=%d\n", port->type, port->status, port->peer,
             list_empty( &port->queue ) ? 0 : 1 );
}

static bool alpc_port_init( struct object *obj, const void *init_data )
{
    struct alpc_port *port = (struct alpc_port *)obj;
    const struct alpc_port_init_data *data = init_data;

    port->type        = data->type;
    port->flags       = data->flags;
    port->max_msg_len = data->max_msg_len;
    port->status      = data->type == CONNECTION_PORT ? CONNECTED : UNINITIALIZED;
    port->thread      = (struct thread *)grab_object( current );
    port->peer        = NULL;
    port->connection  = NULL;
    port->context     = 0;
    port->sequence    = 0;
    list_init( &port->queue );
    list_init( &port->pending );
    port->sync = create_internal_sync( 1, 0 );
    return !!port->sync;
}

static struct object *alpc_port_get_sync( struct object *obj )
{
    struct alpc_port *port = (struct alpc_port *)obj;
    assert( obj->ops == &alpc_port_ops );
    /* server-side communication ports share their connection port's queue, so wait on its sync */
    if (port->connection && port->status != DISCONNECTED) return grab_object( port->connection->sync );
    return grab_object( port->sync );
}

/* a port is signaled while something is waiting to be picked up */
static void update_signal( struct alpc_port *port )
{
    if (!list_empty( &port->queue ) || port->status == REFUSED || port->status == DISCONNECTED)
        signal_sync( port->sync );
    else
        reset_sync( port->sync );
}

static void free_msg( struct alpc_msg *msg )
{
    if (msg->reply_port) release_object( msg->reply_port );
    if (msg->dest_port) release_object( msg->dest_port );
    if (msg->handle_obj) release_object( msg->handle_obj );
    free( msg );
}

static struct alpc_msg *alloc_msg( unsigned short type, const void *data, data_size_t size )
{
    struct alpc_msg *msg = mem_alloc( offsetof( struct alpc_msg, data[size] ) );
    if (!msg) return NULL;
    msg->id            = next_msg_id++;
    msg->type          = type;
    msg->pid           = current->process->id;
    msg->tid           = current->id;
    msg->context       = 0;
    msg->reply_port    = NULL;
    msg->dest_port     = NULL;
    msg->handle_obj    = NULL;
    msg->handle_access = 0;
    msg->size          = size;
    if (size) memcpy( msg->data, data, size );
    if (!next_msg_id) next_msg_id = 1;
    return msg;
}

/* the port whose queue holds messages for this port: server-side ports share their connection port's queue */
static struct alpc_port *queue_owner( struct alpc_port *port )
{
    return port->connection ? port->connection : port;
}

static void queue_msg( struct alpc_port *port, struct alpc_msg *msg )
{
    struct alpc_port *owner = queue_owner( port );
    if (owner != port)
    {
        if (msg->dest_port) release_object( msg->dest_port );
        msg->dest_port = (struct alpc_port *)grab_object( port );
    }
    list_add_tail( &owner->queue, &msg->entry );
    update_signal( owner );
}

static struct alpc_port *create_port( enum alpc_port_enum_type type, unsigned int flags, mem_size_t max_msg_len )
{
    struct alpc_port_init_data data = { .type = type, .flags = flags, .max_msg_len = max_msg_len };
    struct object_params params = { .ops = &alpc_port_ops, .init_data = &data };
    return create_named_object( &params );  /* unnamed */
}

/* tell the peer that we are gone */
static void disconnect_port( struct alpc_port *port )
{
    struct alpc_port *peer = port->peer;
    struct alpc_msg *msg, *next;

    if (port->status != DISCONNECTED)
    {
        port->status = DISCONNECTED;
        update_signal( port );
    }
    if (peer)
    {
        port->peer = NULL;
        peer->peer = NULL;
        if (peer->status != DISCONNECTED)
        {
            peer->status = DISCONNECTED;
            if ((msg = alloc_msg( ALPC_MESSAGE_TYPE_PORT_CLOSED, NULL, 0 ))) queue_msg( peer, msg );
            else update_signal( peer );
        }
        release_object( peer );
    }
    /* refuse whatever connection requests are still pending on a connection port */
    LIST_FOR_EACH_ENTRY_SAFE( msg, next, &port->pending, struct alpc_msg, entry )
    {
        list_remove( &msg->entry );
        if (msg->reply_port && msg->reply_port->status == UNINITIALIZED)
        {
            msg->reply_port->status = REFUSED;
            update_signal( msg->reply_port );
        }
        free_msg( msg );
    }
}

static void alpc_port_destroy( struct object *obj )
{
    struct alpc_port *port = (struct alpc_port *)obj;
    struct alpc_msg *msg, *next;

    assert( obj->ops == &alpc_port_ops );
    disconnect_port( port );
    LIST_FOR_EACH_ENTRY_SAFE( msg, next, &port->queue, struct alpc_msg, entry )
    {
        list_remove( &msg->entry );
        if (msg->type == ALPC_MESSAGE_TYPE_CONNECTION_REQUEST && msg->reply_port &&
            msg->reply_port->status == UNINITIALIZED)
        {
            msg->reply_port->status = REFUSED;
            update_signal( msg->reply_port );
        }
        free_msg( msg );
    }
    if (port->connection) release_object( port->connection );
    if (port->sync) release_object( port->sync );
    release_object( port->thread );
}

static struct alpc_port *get_port_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct alpc_port *)get_handle_obj( process, handle, access, &alpc_port_ops );
}

/* create a connection port */
DECL_HANDLER(alpc_create_port)
{
    struct alpc_port *alpc_port;
    struct alpc_port_init_data data = { .type = CONNECTION_PORT, .flags = req->flags,
                                        .max_msg_len = req->max_msg_len };
    struct object_params params = { .ops = &alpc_port_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;

    if ((alpc_port = create_named_object( &params )))
    {
        reply->handle = alloc_handle( current->process, alpc_port, ALPC_PORT_ALL_ACCESS, params.attr );
        release_object( alpc_port );
    }
    if (params.root) release_object( params.root );
}

/* connect to a connection port: creates the client communication port and queues the request */
DECL_HANDLER(alpc_connect_port)
{
    struct object_params params = { .ops = &alpc_port_ops };
    struct alpc_port *conn, *client;
    struct alpc_msg *msg;
    const void *data;
    data_size_t size;
    if (!get_req_object_attributes( &params )) return;
    data = get_req_data_after_objattr( &params, &size );


    if (!(conn = open_named_object( &params )))
    {
        if (params.root) release_object( params.root );
        return;
    }
    if (params.root) release_object( params.root );

    if (conn->type != CONNECTION_PORT || conn->status != CONNECTED)
    {
        set_error( STATUS_INVALID_PORT_HANDLE );
        release_object( conn );
        return;
    }
    if (!(client = create_port( COMMUNICATION_PORT, req->flags, conn->max_msg_len )))
    {
        release_object( conn );
        return;
    }
    if (!(msg = alloc_msg( ALPC_MESSAGE_TYPE_CONNECTION_REQUEST, data, size )))
    {
        release_object( client );
        release_object( conn );
        return;
    }
    msg->reply_port = (struct alpc_port *)grab_object( client );
    if (req->handle)
    {
        if ((msg->handle_obj = get_handle_obj( current->process, req->handle, 0, NULL )))
            msg->handle_access = req->handle_access;
    }
    reply->msg_id = msg->id;
    queue_msg( conn, msg );
    reply->handle = alloc_handle( current->process, client, ALPC_PORT_ALL_ACCESS, 0 );
    release_object( client );
    release_object( conn );
}

/* accept or refuse a received connection request */
DECL_HANDLER(alpc_accept_connect)
{
    struct alpc_port *conn, *client, *server;
    struct alpc_msg *msg, *pending = NULL, *reply_msg;

    if (!(conn = get_port_obj( current->process, req->handle, 0 ))) return;
    LIST_FOR_EACH_ENTRY( msg, &conn->pending, struct alpc_msg, entry )
        if (msg->id == req->msg_id) { pending = msg; break; }
    if (!pending)
    {
        set_error( STATUS_REPLY_MESSAGE_MISMATCH );
        release_object( conn );
        return;
    }
    list_remove( &pending->entry );
    client = pending->reply_port;

    if (!req->accept || !client || client->status != UNINITIALIZED)
    {
        struct alpc_msg *reply_msg;
        if (client && client->status == UNINITIALIZED)
        {
            client->status = REFUSED;
            /* the refusal may carry data; deliver it as the connection reply */
            if ((reply_msg = alloc_msg( ALPC_MESSAGE_TYPE_CONNECTION_REPLY, get_req_data(), get_req_data_size() )))
            {
                reply_msg->id = pending->id;
                queue_msg( client, reply_msg );
            }
            update_signal( client );
        }
        free_msg( pending );
        release_object( conn );
        return;
    }
    if (!(server = create_port( COMMUNICATION_PORT, req->flags, conn->max_msg_len )))
    {
        client->status = REFUSED;
        update_signal( client );
        free_msg( pending );
        release_object( conn );
        return;
    }
    server->context = req->context;
    server->status  = CONNECTED;
    server->peer    = (struct alpc_port *)grab_object( client );
    client->peer    = (struct alpc_port *)grab_object( server );
    client->status  = CONNECTED;
    server->connection = (struct alpc_port *)grab_object( conn );

    if ((reply_msg = alloc_msg( ALPC_MESSAGE_TYPE_CONNECTION_REPLY, get_req_data(), get_req_data_size() )))
    {
        reply_msg->id = pending->id;
        queue_msg( client, reply_msg );
    }
    else update_signal( client );

    reply->handle = alloc_handle( current->process, server, ALPC_PORT_ALL_ACCESS, 0 );
    free_msg( pending );
    release_object( server );
    release_object( conn );
}

/* send a message to the peer of a communication port, or reply to a pending request */
DECL_HANDLER(alpc_send)
{
    struct alpc_port *port, *dest = NULL, *owner;
    struct alpc_msg *msg, *cur, *pending = NULL;
    unsigned short type;

    if (!(port = get_port_obj( current->process, req->handle, 0 ))) return;
    owner = queue_owner( port );

    /* a message whose id matches a request still waiting on this queue is the reply to it */
    if (req->msg_id)
    {
        LIST_FOR_EACH_ENTRY( cur, &owner->pending, struct alpc_msg, entry )
            if (cur->id == req->msg_id) { pending = cur; break; }
    }
    if (pending)
    {
        dest = pending->reply_port;
        type = ALPC_MESSAGE_TYPE_REPLY;
    }
    else if ((req->flags & ALPC_MSGFLG_RELEASE_MESSAGE) && port->type == CONNECTION_PORT)
    {
        /* releasing a message that is not (or no longer) held: nothing to do */
        reply->msg_id = 0;
        release_object( port );
        return;
    }
    else
    {
        if (port->type == COMMUNICATION_PORT) dest = port->peer;
        type = (req->flags & ALPC_MSGFLG_SYNC_REQUEST) ? ALPC_MESSAGE_TYPE_REQUEST : ALPC_MESSAGE_TYPE_DATAGRAM;
    }
    if (!dest)
    {
        set_error( port->status == DISCONNECTED ? STATUS_PORT_DISCONNECTED : STATUS_INVALID_PORT_HANDLE );
        release_object( port );
        return;
    }
    if (!(msg = alloc_msg( type, get_req_data(), get_req_data_size() )))
    {
        release_object( port );
        return;
    }
    if (type == ALPC_MESSAGE_TYPE_REPLY) msg->id = pending->id;
    msg->context = req->context;
    msg->reply_port = (struct alpc_port *)grab_object( port );
    if (req->msg_handle)
    {
        if ((msg->handle_obj = get_handle_obj( current->process, req->msg_handle, 0, NULL )))
            msg->handle_access = req->handle_access;
        else clear_error();
    }
    reply->msg_id = msg->id;
    grab_object( dest );
    if (pending && pending->type != ALPC_MESSAGE_TYPE_CONNECTION_REQUEST)
    {
        list_remove( &pending->entry );
        free_msg( pending );
    }
    queue_msg( dest, msg );
    release_object( dest );
    release_object( port );
}

/* receive the next message queued on a port */
DECL_HANDLER(alpc_receive)
{
    struct alpc_port *port, *owner, *addressed;
    struct alpc_msg *msg = NULL, *cur;
    data_size_t size;

    if (!(port = get_port_obj( current->process, req->handle, 0 ))) return;
    owner = queue_owner( port );

    if (req->reply_id)
    {
        LIST_FOR_EACH_ENTRY( cur, &owner->queue, struct alpc_msg, entry )
        {
            if (cur->type == ALPC_MESSAGE_TYPE_PORT_CLOSED ||
                ((cur->type == ALPC_MESSAGE_TYPE_REPLY || cur->type == ALPC_MESSAGE_TYPE_CONNECTION_REPLY) && cur->id == req->reply_id))
            {
                msg = cur;
                break;
            }
        }
    }
    else if (!list_empty( &owner->queue )) msg = LIST_ENTRY( list_head( &owner->queue ), struct alpc_msg, entry );

    if (!msg)
    {
        if (port->status == REFUSED) set_error( STATUS_PORT_CONNECTION_REFUSED );
        else if (port->status == DISCONNECTED) set_error( STATUS_PORT_DISCONNECTED );
        else set_error( STATUS_PENDING );
        release_object( port );
        return;
    }
    size = min( msg->size, get_reply_max_size() );
    if (size < msg->size)
    {
        set_error( STATUS_BUFFER_TOO_SMALL );
        reply->size = msg->size;
        release_object( port );
        return;
    }
    list_remove( &msg->entry );
    update_signal( owner );
    owner->sequence++;
    addressed = msg->dest_port ? msg->dest_port : port;
    reply->refused  = (port->status == REFUSED);
    reply->msg_id   = msg->id;
    reply->type     = msg->type;
    reply->pid      = msg->pid;
    reply->tid      = msg->tid;
    reply->context  = msg->context;
    reply->port_context = addressed->context;
    reply->sequence = owner->sequence;
    reply->size     = msg->size;
    reply->msg_handle = 0;
    if (msg->handle_obj)
    {
        reply->msg_handle = alloc_handle( current->process, msg->handle_obj, msg->handle_access, 0 );
        clear_error();
    }
    set_reply_data( msg->data, size );

    if (msg->type == ALPC_MESSAGE_TYPE_CONNECTION_REQUEST || msg->type == ALPC_MESSAGE_TYPE_REQUEST)
        list_add_tail( &owner->pending, &msg->entry );   /* kept until accepted, refused or replied to */
    else
        free_msg( msg );
    release_object( port );
}

/* disconnect a communication port */
DECL_HANDLER(alpc_disconnect)
{
    struct alpc_port *port;

    if (!(port = get_port_obj( current->process, req->handle, 0 ))) return;
    disconnect_port( port );
    release_object( port );
}

/* query basic port information */
DECL_HANDLER(alpc_query_info)
{
    struct alpc_port *port;

    if (!(port = get_port_obj( current->process, req->handle, 0 ))) return;
    reply->flags    = port->flags;
    reply->sequence = port->sequence;
    reply->context  = port->context;
    release_object( port );
}

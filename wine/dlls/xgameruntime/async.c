/*
 * XTaskQueue and XAsync
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

#include "private.h"
#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdk);

/*
 * XTaskQueue: two ports (work, completion), each with its own dispatch mode.
 *
 * A title hands its queues to libHttpClient, XSAPI and every XAsync call, and chooses where their callbacks run:
 * Manual ports run only inside the title's own XTaskQueueDispatch, on the title's thread. Running submitted callbacks
 * inline on the submitting thread made WinHTTP's workers run Halo's network callbacks in parallel with its own BG NET
 * threads, and corrupted its unsynchronised allocator freelists as soon as a WebSocket stayed open. Delays, waiters,
 * monitors and termination are real too.
 */

struct task_entry
{
    struct list entry;
    void *context;
    TaskQueueCallback *callback;
};

struct task_monitor
{
    UINT64 token;
    void *context;
    TaskQueueMonitorCallback *callback;
};

struct task_port
{
    struct task_queue *owner;
    UINT32 which;
    UINT32 mode;
    SRWLOCK lock;
    CONDITION_VARIABLE ready_cv;
    struct list ready;
    LONG pending;        /* delayed callbacks and waiters not yet ready */
    BOOL running;        /* SerializedThreadPool: a drain is scheduled */
    BOOL terminated;
};

struct task_queue
{
    LONG refs;
    struct task_port *ports[2];
    struct task_queue *sources[2];  /* a composite queue borrows other queues' ports */
    SRWLOCK lock;
    struct task_monitor *monitors;
    unsigned int monitor_count, monitor_size;
    UINT64 next_token;
};

void taskqueue_addref( XTaskQueueHandle q )
{
    InterlockedIncrement( &q->refs );
}

void taskqueue_release( XTaskQueueHandle q )
{
    int i;

    if (!q || InterlockedDecrement( &q->refs )) return;
    for (i = 0; i < 2; i++)
    {
        if (q->sources[i]) taskqueue_release( q->sources[i] );
        else free( q->ports[i] );
    }
    free( q->monitors );
    free( q );
}

XTaskQueueHandle taskqueue_new( UINT32 work, UINT32 completion )
{
    struct task_queue *q = calloc( 1, sizeof(*q) );
    UINT32 modes[2] = { work, completion };
    int i;

    q->refs = 1;
    q->next_token = 1;
    InitializeSRWLock( &q->lock );
    for (i = 0; i < 2; i++)
    {
        struct task_port *p = calloc( 1, sizeof(*p) );
        p->owner = q;
        p->which = i;
        p->mode = modes[i];
        InitializeSRWLock( &p->lock );
        InitializeConditionVariable( &p->ready_cv );
        list_init( &p->ready );
        q->ports[i] = p;
    }
    TRACE( "queue %p work %u completion %u\n", q, work, completion );
    return q;
}

static struct task_queue *process_queue;
static INIT_ONCE process_queue_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK create_process_queue( INIT_ONCE *once, void *param, void **context )
{
    process_queue = taskqueue_new( XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool );
    return TRUE;
}

/* The queue a call without one uses. Not referenced: the caller borrows it. */
XTaskQueueHandle taskqueue_resolve( XTaskQueueHandle q )
{
    if (q) return q;
    InitOnceExecuteOnce( &process_queue_once, create_process_queue, NULL, NULL );
    return process_queue;
}

XTaskQueuePortHandle taskqueue_port( XTaskQueueHandle q, UINT32 port )
{
    q = taskqueue_resolve( q );
    return port < 2 ? q->ports[port] : NULL;
}

BOOLEAN taskqueue_get_process_queue( XTaskQueueHandle *out )
{
    struct task_queue *q;

    if (!out) return FALSE;
    q = taskqueue_resolve( NULL );
    taskqueue_addref( q );
    *out = q;
    return TRUE;
}

void taskqueue_set_process_queue( XTaskQueueHandle q )
{
    taskqueue_resolve( NULL );
    /* the old one stays referenced by whoever obtained it */
    if (q) taskqueue_addref( q );
    process_queue = q ? q : taskqueue_new( XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool );
}

void taskqueue_close( XTaskQueueHandle q )
{
    if (q && q != process_queue) taskqueue_release( q );
}

static void notify_monitors( struct task_port *p )
{
    struct task_queue *q = p->owner;
    struct task_monitor *monitors;
    unsigned int i, count;

    AcquireSRWLockShared( &q->lock );
    count = q->monitor_count;
    monitors = count ? malloc( count * sizeof(*monitors) ) : NULL;
    if (monitors) memcpy( monitors, q->monitors, count * sizeof(*monitors) );
    else count = 0;
    ReleaseSRWLockShared( &q->lock );
    for (i = 0; i < count; i++) monitors[i].callback( monitors[i].context, q, p->which );
    free( monitors );
}

static struct task_entry *pop_ready( struct task_port *p )
{
    struct list *head = list_head( &p->ready );
    if (!head) return NULL;
    list_remove( head );
    return LIST_ENTRY( head, struct task_entry, entry );
}

static void CALLBACK drain_port( TP_CALLBACK_INSTANCE *instance, void *param )
{
    struct task_port *p = param;
    struct task_entry *e;
    BOOL canceled;

    for (;;)
    {
        AcquireSRWLockExclusive( &p->lock );
        if (!(e = pop_ready( p )))
        {
            p->running = FALSE;
            ReleaseSRWLockExclusive( &p->lock );
            break;
        }
        canceled = p->terminated;
        ReleaseSRWLockExclusive( &p->lock );
        e->callback( e->context, canceled );
        free( e );
    }
    taskqueue_release( p->owner );
}

struct pool_entry
{
    void *context;
    TaskQueueCallback *callback;
    struct task_port *port;
};

static void CALLBACK pool_run( TP_CALLBACK_INSTANCE *instance, void *param )
{
    struct pool_entry *pe = param;
    struct task_queue *owner = pe->port->owner;

    pe->callback( pe->context, pe->port->terminated );
    free( pe );
    taskqueue_release( owner );
}

/* An entry is ready to run: run it or park it as the port's mode says. */
static void port_ready( struct task_port *p, void *context, TaskQueueCallback *callback )
{
    struct task_entry *e;
    BOOL start;

    if (p->terminated)
    {
        callback( context, TRUE );
        return;
    }
    switch (p->mode)
    {
    case XTaskQueueDispatchMode_Immediate:
        callback( context, FALSE );
        return;
    case XTaskQueueDispatchMode_ThreadPool:
    {
        struct pool_entry *pe = malloc( sizeof(*pe) );
        pe->context = context;
        pe->callback = callback;
        pe->port = p;
        taskqueue_addref( p->owner );
        if (!TrySubmitThreadpoolCallback( pool_run, pe, NULL ))
        {
            taskqueue_release( p->owner );
            free( pe );
            callback( context, FALSE );
        }
        return;
    }
    case XTaskQueueDispatchMode_SerializedThreadPool:
        e = malloc( sizeof(*e) );
        e->context = context;
        e->callback = callback;
        AcquireSRWLockExclusive( &p->lock );
        list_add_tail( &p->ready, &e->entry );
        start = !p->running;
        p->running = TRUE;
        ReleaseSRWLockExclusive( &p->lock );
        if (start)
        {
            taskqueue_addref( p->owner );
            if (!TrySubmitThreadpoolCallback( drain_port, p, NULL )) drain_port( NULL, p );
        }
        return;
    case XTaskQueueDispatchMode_Manual:
    default:
        e = malloc( sizeof(*e) );
        e->context = context;
        e->callback = callback;
        AcquireSRWLockExclusive( &p->lock );
        list_add_tail( &p->ready, &e->entry );
        ReleaseSRWLockExclusive( &p->lock );
        WakeAllConditionVariable( &p->ready_cv );
        notify_monitors( p );
        return;
    }
}

struct delayed_entry
{
    void *context;
    TaskQueueCallback *callback;
    struct task_port *port;
};

static void CALLBACK delay_expired( TP_CALLBACK_INSTANCE *instance, void *param, TP_TIMER *timer )
{
    struct delayed_entry *d = param;
    struct task_port *p = d->port;

    port_ready( p, d->context, d->callback );
    InterlockedDecrement( &p->pending );
    taskqueue_release( p->owner );
    free( d );
    CloseThreadpoolTimer( timer );
}

HRESULT taskqueue_submit( XTaskQueueHandle q, UINT32 port, UINT32 delay, void *context, TaskQueueCallback *callback )
{
    struct task_port *p;
    struct delayed_entry *d;
    ULARGE_INTEGER due;
    FILETIME ft;
    TP_TIMER *timer;

    if (!callback) return E_POINTER;
    if (!(p = taskqueue_port( q, port ))) return E_INVALIDARG;
    if (p->terminated) return E_ABORT;
    if (!delay)
    {
        port_ready( p, context, callback );
        return S_OK;
    }
    d = malloc( sizeof(*d) );
    d->context = context;
    d->callback = callback;
    d->port = p;
    if (!(timer = CreateThreadpoolTimer( delay_expired, d, NULL )))
    {
        free( d );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    InterlockedIncrement( &p->pending );
    taskqueue_addref( p->owner );
    due.QuadPart = (ULONGLONG)-(LONGLONG)delay * 10000;
    ft.dwLowDateTime = due.LowPart;
    ft.dwHighDateTime = due.HighPart;
    SetThreadpoolTimer( timer, &ft, 0, 0 );
    return S_OK;
}

BOOLEAN taskqueue_dispatch( XTaskQueueHandle q, UINT32 port, UINT32 timeout )
{
    struct task_port *p = taskqueue_port( q, port );
    ULONGLONG end = GetTickCount64() + timeout, now;
    struct task_entry *e;
    DWORD wait;
    BOOL canceled;

    if (!p || p->mode != XTaskQueueDispatchMode_Manual) return FALSE;
    AcquireSRWLockExclusive( &p->lock );
    while (list_empty( &p->ready ))
    {
        if (!timeout) break;
        wait = INFINITE;
        if (timeout != INFINITE)
        {
            now = GetTickCount64();
            if (now >= end) break;
            wait = end - now;
        }
        if (!SleepConditionVariableSRW( &p->ready_cv, &p->lock, wait, 0 )) break;
    }
    if (!(e = pop_ready( p )))
    {
        ReleaseSRWLockExclusive( &p->lock );
        return FALSE;
    }
    canceled = p->terminated;
    ReleaseSRWLockExclusive( &p->lock );
    e->callback( e->context, canceled );
    free( e );
    return TRUE;
}

BOOLEAN taskqueue_is_empty( XTaskQueueHandle q, UINT32 port )
{
    struct task_port *p = taskqueue_port( q, port );
    BOOL empty;

    if (!p) return TRUE;
    AcquireSRWLockShared( &p->lock );
    empty = list_empty( &p->ready ) && !p->pending;
    ReleaseSRWLockShared( &p->lock );
    return empty;
}

struct task_waiter
{
    HANDLE wait;
    struct task_port *port;
    void *context;
    TaskQueueCallback *callback;
};

static void CALLBACK waiter_fired( void *param, BOOLEAN timed_out )
{
    struct task_waiter *w = param;
    port_ready( w->port, w->context, w->callback );
}

HRESULT taskqueue_register_waiter( XTaskQueueHandle q, UINT32 port, HANDLE handle, void *context,
                                   TaskQueueCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct task_port *p;
    struct task_waiter *w;

    if (!handle || !callback || !token) return E_INVALIDARG;
    if (!(p = taskqueue_port( q, port ))) return E_INVALIDARG;
    w = calloc( 1, sizeof(*w) );
    w->port = p;
    w->context = context;
    w->callback = callback;
    taskqueue_addref( p->owner );
    if (!RegisterWaitForSingleObject( &w->wait, handle, waiter_fired, w, INFINITE, WT_EXECUTEONLYONCE ))
    {
        taskqueue_release( p->owner );
        free( w );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    token->token = (UINT_PTR)w;
    TRACE( "waiter %p on %p port %u handle %p callback %p\n", w, p->owner, port, handle, callback );
    return S_OK;
}

void taskqueue_unregister_waiter( XTaskQueueRegistrationToken token )
{
    struct task_waiter *w = (struct task_waiter *)(UINT_PTR)token.token;

    if (!w) return;
    /* not waiting for a running callback: the title may unregister from inside it */
    UnregisterWaitEx( w->wait, NULL );
    taskqueue_release( w->port->owner );
    free( w );
}

HRESULT taskqueue_register_monitor( XTaskQueueHandle q, void *context, TaskQueueMonitorCallback *callback,
                                    XTaskQueueRegistrationToken *token )
{
    struct task_monitor *grown;

    if (!callback || !token) return E_INVALIDARG;
    q = taskqueue_resolve( q );
    AcquireSRWLockExclusive( &q->lock );
    if (q->monitor_count == q->monitor_size)
    {
        unsigned int size = q->monitor_size ? q->monitor_size * 2 : 4;
        if (!(grown = realloc( q->monitors, size * sizeof(*grown) )))
        {
            ReleaseSRWLockExclusive( &q->lock );
            return E_OUTOFMEMORY;
        }
        q->monitors = grown;
        q->monitor_size = size;
    }
    q->monitors[q->monitor_count].token = q->next_token++;
    q->monitors[q->monitor_count].context = context;
    q->monitors[q->monitor_count].callback = callback;
    token->token = q->monitors[q->monitor_count++].token;
    ReleaseSRWLockExclusive( &q->lock );
    TRACE( "monitor on %p callback %p\n", q, callback );
    return S_OK;
}

void taskqueue_unregister_monitor( XTaskQueueHandle q, XTaskQueueRegistrationToken token )
{
    unsigned int i;

    q = taskqueue_resolve( q );
    AcquireSRWLockExclusive( &q->lock );
    for (i = 0; i < q->monitor_count; i++)
    {
        if (q->monitors[i].token != token.token) continue;
        memmove( &q->monitors[i], &q->monitors[i + 1], (q->monitor_count - i - 1) * sizeof(*q->monitors) );
        q->monitor_count--;
        break;
    }
    ReleaseSRWLockExclusive( &q->lock );
}

/* Pending callbacks run with canceled = TRUE, later submissions fail, then the callback. */
HRESULT taskqueue_terminate( XTaskQueueHandle q, BOOLEAN wait, void *context, TaskQueueTerminatedCallback *callback )
{
    struct task_entry *e;
    struct list pending;
    int i;

    q = taskqueue_resolve( q );
    TRACE( "terminate %p\n", q );
    for (i = 0; i < 2; i++)
    {
        struct task_port *p = q->ports[i];

        if (q->sources[i]) continue; /* a composite does not own the ports it borrows */
        list_init( &pending );
        AcquireSRWLockExclusive( &p->lock );
        p->terminated = TRUE;
        if (p->mode == XTaskQueueDispatchMode_Manual) list_move_tail( &pending, &p->ready );
        ReleaseSRWLockExclusive( &p->lock );
        WakeAllConditionVariable( &p->ready_cv );
        while (!list_empty( &pending ))
        {
            e = LIST_ENTRY( list_head( &pending ), struct task_entry, entry );
            list_remove( &e->entry );
            e->callback( e->context, TRUE );
            free( e );
        }
    }
    if (callback) callback( context );
    return S_OK;
}

HRESULT taskqueue_create_composite( XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle *out )
{
    struct task_port *ports[2] = { work, completion };
    struct task_queue *q;
    int i;

    if (!work || !completion || !out) return E_INVALIDARG;
    q = calloc( 1, sizeof(*q) );
    q->refs = 1;
    q->next_token = 1;
    InitializeSRWLock( &q->lock );
    for (i = 0; i < 2; i++)
    {
        q->ports[i] = ports[i];
        q->sources[i] = ports[i]->owner;
        taskqueue_addref( ports[i]->owner );
    }
    *out = q;
    return S_OK;
}

/*
 * XAsync. A call's state lives behind a pointer in XAsyncBlock.internal.
 */

#define ASYNC_SIGNATURE 0x41535942 /* ASYB */

struct async_state
{
    UINT32 signature;
    LONG refs;
    XAsyncProvider *provider;
    void *context;
    const void *identity;
    const char *name;
    XAsyncBlock *user;
    HRESULT status;
    SIZE_T result_size;
    volatile LONG completed;
    HANDLE done_event;
    BOOL external;
    SRWLOCK lock;
};

static void async_addref( struct async_state *s )
{
    InterlockedIncrement( &s->refs );
}

static void async_release( struct async_state *s )
{
    XAsyncProviderData data = { s->user, 0, NULL, s->context };

    if (InterlockedDecrement( &s->refs )) return;
    if (s->provider) s->provider( XAsyncOp_Cleanup, &data );
    if (s->done_event) CloseHandle( s->done_event );
    free( s );
}

static struct async_state *async_state( XAsyncBlock *block )
{
    struct async_state *s;

    if (!block) return NULL;
    memcpy( &s, block->internal, sizeof(s) );
    if (!s || s->signature != ASYNC_SIGNATURE) return NULL;
    return s;
}

static void async_set_state( XAsyncBlock *block, struct async_state *s )
{
    memcpy( block->internal, &s, sizeof(s) );
}

static struct async_state *async_new( XAsyncBlock *block, void *context, const void *identity, const char *name,
                                      XAsyncProvider *provider )
{
    struct async_state *s = calloc( 1, sizeof(*s) );

    if (!s) return NULL;
    s->signature = ASYNC_SIGNATURE;
    s->refs = 1;
    s->provider = provider;
    s->context = context;
    s->identity = identity;
    s->name = name;
    s->user = block;
    s->status = E_PENDING;
    s->done_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    InitializeSRWLock( &s->lock );
    async_set_state( block, s );
    return s;
}

/* Our own providers: Begin, then DoWork once on the calling thread. */
HRESULT async_begin( XAsyncBlock *block, void *context, const void *identity, const char *name, XAsyncProvider *provider )
{
    XAsyncProviderData data = { block, 0, NULL, context };
    struct async_state *s;
    HRESULT hr;

    if (!block || !provider) return E_INVALIDARG;
    if (name && !strncmp( name, "XStore", 6 )) TRACE( "store: %s\n", name );
    if (!(s = async_new( block, context, identity, name, provider ))) return E_OUTOFMEMORY;

    if (FAILED(hr = provider( XAsyncOp_Begin, &data )))
    {
        async_set_state( block, NULL );
        async_release( s );
        return hr;
    }
    hr = provider( XAsyncOp_DoWork, &data );
    if (hr == E_PENDING) return S_OK; /* the provider completes it later */
    if (FAILED(hr))
    {
        BOOL fire = FALSE;

        AcquireSRWLockExclusive( &s->lock );
        if (!s->completed)
        {
            s->status = hr;
            s->completed = TRUE;
            fire = TRUE;
            if (s->done_event) SetEvent( s->done_event );
        }
        ReleaseSRWLockExclusive( &s->lock );
        if (fire && block->callback) block->callback( block );
    }
    return S_OK;
}

struct async_callback
{
    XAsyncBlock *block;
    struct async_state *state;
};

/* The title may free or reuse its XAsyncBlock inside the completion callback (that is the normal pattern), so
 * nothing here may look at the block after the call. Reading block->internal afterwards released whichever call had
 * reused the block. */
static void WINAPI completion_port_callback( void *param, BOOLEAN canceled )
{
    struct async_callback *cb = param;
    XAsyncCompletionRoutine *routine = cb->block->callback;

    if (routine) routine( cb->block );
    async_release( cb->state );
    free( cb );
}

void async_complete( XAsyncBlock *block, HRESULT result, SIZE_T size )
{
    struct async_state *s = async_state( block );
    struct async_callback *cb;
    BOOL fire = FALSE;

    if (!s) return;
    AcquireSRWLockExclusive( &s->lock );
    if (s->external)
        TRACE( "complete %p %s hr %#lx size %Iu%s\n", block, s->name ? s->name : "?", result, size,
               s->completed ? " (already)" : "" );
    if (!s->completed)
    {
        s->status = result;
        s->result_size = size;
        s->completed = TRUE;
        fire = TRUE;
        if (s->done_event) SetEvent( s->done_event );
    }
    ReleaseSRWLockExclusive( &s->lock );

    /* Deferred to the block's completion port, which is where the title dispatches it: XUserAddAsync must return
     * before the title's handler runs (PPL "unobserved exception" re-entrancy otherwise). */
    if (fire && block->callback)
    {
        async_addref( s );
        if (!(cb = malloc( sizeof(*cb) )))
        {
            block->callback( block );
            async_release( s );
            return;
        }
        cb->block = block;
        cb->state = s;
        if (FAILED(taskqueue_submit( block->queue, XTaskQueuePort_Completion, 0, cb, completion_port_callback )))
        {
            free( cb );
            block->callback( block );
            async_release( s );
        }
    }
}

/* XAsyncBegin from a title or library (libHttpClient, XSAPI) follows the GDK provider contract: Begin only, and
 * DoWork runs when the provider asks through XAsyncSchedule, as often as it asks. A DoWork result other than E_PENDING
 * completes the call. Running DoWork unasked, as async_begin() does for our own providers, did an external
 * provider's work twice; libHttpClient's WebSocket send then looked failed, and Halo dropped its telemetry and lobby
 * connections right after their first frame. */
HRESULT async_begin_external( XAsyncBlock *block, void *context, const void *identity, const char *name,
                              XAsyncProvider *provider )
{
    XAsyncProviderData data = { block, 0, NULL, context };
    struct async_state *s;
    HRESULT hr;

    if (!block || !provider) return E_INVALIDARG;
    if (!(s = async_new( block, context, identity, name, provider ))) return E_OUTOFMEMORY;
    s->external = TRUE;
    hr = provider( XAsyncOp_Begin, &data );
    TRACE( "begin %p %s queue %p -> %#lx\n", block, name ? name : "?", block->queue, hr );
    /* E_PENDING from Begin means "under way" (libHttpClient's WebSocket send returns it, and may have completed the
     * call already); only other failures abort the call. */
    if (hr == E_PENDING) hr = S_OK;
    if (FAILED(hr))
    {
        async_set_state( block, NULL );
        async_release( s );
        return hr;
    }
    return S_OK;
}

static void WINAPI dowork_port_callback( void *param, BOOLEAN canceled )
{
    struct async_state *s = param;
    XAsyncProviderData data;
    HRESULT hr;

    /* the block belongs to the title once completed, and may already carry another call */
    if (!s->completed && async_state( s->user ) == s)
    {
        if (canceled) async_complete( s->user, E_ABORT, 0 );
        else
        {
            data.async = s->user;
            data.bufferSize = 0;
            data.buffer = NULL;
            data.context = s->context;
            hr = s->provider( XAsyncOp_DoWork, &data );
            if (hr != E_PENDING && !s->completed && async_state( s->user ) == s) async_complete( s->user, hr, 0 );
        }
    }
    async_release( s );
}

HRESULT async_schedule( XAsyncBlock *block, UINT32 delay )
{
    struct async_state *s = async_state( block );
    HRESULT hr;

    if (!s) return E_INVALIDARG;
    if (s->completed) return E_UNEXPECTED;
    async_addref( s );
    if (FAILED(hr = taskqueue_submit( block->queue, XTaskQueuePort_Work, delay, s, dowork_port_callback )))
        async_release( s );
    return hr;
}

HRESULT async_get_status( XAsyncBlock *block, BOOLEAN wait )
{
    struct async_state *s = async_state( block );

    if (!s) return E_INVALIDARG;
    if (!s->completed && wait && s->done_event) WaitForSingleObject( s->done_event, INFINITE );
    return s->completed ? s->status : E_PENDING;
}

HRESULT async_get_result_size( XAsyncBlock *block, SIZE_T *size )
{
    struct async_state *s;
    HRESULT hr;

    if (!size) return E_POINTER;
    if (!(s = async_state( block ))) return E_INVALIDARG;
    hr = async_get_status( block, TRUE );
    if (FAILED(hr) && hr != E_PENDING) return hr;
    *size = s->result_size;
    return s->status;
}

HRESULT async_get_result( XAsyncBlock *block, const void *identity, SIZE_T size, void *buffer, SIZE_T *used )
{
    struct async_state *s = async_state( block );
    XAsyncProviderData data = { block, size, buffer, NULL };
    HRESULT hr;

    if (!s) return E_INVALIDARG;
    if (identity && s->identity && identity != s->identity) return E_INVALIDARG;
    if (FAILED(hr = async_get_status( block, TRUE ))) return hr;
    data.context = s->context;
    hr = s->provider( XAsyncOp_GetResult, &data );
    if (s->external)
        TRACE( "result %p %s size %Iu/%Iu -> %#lx\n", block, s->name ? s->name : "?", size, s->result_size, hr );
    if (SUCCEEDED(hr) && used) *used = s->result_size;
    return hr;
}

void async_cancel( XAsyncBlock *block )
{
    struct async_state *s = async_state( block );
    XAsyncProviderData data = { block, 0, NULL, NULL };

    if (!s) return;
    data.context = s->context;
    s->provider( XAsyncOp_Cancel, &data );
    async_complete( block, E_ABORT, 0 );
}

static HRESULT WINAPI empty_provider( UINT32 op, const XAsyncProviderData *data )
{
    if (op == XAsyncOp_DoWork) async_complete( data->async, S_OK, 0 );
    return S_OK;
}

/* a call that completes at once with nothing to return */
HRESULT async_begin_empty( XAsyncBlock *block, const char *name )
{
    return async_begin( block, NULL, NULL, name, empty_provider );
}

static HRESULT WINAPI run_provider( UINT32 op, const XAsyncProviderData *data )
{
    XAsyncWork *work = data->context;
    HRESULT hr;

    if (op != XAsyncOp_DoWork) return S_OK;
    if ((hr = work( data->async )) != E_PENDING) async_complete( data->async, hr, 0 );
    return hr;
}

HRESULT async_run( XAsyncBlock *block, XAsyncWork *work )
{
    if (!block || !work) return E_INVALIDARG;
    return async_begin( block, work, work, "XAsyncRun", run_provider );
}

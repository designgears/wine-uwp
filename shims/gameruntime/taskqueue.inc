/* XTaskQueue: two ports (work, completion), each with its own dispatch mode.
 *
 * A title hands its queues to libHttpClient, XSAPI and every XAsync call, and chooses where their
 * callbacks run: Manual ports run only inside the title's own XTaskQueueDispatch, on the title's
 * thread. This used to be a stand-in that ran every submitted callback inline on the submitting
 * thread and dispatched nothing, so WinHTTP's worker threads ran Halo's network callbacks in
 * parallel with its own BG NET threads and corrupted its (unsynchronised) allocator freelists as
 * soon as a WebSocket stayed open. Delays, waiters, monitors and termination are real here too. */

typedef void __stdcall TaskQueueCallback(void *context, BOOLEAN canceled);
typedef void __stdcall TaskQueueMonitorCallback(void *context, XTaskQueueHandle queue, XTaskQueuePort port);
typedef void __stdcall TaskQueueTerminatedCallback(void *context);

struct TaskEntry
{
    void *context;
    TaskQueueCallback *callback;
};

struct TaskPort;

struct TaskMonitor
{
    UINT64 token;
    void *context;
    TaskQueueMonitorCallback *callback;
};

struct XTaskQueueObject
{
    LONG refs = 1;
    TaskPort *ports[2] = {};
    XTaskQueueObject *sources[2] = {};   /* composite queues borrow other queues' ports */
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<TaskMonitor> monitors;
    UINT64 next_token = 1;
};

struct TaskPort
{
    XTaskQueueObject *owner;
    XTaskQueuePort which;
    XTaskQueueDispatchMode mode;
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE ready_cv = CONDITION_VARIABLE_INIT;
    std::deque<TaskEntry> ready;
    LONG pending = 0;          /* delayed callbacks and waiters not yet ready */
    bool running = false;      /* SerializedThreadPool: a drain is scheduled */
    bool terminated = false;
};

static void taskqueue_addref(XTaskQueueObject *q) { InterlockedIncrement(&q->refs); }

static void taskqueue_release(XTaskQueueObject *q)
{
    if (!q || InterlockedDecrement(&q->refs)) return;
    for (int i = 0; i < 2; i++)
    {
        if (q->sources[i]) taskqueue_release(q->sources[i]);
        else delete q->ports[i];
    }
    delete q;
}

static XTaskQueueObject *taskqueue_new(XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion)
{
    auto *q = new XTaskQueueObject();
    XTaskQueueDispatchMode modes[2] = { work, completion };
    for (int i = 0; i < 2; i++)
    {
        q->ports[i] = new TaskPort();
        q->ports[i]->owner = q;
        q->ports[i]->which = (XTaskQueuePort)i;
        q->ports[i]->mode = modes[i];
    }
    gdk_log("GDK queue: create %p work=%u completion=%u\n", q, (unsigned)work, (unsigned)completion);
    return q;
}

static XTaskQueueObject *g_process_queue;
static INIT_ONCE g_process_queue_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK taskqueue_create_process_queue(INIT_ONCE *, void *, void **)
{
    g_process_queue = taskqueue_new(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool);
    return TRUE;
}

/* The queue a call without one uses. Not referenced: the caller borrows it. */
static XTaskQueueObject *taskqueue_resolve(XTaskQueueHandle q)
{
    if (q) return q;
    InitOnceExecuteOnce(&g_process_queue_once, taskqueue_create_process_queue, nullptr, nullptr);
    return g_process_queue;
}

static TaskPort *taskqueue_port(XTaskQueueHandle q, XTaskQueuePort port)
{
    q = taskqueue_resolve(q);
    return (UINT32)port < 2 ? q->ports[(UINT32)port] : nullptr;
}

static void taskqueue_notify_monitors(TaskPort *p)
{
    XTaskQueueObject *q = p->owner;
    AcquireSRWLockShared(&q->lock);
    std::vector<TaskMonitor> monitors = q->monitors;
    ReleaseSRWLockShared(&q->lock);
    for (const auto &m : monitors) m.callback(m.context, q, p->which);
}

struct SerializedDrain { TaskPort *port; };

static void CALLBACK taskqueue_drain(PTP_CALLBACK_INSTANCE, void *param)
{
    TaskPort *p = (TaskPort *)param;
    for (;;)
    {
        AcquireSRWLockExclusive(&p->lock);
        if (p->ready.empty())
        {
            p->running = false;
            ReleaseSRWLockExclusive(&p->lock);
            break;
        }
        TaskEntry e = p->ready.front();
        p->ready.pop_front();
        bool canceled = p->terminated;
        ReleaseSRWLockExclusive(&p->lock);
        e.callback(e.context, canceled);
    }
    taskqueue_release(p->owner);
}

struct PoolEntry { TaskEntry entry; TaskPort *port; };

static void CALLBACK taskqueue_pool_run(PTP_CALLBACK_INSTANCE, void *param)
{
    auto *pe = (PoolEntry *)param;
    pe->entry.callback(pe->entry.context, pe->port->terminated);
    taskqueue_release(pe->port->owner);
    delete pe;
}

/* An entry is ready to run: run or park it as the port's mode says. */
static void taskqueue_ready(TaskPort *p, const TaskEntry &e)
{
    if (p->terminated)
    {
        e.callback(e.context, TRUE);
        return;
    }
    switch (p->mode)
    {
    case XTaskQueueDispatchMode::Immediate:
        e.callback(e.context, FALSE);
        return;
    case XTaskQueueDispatchMode::ThreadPool:
    {
        auto *pe = new PoolEntry{ e, p };
        taskqueue_addref(p->owner);
        if (!TrySubmitThreadpoolCallback(taskqueue_pool_run, pe, nullptr))
        {
            taskqueue_release(p->owner);
            delete pe;
            e.callback(e.context, FALSE);
        }
        return;
    }
    case XTaskQueueDispatchMode::SerializedThreadPool:
    {
        AcquireSRWLockExclusive(&p->lock);
        p->ready.push_back(e);
        bool start = !p->running;
        p->running = true;
        ReleaseSRWLockExclusive(&p->lock);
        if (start)
        {
            taskqueue_addref(p->owner);
            if (!TrySubmitThreadpoolCallback(taskqueue_drain, p, nullptr))
                taskqueue_drain(nullptr, p);
        }
        return;
    }
    case XTaskQueueDispatchMode::Manual:
    default:
        AcquireSRWLockExclusive(&p->lock);
        p->ready.push_back(e);
        ReleaseSRWLockExclusive(&p->lock);
        WakeAllConditionVariable(&p->ready_cv);
        taskqueue_notify_monitors(p);
        return;
    }
}

struct DelayedEntry { TaskEntry entry; TaskPort *port; };

static void CALLBACK taskqueue_timer(PTP_CALLBACK_INSTANCE, void *param, PTP_TIMER timer)
{
    auto *d = (DelayedEntry *)param;
    TaskPort *p = d->port;
    taskqueue_ready(p, d->entry);
    InterlockedDecrement(&p->pending);
    taskqueue_release(p->owner);
    delete d;
    CloseThreadpoolTimer(timer);
}

static HRESULT taskqueue_submit(XTaskQueueHandle q, XTaskQueuePort port, UINT32 delay, void *context,
                                TaskQueueCallback *callback)
{
    if (!callback) return E_POINTER;
    TaskPort *p = taskqueue_port(q, port);
    if (!p) return E_INVALIDARG;
    if (p->terminated) return E_ABORT;
    TaskEntry e{ context, callback };
    if (!delay)
    {
        taskqueue_ready(p, e);
        return S_OK;
    }
    auto *d = new DelayedEntry{ e, p };
    PTP_TIMER timer = CreateThreadpoolTimer(taskqueue_timer, d, nullptr);
    if (!timer)
    {
        delete d;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    InterlockedIncrement(&p->pending);
    taskqueue_addref(p->owner);
    ULARGE_INTEGER due;
    due.QuadPart = (ULONGLONG)-(LONGLONG)delay * 10000;
    FILETIME ft{ due.LowPart, due.HighPart };
    SetThreadpoolTimer(timer, &ft, 0, 0);
    return S_OK;
}

static BOOLEAN taskqueue_dispatch(XTaskQueueHandle q, XTaskQueuePort port, UINT32 timeout)
{
    TaskPort *p = taskqueue_port(q, port);
    if (!p || p->mode != XTaskQueueDispatchMode::Manual) return FALSE;
    ULONGLONG end = GetTickCount64() + timeout;
    AcquireSRWLockExclusive(&p->lock);
    while (p->ready.empty())
    {
        if (!timeout) break;
        DWORD wait = INFINITE;
        if (timeout != INFINITE)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= end) break;
            wait = (DWORD)(end - now);
        }
        if (!SleepConditionVariableSRW(&p->ready_cv, &p->lock, wait, 0)) break;
    }
    if (p->ready.empty())
    {
        ReleaseSRWLockExclusive(&p->lock);
        return FALSE;
    }
    TaskEntry e = p->ready.front();
    p->ready.pop_front();
    bool canceled = p->terminated;
    ReleaseSRWLockExclusive(&p->lock);
    e.callback(e.context, canceled);
    return TRUE;
}

static BOOLEAN taskqueue_is_empty(XTaskQueueHandle q, XTaskQueuePort port)
{
    TaskPort *p = taskqueue_port(q, port);
    if (!p) return TRUE;
    AcquireSRWLockShared(&p->lock);
    bool empty = p->ready.empty() && !p->pending;
    ReleaseSRWLockShared(&p->lock);
    return empty;
}

struct TaskWaiter
{
    HANDLE wait = nullptr;
    TaskPort *port;
    TaskEntry entry;
};

static void CALLBACK taskqueue_wait_fired(void *param, BOOLEAN)
{
    auto *w = (TaskWaiter *)param;
    taskqueue_ready(w->port, w->entry);
}

static HRESULT taskqueue_register_waiter(XTaskQueueHandle q, XTaskQueuePort port, HANDLE handle, void *context,
                                         TaskQueueCallback *callback, XTaskQueueRegistrationToken *token)
{
    if (!handle || !callback || !token) return E_INVALIDARG;
    TaskPort *p = taskqueue_port(q, port);
    if (!p) return E_INVALIDARG;
    auto *w = new TaskWaiter{ nullptr, p, { context, callback } };
    taskqueue_addref(p->owner);
    if (!RegisterWaitForSingleObject(&w->wait, handle, taskqueue_wait_fired, w, INFINITE, WT_EXECUTEONLYONCE))
    {
        taskqueue_release(p->owner);
        delete w;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    token->token = (UINT64)(uintptr_t)w;
    gdk_log("GDK queue: waiter %p on %p port %u handle %p cb %p\n", w, p->owner, (unsigned)port, handle, callback);
    return S_OK;
}

static void taskqueue_unregister_waiter(XTaskQueueRegistrationToken token)
{
    auto *w = (TaskWaiter *)(uintptr_t)token.token;
    if (!w) return;
    /* Not waiting for a running callback: the title may unregister from inside it. */
    UnregisterWaitEx(w->wait, nullptr);
    taskqueue_release(w->port->owner);
    delete w;
}

static HRESULT taskqueue_register_monitor(XTaskQueueHandle q, void *context, TaskQueueMonitorCallback *callback,
                                          XTaskQueueRegistrationToken *token)
{
    if (!callback || !token) return E_INVALIDARG;
    q = taskqueue_resolve(q);
    AcquireSRWLockExclusive(&q->lock);
    UINT64 id = q->next_token++;
    q->monitors.push_back({ id, context, callback });
    ReleaseSRWLockExclusive(&q->lock);
    token->token = id;
    gdk_log("GDK queue: monitor on %p cb %p\n", q, callback);
    return S_OK;
}

static void taskqueue_unregister_monitor(XTaskQueueHandle q, XTaskQueueRegistrationToken token)
{
    q = taskqueue_resolve(q);
    AcquireSRWLockExclusive(&q->lock);
    for (auto it = q->monitors.begin(); it != q->monitors.end(); ++it)
        if (it->token == token.token) { q->monitors.erase(it); break; }
    ReleaseSRWLockExclusive(&q->lock);
}

/* Pending callbacks run with canceled = TRUE, later submissions fail, then the callback. */
static HRESULT taskqueue_terminate(XTaskQueueHandle q, BOOLEAN, void *context, TaskQueueTerminatedCallback *callback)
{
    q = taskqueue_resolve(q);
    gdk_log("GDK queue: terminate %p\n", q);
    for (int i = 0; i < 2; i++)
    {
        if (q->sources[i]) continue; /* a composite does not own the ports it borrows */
        TaskPort *p = q->ports[i];
        AcquireSRWLockExclusive(&p->lock);
        p->terminated = true;
        std::deque<TaskEntry> pending;
        if (p->mode == XTaskQueueDispatchMode::Manual) pending.swap(p->ready);
        ReleaseSRWLockExclusive(&p->lock);
        WakeAllConditionVariable(&p->ready_cv);
        for (const auto &e : pending) e.callback(e.context, TRUE);
    }
    if (callback) callback(context);
    return S_OK;
}

static HRESULT taskqueue_create_composite(XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle *out)
{
    if (!work || !completion || !out) return E_INVALIDARG;
    auto *q = new XTaskQueueObject();
    TaskPort *ports[2] = { (TaskPort *)work, (TaskPort *)completion };
    for (int i = 0; i < 2; i++)
    {
        q->ports[i] = ports[i];
        q->sources[i] = ports[i]->owner;
        taskqueue_addref(ports[i]->owner);
    }
    *out = q;
    return S_OK;
}

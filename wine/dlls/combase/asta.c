/*
 * Private combase exports used by the UWP UI stack: application STA, message-pump cooperation,
 * design mode, as-user activation. Exported by ordinal only on Windows.
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windows.h"
#include "winternl.h"
#include "roapi.h"
#include "winstring.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

static void *message_dispatcher;

/* apartment.c */
extern void apartment_mark_asta(void);
extern void apartment_drain_asta_calls(void);
extern HANDLE apartment_asta_event(void);

/* ordinal 69 */
HRESULT WINAPI CoRegisterServerShutdownDelay( DWORD delay )
{
    FIXME( "%lu: stub\n", delay );
    return S_OK;
}

/* ordinal 86: called by CoreMessaging / Windows.UI around their message pump.
 * Prototypes below are inferred from call sites; traces print the raw arguments so they can be verified. */
HRESULT WINAPI CoBeginProcessEvents( void **context )
{
    /* called around every dispatcher wait, thousands of times a second in a busy app */
    static LONG once;
    if (!InterlockedExchange( &once, 1 )) FIXME( "%p: semi-stub\n", context );
    else TRACE( "%p\n", context );
    if (context) *context = NULL;
    return S_OK;
}

/* ordinal 87: CoMsgWaitInProcessEvents(context, count, handles, timeout, wake_mask). Observed from
 * Windows.UI.dll: context comes from CoBeginProcessEvents, wake_mask is a QS_* mask (0x1cff). Returns the
 * wait result like MsgWaitForMultipleObjectsEx. */
static void dispatch_pending_messages(void)
{
    MSG msg;
    unsigned int n = 0;

    while (n++ < 64 && PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        if (msg.message == WM_QUIT)
        {
            PostQuitMessage( (int)msg.wParam );
            break;
        }
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
}

DWORD WINAPI CoMsgWaitInProcessEvents( void *context, DWORD count, const HANDLE *handles, DWORD timeout, DWORD wake_mask )
{
    DWORD res;
    static LONG calls;
    LONG n;
    HANDLE wait_buf[16];
    DWORD wait_count = count;

    n = InterlockedIncrement( &calls );
    /* No poll and no private wake event any more: win32u adds the thread's CoreMessaging completion port to
     * the message wait (NtUserInitThreadCoreMessagingIocp2), so a timer or wait packet firing on it wakes
     * this wait the way it does on Windows. The 16 ms cap that stood in for that hid lost wakes as latency. */
    if (n <= 3 || (n % 200) == 0)
        FIXME( "%p, %lu handles, timeout %lu (call %ld): semi-stub\n", context, wait_count, timeout, n );
    if (TRACE_ON(combase) && n <= 40)
    {
        DWORD i;
        for (i = 0; i < count; i++)
        {
            char buf[256];
            OBJECT_TYPE_INFORMATION *info = (OBJECT_TYPE_INFORMATION *)buf;
            ULONG len;
            if (!NtQueryObject( handles[i], ObjectTypeInformation, info, sizeof(buf), &len ))
                TRACE( "call %ld tid %04lx handle %lu: %p %s\n", n, GetCurrentThreadId(), i, handles[i],
                       debugstr_wn( info->TypeName.Buffer, info->TypeName.Length / sizeof(WCHAR) ) );
        }
    }
    {
        HANDLE asta = apartment_asta_event();
        if (asta && wait_count < ARRAY_SIZE(wait_buf))
        {
            if (handles != wait_buf)
            {
                if (wait_count && handles) memcpy( wait_buf, handles, wait_count * sizeof(HANDLE) );
                handles = wait_buf;
            }
            wait_buf[wait_count++] = asta;
        }
    }
    res = MsgWaitForMultipleObjectsEx( wait_count, handles, timeout, wake_mask, MWMO_INPUTAVAILABLE | MWMO_ALERTABLE );
    /* Queued incoming COM calls (see apartment_queue_asta_call) run here, not from the message queue. */
    apartment_drain_asta_calls();
    /* A wake from one of our extra handles is reported as the app's wake handle. */
    if (res >= WAIT_OBJECT_0 + count && res < WAIT_OBJECT_0 + wait_count) res = WAIT_OBJECT_0;
    /* Messages available comes back one past the handles we waited on, which counts our extra
     * handles; the caller knows only its own and expects WAIT_OBJECT_0 + count. Passing the larger
     * index through handed CoreMessaging an index past its own array. */
    else if (res == WAIT_OBJECT_0 + wait_count) res = WAIT_OBJECT_0 + count;
    /* Handle-ready returns do not drain posted COM (DM_EXECUTERPC). Always peek. */
    dispatch_pending_messages();
    /* This used to post WM_NULL to every window of the thread on every call, to keep the dispatcher turning while
     * nothing woke it. win32u now wakes the message wait on a CoreMessaging completion, and a queue with a message
     * posts one to that port: the broadcast became a busy loop. */
    return res;
}

/* ordinal 88 */
HRESULT WINAPI CoEndProcessEvents( void *context )
{
    static LONG once;
    if (!InterlockedExchange( &once, 1 )) FIXME( "%p: semi-stub\n", context );
    else TRACE( "%p\n", context );
    return S_OK;
}

/* ordinal 90 */
HRESULT WINAPI RoGetDesignMode( BOOL *design_mode )
{
    TRACE( "%p\n", design_mode );
    if (!design_mode) return E_POINTER;
    *design_mode = FALSE;
    return S_OK;
}

/* ordinal 157 */
HRESULT WINAPI RoGetDesignModeV2( BOOL *design_mode )
{
    return RoGetDesignMode( design_mode );
}

/* ordinal 95 */
HRESULT WINAPI CoSignalPendingGitRegistrationWaits( void )
{
    FIXME( "stub\n" );
    return S_OK;
}

/* ordinal 110, MessageDispatcherApi.h: void CoSetMessageDispatcher(PMessageDispatcher) */
void WINAPI CoSetMessageDispatcher( void *dispatcher )
{
    FIXME( "%p: semi-stub\n", dispatcher );
    message_dispatcher = dispatcher;
    if (dispatcher) apartment_mark_asta();
}

/* ordinal 111: ASTA message dispatchers call this after each wait so short COM
 * work (marshalled Activate, GIT, etc.) can run between CoreDispatcher messages.
 * Wine delivers those as posted messages on the apartment window; drain them. */
void WINAPI CoHandlePriorityEventsFromMessagePump( void )
{
    apartment_drain_asta_calls();
    dispatch_pending_messages();
}

/* ordinal 133 */
HRESULT WINAPI RoIsASTAWorkScheduledAtPriorityOrHigher( DWORD priority, BOOL *scheduled )
{
    FIXME( "%lu, %p: semi-stub\n", priority, scheduled );
    if (scheduled) *scheduled = FALSE;
    return S_OK;
}

/* ordinal 134: Windows.UI.Xaml initializes the UI thread as an application STA. */
HRESULT WINAPI RoInitializeASTA( void )
{
    HRESULT hr = RoInitialize( RO_INIT_SINGLETHREADED );
    FIXME( "semi-stub, RoInitialize(STA) -> %#lx\n", hr );
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    return hr;
}

/* ordinal 147 */
HRESULT WINAPI RoActivateInstanceAsUser( HSTRING classid, void *user, IInspectable **instance )
{
    FIXME( "%s, %p, %p: ignoring user\n", debugstr_w(WindowsGetStringRawBuffer(classid, NULL)), user, instance );
    return RoActivateInstance( classid, instance );
}

/* ordinal 148 */
HRESULT WINAPI RoGetActivationFactoryAsUser( HSTRING classid, REFIID iid, void *user, void **factory )
{
    FIXME( "%s, %s, %p, %p: ignoring user\n", debugstr_w(WindowsGetStringRawBuffer(classid, NULL)), debugstr_guid(iid), user, factory );
    return RoGetActivationFactory( classid, iid, factory );
}

/* ordinal 167 */
HRESULT WINAPI CoGetCallContextOfObject( IUnknown *object, REFIID iid, void **out )
{
    FIXME( "%p, %s, %p: stub\n", object, debugstr_guid(iid), out );
    if (out) *out = NULL;
    return E_NOTIMPL;
}

/* ordinal 168 */
HRESULT WINAPI CoImpersonateClientOfObject( IUnknown *object )
{
    FIXME( "%p: stub\n", object );
    return S_OK;
}

/***********************************************************************
 *           IsErrorPropagationEnabled    (combase.@)
 */
BOOL WINAPI IsErrorPropagationEnabled( void )
{
    return FALSE;
}


/***********************************************************************
 *           RoReportFailedDelegate    (combase.@)
 */
HRESULT WINAPI RoReportFailedDelegate( IUnknown *delegate, void *error_info )
{
    FIXME( "%p %p: stub\n", delegate, error_info );
    return S_OK;
}

/***********************************************************************
 *           CoCancelCall    (combase.@)
 */
HRESULT WINAPI CoCancelCall( DWORD thread_id, ULONG timeout )
{
    FIXME( "%lu %lu: stub\n", thread_id, timeout );
    return E_NOTIMPL;
}


HRESULT WINAPI RoGetActivatableClassRegistration( HSTRING classid, void **registration )
{
    FIXME( "%p %p: stub\n", classid, registration );
    if (registration) *registration = NULL;
    return E_NOTIMPL;
}

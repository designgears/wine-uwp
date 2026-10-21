/*
 * XThreading, XGame, XSystem, XNetworking and XGameRuntimeFeature
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
#include "winnls.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdk);

static ULONG WINAPI service_AddRef( gdk_object *iface ) { return 2; }
static ULONG WINAPI service_Release( gdk_object *iface ) { return 1; }

static HRESULT WINAPI service_notimpl( gdk_object *iface )
{
    return E_NOTIMPL;
}

static HRESULT query_service( gdk_object *service, const GUID *const *iids, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ))
    {
        while (*iids && !IsEqualGUID( iid, *iids )) iids++;
        if (!*iids) return E_NOINTERFACE;
    }
    *out = service;
    return S_OK;
}

/* --- IXThreadingImpl --- */

static HRESULT WINAPI threading_QueryInterface( gdk_object *iface, REFIID iid, void **out );

static HRESULT WINAPI threading_XAsyncGetStatus( gdk_object *iface, XAsyncBlock *block, BOOLEAN wait )
{
    return async_get_status( block, wait );
}

static HRESULT WINAPI threading_XAsyncGetResultSize( gdk_object *iface, XAsyncBlock *block, SIZE_T *size )
{
    return async_get_result_size( block, size );
}

static void WINAPI threading_XAsyncCancel( gdk_object *iface, XAsyncBlock *block )
{
    async_cancel( block );
}

static HRESULT WINAPI threading_XAsyncRun( gdk_object *iface, XAsyncBlock *block, XAsyncWork *work )
{
    return async_run( block, work );
}

static HRESULT WINAPI threading_XAsyncBegin( gdk_object *iface, XAsyncBlock *block, void *context,
                                             const void *identity, const char *name, XAsyncProvider *provider )
{
    return async_begin_external( block, context, identity, name, provider );
}

static HRESULT WINAPI threading_XAsyncSchedule( gdk_object *iface, XAsyncBlock *block, UINT32 delay )
{
    return async_schedule( block, delay );
}

static void WINAPI threading_XAsyncComplete( gdk_object *iface, XAsyncBlock *block, HRESULT result, SIZE_T size )
{
    async_complete( block, result, size );
}

static HRESULT WINAPI threading_XAsyncGetResult( gdk_object *iface, XAsyncBlock *block, const void *identity,
                                                 SIZE_T size, void *buffer, SIZE_T *used )
{
    return async_get_result( block, identity, size, buffer, used );
}

static HRESULT WINAPI threading_XTaskQueueCreate( gdk_object *iface, UINT32 work, UINT32 completion,
                                                  XTaskQueueHandle *queue )
{
    if (!queue) return E_POINTER;
    *queue = taskqueue_new( work, completion );
    return S_OK;
}

static HRESULT WINAPI threading_XTaskQueueCreateComposite( gdk_object *iface, XTaskQueuePortHandle work,
                                                           XTaskQueuePortHandle completion, XTaskQueueHandle *queue )
{
    return taskqueue_create_composite( work, completion, queue );
}

static HRESULT WINAPI threading_XTaskQueueGetPort( gdk_object *iface, XTaskQueueHandle queue, UINT32 port,
                                                   XTaskQueuePortHandle *handle )
{
    if (!handle) return E_POINTER;
    if (!(*handle = taskqueue_port( queue, port ))) return E_INVALIDARG;
    return S_OK;
}

static HRESULT WINAPI threading_XTaskQueueDuplicateHandle( gdk_object *iface, XTaskQueueHandle queue,
                                                           XTaskQueueHandle *duplicated )
{
    if (!duplicated || !queue) return E_INVALIDARG;
    taskqueue_addref( queue );
    *duplicated = queue;
    return S_OK;
}

static BOOLEAN WINAPI threading_XTaskQueueDispatch( gdk_object *iface, XTaskQueueHandle queue, UINT32 port,
                                                    UINT32 timeout )
{
    return taskqueue_dispatch( queue, port, timeout );
}

static void WINAPI threading_XTaskQueueCloseHandle( gdk_object *iface, XTaskQueueHandle queue )
{
    taskqueue_close( queue );
}

static HRESULT WINAPI threading_XTaskQueueSubmitCallback( gdk_object *iface, XTaskQueueHandle queue, UINT32 port,
                                                          void *context, TaskQueueCallback *callback )
{
    return taskqueue_submit( queue, port, 0, context, callback );
}

static HRESULT WINAPI threading_XTaskQueueSubmitDelayedCallback( gdk_object *iface, XTaskQueueHandle queue,
                                                                 UINT32 port, UINT32 delay, void *context,
                                                                 TaskQueueCallback *callback )
{
    return taskqueue_submit( queue, port, delay, context, callback );
}

static HRESULT WINAPI threading_XTaskQueueRegisterWaiter( gdk_object *iface, XTaskQueueHandle queue, UINT32 port,
                                                          HANDLE handle, void *context, TaskQueueCallback *callback,
                                                          XTaskQueueRegistrationToken *token )
{
    return taskqueue_register_waiter( queue, port, handle, context, callback, token );
}

static void WINAPI threading_XTaskQueueUnregisterWaiter( gdk_object *iface, XTaskQueueHandle queue,
                                                         XTaskQueueRegistrationToken token )
{
    taskqueue_unregister_waiter( token );
}

static HRESULT WINAPI threading_XTaskQueueTerminate( gdk_object *iface, XTaskQueueHandle queue, BOOLEAN wait,
                                                     void *context, TaskQueueTerminatedCallback *callback )
{
    return taskqueue_terminate( queue, wait, context, callback );
}

static HRESULT WINAPI threading_XTaskQueueRegisterMonitor( gdk_object *iface, XTaskQueueHandle queue, void *context,
                                                           TaskQueueMonitorCallback *callback,
                                                           XTaskQueueRegistrationToken *token )
{
    return taskqueue_register_monitor( queue, context, callback, token );
}

static void WINAPI threading_XTaskQueueUnregisterMonitor( gdk_object *iface, XTaskQueueHandle queue,
                                                          XTaskQueueRegistrationToken token )
{
    taskqueue_unregister_monitor( queue, token );
}

static BOOLEAN WINAPI threading_XTaskQueueGetCurrentProcessTaskQueue( gdk_object *iface, XTaskQueueHandle *queue )
{
    return taskqueue_get_process_queue( queue );
}

static void WINAPI threading_XTaskQueueSetCurrentProcessTaskQueue( gdk_object *iface, XTaskQueueHandle queue )
{
    taskqueue_set_process_queue( queue );
}

static void WINAPI threading_nothing( gdk_object *iface )
{
}

static BOOLEAN WINAPI threading_XTaskQueueIsEmpty( gdk_object *iface, XTaskQueueHandle queue, UINT32 port )
{
    return taskqueue_is_empty( queue, port );
}

static BOOLEAN WINAPI threading_false( gdk_object *iface )
{
    return FALSE;
}

static const void *const threading_vtbl[] =
{
    threading_QueryInterface,
    service_AddRef,
    service_Release,
    threading_XAsyncGetStatus,
    threading_XAsyncGetResultSize,
    threading_XAsyncCancel,
    threading_XAsyncRun,
    threading_XAsyncBegin,
    service_notimpl,                                /* padding */
    threading_XAsyncSchedule,
    threading_XAsyncComplete,
    threading_XAsyncGetResult,
    threading_XTaskQueueCreate,
    threading_XTaskQueueCreateComposite,
    threading_XTaskQueueGetPort,
    threading_XTaskQueueDuplicateHandle,
    threading_XTaskQueueDispatch,
    threading_XTaskQueueCloseHandle,
    threading_XTaskQueueSubmitCallback,
    threading_XTaskQueueSubmitDelayedCallback,
    threading_XTaskQueueRegisterWaiter,
    threading_XTaskQueueUnregisterWaiter,
    threading_XTaskQueueTerminate,
    threading_XTaskQueueRegisterMonitor,
    threading_XTaskQueueUnregisterMonitor,
    threading_XTaskQueueGetCurrentProcessTaskQueue,
    threading_XTaskQueueSetCurrentProcessTaskQueue,
    threading_nothing,                              /* XThreadSetTimeSensitive */
    threading_XTaskQueueIsEmpty,
    threading_nothing,                              /* XThreadAssertNotTimeSensitive */
    threading_false,                                /* XThreadIsTimeSensitive */
};

static gdk_object threading_service = { threading_vtbl };

static const GUID IID_IXThreadingImpl = {0x073b7dcb,0x1fcf,0x4030,{0x94,0xbe,0xe3,0xc9,0xeb,0x62,0x34,0x28}};
static const GUID *const threading_iids[] = { &IID_IXThreadingImpl, NULL };

static HRESULT WINAPI threading_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    return query_service( iface, threading_iids, iid, out );
}

HRESULT query_xthreading( REFIID iid, void **out )
{
    return query_service( &threading_service, threading_iids, iid, out );
}

/* --- IXGameImpl3 --- */

static HRESULT WINAPI game_QueryInterface( gdk_object *iface, REFIID iid, void **out );

static HRESULT WINAPI game_XGameGetXboxTitleId( gdk_object *iface, UINT32 *title_id )
{
    struct title_identity id;

    if (!title_id) return E_POINTER;
    load_title_identity( &id );
    if (!id.have_title)
    {
        WARN( "no title id (environment or MicrosoftGame.Config)\n" );
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    *title_id = id.title_id;
    TRACE( "-> %08X\n", *title_id );
    return S_OK;
}

static void WINAPI game_XLaunchNewGame( gdk_object *iface, const char *path, const char *args, void *user )
{
    FIXME( "%s %s: not implemented\n", debugstr_a(path), debugstr_a(args) );
}

static HRESULT WINAPI game_XLaunchRestartOnCrash( gdk_object *iface, const char *args, UINT32 reserved )
{
    return S_OK;
}

static const void *const game_vtbl[] =
{
    game_QueryInterface,
    service_AddRef,
    service_Release,
    game_XGameGetXboxTitleId,
    game_XLaunchNewGame,
    game_XLaunchRestartOnCrash,
};

static gdk_object game_service = { game_vtbl };

static const GUID IID_IXGameImpl = {0x973a344e,0x24bf,0x4d0f,{0x84,0x57,0x56,0xc5,0x34,0x89,0x2b,0x29}};
static const GUID IID_IXGameImpl2 = {0x50849859,0x0ad8,0x4f81,{0x80,0xe4,0x5b,0xc7,0x86,0x26,0xf8,0x52}};
static const GUID IID_IXGameImpl3 = {0x2549f142,0x6419,0x4a06,{0x97,0xb5,0x93,0x1a,0xab,0x7c,0x2f,0x34}};
static const GUID *const game_iids[] = { &IID_IXGameImpl, &IID_IXGameImpl2, &IID_IXGameImpl3, NULL };

static HRESULT WINAPI game_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    return query_service( iface, game_iids, iid, out );
}

HRESULT query_xgame( REFIID iid, void **out )
{
    return query_service( &game_service, game_iids, iid, out );
}

/* --- IXSystemImpl3 --- */

static HRESULT WINAPI system_QueryInterface( gdk_object *iface, REFIID iid, void **out );

static HRESULT copy_id( const char *value, INT32 size, char *buffer, SIZE_T *used )
{
    size_t need = strlen( value ) + 1;

    if (!buffer || size <= 0) return E_INVALIDARG;
    if ((SIZE_T)size < need) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( buffer, value, need );
    if (used) *used = need;
    return S_OK;
}

static HRESULT WINAPI system_XSystemGetConsoleId( gdk_object *iface, INT32 size, char *buffer, SIZE_T *used )
{
    /* a stable console id for a PC */
    return copy_id( "WINEUWPPC00000001", size, buffer, used );
}

static HRESULT WINAPI system_XSystemGetXboxLiveSandboxId( gdk_object *iface, INT32 size, char *buffer, SIZE_T *used )
{
    char env[64];
    const char *sandbox = "RETAIL";
    DWORD n = GetEnvironmentVariableA( "WINE_UWP_XBOX_SANDBOX", env, sizeof(env) );

    if (n && n < sizeof(env)) sandbox = env;
    TRACE( "-> %s\n", sandbox );
    return copy_id( sandbox, size, buffer, used );
}

static HRESULT WINAPI system_XSystemGetAppSpecificDeviceId( gdk_object *iface, INT32 size, char *buffer, SIZE_T *used )
{
    return copy_id( "wine-uwp-app-device-0001", size, buffer, used );
}

static HRESULT WINAPI system_XSystemHandleTrack( gdk_object *iface, void *callback, void *context )
{
    return S_OK;
}

static BOOLEAN WINAPI system_XSystemIsHandleValid( gdk_object *iface, void *handle )
{
    return handle != NULL;
}

static const void *const system_vtbl[] =
{
    system_QueryInterface,
    service_AddRef,
    service_Release,
    system_XSystemGetConsoleId,
    system_XSystemGetXboxLiveSandboxId,
    system_XSystemGetAppSpecificDeviceId,
    system_XSystemHandleTrack,
    system_XSystemIsHandleValid,
};

static gdk_object system_service = { system_vtbl };

static const GUID IID_IXSystemImpl = {0xe349bd1a,0xfc20,0x4e40,{0xb9,0x9c,0x41,0x78,0xcc,0x6b,0x40,0x9f}};
static const GUID IID_IXSystemImpl2 = {0x6fd71f09,0x7513,0x49f0,{0x89,0xbc,0xbf,0xaf,0x5d,0xf6,0xf8,0x52}};
static const GUID IID_IXSystemImpl3 = {0x67ce4bfc,0xb1d1,0x4ac7,{0xbc,0x3a,0xcb,0x92,0x19,0xa9,0x7a,0x85}};
static const GUID IID_IXSystemImpl4 = {0xdadc2895,0x34b0,0x4ef5,{0xa8,0x3e,0x45,0x11,0x4d,0x62,0x9b,0x80}};
static const GUID IID_IXSystemImpl5 = {0x1861cf2e,0xe18b,0x4834,{0xa9,0xf5,0xb4,0xa4,0xe6,0xef,0xb4,0xcf}};
static const GUID *const system_iids[] =
    { &IID_IXSystemImpl, &IID_IXSystemImpl2, &IID_IXSystemImpl3, &IID_IXSystemImpl4, &IID_IXSystemImpl5, NULL };

static HRESULT WINAPI system_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    return query_service( iface, system_iids, iid, out );
}

HRESULT query_xsystem( REFIID iid, void **out )
{
    return query_service( &system_service, system_iids, iid, out );
}

/* --- IXNetworkingImpl2 --- */

/* Halo's libHttpClient always calls XNetworkingQuerySecurityInformationForUrlUtf16Async, without asking
 * IsFeatureAvailable first, and then waits for it with XAsyncGetStatus(wait). A failure without XAsyncComplete left
 * it waiting for good: the offline spinner. */

#define SECURE_PROTOCOL_TLS1   0x00000080
#define SECURE_PROTOCOL_TLS1_1 0x00000200
#define SECURE_PROTOCOL_TLS1_2 0x00000800

typedef struct
{
    UINT32 enabledHttpSecurityProtocolFlags;
    SIZE_T thumbprintCount;
    void *thumbprints;
} XNetworkingSecurityInformation;

enum { XNetworkingConnectivityLevelHint_InternetAccess = 3 };
enum { XNetworkingConnectivityCostHint_Unrestricted = 1 };

typedef struct
{
    UINT32 connectivityLevel;
    UINT32 connectivityCost;
    UINT32 ianaInterfaceType;
    BOOLEAN networkInitialized;
    BOOLEAN approachingDataLimit;
    BOOLEAN overDataLimit;
    BOOLEAN roaming;
} XNetworkingConnectivityHint;

#define PREFERRED_UDP_PORT 3074

static HRESULT WINAPI security_info_provider( UINT32 op, const XAsyncProviderData *data )
{
    XNetworkingSecurityInformation *info;

    switch (op)
    {
    case XAsyncOp_DoWork:
        /* inline: the caller waits with XAsyncGetStatus(wait) */
        async_complete( data->async, S_OK, sizeof(XNetworkingSecurityInformation) );
        return S_OK;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XNetworkingSecurityInformation)) return E_INVALIDARG;
        info = data->buffer;
        info->enabledHttpSecurityProtocolFlags = SECURE_PROTOCOL_TLS1 | SECURE_PROTOCOL_TLS1_1 | SECURE_PROTOCOL_TLS1_2;
        info->thumbprintCount = 0;
        info->thumbprints = NULL;
        return S_OK;
    }
    return S_OK;
}

static HRESULT security_info_begin( XAsyncBlock *block, const char *url )
{
    if (!block) return E_INVALIDARG;
    TRACE( "%s\n", debugstr_a(url) );
    return async_begin( block, NULL, security_info_provider, "XNetworkingQuerySecurityInformation",
                        security_info_provider );
}

static HRESULT security_info_result( XAsyncBlock *block, SIZE_T size, SIZE_T *used, UINT8 *buffer,
                                     XNetworkingSecurityInformation **info )
{
    HRESULT hr;

    if (!buffer || !info) return E_POINTER;
    *info = NULL;
    if (FAILED(hr = async_get_result( block, NULL, size, buffer, used ))) return hr;
    *info = (XNetworkingSecurityInformation *)buffer;
    return S_OK;
}

static HRESULT WINAPI udp_port_provider( UINT32 op, const XAsyncProviderData *data )
{
    switch (op)
    {
    case XAsyncOp_DoWork:
        async_complete( data->async, S_OK, sizeof(UINT16) );
        return S_OK;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(UINT16)) return E_INVALIDARG;
        *(UINT16 *)data->buffer = PREFERRED_UDP_PORT;
        return S_OK;
    }
    return S_OK;
}

static HRESULT WINAPI networking_QueryInterface( gdk_object *iface, REFIID iid, void **out );

static HRESULT WINAPI networking_QueryPreferredLocalUdpMultiplayerPort( gdk_object *iface, UINT16 *port )
{
    if (!port) return E_POINTER;
    *port = PREFERRED_UDP_PORT;
    return S_OK;
}

static HRESULT WINAPI networking_QueryPreferredLocalUdpMultiplayerPortAsync( gdk_object *iface, XAsyncBlock *block )
{
    if (!block) return E_INVALIDARG;
    return async_begin( block, NULL, NULL, "XNetworkingQueryPreferredLocalUdpMultiplayerPort", udp_port_provider );
}

static HRESULT WINAPI networking_QueryPreferredLocalUdpMultiplayerPortAsyncResult( gdk_object *iface,
                                                                                   XAsyncBlock *block, UINT16 *port )
{
    if (!port) return E_POINTER;
    return async_get_result( block, NULL, sizeof(UINT16), port, NULL );
}

static HRESULT WINAPI networking_register( gdk_object *iface, XTaskQueueHandle queue, void *context, void *callback,
                                           XTaskQueueRegistrationToken *token )
{
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI networking_unregister( gdk_object *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    return TRUE;
}

static HRESULT WINAPI networking_QuerySecurityInformationForUrlAsync( gdk_object *iface, const char *url,
                                                                      XAsyncBlock *block )
{
    return security_info_begin( block, url );
}

static HRESULT WINAPI networking_result_size( gdk_object *iface, XAsyncBlock *block, SIZE_T *size )
{
    return async_get_result_size( block, size );
}

static HRESULT WINAPI networking_QuerySecurityInformationResult( gdk_object *iface, XAsyncBlock *block, SIZE_T size,
                                                                 SIZE_T *used, UINT8 *buffer,
                                                                 XNetworkingSecurityInformation **info )
{
    return security_info_result( block, size, used, buffer, info );
}

static HRESULT WINAPI networking_QuerySecurityInformationForUrlUtf16Async( gdk_object *iface, const WCHAR *url,
                                                                           XAsyncBlock *block )
{
    char narrow[512] = {0};

    if (url) WideCharToMultiByte( CP_UTF8, 0, url, -1, narrow, sizeof(narrow) - 1, NULL, NULL );
    return security_info_begin( block, narrow );
}

/* an empty thumbprint list: Schannel's chain validation decides */
static HRESULT WINAPI networking_VerifyServerCertificate( gdk_object *iface, void *request,
                                                          const XNetworkingSecurityInformation *info )
{
    return S_OK;
}

static HRESULT WINAPI networking_GetConnectivityHint( gdk_object *iface, XNetworkingConnectivityHint *hint )
{
    if (!hint) return E_POINTER;
    memset( hint, 0, sizeof(*hint) );
    hint->connectivityLevel = XNetworkingConnectivityLevelHint_InternetAccess;
    hint->connectivityCost = XNetworkingConnectivityCostHint_Unrestricted;
    hint->ianaInterfaceType = 6; /* ethernet */
    hint->networkInitialized = TRUE;
    return S_OK;
}

static BOOLEAN WINAPI networking_UnregisterConnectivityHintChanged( gdk_object *iface,
                                                                    XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    return TRUE;
}

static HRESULT WINAPI networking_QueryConfigurationSetting( gdk_object *iface, UINT32 setting, UINT64 *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI networking_SetConfigurationSetting( gdk_object *iface, UINT32 setting, UINT64 value )
{
    return S_OK;
}

static HRESULT WINAPI networking_QueryStatistics( gdk_object *iface, UINT32 type, void *buffer )
{
    if (buffer) memset( buffer, 0, 5 * sizeof(UINT64) );
    return S_OK;
}

static const void *const networking_vtbl[] =
{
    networking_QueryInterface,
    service_AddRef,
    service_Release,
    networking_QueryPreferredLocalUdpMultiplayerPort,
    networking_QueryPreferredLocalUdpMultiplayerPortAsync,
    networking_QueryPreferredLocalUdpMultiplayerPortAsyncResult,
    networking_register,                            /* RegisterPreferredLocalUdpMultiplayerPortChanged */
    networking_unregister,                          /* UnregisterPreferredLocalUdpMultiplayerPortChanged */
    networking_QuerySecurityInformationForUrlAsync,
    networking_result_size,                         /* QuerySecurityInformationForUrlAsyncResultSize */
    networking_QuerySecurityInformationResult,      /* QuerySecurityInformationForUrlAsyncResult */
    networking_QuerySecurityInformationForUrlUtf16Async,
    networking_result_size,                         /* QuerySecurityInformationForUrlUtf16AsyncResultSize */
    networking_QuerySecurityInformationResult,      /* QuerySecurityInformationForUrlUtf16AsyncResult */
    networking_VerifyServerCertificate,
    networking_GetConnectivityHint,
    networking_register,                            /* RegisterConnectivityHintChanged */
    networking_UnregisterConnectivityHintChanged,
    networking_QueryConfigurationSetting,
    networking_SetConfigurationSetting,
    networking_QueryStatistics,
};

static gdk_object networking_service = { networking_vtbl };

static const GUID IID_IXNetworkingImpl = {0x37e56907,0x2f10,0x41e8,{0xb7,0x2f,0x36,0xed,0xb1,0x85,0x33,0x1a}};
static const GUID IID_IXNetworkingImpl2 = {0xbf2346b2,0x39af,0x4658,{0xb5,0xea,0x44,0x71,0x3c,0x7e,0x83,0xb3}};
static const GUID *const networking_iids[] = { &IID_IXNetworkingImpl, &IID_IXNetworkingImpl2, NULL };

static HRESULT WINAPI networking_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    return query_service( iface, networking_iids, iid, out );
}

HRESULT query_xnetworking( REFIID iid, void **out )
{
    return query_service( &networking_service, networking_iids, iid, out );
}

/* libHttpClient's XsapiThunks look these up by name for the security path */

HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlAsync( const char *url, XAsyncBlock *block )
{
    return security_info_begin( block, url );
}

HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlAsyncResultSize( XAsyncBlock *block, SIZE_T *size )
{
    return async_get_result_size( block, size );
}

HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlAsyncResult( XAsyncBlock *block, SIZE_T size, SIZE_T *used,
                                                                     UINT8 *buffer,
                                                                     XNetworkingSecurityInformation **info )
{
    return security_info_result( block, size, used, buffer, info );
}

HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlUtf16Async( const WCHAR *url, XAsyncBlock *block )
{
    return networking_QuerySecurityInformationForUrlUtf16Async( &networking_service, url, block );
}

HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize( XAsyncBlock *block, SIZE_T *size )
{
    return async_get_result_size( block, size );
}

HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult( XAsyncBlock *block, SIZE_T size,
                                                                          SIZE_T *used, UINT8 *buffer,
                                                                          XNetworkingSecurityInformation **info )
{
    return security_info_result( block, size, used, buffer, info );
}

HRESULT WINAPI XNetworkingVerifyServerCertificate( void *request, const XNetworkingSecurityInformation *info )
{
    return S_OK;
}

HRESULT WINAPI XNetworkingGetConnectivityHint( XNetworkingConnectivityHint *hint )
{
    return networking_GetConnectivityHint( &networking_service, hint );
}

/* --- IXGameRuntimeFeatureImpl --- */

/* the XGameRuntimeFeature enumeration's order */
enum
{
    XFeature_XAsync = 2,
    XFeature_XAsyncProvider = 3,
    XFeature_XGame = 5,
    XFeature_XNetworking = 10,
    XFeature_XPackage = 11,
    XFeature_XStore = 14,
    XFeature_XSystem = 15,
    XFeature_XTaskQueue = 16,
    XFeature_XThread = 17,
    XFeature_XUser = 18,
    XFeature_XError = 19,
};

/* the services implemented here; other titles than Halo do gate on XNetworking */
BOOLEAN feature_available( UINT32 feature )
{
    switch (feature)
    {
    case XFeature_XAsync:
    case XFeature_XAsyncProvider:
    case XFeature_XGame:
    case XFeature_XNetworking:
    case XFeature_XPackage:
    case XFeature_XStore:
    case XFeature_XSystem:
    case XFeature_XTaskQueue:
    case XFeature_XThread:
    case XFeature_XUser:
    case XFeature_XError:
        return TRUE;
    }
    return FALSE;
}

static HRESULT WINAPI feature_QueryInterface( gdk_object *iface, REFIID iid, void **out );

static BOOLEAN WINAPI feature_IsFeatureAvailable( gdk_object *iface, UINT32 feature )
{
    BOOLEAN available = feature_available( feature );
    TRACE( "%u -> %u\n", feature, available );
    return available;
}

static const void *const feature_vtbl[] =
{
    feature_QueryInterface,
    service_AddRef,
    service_Release,
    feature_IsFeatureAvailable,
};

static gdk_object feature_service = { feature_vtbl };

/* the coclass's UUID is the interface's */
static const GUID IID_IXGameRuntimeFeatureImpl = {0x8836fe87,0xedb9,0x4fe3,{0x8d,0xad,0x05,0xf0,0xd2,0xcd,0x5b,0x40}};
static const GUID *const feature_iids[] = { &IID_IXGameRuntimeFeatureImpl, NULL };

static HRESULT WINAPI feature_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    return query_service( iface, feature_iids, iid, out );
}

HRESULT query_xfeature( REFIID iid, void **out )
{
    return query_service( &feature_service, feature_iids, iid, out );
}

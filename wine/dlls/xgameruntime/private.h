/*
 * GDK game runtime (xgameruntime.dll)
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

#ifndef __WINE_XGAMERUNTIME_PRIVATE_H
#define __WINE_XGAMERUNTIME_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "objbase.h"

/*
 * GdkSupport, the GDK's in-title shim, loads xgameruntime.dll and asks QueryApiImpl for one object per service
 * (XPackage, XUser, XStore, XThreading, ...). Those are private COM-style interfaces: IUnknown plus flat methods in
 * the order of the GDK's public API. The layouts follow the clean-room xgameruntime project's IDL
 * (github.com/nocts-software/xgameruntime). Objects here are static, so their vtables are plain arrays.
 */

typedef struct { const void *const *vtbl; } gdk_object;

/* --- XTaskQueue / XAsync --- */

typedef struct task_queue *XTaskQueueHandle;
typedef struct task_port *XTaskQueuePortHandle;
typedef struct { UINT64 token; } XTaskQueueRegistrationToken;

enum { XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_SerializedThreadPool,
       XTaskQueueDispatchMode_Immediate };
enum { XTaskQueuePort_Work, XTaskQueuePort_Completion };

typedef struct XAsyncBlock XAsyncBlock;
typedef void (WINAPI XAsyncCompletionRoutine)( XAsyncBlock *block );
typedef HRESULT (WINAPI XAsyncWork)( XAsyncBlock *block );

struct XAsyncBlock
{
    XTaskQueueHandle queue;
    void *context;
    XAsyncCompletionRoutine *callback;
    unsigned char internal[sizeof(void *) * 4];
};

enum { XAsyncOp_Begin, XAsyncOp_DoWork, XAsyncOp_GetResult, XAsyncOp_Cancel, XAsyncOp_Cleanup };

typedef struct
{
    XAsyncBlock *async;
    SIZE_T bufferSize;
    void *buffer;
    void *context;
} XAsyncProviderData;

typedef HRESULT (WINAPI XAsyncProvider)( UINT32 op, const XAsyncProviderData *data );

typedef void (WINAPI TaskQueueCallback)( void *context, BOOLEAN canceled );
typedef void (WINAPI TaskQueueMonitorCallback)( void *context, XTaskQueueHandle queue, UINT32 port );
typedef void (WINAPI TaskQueueTerminatedCallback)( void *context );

extern XTaskQueueHandle taskqueue_new( UINT32 work, UINT32 completion );
extern void taskqueue_addref( XTaskQueueHandle q );
extern void taskqueue_release( XTaskQueueHandle q );
extern XTaskQueueHandle taskqueue_resolve( XTaskQueueHandle q );
extern XTaskQueuePortHandle taskqueue_port( XTaskQueueHandle q, UINT32 port );
extern HRESULT taskqueue_submit( XTaskQueueHandle q, UINT32 port, UINT32 delay, void *context, TaskQueueCallback *callback );
extern BOOLEAN taskqueue_dispatch( XTaskQueueHandle q, UINT32 port, UINT32 timeout );
extern BOOLEAN taskqueue_is_empty( XTaskQueueHandle q, UINT32 port );
extern HRESULT taskqueue_register_waiter( XTaskQueueHandle q, UINT32 port, HANDLE handle, void *context,
                                          TaskQueueCallback *callback, XTaskQueueRegistrationToken *token );
extern void taskqueue_unregister_waiter( XTaskQueueRegistrationToken token );
extern HRESULT taskqueue_register_monitor( XTaskQueueHandle q, void *context, TaskQueueMonitorCallback *callback,
                                           XTaskQueueRegistrationToken *token );
extern void taskqueue_unregister_monitor( XTaskQueueHandle q, XTaskQueueRegistrationToken token );
extern HRESULT taskqueue_terminate( XTaskQueueHandle q, BOOLEAN wait, void *context, TaskQueueTerminatedCallback *callback );
extern HRESULT taskqueue_create_composite( XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle *out );
extern BOOLEAN taskqueue_get_process_queue( XTaskQueueHandle *out );
extern void taskqueue_set_process_queue( XTaskQueueHandle q );
extern void taskqueue_close( XTaskQueueHandle q );

extern HRESULT async_begin( XAsyncBlock *block, void *context, const void *identity, const char *name, XAsyncProvider *provider );
extern HRESULT async_begin_external( XAsyncBlock *block, void *context, const void *identity, const char *name,
                                     XAsyncProvider *provider );
extern HRESULT async_begin_empty( XAsyncBlock *block, const char *name );
extern void async_complete( XAsyncBlock *block, HRESULT result, SIZE_T size );
extern HRESULT async_schedule( XAsyncBlock *block, UINT32 delay );
extern HRESULT async_get_status( XAsyncBlock *block, BOOLEAN wait );
extern HRESULT async_get_result_size( XAsyncBlock *block, SIZE_T *size );
extern HRESULT async_get_result( XAsyncBlock *block, const void *identity, SIZE_T size, void *buffer, SIZE_T *used );
extern void async_cancel( XAsyncBlock *block );
extern HRESULT async_run( XAsyncBlock *block, XAsyncWork *work );

/* --- services --- */

extern HRESULT package_initialize(void);
extern HRESULT query_xpackage( REFIID iid, void **out );
extern HRESULT query_xuser( REFIID iid, void **out );
extern HRESULT query_xstore( REFIID iid, void **out );
extern HRESULT query_xthreading( REFIID iid, void **out );
extern HRESULT query_xgame( REFIID iid, void **out );
extern HRESULT query_xsystem( REFIID iid, void **out );
extern HRESULT query_xfeature( REFIID iid, void **out );
extern HRESULT query_xnetworking( REFIID iid, void **out );
extern BOOLEAN feature_available( UINT32 feature );

/* Title identity from the environment or MicrosoftGame.Config, and the xboxauth.dll bridge. */
struct title_identity
{
    UINT32 title_id;
    BOOL have_title;
    char msa_app_id[64];
    BOOL have_msa;
};
extern void load_title_identity( struct title_identity *id );
extern void warm_xboxauth_title_context(void);

/* the stage directory: WINE_UWP_PACKAGE_PATH, else beside this DLL or its parent (a copy in stage/ or stage/game/) */
typedef BOOL (*stage_dir_callback)( const WCHAR *dir, void *context );
extern BOOL for_stage_dirs( stage_dir_callback callback, void *context );

extern HRESULT copy_string( const char *value, SIZE_T size, char *buffer );

#endif /* __WINE_XGAMERUNTIME_PRIVATE_H */

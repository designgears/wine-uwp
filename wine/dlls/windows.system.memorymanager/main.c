/*
 * Windows.System.MemoryManager
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

/*
 * Windows answers from the resource manager service through rmclient.dll, which a prefix does not have: its
 * add_AppMemoryUsageIncreased returned EPT_S_NOT_REGISTERED, and Solitaire, which subscribes in its page
 * constructor, stopped in OnLaunched without a word. There is no app memory quota here and nothing raises the
 * pressure events, so the usage is the process's own, the limit is the machine's memory, and the registrations are
 * kept until removed. Windows registers the class in rmclient.dll; this module has its own name so as not to take
 * over rmclient's other classes.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "winstring.h"
#include "psapi.h"
#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_System
#include "windows.foundation.h"
#include "windows.system.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(memorymanager);

static const WCHAR class_name[] = L"Windows.System.MemoryManager";

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT make_name( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

static BOOL is_base_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject );
}

/* the process's commit charge is PagefileUsage: Windows documents PROCESS_MEMORY_COUNTERS_EX's PrivateUsage as the same
 * value, and Wine fills only the plain structure */
static HRESULT process_counters( PROCESS_MEMORY_COUNTERS *counters )
{
    memset( counters, 0, sizeof(*counters) );
    counters->cb = sizeof(*counters);
    if (!GetProcessMemoryInfo( GetCurrentProcess(), counters, sizeof(*counters) ))
        return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static HRESULT memory_status( MEMORYSTATUSEX *status )
{
    status->dwLength = sizeof(*status);
    return GlobalMemoryStatusEx( status ) ? S_OK : HRESULT_FROM_WIN32( GetLastError() );
}

/* ---- AppMemoryReport and ProcessMemoryReport: snapshots ---- */

struct app_report
{
    IAppMemoryReport IAppMemoryReport_iface;
    LONG ref;
    UINT64 private_commit, peak_private_commit, total_commit, commit_limit;
};

static inline struct app_report *impl_from_IAppMemoryReport( IAppMemoryReport *iface )
{
    return CONTAINING_RECORD( iface, struct app_report, IAppMemoryReport_iface );
}

static HRESULT WINAPI app_report_QueryInterface( IAppMemoryReport *iface, REFIID iid, void **out )
{
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_IAppMemoryReport ))
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    *out = iface;
    IAppMemoryReport_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI app_report_AddRef( IAppMemoryReport *iface )
{
    return InterlockedIncrement( &impl_from_IAppMemoryReport( iface )->ref );
}

static ULONG WINAPI app_report_Release( IAppMemoryReport *iface )
{
    struct app_report *impl = impl_from_IAppMemoryReport( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI app_report_GetIids( IAppMemoryReport *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI app_report_GetRuntimeClassName( IAppMemoryReport *iface, HSTRING *name ) { return make_name( RuntimeClass_Windows_System_AppMemoryReport, name ); }
static HRESULT WINAPI app_report_GetTrustLevel( IAppMemoryReport *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

#define REPORT_GETTER( type, field ) \
    static HRESULT WINAPI type##_get_##field( I##type *iface, UINT64 *value ) \
    { \
        if (!value) return E_POINTER; \
        *value = impl_from_I##type( iface )->field; \
        return S_OK; \
    }

REPORT_GETTER( AppMemoryReport, private_commit )
REPORT_GETTER( AppMemoryReport, peak_private_commit )
REPORT_GETTER( AppMemoryReport, total_commit )
REPORT_GETTER( AppMemoryReport, commit_limit )

static const struct IAppMemoryReportVtbl app_report_vtbl =
{
    app_report_QueryInterface,
    app_report_AddRef,
    app_report_Release,
    /* IInspectable methods */
    app_report_GetIids,
    app_report_GetRuntimeClassName,
    app_report_GetTrustLevel,
    /* IAppMemoryReport methods */
    AppMemoryReport_get_private_commit,
    AppMemoryReport_get_peak_private_commit,
    AppMemoryReport_get_total_commit,
    AppMemoryReport_get_commit_limit,
};

struct process_report
{
    IProcessMemoryReport IProcessMemoryReport_iface;
    LONG ref;
    UINT64 private_working_set, total_working_set;
};

static inline struct process_report *impl_from_IProcessMemoryReport( IProcessMemoryReport *iface )
{
    return CONTAINING_RECORD( iface, struct process_report, IProcessMemoryReport_iface );
}

static HRESULT WINAPI process_report_QueryInterface( IProcessMemoryReport *iface, REFIID iid, void **out )
{
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_IProcessMemoryReport ))
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    *out = iface;
    IProcessMemoryReport_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI process_report_AddRef( IProcessMemoryReport *iface )
{
    return InterlockedIncrement( &impl_from_IProcessMemoryReport( iface )->ref );
}

static ULONG WINAPI process_report_Release( IProcessMemoryReport *iface )
{
    struct process_report *impl = impl_from_IProcessMemoryReport( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI process_report_GetIids( IProcessMemoryReport *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI process_report_GetRuntimeClassName( IProcessMemoryReport *iface, HSTRING *name ) { return make_name( RuntimeClass_Windows_System_ProcessMemoryReport, name ); }
static HRESULT WINAPI process_report_GetTrustLevel( IProcessMemoryReport *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

REPORT_GETTER( ProcessMemoryReport, private_working_set )
REPORT_GETTER( ProcessMemoryReport, total_working_set )

static const struct IProcessMemoryReportVtbl process_report_vtbl =
{
    process_report_QueryInterface,
    process_report_AddRef,
    process_report_Release,
    /* IInspectable methods */
    process_report_GetIids,
    process_report_GetRuntimeClassName,
    process_report_GetTrustLevel,
    /* IProcessMemoryReport methods */
    ProcessMemoryReport_get_private_working_set,
    ProcessMemoryReport_get_total_working_set,
};

/* ---- the statics ---- */

enum event_kind { USAGE_INCREASED, USAGE_DECREASED, LIMIT_CHANGING };

struct subscription
{
    struct list entry;
    enum event_kind kind;
    INT64 token;
    IUnknown *handler;
};

static struct
{
    IActivationFactory IActivationFactory_iface;
    IMemoryManagerStatics IMemoryManagerStatics_iface;
    IMemoryManagerStatics2 IMemoryManagerStatics2_iface;
} manager;

static CRITICAL_SECTION subscriptions_cs;
static CRITICAL_SECTION_DEBUG subscriptions_cs_debug =
{
    0, 0, &subscriptions_cs,
    { &subscriptions_cs_debug.ProcessLocksList, &subscriptions_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": subscriptions_cs") }
};
static CRITICAL_SECTION subscriptions_cs = { &subscriptions_cs_debug, -1, 0, 0, 0, 0 };
static struct list subscriptions = LIST_INIT( subscriptions );
static INT64 next_token;

/* registering an event has to succeed even though nothing ever raises it here */
static HRESULT subscribe( enum event_kind kind, IUnknown *handler, EventRegistrationToken *token )
{
    struct subscription *sub;

    if (!token) return E_POINTER;
    token->value = 0;
    if (!handler) return E_INVALIDARG;
    if (!(sub = calloc( 1, sizeof(*sub) ))) return E_OUTOFMEMORY;
    sub->kind = kind;
    sub->handler = handler;
    IUnknown_AddRef( handler );
    EnterCriticalSection( &subscriptions_cs );
    sub->token = token->value = ++next_token;
    list_add_tail( &subscriptions, &sub->entry );
    LeaveCriticalSection( &subscriptions_cs );
    TRACE( "event %u, token %#I64x\n", kind, token->value );
    return S_OK;
}

static HRESULT unsubscribe( enum event_kind kind, EventRegistrationToken token )
{
    struct subscription *sub, *found = NULL;

    EnterCriticalSection( &subscriptions_cs );
    LIST_FOR_EACH_ENTRY( sub, &subscriptions, struct subscription, entry )
    {
        if (sub->kind != kind || sub->token != token.value) continue;
        list_remove( &sub->entry );
        found = sub;
        break;
    }
    LeaveCriticalSection( &subscriptions_cs );
    if (found)
    {
        IUnknown_Release( found->handler );
        free( found );
    }
    return S_OK;
}

static HRESULT query_interface( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &manager.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IMemoryManagerStatics )) *out = &manager.IMemoryManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IMemoryManagerStatics2 )) *out = &manager.IMemoryManagerStatics2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out ) { return query_interface( iid, out ); }
static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return make_name( class_name, name ); }
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* a static class */
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

static HRESULT WINAPI statics_QueryInterface( IMemoryManagerStatics *iface, REFIID iid, void **out ) { return query_interface( iid, out ); }
static ULONG WINAPI statics_AddRef( IMemoryManagerStatics *iface ) { return 2; }
static ULONG WINAPI statics_Release( IMemoryManagerStatics *iface ) { return 1; }
static HRESULT WINAPI statics_GetIids( IMemoryManagerStatics *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI statics_GetRuntimeClassName( IMemoryManagerStatics *iface, HSTRING *name ) { return make_name( class_name, name ); }
static HRESULT WINAPI statics_GetTrustLevel( IMemoryManagerStatics *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI statics_get_AppMemoryUsage( IMemoryManagerStatics *iface, UINT64 *value )
{
    PROCESS_MEMORY_COUNTERS counters;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = 0;
    if (FAILED(hr = process_counters( &counters ))) return hr;
    *value = counters.PagefileUsage;
    return S_OK;
}

static HRESULT WINAPI statics_get_AppMemoryUsageLimit( IMemoryManagerStatics *iface, UINT64 *value )
{
    MEMORYSTATUSEX status;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = 0;
    if (FAILED(hr = memory_status( &status ))) return hr;
    *value = status.ullTotalPhys;
    return S_OK;
}

static HRESULT WINAPI statics_get_AppMemoryUsageLevel( IMemoryManagerStatics *iface, AppMemoryUsageLevel *value )
{
    UINT64 usage, limit;
    HRESULT hr;

    if (!value) return E_POINTER;
    if (FAILED(hr = statics_get_AppMemoryUsage( iface, &usage ))) return hr;
    if (FAILED(hr = statics_get_AppMemoryUsageLimit( iface, &limit ))) return hr;
    if (usage >= limit) *value = AppMemoryUsageLevel_OverLimit;
    else if (usage >= limit - limit / 5) *value = AppMemoryUsageLevel_High;
    else if (usage >= limit / 2) *value = AppMemoryUsageLevel_Medium;
    else *value = AppMemoryUsageLevel_Low;
    return S_OK;
}

static HRESULT WINAPI statics_add_AppMemoryUsageIncreased( IMemoryManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                           EventRegistrationToken *token )
{
    return subscribe( USAGE_INCREASED, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics_remove_AppMemoryUsageIncreased( IMemoryManagerStatics *iface, EventRegistrationToken token )
{
    return unsubscribe( USAGE_INCREASED, token );
}

static HRESULT WINAPI statics_add_AppMemoryUsageDecreased( IMemoryManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                           EventRegistrationToken *token )
{
    return subscribe( USAGE_DECREASED, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics_remove_AppMemoryUsageDecreased( IMemoryManagerStatics *iface, EventRegistrationToken token )
{
    return unsubscribe( USAGE_DECREASED, token );
}

static HRESULT WINAPI statics_add_AppMemoryUsageLimitChanging( IMemoryManagerStatics *iface,
                                                               IEventHandler_AppMemoryUsageLimitChangingEventArgs *handler,
                                                               EventRegistrationToken *token )
{
    return subscribe( LIMIT_CHANGING, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics_remove_AppMemoryUsageLimitChanging( IMemoryManagerStatics *iface, EventRegistrationToken token )
{
    return unsubscribe( LIMIT_CHANGING, token );
}

static const struct IMemoryManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IMemoryManagerStatics methods */
    statics_get_AppMemoryUsage,
    statics_get_AppMemoryUsageLimit,
    statics_get_AppMemoryUsageLevel,
    statics_add_AppMemoryUsageIncreased,
    statics_remove_AppMemoryUsageIncreased,
    statics_add_AppMemoryUsageDecreased,
    statics_remove_AppMemoryUsageDecreased,
    statics_add_AppMemoryUsageLimitChanging,
    statics_remove_AppMemoryUsageLimitChanging,
};

static HRESULT WINAPI statics2_QueryInterface( IMemoryManagerStatics2 *iface, REFIID iid, void **out ) { return query_interface( iid, out ); }
static ULONG WINAPI statics2_AddRef( IMemoryManagerStatics2 *iface ) { return 2; }
static ULONG WINAPI statics2_Release( IMemoryManagerStatics2 *iface ) { return 1; }
static HRESULT WINAPI statics2_GetIids( IMemoryManagerStatics2 *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI statics2_GetRuntimeClassName( IMemoryManagerStatics2 *iface, HSTRING *name ) { return make_name( class_name, name ); }
static HRESULT WINAPI statics2_GetTrustLevel( IMemoryManagerStatics2 *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

/* .NET Native asks for these while its DLLs initialise */
static HRESULT WINAPI statics2_GetAppMemoryReport( IMemoryManagerStatics2 *iface, IAppMemoryReport **result )
{
    PROCESS_MEMORY_COUNTERS counters;
    MEMORYSTATUSEX status;
    struct app_report *report;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    if (FAILED(hr = process_counters( &counters )) || FAILED(hr = memory_status( &status ))) return hr;
    if (!(report = calloc( 1, sizeof(*report) ))) return E_OUTOFMEMORY;
    report->IAppMemoryReport_iface.lpVtbl = &app_report_vtbl;
    report->ref = 1;
    report->private_commit = counters.PagefileUsage;
    report->peak_private_commit = counters.PeakPagefileUsage;
    report->total_commit = counters.PagefileUsage;
    report->commit_limit = status.ullTotalPageFile;
    *result = &report->IAppMemoryReport_iface;
    return S_OK;
}

static HRESULT WINAPI statics2_GetProcessMemoryReport( IMemoryManagerStatics2 *iface, IProcessMemoryReport **result )
{
    PROCESS_MEMORY_COUNTERS counters;
    struct process_report *report;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    if (FAILED(hr = process_counters( &counters ))) return hr;
    if (!(report = calloc( 1, sizeof(*report) ))) return E_OUTOFMEMORY;
    report->IProcessMemoryReport_iface.lpVtbl = &process_report_vtbl;
    report->ref = 1;
    /* the working set is not split into private and shared pages here */
    report->private_working_set = counters.WorkingSetSize;
    report->total_working_set = counters.WorkingSetSize;
    *result = &report->IProcessMemoryReport_iface;
    return S_OK;
}

static const struct IMemoryManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IMemoryManagerStatics2 methods */
    statics2_GetAppMemoryReport,
    statics2_GetProcessMemoryReport,
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    manager.IActivationFactory_iface.lpVtbl = &factory_vtbl;
    manager.IMemoryManagerStatics_iface.lpVtbl = &statics_vtbl;
    manager.IMemoryManagerStatics2_iface.lpVtbl = &statics2_vtbl;

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), class_name )) return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &manager.IActivationFactory_iface;
    return S_OK;
}

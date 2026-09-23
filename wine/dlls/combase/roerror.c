/*
 * WinRT error origination (RoOriginateError and friends)
 *
 * The originated error is a single object exposing IRestrictedErrorInfo, IErrorInfo and
 * ILanguageExceptionErrorInfo2, stored in the thread's IErrorInfo slot exactly like Windows does, so
 * GetErrorInfo() and GetRestrictedErrorInfo() both find it. Language runtimes (.NET Native's
 * McgMarshal, C++/WinRT) round-trip their exception objects through the language exception.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "oleauto.h"
#include "roapi.h"
#include "roerrorapi.h"
#include "restrictederrorinfo.h"
#include "winstring.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

HRESULT combase_get_errorinfo( IErrorInfo **info );
HRESULT combase_set_errorinfo( IErrorInfo *info );

static const IID IID_ILanguageExceptionErrorInfo  = {0x04a2dbf3, 0xdf83, 0x116c, {0x09, 0x46, 0x08, 0x12, 0xab, 0xf6, 0xe0, 0x7d}};
static const IID IID_ILanguageExceptionErrorInfo2 = {0x5746e5c4, 0x5b97, 0x424c, {0xb6, 0x20, 0x28, 0x22, 0x91, 0x57, 0x34, 0xdd}};

typedef struct ILanguageExceptionErrorInfo2 ILanguageExceptionErrorInfo2;
struct ILanguageExceptionErrorInfo2Vtbl
{
    HRESULT (WINAPI *QueryInterface)(ILanguageExceptionErrorInfo2 *iface, REFIID riid, void **out);
    ULONG   (WINAPI *AddRef)(ILanguageExceptionErrorInfo2 *iface);
    ULONG   (WINAPI *Release)(ILanguageExceptionErrorInfo2 *iface);
    HRESULT (WINAPI *GetLanguageException)(ILanguageExceptionErrorInfo2 *iface, IUnknown **exception);
    HRESULT (WINAPI *GetPreviousLanguageExceptionErrorInfo)(ILanguageExceptionErrorInfo2 *iface, ILanguageExceptionErrorInfo2 **previous);
    HRESULT (WINAPI *CapturePropagationContext)(ILanguageExceptionErrorInfo2 *iface, IUnknown *exception);
    HRESULT (WINAPI *GetPropagationContextHead)(ILanguageExceptionErrorInfo2 *iface, ILanguageExceptionErrorInfo2 **head);
};
struct ILanguageExceptionErrorInfo2 { const struct ILanguageExceptionErrorInfo2Vtbl *lpVtbl; };

struct ro_error
{
    IRestrictedErrorInfo IRestrictedErrorInfo_iface;
    IErrorInfo IErrorInfo_iface;
    ILanguageExceptionErrorInfo2 ILanguageExceptionErrorInfo2_iface;
    LONG ref;
    HRESULT hr;
    BSTR description;
    BSTR restricted;
    IUnknown *language_exception;
    IUnknown *propagation_context;
    struct ro_error *previous;
};

static inline struct ro_error *impl_from_IRestrictedErrorInfo( IRestrictedErrorInfo *iface )
{
    return CONTAINING_RECORD( iface, struct ro_error, IRestrictedErrorInfo_iface );
}
static inline struct ro_error *impl_from_IErrorInfo( IErrorInfo *iface )
{
    return CONTAINING_RECORD( iface, struct ro_error, IErrorInfo_iface );
}
static inline struct ro_error *impl_from_ILanguageExceptionErrorInfo2( ILanguageExceptionErrorInfo2 *iface )
{
    return CONTAINING_RECORD( iface, struct ro_error, ILanguageExceptionErrorInfo2_iface );
}

static HRESULT WINAPI ro_error_QueryInterface( IRestrictedErrorInfo *iface, REFIID riid, void **out )
{
    struct ro_error *error = impl_from_IRestrictedErrorInfo( iface );

    TRACE( "%p %s %p\n", iface, debugstr_guid(riid), out );

    if (IsEqualGUID( riid, &IID_IUnknown ) || IsEqualGUID( riid, &IID_IRestrictedErrorInfo ))
        *out = &error->IRestrictedErrorInfo_iface;
    else if (IsEqualGUID( riid, &IID_IErrorInfo ))
        *out = &error->IErrorInfo_iface;
    else if (IsEqualGUID( riid, &IID_ILanguageExceptionErrorInfo ) || IsEqualGUID( riid, &IID_ILanguageExceptionErrorInfo2 ))
        *out = &error->ILanguageExceptionErrorInfo2_iface;
    else
    {
        *out = NULL;
        WARN( "%s not supported\n", debugstr_guid(riid) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI ro_error_AddRef( IRestrictedErrorInfo *iface )
{
    struct ro_error *error = impl_from_IRestrictedErrorInfo( iface );
    return InterlockedIncrement( &error->ref );
}

static ULONG WINAPI ro_error_Release( IRestrictedErrorInfo *iface )
{
    struct ro_error *error = impl_from_IRestrictedErrorInfo( iface );
    ULONG ref = InterlockedDecrement( &error->ref );

    if (!ref)
    {
        SysFreeString( error->description );
        SysFreeString( error->restricted );
        if (error->language_exception) IUnknown_Release( error->language_exception );
        if (error->propagation_context) IUnknown_Release( error->propagation_context );
        if (error->previous) IRestrictedErrorInfo_Release( &error->previous->IRestrictedErrorInfo_iface );
        free( error );
    }
    return ref;
}

static HRESULT WINAPI ro_error_GetErrorDetails( IRestrictedErrorInfo *iface, BSTR *description, HRESULT *hr,
                                                BSTR *restricted, BSTR *capability_sid )
{
    struct ro_error *error = impl_from_IRestrictedErrorInfo( iface );

    TRACE( "%p %p %p %p %p\n", iface, description, hr, restricted, capability_sid );

    if (description) *description = error->description ? SysAllocString( error->description ) : NULL;
    if (hr) *hr = error->hr;
    if (restricted) *restricted = error->restricted ? SysAllocString( error->restricted ) : NULL;
    if (capability_sid) *capability_sid = NULL;
    return S_OK;
}

static HRESULT WINAPI ro_error_GetReference( IRestrictedErrorInfo *iface, BSTR *reference )
{
    TRACE( "%p %p\n", iface, reference );
    if (reference) *reference = NULL;
    return S_OK;
}

static const IRestrictedErrorInfoVtbl ro_error_vtbl =
{
    ro_error_QueryInterface,
    ro_error_AddRef,
    ro_error_Release,
    ro_error_GetErrorDetails,
    ro_error_GetReference,
};

static HRESULT WINAPI ro_errorinfo_QueryInterface( IErrorInfo *iface, REFIID riid, void **out )
{
    return ro_error_QueryInterface( &impl_from_IErrorInfo( iface )->IRestrictedErrorInfo_iface, riid, out );
}
static ULONG WINAPI ro_errorinfo_AddRef( IErrorInfo *iface )
{
    return ro_error_AddRef( &impl_from_IErrorInfo( iface )->IRestrictedErrorInfo_iface );
}
static ULONG WINAPI ro_errorinfo_Release( IErrorInfo *iface )
{
    return ro_error_Release( &impl_from_IErrorInfo( iface )->IRestrictedErrorInfo_iface );
}
static HRESULT WINAPI ro_errorinfo_GetGUID( IErrorInfo *iface, GUID *guid )
{
    if (!guid) return E_INVALIDARG;
    memset( guid, 0, sizeof(*guid) );
    return S_OK;
}
static HRESULT WINAPI ro_errorinfo_GetSource( IErrorInfo *iface, BSTR *source )
{
    if (!source) return E_INVALIDARG;
    *source = NULL;
    return S_OK;
}
static HRESULT WINAPI ro_errorinfo_GetDescription( IErrorInfo *iface, BSTR *description )
{
    struct ro_error *error = impl_from_IErrorInfo( iface );
    if (!description) return E_INVALIDARG;
    *description = error->description ? SysAllocString( error->description ) : NULL;
    return S_OK;
}
static HRESULT WINAPI ro_errorinfo_GetHelpFile( IErrorInfo *iface, BSTR *file )
{
    if (!file) return E_INVALIDARG;
    *file = NULL;
    return S_OK;
}
static HRESULT WINAPI ro_errorinfo_GetHelpContext( IErrorInfo *iface, DWORD *context )
{
    if (!context) return E_INVALIDARG;
    *context = 0;
    return S_OK;
}

static const IErrorInfoVtbl ro_errorinfo_vtbl =
{
    ro_errorinfo_QueryInterface,
    ro_errorinfo_AddRef,
    ro_errorinfo_Release,
    ro_errorinfo_GetGUID,
    ro_errorinfo_GetSource,
    ro_errorinfo_GetDescription,
    ro_errorinfo_GetHelpFile,
    ro_errorinfo_GetHelpContext,
};

static HRESULT WINAPI ro_langexc_QueryInterface( ILanguageExceptionErrorInfo2 *iface, REFIID riid, void **out )
{
    return ro_error_QueryInterface( &impl_from_ILanguageExceptionErrorInfo2( iface )->IRestrictedErrorInfo_iface, riid, out );
}
static ULONG WINAPI ro_langexc_AddRef( ILanguageExceptionErrorInfo2 *iface )
{
    return ro_error_AddRef( &impl_from_ILanguageExceptionErrorInfo2( iface )->IRestrictedErrorInfo_iface );
}
static ULONG WINAPI ro_langexc_Release( ILanguageExceptionErrorInfo2 *iface )
{
    return ro_error_Release( &impl_from_ILanguageExceptionErrorInfo2( iface )->IRestrictedErrorInfo_iface );
}
static HRESULT WINAPI ro_langexc_GetLanguageException( ILanguageExceptionErrorInfo2 *iface, IUnknown **exception )
{
    struct ro_error *error = impl_from_ILanguageExceptionErrorInfo2( iface );

    TRACE( "%p %p\n", iface, exception );
    if (!exception) return E_INVALIDARG;
    if ((*exception = error->language_exception)) IUnknown_AddRef( *exception );
    return S_OK;
}
static HRESULT WINAPI ro_langexc_GetPreviousLanguageExceptionErrorInfo( ILanguageExceptionErrorInfo2 *iface, ILanguageExceptionErrorInfo2 **previous )
{
    struct ro_error *error = impl_from_ILanguageExceptionErrorInfo2( iface );

    TRACE( "%p %p\n", iface, previous );
    if (!previous) return E_INVALIDARG;
    *previous = error->previous ? &error->previous->ILanguageExceptionErrorInfo2_iface : NULL;
    if (*previous) ro_langexc_AddRef( *previous );
    return S_OK;
}
static HRESULT WINAPI ro_langexc_CapturePropagationContext( ILanguageExceptionErrorInfo2 *iface, IUnknown *exception )
{
    struct ro_error *error = impl_from_ILanguageExceptionErrorInfo2( iface );

    TRACE( "%p %p\n", iface, exception );
    if (error->propagation_context) IUnknown_Release( error->propagation_context );
    if ((error->propagation_context = exception)) IUnknown_AddRef( exception );
    return S_OK;
}
static HRESULT WINAPI ro_langexc_GetPropagationContextHead( ILanguageExceptionErrorInfo2 *iface, ILanguageExceptionErrorInfo2 **head )
{
    TRACE( "%p %p\n", iface, head );
    if (!head) return E_INVALIDARG;
    *head = iface;
    ro_langexc_AddRef( iface );
    return S_OK;
}

static const struct ILanguageExceptionErrorInfo2Vtbl ro_langexc_vtbl =
{
    ro_langexc_QueryInterface,
    ro_langexc_AddRef,
    ro_langexc_Release,
    ro_langexc_GetLanguageException,
    ro_langexc_GetPreviousLanguageExceptionErrorInfo,
    ro_langexc_CapturePropagationContext,
    ro_langexc_GetPropagationContextHead,
};

/* the current thread's error, if it is one of ours */
static struct ro_error *get_current_ro_error( void )
{
    IErrorInfo *info;
    struct ro_error *error = NULL;

    if (combase_get_errorinfo( &info ) != S_OK || !info) return NULL;
    if (info->lpVtbl == &ro_errorinfo_vtbl) error = impl_from_IErrorInfo( info );
    else IErrorInfo_Release( info );
    return error;   /* caller owns the reference taken by GetErrorInfo() */
}

static BOOL originate_error( HRESULT hr, const WCHAR *message, UINT len, IUnknown *language_exception, BOOL chain )
{
    struct ro_error *error;

    if (!(error = calloc( 1, sizeof(*error) ))) return FALSE;
    error->IRestrictedErrorInfo_iface.lpVtbl = &ro_error_vtbl;
    error->IErrorInfo_iface.lpVtbl = &ro_errorinfo_vtbl;
    error->ILanguageExceptionErrorInfo2_iface.lpVtbl = &ro_langexc_vtbl;
    error->ref = 1;
    error->hr = hr;
    if (message && len)
    {
        error->description = SysAllocStringLen( message, len );
        error->restricted = SysAllocStringLen( message, len );
    }
    if ((error->language_exception = language_exception)) IUnknown_AddRef( language_exception );
    if (chain) error->previous = get_current_ro_error();
    combase_set_errorinfo( &error->IErrorInfo_iface );
    IErrorInfo_Release( &error->IErrorInfo_iface );
    return TRUE;
}

/***********************************************************************
 *      RoOriginateError (combase.@)
 */
BOOL WINAPI RoOriginateError( HRESULT error, HSTRING message )
{
    UINT32 len = 0;
    const WCHAR *str = WindowsGetStringRawBuffer( message, &len );

    TRACE( "%#lx, %s\n", error, debugstr_wn(str, len) );
    if (SUCCEEDED(error)) return FALSE;
    return originate_error( error, str, len, NULL, FALSE );
}

/***********************************************************************
 *      RoOriginateErrorW (combase.@)
 */
/* Debugging aid: WINE_UWP_TRACE_ORIGINATE=<hex hresult> dumps the stack of whoever originated
 * that error, which is the only way to find the failing call inside a native WinRT DLL. */
static void trace_originate( HRESULT error )
{
#ifdef __x86_64__
    CONTEXT unwind;
    unsigned int frame;
    char var[32];

    if (!GetEnvironmentVariableA( "WINE_UWP_TRACE_ORIGINATE", var, sizeof(var) )) return;
    if (strcmp( var, "all" ) && (HRESULT)strtoul( var, NULL, 16 ) != error) return;

    RtlCaptureContext( &unwind );
    for (frame = 0; frame < 32 && unwind.Rip; frame++)
    {
        ULONG_PTR image_base = 0, establisher = 0, rva = unwind.Rip;
        RUNTIME_FUNCTION *func;
        PVOID handler_data = NULL;
        HMODULE module = NULL;
        WCHAR name[MAX_PATH];
        const WCHAR *base_name = L"?";

        if (GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)unwind.Rip, &module ))
        {
            if (GetModuleFileNameW( module, name, ARRAY_SIZE(name) ))
            {
                base_name = wcsrchr( name, '\\' );
                base_name = base_name ? base_name + 1 : name;
            }
            rva = unwind.Rip - (ULONG_PTR)module;
        }
        ERR( "  originate[%u] %s+0x%Ix\n", frame, debugstr_w(base_name), rva );
        if (!(func = RtlLookupFunctionEntry( unwind.Rip, &image_base, NULL ))) break;
        RtlVirtualUnwind( UNW_FLAG_NHANDLER, image_base, unwind.Rip, func,
                          &unwind, &handler_data, &establisher, NULL );
    }
#endif
}

BOOL WINAPI RoOriginateErrorW( HRESULT error, UINT max_len, const WCHAR *message )
{
    UINT len = 0;

    if (message) len = max_len == (UINT)-1 ? lstrlenW( message ) : wcsnlen( message, max_len );
    TRACE( "%#lx, %u, %s from %p\n", error, max_len, debugstr_wn(message, len), __builtin_return_address(0) );
    if (SUCCEEDED(error)) return FALSE;
    trace_originate( error );
    return originate_error( error, message, len, NULL, FALSE );
}

/***********************************************************************
 *      RoOriginateLanguageException (combase.@)
 */
/* Debugging aid: a language exception object is the managed exception itself. Its IStringable
 * rendering carries the managed stack trace, which is the only way to see inside a .NET Native
 * app that has no symbols. Gated on the same variable as trace_originate. */
static void trace_language_exception( IUnknown *language_exception )
{
    static const GUID IID_IStringable_ = { 0x96369f54, 0x8eb6, 0x48f0, { 0xab, 0xce, 0xc1, 0xb2, 0x11, 0xe6, 0x27, 0xc3 } };
    struct stringable_vtbl
    {
        void *unknown_inspectable[6];
        HRESULT (WINAPI *ToString)( void *iface, HSTRING *value );
    };
    struct stringable { const struct stringable_vtbl *vtbl; } *stringable;
    char var[32];
    HSTRING text;

    if (!language_exception) return;
    if (!GetEnvironmentVariableA( "WINE_UWP_TRACE_ORIGINATE", var, sizeof(var) )) return;
    (void)var;
    if (FAILED(IUnknown_QueryInterface( language_exception, &IID_IStringable_, (void **)&stringable )))
    {
        ERR( "language exception %p is not IStringable\n", language_exception );
        return;
    }
    if (SUCCEEDED(stringable->vtbl->ToString( stringable, &text )))
    {
        UINT32 length = 0, pos;
        const WCHAR *buffer = WindowsGetStringRawBuffer( text, &length );
        /* debugstr_wn truncates, and the interesting part of a managed exception is the stack
         * below the message. Print it in chunks so the whole thing survives. */
        for (pos = 0; pos < length; pos += 160)
            ERR( "language exception%s: %s\n", pos ? " (cont)" : "",
                 debugstr_wn( buffer + pos, min( 160, length - pos ) ) );
        WindowsDeleteString( text );
    }
    IUnknown_Release( (IUnknown *)stringable );
}

/* Called from RoReportUnhandledError (roapi.c): the reported info is often one of our own
 * ro_error objects wrapping the managed language exception; dump its IStringable text. */
void combase_dump_unhandled_language_exception( IRestrictedErrorInfo *info )
{
    struct ro_error *error;

    if (!info) return;
    if (info->lpVtbl == &ro_error_vtbl)
    {
        error = impl_from_IRestrictedErrorInfo( info );
        if (error->language_exception) trace_language_exception( error->language_exception );
    }
    {
        BSTR description = NULL, restricted_description = NULL, capability = NULL;
        HRESULT hr;

        if (SUCCEEDED(IRestrictedErrorInfo_GetErrorDetails( info, &description, &hr,
                                                            &restricted_description, &capability )))
        {
            ERR( "unhandled error details: hr %#lx desc %s restricted %s\n", hr,
                 debugstr_w( description ), debugstr_w( restricted_description ) );
            SysFreeString( description );
            SysFreeString( restricted_description );
            SysFreeString( capability );
        }
    }
}

/* With WINE_UWP_TRACE_ORIGINATE set, print who originated the error as module+rva return
 * addresses. A managed ThrowOnExternalCallFailed originates while the frame that made the failing
 * call is still on the stack, which is the only cheap way to find a .NET Native call site. */
static void trace_origin_backtrace( HRESULT error )
{
    void *frames[32];
    char var[8];
    USHORT count, i;

    if (!GetEnvironmentVariableA( "WINE_UWP_TRACE_ORIGINATE", var, sizeof(var) )) return;
    ERR( "originated hr %#lx, from:\n", error );
    count = RtlCaptureStackBackTrace( 1, ARRAY_SIZE(frames), frames, NULL );
    for (i = 0; i < count; i++)
    {
        WCHAR path[MAX_PATH], *name;
        HMODULE module;

        if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 frames[i], &module ) || !GetModuleFileNameW( module, path, ARRAY_SIZE(path) ))
        {
            ERR( "origin frame %u: %p\n", i, frames[i] );
            continue;
        }
        name = wcsrchr( path, '\\' ) ? wcsrchr( path, '\\' ) + 1 : path;
        ERR( "origin frame %u: %s+%#Ix\n", i, debugstr_w( name ), (ULONG_PTR)frames[i] - (ULONG_PTR)module );
    }
}

BOOL WINAPI RoOriginateLanguageException( HRESULT error, HSTRING message, IUnknown *language_exception )
{
    UINT32 len = 0;
    const WCHAR *str = WindowsGetStringRawBuffer( message, &len );

    TRACE( "%#lx, %s, %p\n", error, debugstr_wn(str, len), language_exception );
    if (SUCCEEDED(error)) return FALSE;
    trace_originate( error );
    trace_language_exception( language_exception );
    trace_origin_backtrace( error );
    return originate_error( error, str, len, language_exception, TRUE );
}

/***********************************************************************
 *      RoTransformError (combase.@)
 */
BOOL WINAPI RoTransformError( HRESULT old_error, HRESULT new_error, HSTRING message )
{
    UINT32 len = 0;
    const WCHAR *str = WindowsGetStringRawBuffer( message, &len );

    TRACE( "%#lx, %#lx, %s\n", old_error, new_error, debugstr_wn(str, len) );
    if (old_error == new_error || SUCCEEDED(new_error)) return FALSE;
    return originate_error( new_error, str, len, NULL, TRUE );
}

/***********************************************************************
 *      RoTransformErrorW (combase.@)
 */
BOOL WINAPI RoTransformErrorW( HRESULT old_error, HRESULT new_error, UINT max_len, const WCHAR *message )
{
    UINT len = 0;

    if (message) len = max_len == (UINT)-1 ? lstrlenW( message ) : wcsnlen( message, max_len );
    TRACE( "%#lx, %#lx, %u, %s\n", old_error, new_error, max_len, debugstr_wn(message, len) );
    if (old_error == new_error || SUCCEEDED(new_error)) return FALSE;
    return originate_error( new_error, message, len, NULL, TRUE );
}

/***********************************************************************
 *      GetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI GetRestrictedErrorInfo( IRestrictedErrorInfo **info )
{
    IErrorInfo *errorinfo;
    HRESULT hr;

    TRACE( "%p\n", info );
    if (!info) return E_INVALIDARG;
    *info = NULL;
    /* WIL's RaiseRoOriginateOnWilExceptions treats S_OK as "info is non-NULL".
     * Windows returns S_FALSE when the thread has no IRestrictedErrorInfo
     * (same shape as GetErrorInfo). Returning S_OK with a NULL out pointer
     * AVs in that path. */
    if (combase_get_errorinfo( &errorinfo ) != S_OK || !errorinfo) return S_FALSE;
    hr = IErrorInfo_QueryInterface( errorinfo, &IID_IRestrictedErrorInfo, (void **)info );
    IErrorInfo_Release( errorinfo );
    if (FAILED(hr))
    {
        *info = NULL;
        return S_FALSE;
    }
    return S_OK;
}

/***********************************************************************
 *      SetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI SetRestrictedErrorInfo( IRestrictedErrorInfo *info )
{
    IErrorInfo *errorinfo = NULL;
    HRESULT hr;

    TRACE( "%p\n", info );
    if (info && FAILED(hr = IRestrictedErrorInfo_QueryInterface( info, &IID_IErrorInfo, (void **)&errorinfo ))) return hr;
    hr = combase_set_errorinfo( errorinfo );
    if (errorinfo) IErrorInfo_Release( errorinfo );
    return hr;
}

/***********************************************************************
 *      RoClearError (combase.@)
 */
void WINAPI RoClearError( void )
{
    TRACE( "\n" );
    combase_set_errorinfo( NULL );
}

/***********************************************************************
 *      RoCaptureErrorContext (combase.@)
 */
HRESULT WINAPI RoCaptureErrorContext( HRESULT hr )
{
    TRACE( "%#lx\n", hr );
    return S_OK;
}

/*
 * Copyright 2014 Martin Storsjo
 * Copyright 2016 Michael Müller
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
#define COBJMACROS
#include "objbase.h"
#include "ctxtcall.h"
#include "comsvcs.h"
#include "initguid.h"
#include "roapi.h"
#include "roparameterizediid.h"
#include "roerrorapi.h"
#include "restrictederrorinfo.h"
#include "oleauto.h"
#include "winstring.h"
#include "errhandlingapi.h"

#include "combase_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

struct activatable_class_data
{
    ULONG size;
    DWORD unk;
    DWORD module_len;
    DWORD module_offset;
    DWORD threading_model;
};


/***********************************************************************
 * Package graph activatable classes (wine-uwp)
 *
 * Windows resolves a packaged app's WinRT classes from the AppxManifest of every package in its
 * package graph, not from the registry. We do the same for the main package (WINE_UWP_PACKAGE_PATH)
 * and its framework dependencies (WINE_UWP_PACKAGE_DEPENDENCIES, "fullname|family|path;...").
 * Only <InProcessServer> extensions matter here: <Path> names the DLL relative to the package root.
 */
struct uwp_class
{
    WCHAR *classid;
    WCHAR *dll;
};

static struct uwp_class *uwp_classes;
static unsigned int uwp_classes_count, uwp_classes_size;
static INIT_ONCE uwp_classes_once = INIT_ONCE_STATIC_INIT;

static void uwp_add_class( const WCHAR *classid, unsigned int classid_len, const WCHAR *dll )
{
    if (uwp_classes_count == uwp_classes_size)
    {
        unsigned int new_size = uwp_classes_size ? uwp_classes_size * 2 : 256;
        struct uwp_class *tmp = realloc( uwp_classes, new_size * sizeof(*tmp) );
        if (!tmp) return;
        uwp_classes = tmp;
        uwp_classes_size = new_size;
    }
    if (!(uwp_classes[uwp_classes_count].classid = malloc( (classid_len + 1) * sizeof(WCHAR) ))) return;
    memcpy( uwp_classes[uwp_classes_count].classid, classid, classid_len * sizeof(WCHAR) );
    uwp_classes[uwp_classes_count].classid[classid_len] = 0;
    if (!(uwp_classes[uwp_classes_count].dll = wcsdup( dll )))
    {
        free( uwp_classes[uwp_classes_count].classid );
        return;
    }
    uwp_classes_count++;
}

static WCHAR *uwp_read_manifest( const WCHAR *package_dir )
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD size, read, wlen;
    char *data;
    WCHAR *text = NULL;

    swprintf( path, ARRAY_SIZE(path), L"%s\\AppxManifest.xml", package_dir );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        WARN( "no manifest at %s\n", debugstr_w(path) );
        return NULL;
    }
    size = GetFileSize( file, NULL );
    if (size != INVALID_FILE_SIZE && (data = malloc( size + 1 )) && ReadFile( file, data, size, &read, NULL ))
    {
        data[read] = 0;
        wlen = MultiByteToWideChar( CP_UTF8, 0, data, read, NULL, 0 );
        if ((text = malloc( (wlen + 1) * sizeof(WCHAR) )))
        {
            MultiByteToWideChar( CP_UTF8, 0, data, read, text, wlen );
            text[wlen] = 0;
        }
        free( data );
    }
    CloseHandle( file );
    return text;
}

/* minimal scan: <InProcessServer> ... <Path>x.dll</Path> ... <ActivatableClass ActivatableClassId="..."/> ... </InProcessServer> */
static void uwp_scan_manifest( const WCHAR *package_dir )
{
    WCHAR *text = uwp_read_manifest( package_dir ), *p, *end, *q;
    unsigned int before = uwp_classes_count;

    if (!text) return;
    for (p = wcsstr( text, L"<InProcessServer>" ); p; p = wcsstr( end, L"<InProcessServer>" ))
    {
        WCHAR dll[MAX_PATH];

        if (!(end = wcsstr( p, L"</InProcessServer>" ))) break;
        *end = 0;
        dll[0] = 0;
        if ((q = wcsstr( p, L"<Path>" )))
        {
            WCHAR *e = wcsstr( q, L"</Path>" );
            q += 6;
            if (e && e > q)
            {
                unsigned int i, n = e - q;
                swprintf( dll, ARRAY_SIZE(dll), L"%s\\", package_dir );
                i = lstrlenW( dll );
                if (i + n < ARRAY_SIZE(dll))
                {
                    memcpy( dll + i, q, n * sizeof(WCHAR) );
                    dll[i + n] = 0;
                    for (; i < ARRAY_SIZE(dll) && dll[i]; i++) if (dll[i] == '/') dll[i] = '\\';
                }
            }
        }
        if (dll[0])
        {
            for (q = wcsstr( p, L"ActivatableClassId=\"" ); q; q = wcsstr( q, L"ActivatableClassId=\"" ))
            {
                WCHAR *e;
                q += 20;
                if (!(e = wcschr( q, '"' ))) break;
                uwp_add_class( q, e - q, dll );
                q = e;
            }
        }
        *end = '<';
        end += 18;
    }
    /* <com:ExeServer Executable="x.exe" ...> <com:Class Id="GUID" .../> ... </com:ExeServer>: packaged
     * out-of-process COM servers (the Xbox app's XboxPcAppFT.exe and XboxPcAppAdminServer.exe). They
     * go in the same table as "{GUID}" -> exe, for create_server(). */
    for (p = wcsstr( text, L"ExeServer " ); p; p = wcsstr( end, L"ExeServer " ))
    {
        WCHAR exe[MAX_PATH], *e;

        if (!(end = wcsstr( p, L"ExeServer>" ))) break;   /* the closing tag, whatever its prefix */
        *end = 0;
        exe[0] = 0;
        if ((q = wcsstr( p, L"Executable=\"" )) && (e = wcschr( q += 12, '"' )) && e > q)
        {
            unsigned int i, n = e - q;
            swprintf( exe, ARRAY_SIZE(exe), L"%s\\", package_dir );
            i = lstrlenW( exe );
            if (i + n < ARRAY_SIZE(exe))
            {
                memcpy( exe + i, q, n * sizeof(WCHAR) );
                exe[i + n] = 0;
                for (; exe[i]; i++) if (exe[i] == '/') exe[i] = '\\';
            }
            else exe[0] = 0;
        }
        for (q = p; exe[0] && (q = wcsstr( q, L"Class Id=\"" )); q = e)
        {
            WCHAR guid[40];
            q += 10;
            if (!(e = wcschr( q, '"' ))) break;
            if (e - q != 36) continue;
            swprintf( guid, ARRAY_SIZE(guid), L"{%.36s}", q );
            uwp_add_class( guid, lstrlenW( guid ), exe );
        }
        *end = 'E';
        end += 10;
    }
    TRACE( "%s: %u classes\n", debugstr_w(package_dir), uwp_classes_count - before );
    free( text );
}

static BOOL WINAPI uwp_classes_init( INIT_ONCE *once, void *param, void **context )
{
    WCHAR path[MAX_PATH], *deps, *p;
    DWORD len;

    if (GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", path, ARRAY_SIZE(path) )) uwp_scan_manifest( path );
    if ((len = GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", NULL, 0 )) && (deps = malloc( len * sizeof(WCHAR) )))
    {
        GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", deps, len );
        for (p = deps; *p; )
        {
            WCHAR *end = wcschr( p, ';' ), *f, *pa;
            if (end) *end++ = 0;
            if ((f = wcschr( p, '|' )) && (pa = wcschr( f + 1, '|' ))) uwp_scan_manifest( pa + 1 );
            p = end ? end : p + lstrlenW( p );
        }
        free( deps );
    }
    return TRUE;
}

static const WCHAR *uwp_find_class( const WCHAR *classid )
{
    unsigned int i;

    InitOnceExecuteOnce( &uwp_classes_once, uwp_classes_init, NULL, NULL );
    for (i = 0; i < uwp_classes_count; i++)
        if (!wcscmp( uwp_classes[i].classid, classid )) return uwp_classes[i].dll;
    return NULL;
}

/* the executable of a packaged COM server (<com:ExeServer>) serving clsid, if any */
BOOL uwp_find_exe_server( REFCLSID clsid, WCHAR *exe, DWORD size )
{
    WCHAR guid[40];
    unsigned int i;

    InitOnceExecuteOnce( &uwp_classes_once, uwp_classes_init, NULL, NULL );
    StringFromGUID2( clsid, guid, ARRAY_SIZE(guid) );
    for (i = 0; i < uwp_classes_count; i++)
    {
        if (wcsicmp( uwp_classes[i].classid, guid )) continue;
        if (lstrlenW( uwp_classes[i].dll ) >= size) return FALSE;
        lstrcpyW( exe, uwp_classes[i].dll );
        return TRUE;
    }
    return FALSE;
}

static HRESULT get_library_for_classid(const WCHAR *classid, WCHAR **out)
{
    ACTCTX_SECTION_KEYED_DATA data;
    HKEY hkey_root, hkey_class;
    DWORD type, size;
    HRESULT hr;
    WCHAR *buf = NULL;

    *out = NULL;

    /* search activation context first */
    data.cbSize = sizeof(data);
    if (FindActCtxSectionStringW(FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX, NULL,
            ACTIVATION_CONTEXT_SECTION_WINRT_ACTIVATABLE_CLASSES, classid, &data))
    {
        struct activatable_class_data *activatable_class = (struct activatable_class_data *)data.lpData;
        void *ptr = (BYTE *)data.lpSectionBase + activatable_class->module_offset;
        *out = wcsdup(ptr);
        return S_OK;
    }

    /* a stand-in that explicitly replaces a package's own class (wine-uwp): the registry entry carries
     * OverridesPackage=1. Used for the Gaming Services client classes, whose service cannot run here. */
    {
        DWORD override = 0, len = sizeof(override);
        WCHAR key[512];

        swprintf( key, ARRAY_SIZE(key), L"Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\%s", classid );
        if (!RegGetValueW( HKEY_LOCAL_MACHINE, key, L"OverridesPackage", RRF_RT_REG_DWORD, NULL, &override, &len ) &&
            override)
            goto registry;
    }

    /* package graph manifests (wine-uwp) */
    {
        const WCHAR *dll = uwp_find_class( classid );
        if (dll)
        {
            TRACE( "%s -> package dll %s\n", debugstr_w(classid), debugstr_w(dll) );
            *out = wcsdup( dll );
            return *out ? S_OK : E_OUTOFMEMORY;
        }
    }

registry:
    /* load class registry key */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\WindowsRuntime\\ActivatableClassId",
                      0, KEY_READ, &hkey_root))
        return REGDB_E_READREGDB;
    if (RegOpenKeyExW(hkey_root, classid, 0, KEY_READ, &hkey_class))
    {
        WARN("Class %s not found in registry\n", debugstr_w(classid));
        RegCloseKey(hkey_root);
        return REGDB_E_CLASSNOTREG;
    }
    RegCloseKey(hkey_root);

    /* load (and expand) DllPath registry value */
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, &type, NULL, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ)
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (!(buf = malloc(size)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, NULL, (BYTE *)buf, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type == REG_EXPAND_SZ)
    {
        WCHAR *expanded;
        DWORD len = ExpandEnvironmentStringsW(buf, NULL, 0);
        if (!(expanded = malloc(len * sizeof(WCHAR))))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ExpandEnvironmentStringsW(buf, expanded, len);
        free(buf);
        buf = expanded;
    }

    *out = buf;
    RegCloseKey(hkey_class);
    return S_OK;

done:
    free(buf);
    RegCloseKey(hkey_class);
    return hr;
}


/***********************************************************************
 *      RoInitialize (combase.@)
 */
HRESULT WINAPI RoInitialize(RO_INIT_TYPE type)
{
    switch (type) {
    case RO_INIT_SINGLETHREADED:
        return CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    default:
        FIXME("type %d\n", type);
    case RO_INIT_MULTITHREADED:
        return CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }
}

/***********************************************************************
 *      RoUninitialize (combase.@)
 */
void WINAPI RoUninitialize(void)
{
    CoUninitialize();
}

/* Windows keeps the activation factory of a class around once it has been created; an app that
 * activates a class per input event (XAML asks for Windows.UI.Input.PointerPoint on every pointer
 * message) otherwise pays four registry lookups, a LoadLibrary and a DllGetActivationFactory each
 * time - 600 registry reads in a two second drag of Calculator's graph. */
struct factory_cache_entry
{
    struct list entry;
    IActivationFactory *factory;
    WCHAR classid[1];
};
static struct list factory_cache = LIST_INIT( factory_cache );
static CRITICAL_SECTION factory_cache_cs;
static CRITICAL_SECTION_DEBUG factory_cache_cs_debug =
{
    0, 0, &factory_cache_cs,
    { &factory_cache_cs_debug.ProcessLocksList, &factory_cache_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": factory_cache_cs") }
};
static CRITICAL_SECTION factory_cache_cs = { &factory_cache_cs_debug, -1, 0, 0, 0, 0 };

static IActivationFactory *factory_cache_get( const WCHAR *classid )
{
    struct factory_cache_entry *entry;
    IActivationFactory *factory = NULL;

    EnterCriticalSection( &factory_cache_cs );
    LIST_FOR_EACH_ENTRY( entry, &factory_cache, struct factory_cache_entry, entry )
    {
        if (wcscmp( entry->classid, classid )) continue;
        factory = entry->factory;
        IActivationFactory_AddRef( factory );
        break;
    }
    LeaveCriticalSection( &factory_cache_cs );
    return factory;
}

static void factory_cache_add( const WCHAR *classid, IActivationFactory *factory )
{
    struct factory_cache_entry *entry, *existing;
    SIZE_T len = wcslen( classid );

    if (!(entry = malloc( offsetof( struct factory_cache_entry, classid[len + 1] ) ))) return;
    memcpy( entry->classid, classid, (len + 1) * sizeof(WCHAR) );
    entry->factory = factory;

    EnterCriticalSection( &factory_cache_cs );
    LIST_FOR_EACH_ENTRY( existing, &factory_cache, struct factory_cache_entry, entry )
    {
        if (wcscmp( existing->classid, classid )) continue;
        LeaveCriticalSection( &factory_cache_cs );
        free( entry );
        return;
    }
    IActivationFactory_AddRef( factory );
    list_add_tail( &factory_cache, &entry->entry );
    LeaveCriticalSection( &factory_cache_cs );
}

/* The DllGetActivationFactory of every library that served a class, kept with a module reference for good (Windows
 * keeps activated WinRT servers loaded too). Activating another class of an already loaded library then needs no
 * LoadLibrary, and so no loader lock. That matters: the Xbox app's UI thread activates a Microsoft.UI.Xaml class
 * while XAML holds its g_csStatic, and a pool thread running a DLL's DllMain (holding the loader lock) wants
 * g_csStatic - one start in three hung there. Windows' loader does not take the loader lock for a module that is
 * already loaded and initialised either. */
struct library_cache_entry
{
    struct list entry;
    PFNGETACTIVATIONFACTORY get_factory;
    WCHAR library[1];
};
static struct list library_cache = LIST_INIT( library_cache );

static PFNGETACTIVATIONFACTORY library_cache_get( const WCHAR *library )
{
    struct library_cache_entry *entry;
    PFNGETACTIVATIONFACTORY fn = NULL;

    EnterCriticalSection( &factory_cache_cs );
    LIST_FOR_EACH_ENTRY( entry, &library_cache, struct library_cache_entry, entry )
    {
        if (wcsicmp( entry->library, library )) continue;
        fn = entry->get_factory;
        break;
    }
    LeaveCriticalSection( &factory_cache_cs );
    return fn;
}

static BOOL library_cache_add( const WCHAR *library, PFNGETACTIVATIONFACTORY fn )
{
    struct library_cache_entry *entry;
    SIZE_T len = wcslen( library );

    if (library_cache_get( library )) return FALSE;
    if (!(entry = malloc( offsetof( struct library_cache_entry, library[len + 1] ) ))) return FALSE;
    memcpy( entry->library, library, (len + 1) * sizeof(WCHAR) );
    entry->get_factory = fn;
    EnterCriticalSection( &factory_cache_cs );
    list_add_tail( &library_cache, &entry->entry );
    LeaveCriticalSection( &factory_cache_cs );
    return TRUE;
}

/* CoreApplication.MainView for a hosted app. The app's window can belong to a view other than the one
 * CoreApplication.Run made first (the activation stands its own view up), which leaves MainView with no
 * CoreWindow; the Xbox app reads MainView.CoreWindow.Dispatcher from the thread pool and faults on the NULL.
 * On Windows the main view is the one with the window, so when MainView has none, answer the first view
 * that does. Everything else goes to CoreApplication itself. */
DEFINE_GUID(IID_ICoreImmersiveApplication_, 0x1ada0e3e, 0xe4a2, 0x4123, 0xb4, 0x51, 0xdc, 0x96, 0xbf, 0x80, 0x04, 0x19);
DEFINE_GUID(IID_ICoreApplicationView_, 0x638bb2db, 0x451d, 0x4661, 0xb0, 0x99, 0x41, 0x4f, 0x34, 0xff, 0xb9, 0xf1);

struct slots { HRESULT (WINAPI *slot[16])(); };

struct main_view_app
{
    const void **vtbl;
    LONG refs;
    IInspectable *real;   /* ICoreImmersiveApplication */
};

static HRESULT view_has_window( IInspectable *view )
{
    struct slots **cav;
    IUnknown *window = NULL;
    HRESULT hr;

    if (FAILED(hr = IInspectable_QueryInterface( view, &IID_ICoreApplicationView_, (void **)&cav ))) return hr;
    hr = ((HRESULT (WINAPI *)(void *, IUnknown **))(*cav)->slot[6])( cav, &window );   /* get_CoreWindow */
    IUnknown_Release( (IUnknown *)cav );
    if (window) IUnknown_Release( window );
    return SUCCEEDED(hr) && window ? S_OK : S_FALSE;
}

static HRESULT WINAPI mva_QueryInterface( struct main_view_app *app, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_ICoreImmersiveApplication_ ))
    {
        InterlockedIncrement( &app->refs );
        *out = app;
        return S_OK;
    }
    return IInspectable_QueryInterface( app->real, iid, out );
}
static ULONG WINAPI mva_AddRef( struct main_view_app *app ) { return InterlockedIncrement( &app->refs ); }
static ULONG WINAPI mva_Release( struct main_view_app *app )
{
    ULONG refs = InterlockedDecrement( &app->refs );
    if (!refs)
    {
        IInspectable_Release( app->real );
        free( app );
    }
    return refs;
}
static HRESULT WINAPI mva_GetIids( struct main_view_app *app, ULONG *count, IID **iids )
{
    return IInspectable_GetIids( app->real, count, iids );
}
static HRESULT WINAPI mva_GetRuntimeClassName( struct main_view_app *app, HSTRING *name )
{
    return IInspectable_GetRuntimeClassName( app->real, name );
}
static HRESULT WINAPI mva_GetTrustLevel( struct main_view_app *app, TrustLevel *level )
{
    return IInspectable_GetTrustLevel( app->real, level );
}
static HRESULT WINAPI mva_get_Views( struct main_view_app *app, void **views )
{
    return ((HRESULT (WINAPI *)(void *, void **))(*(struct slots **)app->real)->slot[6])( app->real, views );
}
static HRESULT WINAPI mva_CreateNewView( struct main_view_app *app, HSTRING type, HSTRING entry, void **view )
{
    return ((HRESULT (WINAPI *)(void *, HSTRING, HSTRING, void **))(*(struct slots **)app->real)->slot[7])( app->real, type, entry, view );
}
/* The first view that has a window, or NULL. */
static IInspectable *find_window_view( IInspectable *real )
{
    struct slots **views = NULL;
    IInspectable *view = NULL;
    UINT32 count = 0, i;

    if (FAILED(((HRESULT (WINAPI *)(void *, void **))(*(struct slots **)real)->slot[6])( real, (void **)&views )) || !views)
        return NULL;
    ((HRESULT (WINAPI *)(void *, UINT32 *))(*views)->slot[7])( views, &count );   /* get_Size */
    TRACE( "%u view(s)\n", count );
    for (i = 0; i < count; i++)
    {
        if (FAILED(((HRESULT (WINAPI *)(void *, UINT32, IInspectable **))(*views)->slot[6])( views, i, &view )) || !view) continue;
        if (view_has_window( view ) == S_OK)
        {
            TRACE( "view %u (%p) has the window\n", i, view );
            break;
        }
        IInspectable_Release( view );
        view = NULL;
    }
    IUnknown_Release( (IUnknown *)views );
    return view;
}

/* Finding the window's view calls into each view's thread: getting the views unmarshals them, which in Wine makes a
 * RemAddRef call to the thread that owns each one, and then asks each for its CoreWindow. Phantom Dust's Xbox Live
 * code reads MainView while holding a lock its own view thread is waiting for, so doing that in the caller deadlocked
 * at start-up (black screen, audio playing); the title only reads MainView.CoreWindow there and copes with NULL. So
 * an MTA caller waits for the lookup on another MTA thread for a short while only, and otherwise gets the real main
 * view, as it did before the wrapper; a late lookup finishes and cleans up on its own. */
struct view_lookup
{
    LONG refs;
    HANDLE done;
    IInspectable *real;     /* ICoreImmersiveApplication */
    IInspectable *result;
};

static LONG view_lookup_busy;

static void view_lookup_release( struct view_lookup *lookup )
{
    if (InterlockedDecrement( &lookup->refs )) return;
    if (lookup->result) IInspectable_Release( lookup->result );
    IInspectable_Release( lookup->real );
    CloseHandle( lookup->done );
    free( lookup );
}

static DWORD WINAPI view_lookup_thread( void *arg )
{
    struct view_lookup *lookup = arg;
    HRESULT init = CoInitializeEx( NULL, COINIT_MULTITHREADED );

    lookup->result = find_window_view( lookup->real );
    SetEvent( lookup->done );
    view_lookup_release( lookup );
    InterlockedExchange( &view_lookup_busy, 0 );
    if (SUCCEEDED(init)) CoUninitialize();
    return 0;
}

static IInspectable *find_window_view_bounded( IInspectable *real )
{
    struct view_lookup *lookup;
    IInspectable *view = NULL;
    HANDLE thread;

    if (InterlockedCompareExchange( &view_lookup_busy, 1, 0 ))
    {
        TRACE( "an earlier lookup is still waiting for a view's thread\n" );
        return NULL;
    }
    if (!(lookup = calloc( 1, sizeof(*lookup) )) || !(lookup->done = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        free( lookup );
        InterlockedExchange( &view_lookup_busy, 0 );
        return NULL;
    }
    lookup->refs = 2;
    IInspectable_AddRef( (lookup->real = real) );
    if (!(thread = CreateThread( NULL, 0, view_lookup_thread, lookup, 0, NULL )))
    {
        lookup->refs = 1;
        view_lookup_release( lookup );
        InterlockedExchange( &view_lookup_busy, 0 );
        return NULL;
    }
    CloseHandle( thread );
    if (!WaitForSingleObject( lookup->done, 250 ))
        view = InterlockedExchangePointer( (void **)&lookup->result, NULL );
    else
        TRACE( "a view's thread is busy; answering the main view\n" );
    view_lookup_release( lookup );
    return view;
}

static HRESULT WINAPI mva_get_MainView( struct main_view_app *app, IInspectable **out )
{
    APTTYPEQUALIFIER qualifier;
    IInspectable *view;
    APTTYPE type;
    HRESULT hr;

    hr = ((HRESULT (WINAPI *)(void *, IInspectable **))(*(struct slots **)app->real)->slot[8])( app->real, out );
    if (FAILED(hr) || !*out || view_has_window( *out ) == S_OK) return hr;
    TRACE( "main view %p has no window\n", *out );
    /* an STA caller can't use an MTA thread's answer, so it looks itself */
    if (SUCCEEDED(CoGetApartmentType( &type, &qualifier )) && type == APTTYPE_MTA)
        view = find_window_view_bounded( app->real );
    else
        view = find_window_view( app->real );
    if (view)
    {
        TRACE( "main view %p has no window, answering %p\n", *out, view );
        IInspectable_Release( *out );
        *out = view;
    }
    return hr;
}

static const void *main_view_app_vtbl[] =
{
    mva_QueryInterface, mva_AddRef, mva_Release, mva_GetIids, mva_GetRuntimeClassName, mva_GetTrustLevel,
    mva_get_Views, mva_CreateNewView, mva_get_MainView,
};

/* CoreApplication's factory asked for as IActivationFactory (C++/CX's vccorlib caches factories that way and
 * QueryInterfaces them later): ICoreImmersiveApplication from it has to be the wrapper above too. GameChat, in a
 * Phantom Dust lobby, reads MainView.CoreWindow.Dispatcher through such a cached factory. */
struct core_app_factory
{
    const void **vtbl;
    LONG refs;
    IActivationFactory *real;
};

static HRESULT WINAPI caf_QueryInterface( struct core_app_factory *f, REFIID iid, void **out )
{
    struct main_view_app *app;
    HRESULT hr;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        InterlockedIncrement( &f->refs );
        *out = f;
        return S_OK;
    }
    if (FAILED(hr = IActivationFactory_QueryInterface( f->real, iid, out )) || !IsEqualGUID( iid, &IID_ICoreImmersiveApplication_ ))
        return hr;
    if (!(app = malloc( sizeof(*app) ))) return hr;
    app->vtbl = main_view_app_vtbl;
    app->refs = 1;
    app->real = *out;
    *out = app;
    return S_OK;
}
static ULONG WINAPI caf_AddRef( struct core_app_factory *f ) { return InterlockedIncrement( &f->refs ); }
static ULONG WINAPI caf_Release( struct core_app_factory *f )
{
    ULONG refs = InterlockedDecrement( &f->refs );
    if (!refs)
    {
        IActivationFactory_Release( f->real );
        free( f );
    }
    return refs;
}
static HRESULT WINAPI caf_GetIids( struct core_app_factory *f, ULONG *count, IID **iids )
{
    return IActivationFactory_GetIids( f->real, count, iids );
}
static HRESULT WINAPI caf_GetRuntimeClassName( struct core_app_factory *f, HSTRING *name )
{
    return IActivationFactory_GetRuntimeClassName( f->real, name );
}
static HRESULT WINAPI caf_GetTrustLevel( struct core_app_factory *f, TrustLevel *level )
{
    return IActivationFactory_GetTrustLevel( f->real, level );
}
static HRESULT WINAPI caf_ActivateInstance( struct core_app_factory *f, IInspectable **out )
{
    return IActivationFactory_ActivateInstance( f->real, out );
}

static const void *core_app_factory_vtbl[] =
{
    caf_QueryInterface, caf_AddRef, caf_Release, caf_GetIids, caf_GetRuntimeClassName, caf_GetTrustLevel,
    caf_ActivateInstance,
};

static void wrap_core_application( const WCHAR *name, REFIID iid, void **factory )
{
    struct main_view_app *app;

    if (!name || wcscmp( name, L"Windows.ApplicationModel.Core.CoreApplication" )) return;
    if (IsEqualGUID( iid, &IID_IActivationFactory ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IUnknown ))
    {
        struct core_app_factory *f;

        if (!(f = malloc( sizeof(*f) ))) return;
        f->vtbl = core_app_factory_vtbl;
        f->refs = 1;
        f->real = *factory;
        *factory = f;
        return;
    }
    if (!IsEqualGUID( iid, &IID_ICoreImmersiveApplication_ )) return;
    if (!(app = malloc( sizeof(*app) ))) return;
    app->vtbl = main_view_app_vtbl;
    app->refs = 1;
    app->real = *factory;
    *factory = app;
}

/***********************************************************************
 *      RoGetActivationFactory (combase.@)
 */
HRESULT WINAPI DECLSPEC_HOTPATCH RoGetActivationFactory(HSTRING classid, REFIID iid, void **class_factory)
{
    PFNGETACTIVATIONFACTORY pDllGetActivationFactory;
    IActivationFactory *factory;
    const WCHAR *name;
    WCHAR *library;
    HMODULE module;
    HRESULT hr;

    if (!iid || !class_factory)
        return E_INVALIDARG;

    if (FAILED(hr = ensure_mta()))
        return hr;

    if ((name = WindowsGetStringRawBuffer(classid, NULL)) && (factory = factory_cache_get(name)))
    {
        hr = IActivationFactory_QueryInterface(factory, iid, class_factory);
        IActivationFactory_Release(factory);
        if (SUCCEEDED(hr)) wrap_core_application(name, iid, class_factory);
        return hr;
    }

    FIXME("(%s, %s, %p): semi-stub\n", debugstr_hstring(classid), debugstr_guid(iid), class_factory);

    hr = get_library_for_classid(WindowsGetStringRawBuffer(classid, NULL), &library);
    if (FAILED(hr))
    {
        ERR("Failed to find library for %s\n", debugstr_hstring(classid));
        return hr;
    }

    module = NULL;
    if (!(pDllGetActivationFactory = library_cache_get(library)))
    {
        if (!(module = LoadLibraryW(library)))
        {
            ERR("Failed to load module %s\n", debugstr_w(library));
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto done;
        }

        if (!(pDllGetActivationFactory = (void *)GetProcAddress(module, "DllGetActivationFactory")))
        {
            ERR("Module %s does not implement DllGetActivationFactory\n", debugstr_w(library));
            hr = E_FAIL;
            goto done;
        }
        /* Cached before the factory is made, and the module kept loaded: a factory's creation can activate another
         * class of the same module (Windows.UI.Xaml.Controls' CommandBarFlyout asks for TextCommandBarFlyout, holding
         * XAML's g_csStatic). That nested request then went to LoadLibraryW again, which takes the loader lock even
         * for a loaded module, while another thread held the loader lock in a DllMain waiting for g_csStatic: the
         * Xbox app's intermittent blue start-up (ReactNativeGameStreaming.dll's DllMain). Windows does not take the
         * loader lock for a module that is already loaded. */
        if (!library_cache_add(library, pDllGetActivationFactory)) FreeLibrary(module);
        module = NULL;
    }

    TRACE("Found library %s for class %s\n", debugstr_w(library), debugstr_hstring(classid));

    hr = pDllGetActivationFactory(classid, &factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_QueryInterface(factory, iid, class_factory);
        if (SUCCEEDED(hr))
        {
            TRACE("Created interface %p\n", *class_factory);
            wrap_core_application(name, iid, class_factory);
            if (name) factory_cache_add(name, factory);
        }
        IActivationFactory_Release(factory);
    }
    else
    {
        ERR("Class %s not found in %s, hr %#lx.\n", wine_dbgstr_hstring(classid), debugstr_w(library), hr);
    }

done:
    free(library);
    if (module) FreeLibrary(module);
    return hr;
}

/***********************************************************************
 *      RoGetParameterizedTypeInstanceIID (combase.@)
 */
HRESULT WINAPI RoGetParameterizedTypeInstanceIID(UINT32 name_element_count, const WCHAR **name_elements,
                                                 const IRoMetaDataLocator *meta_data_locator, GUID *iid,
                                                 ROPARAMIIDHANDLE *hiid)
{
    FIXME("stub: %d %p %p %p %p\n", name_element_count, name_elements, meta_data_locator, iid, hiid);
    if (iid) *iid = GUID_NULL;
    if (hiid) *hiid = INVALID_HANDLE_VALUE;
    return E_NOTIMPL;
}

/***********************************************************************
 *      RoActivateInstance (combase.@)
 */
HRESULT WINAPI RoActivateInstance(HSTRING classid, IInspectable **instance)
{
    IActivationFactory *factory;
    HRESULT hr;

    FIXME("(%p, %p): semi-stub\n", classid, instance);

    hr = RoGetActivationFactory(classid, &IID_IActivationFactory, (void **)&factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_ActivateInstance(factory, instance);
        IActivationFactory_Release(factory);
    }

    return hr;
}

struct agile_reference
{
    IAgileReference IAgileReference_iface;
    enum AgileReferenceOptions option;
    IStream *marshal_stream;
    CRITICAL_SECTION cs;
    IUnknown *obj;
    BOOLEAN is_agile;
    IUnknown *ctx;
    LONG ref;
};

static HRESULT marshal_object_in_agile_reference(struct agile_reference *ref, REFIID riid, IUnknown *obj)
{
    HRESULT hr;

    hr = CreateStreamOnHGlobal(0, TRUE, &ref->marshal_stream);
    if (FAILED(hr))
        return hr;

    hr = CoMarshalInterface(ref->marshal_stream, riid, obj, MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
    if (FAILED(hr))
    {
        IStream_Release(ref->marshal_stream);
        ref->marshal_stream = NULL;
    }
    return hr;
}

static inline struct agile_reference *impl_from_IAgileReference(IAgileReference *iface)
{
    return CONTAINING_RECORD(iface, struct agile_reference, IAgileReference_iface);
}

static HRESULT WINAPI agile_ref_QueryInterface(IAgileReference *iface, REFIID riid, void **obj)
{
    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (!riid || !obj) return E_INVALIDARG;

    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IAgileObject)
        || IsEqualGUID(riid, &IID_IAgileReference))
    {
        IUnknown_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    *obj = NULL;
    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI agile_ref_AddRef(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI agile_ref_Release(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LONG ref = InterlockedDecrement(&impl->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);

        if (impl->obj)
            IUnknown_Release(impl->obj);

        if (impl->marshal_stream)
        {
            LARGE_INTEGER zero = {0};

            IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL);
            CoReleaseMarshalData(impl->marshal_stream);
            IStream_Release(impl->marshal_stream);
        }
        DeleteCriticalSection(&impl->cs);
        free(impl);
    }

    return ref;
}

struct marshal_context_params
{
    struct agile_reference *impl;
    REFIID iid;
};

static HRESULT WINAPI marshal_object_in_context(ComCallData *arg)
{
    struct marshal_context_params *params = (struct marshal_context_params *)arg;
    HRESULT hr;

    hr = marshal_object_in_agile_reference(params->impl, params->iid, params->impl->obj);
    IUnknown_Release(params->impl->obj);
    params->impl->obj = NULL;
    return hr;
}

static HRESULT WINAPI agile_ref_Resolve(IAgileReference *iface, REFIID riid, void **obj)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LARGE_INTEGER zero = {0};
    void *cur_ctx;
    HRESULT hr;

    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (impl->is_agile)
        return IUnknown_QueryInterface(impl->obj, riid, obj);

    if (FAILED(hr = CoGetContextToken((ULONG_PTR *)&cur_ctx)))
        return hr;

    EnterCriticalSection(&impl->cs);
    if (impl->option == AGILEREFERENCE_DELAYEDMARSHAL && impl->marshal_stream == NULL)
    {
        struct marshal_context_params params = { impl, riid };
        IContextCallback *ctx;

        if (FAILED(hr = IUnknown_QueryInterface(impl->ctx, &IID_IContextCallback, (void **)&ctx)))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }

        hr = IContextCallback_ContextCallback(ctx, marshal_object_in_context, (ComCallData *)&params,
                                              &IID_IContextCallback, 5, NULL);
        IContextCallback_Release(ctx);
        if (FAILED(hr))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }
    }

    if (SUCCEEDED(hr = IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL)))
        hr = CoUnmarshalInterface(impl->marshal_stream, riid, obj);

    LeaveCriticalSection(&impl->cs);
    return hr;
}

static const IAgileReferenceVtbl agile_ref_vtbl =
{
    agile_ref_QueryInterface,
    agile_ref_AddRef,
    agile_ref_Release,
    agile_ref_Resolve,
};

static BOOL object_has_interface(IUnknown *obj, REFIID iid)
{
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface(obj, iid, (void **)&unk);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
    return SUCCEEDED(hr);
}

/***********************************************************************
 *      RoGetAgileReference (combase.@)
 */
HRESULT WINAPI RoGetAgileReference(enum AgileReferenceOptions option, REFIID riid, IUnknown *obj,
                                   IAgileReference **agile_reference)
{
    struct apartment *apt;
    struct agile_reference *impl;
    HRESULT hr;

    TRACE("(%d, %s, %p, %p).\n", option, debugstr_guid(riid), obj, agile_reference);

    if (option != AGILEREFERENCE_DEFAULT && option != AGILEREFERENCE_DELAYEDMARSHAL)
        return E_INVALIDARG;

    if (!(apt = apartment_get_current_or_mta()))
    {
        ERR("Apartment not initialized\n");
        return CO_E_NOTINITIALIZED;
    }
    rpc_start_remoting(apt);
    apartment_release(apt);

    if (!object_has_interface(obj, riid))
        return E_NOINTERFACE;
    if (object_has_interface(obj, &IID_INoMarshal))
        return CO_E_NOT_SUPPORTED;

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return E_OUTOFMEMORY;

    impl->IAgileReference_iface.lpVtbl = &agile_ref_vtbl;
    impl->option = option;
    impl->is_agile = object_has_interface(obj, &IID_IAgileObject);
    impl->ref = 1;
    if (FAILED(hr = CoGetContextToken((ULONG_PTR *)&impl->ctx)))
    {
        free( impl );
        return hr;
    }

    if (option == AGILEREFERENCE_DELAYEDMARSHAL || impl->is_agile)
    {
        impl->obj = obj;
        IUnknown_AddRef(impl->obj);
    }
    else if (option == AGILEREFERENCE_DEFAULT)
    {
        if (FAILED(hr = marshal_object_in_agile_reference(impl, riid, obj)))
        {
            free(impl);
            return hr;
        }
    }

    InitializeCriticalSection(&impl->cs);

    *agile_reference = &impl->IAgileReference_iface;
    return S_OK;
}

/***********************************************************************
 *      RoFailFastWithErrorContextInternal2 (combase.@)
 */
struct stowed_exception_v2
{
    ULONG size;
    ULONG signature;      /* 'SE01' / 'SE02' */
    HRESULT result;
    DWORD form_thread;    /* form:2 (1 binary, 2 text), thread:30 */
    union
    {
        struct
        {
            void *address;
            ULONG stack_word_size;
            ULONG stack_words;
            void *stack;
        } binary;
        WCHAR *text;
    } u;
    ULONG nested_type;    /* 'W32E' 'STOW' 'LEO1' */
    void *nested;
};

/* dump a stowed exception chain: this is the only place a failing WinRT UI stack tells us why */
static void dump_stowed_exception( const struct stowed_exception_v2 *info, int depth )
{
    ULONG form;

    if (!info || depth > 4) return;
    form = info->form_thread & 3;
    ERR( "%*sstowed exception hr %#lx form %lu signature %c%c%c%c\n", depth * 2, "", info->result, form,
         (char)info->signature, (char)(info->signature >> 8), (char)(info->signature >> 16), (char)(info->signature >> 24) );
    if (form == 2) ERR( "%*s  text %s\n", depth * 2, "", debugstr_w(info->u.text) );
    else if (form == 1)
    {
        ULONG i;
        ERR( "%*s  address %p, %lu stack words\n", depth * 2, "", info->u.binary.address, info->u.binary.stack_words );
        for (i = 0; i < min( info->u.binary.stack_words, 48 ) && info->u.binary.stack; i++)
        {
            ULONG_PTR word = info->u.binary.stack_word_size == 8 ? ((ULONG_PTR *)info->u.binary.stack)[i]
                                                                  : ((ULONG *)info->u.binary.stack)[i];
            ERR( "%*s  frame[%lu] = %#Ix\n", depth * 2, "", i, word );
        }
    }
    if (info->nested_type == 0x574f5453 /* 'STOW' */) dump_stowed_exception( info->nested, depth + 1 );
    else if (info->nested_type == 0x314f454c /* 'LEO1' */ && info->nested)
    {
        IRestrictedErrorInfo *rei;
        if (SUCCEEDED(IUnknown_QueryInterface( (IUnknown *)info->nested, &IID_IRestrictedErrorInfo, (void **)&rei )))
        {
            BSTR desc = NULL, restricted = NULL, sid = NULL;
            HRESULT hr;
            if (SUCCEEDED(IRestrictedErrorInfo_GetErrorDetails( rei, &desc, &hr, &restricted, &sid )))
                ERR( "%*s  language exception hr %#lx: %s / %s\n", depth * 2, "", hr, debugstr_w(desc), debugstr_w(restricted) );
            SysFreeString( desc ); SysFreeString( restricted ); SysFreeString( sid );
            IRestrictedErrorInfo_Release( rei );
        }
    }
    else if (info->nested_type) ERR( "%*s  nested type %#lx at %p\n", depth * 2, "", info->nested_type, info->nested );
}

void WINAPI RoFailFastWithErrorContextInternal2(HRESULT error, ULONG exception_count, /* PSTOWED_EXCEPTION_INFORMATION_V2 */void *information)
{
    ULONG i;
    struct stowed_exception_v2 **infos = information;
    WCHAR pkg[4];

    FIXME("%#lx, %lu, %p stub.\n", error, exception_count, information);
    for (i = 0; i < exception_count && infos; i++) dump_stowed_exception( infos[i], 0 );
    /* Hosted UWP: CoreMessaging IMessageLoopExtensions::Run can FailFast when the
     * Win32EventLoopBridge is not in host-wait mode. Swallow so DrainCoreMessaging
     * from ProcessMessage can keep the UI pump alive. */
    if (GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", pkg, ARRAY_SIZE(pkg) ) ||
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        ERR( "hosted UWP, ignoring FailFast %#lx\n", error );
        return;
    }
    RaiseFailFastException(NULL, NULL, 0);
}

/***********************************************************************
 *      RoGetApartmentIdentifier (combase.@)
 */
HRESULT WINAPI RoGetApartmentIdentifier(UINT64 *identifier)
{
    APTTYPEQUALIFIER qualifier;
    APTTYPE type;
    HRESULT hr;

    TRACE("(%p)\n", identifier);

    if (!identifier)
        return E_INVALIDARG;

    /* This used to answer 0xdeadbeef for every apartment, which makes every thread in the process
     * look like the same one. .NET Native's RCW layer records the identifier an object belongs to
     * and compares it on each call to decide whether it needs a proxy, so a constant is not a
     * harmless stub: it makes the comparison meaningless in both directions. The value only has to
     * be stable per apartment and distinct between apartments - a single-threaded apartment is its
     * thread, and every thread in the multi-threaded apartment shares one identifier. */
    if (FAILED(hr = CoGetApartmentType(&type, &qualifier)))
        return hr;

    if (type == APTTYPE_MTA)
        *identifier = 1;
    else
        *identifier = ((UINT64)GetCurrentProcessId() << 32) | GetCurrentThreadId();
    return S_OK;
}

/***********************************************************************
 *      RoRegisterForApartmentShutdown (combase.@)
 */
HRESULT WINAPI RoRegisterForApartmentShutdown(IApartmentShutdown *callback,
        UINT64 *identifier, APARTMENT_SHUTDOWN_REGISTRATION_COOKIE *cookie)
{
    HRESULT hr;

    FIXME("(%p, %p, %p): stub\n", callback, identifier, cookie);

    hr = RoGetApartmentIdentifier(identifier);
    if (FAILED(hr))
        return hr;

    if (cookie)
        *cookie = (void *)0xcafecafe;
    return S_OK;
}

/***********************************************************************
 *      RoGetServerActivatableClasses (combase.@)
 */
HRESULT WINAPI RoGetServerActivatableClasses(HSTRING name, HSTRING **classes, DWORD *count)
{
    FIXME("(%p, %p, %p): stub\n", name, classes, count);

    if (count)
        *count = 0;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterActivationFactories (combase.@)
 */
HRESULT WINAPI RoRegisterActivationFactories(HSTRING *classes, PFNGETACTIVATIONFACTORY *callbacks,
                                             UINT32 count, RO_REGISTRATION_COOKIE *cookie)
{
    FIXME("(%p, %p, %d, %p): stub\n", classes, callbacks, count, cookie);

    return S_OK;
}

/***********************************************************************
 *      RoReportUnhandledError (combase.@)
 */
HRESULT WINAPI RoReportUnhandledError(IRestrictedErrorInfo *info)
{
    FIXME("(%p): stub\n", info);
    {
        extern void combase_dump_unhandled_language_exception( IRestrictedErrorInfo *info );
        combase_dump_unhandled_language_exception( info );
    }
    return S_OK;
}

/***********************************************************************
 *      RoSetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoSetErrorReportingFlags(UINT32 flags)
{
    FIXME("(%08x): stub\n", flags);
    return S_OK;
}

/***********************************************************************
 *      RoGetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoGetErrorReportingFlags(UINT32 *flags)
{
    FIXME("(%p): stub\n", flags);

    if (!flags)
        return E_POINTER;

    *flags = RO_ERROR_REPORTING_USESETERRORINFO;
    return S_OK;
}


/***********************************************************************
 *      CleanupTlsOleState (combase.@)
 */
void WINAPI CleanupTlsOleState(void *unknown)
{
    FIXME("(%p): stub\n", unknown);
}

/***********************************************************************
 *      DllGetActivationFactory (combase.@)
 */
/* Windows.Foundation.Diagnostics.AsyncCausalityTracer lives in combase on Windows. The .NET
 * runtime asks for it the first time an app starts a Task; the whole class is diagnostics, so
 * accepting the calls and doing nothing is a complete implementation for a Wine prefix. */
DEFINE_GUID(IID_IAsyncCausalityTracerStatics, 0x50850b26, 0x267e, 0x451b, 0xa8, 0x90, 0xab, 0x6a, 0x37, 0x02, 0x45, 0xee);

struct causality_tracer
{
    IActivationFactory IActivationFactory_iface;
    const void **statics_vtbl;
};

static HRESULT WINAPI causality_QueryInterface(IActivationFactory *iface, REFIID iid, void **out);
static ULONG WINAPI causality_AddRef(IActivationFactory *iface) { return 2; }
static ULONG WINAPI causality_Release(IActivationFactory *iface) { return 1; }
static HRESULT WINAPI causality_GetIids(IActivationFactory *iface, ULONG *count, IID **iids)
{
    *count = 0; *iids = NULL; return S_OK;
}
static HRESULT WINAPI causality_GetRuntimeClassName(IActivationFactory *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.Foundation.Diagnostics.AsyncCausalityTracer", 51, name);
}
static HRESULT WINAPI causality_GetTrustLevel(IActivationFactory *iface, TrustLevel *level)
{
    *level = BaseTrust; return S_OK;
}
static HRESULT WINAPI causality_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    *instance = NULL; return E_NOTIMPL;   /* statics only */
}

static const IActivationFactoryVtbl causality_factory_vtbl =
{
    causality_QueryInterface, causality_AddRef, causality_Release,
    causality_GetIids, causality_GetRuntimeClassName, causality_GetTrustLevel,
    causality_ActivateInstance,
};

static HRESULT WINAPI causality_statics_QueryInterface(void *iface, REFIID iid, void **out);
static ULONG WINAPI causality_statics_AddRef(void *iface) { return 2; }
static ULONG WINAPI causality_statics_Release(void *iface) { return 1; }
static HRESULT WINAPI causality_statics_GetIids(void *iface, ULONG *count, IID **iids)
{
    *count = 0; *iids = NULL; return S_OK;
}
static HRESULT WINAPI causality_statics_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return causality_GetRuntimeClassName(NULL, name);
}
static HRESULT WINAPI causality_statics_GetTrustLevel(void *iface, TrustLevel *level)
{
    *level = BaseTrust; return S_OK;
}
static HRESULT WINAPI causality_trace(void *iface) { return S_OK; }
static HRESULT WINAPI causality_add_TracingStatusChanged(void *iface, void *handler, UINT64 *token)
{
    if (token) *token = 0;
    return S_OK;
}
static HRESULT WINAPI causality_remove_TracingStatusChanged(void *iface, UINT64 token) { return S_OK; }

static const void *causality_statics_vtbl[] =
{
    causality_statics_QueryInterface, causality_statics_AddRef, causality_statics_Release,
    causality_statics_GetIids, causality_statics_GetRuntimeClassName, causality_statics_GetTrustLevel,
    causality_trace,   /* TraceOperationCreation */
    causality_trace,   /* TraceOperationCompletion */
    causality_trace,   /* TraceOperationRelation */
    causality_trace,   /* TraceSynchronousWorkStart */
    causality_trace,   /* TraceSynchronousWorkCompletion */
    causality_add_TracingStatusChanged,
    causality_remove_TracingStatusChanged,
};

static struct causality_tracer causality_tracer =
{
    { &causality_factory_vtbl },
    causality_statics_vtbl,
};

static HRESULT causality_query(REFIID iid, void **out)
{
    /* A WinRT statics object is agile; .NET checks for the marker before it will use it. */
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IActivationFactory) || IsEqualGUID(iid, &IID_IAgileObject))
        *out = &causality_tracer.IActivationFactory_iface;
    else if (IsEqualGUID(iid, &IID_IAsyncCausalityTracerStatics))
        *out = &causality_tracer.statics_vtbl;
    else
    {
        FIXME("AsyncCausalityTracer: no interface %s\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT WINAPI causality_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    return causality_query(iid, out);
}
static HRESULT WINAPI causality_statics_QueryInterface(void *iface, REFIID iid, void **out)
{
    return causality_query(iid, out);
}

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const WCHAR *name = WindowsGetStringRawBuffer(classid, NULL);

    TRACE("(%s, %p)\n", debugstr_hstring(classid), factory);

    if (!factory) return E_INVALIDARG;
    if (name && !wcscmp(name, L"Windows.Foundation.Diagnostics.AsyncCausalityTracer"))
    {
        *factory = &causality_tracer.IActivationFactory_iface;
        return S_OK;
    }

    FIXME("%s not implemented\n", debugstr_hstring(classid));
    *factory = NULL;
    return REGDB_E_CLASSNOTREG;
}

/***********************************************************************
 *      RoFailFastWithErrorContext (combase.@)
 */
void WINAPI RoFailFastWithErrorContext(HRESULT hr)
{
    WCHAR pkg[4];

    FIXME("(0x%08lx)\n", hr);
    if (GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", pkg, ARRAY_SIZE(pkg) ) ||
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        ERR( "hosted UWP, ignoring FailFast %#lx\n", hr );
        return;
    }
    RaiseFailFastException(NULL, NULL, 0);
}

/***********************************************************************
 *      RoRevokeActivationFactories (combase.@)
 */
HRESULT WINAPI RoRevokeActivationFactories(RO_REGISTRATION_COOKIE cookie)
{
    FIXME("(%p): stub\n", cookie);
    return S_OK;
}

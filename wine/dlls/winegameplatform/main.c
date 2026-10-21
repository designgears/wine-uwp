/*
 * Microsoft.GamePlatform.Services classes
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
 * The Xbox app ships Microsoft.GamePlatform.Services.dll, a client of the Gaming Services NT services
 * (GamingServices.exe, LocalSystem, with the xvdd.sys and gameflt.sys kernel drivers). Those cannot
 * run under Wine, so the client reports GamePlatformApiStatus "Missing" and the app tries to install
 * Gaming Services and never leaves its splash.
 *
 * This module answers for the client classes instead. It is registered with OverridesPackage=1, which
 * makes combase prefer it to the package's own DLL. What it says is honest for a machine with a game
 * platform that installs Store packages (install.c, gdk.c) and runs no service:
 *   PlatformStatus.ApiStatus = Ready, its change event registers and never fires;
 *   every event registration succeeds; every other call is traced by name and answered as the
 *   package's own client answers without the service (0x87E00196), unless it is in `specials` below.
 *
 * Every class with an I<Class>Statics interface is covered. The interfaces are in the package's
 * Microsoft.GamePlatform.Services.winmd, not in Wine's IDL, so the vtables here are untyped: the class
 * and slot table (slots.h) is generated from that metadata by gen-gameplatform.py in wine-uwp.
 */

#include "initguid.h"
#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gameplatform);

#include "slots.h"

enum { K_COUNT = ARRAY_SIZE(gp_classes), SLOTS = 40 };

static const void *factory_vtbl[K_COUNT][SLOTS];
static struct factory factories[K_COUNT];

static const struct gp_slot *slot_info( int kind, int n )
{
    unsigned i;
    for (i = 0; i < gp_classes[kind].count; i++)
        if (gp_classes[kind].slots[i].slot == n) return &gp_classes[kind].slots[i];
    return NULL;
}

const char *class_name( int kind )
{
    static char names[K_COUNT][96];
    if (kind < 0) return "?";
    if (!names[kind][0])
        WideCharToMultiByte( CP_UTF8, 0, gp_classes[kind].name + 32 /* after "Microsoft.GamePlatform.Services." */, -1,
                             names[kind], sizeof(names[kind]), NULL, NULL );
    return names[kind];
}

int kind_of( const WCHAR *name )
{
    int k;
    for (k = 0; k < K_COUNT; k++) if (!wcscmp( gp_classes[k].name, name )) return k;
    return -1;
}

const void **factory_vtbl_of( int kind ) { return factory_vtbl[kind]; }
struct factory *factory_of( int kind ) { return &factories[kind]; }

void log_guid( const char *what, REFIID iid )
{
    TRACE( "%s %s\n", what, debugstr_guid( iid ) );
}

static HRESULT WINAPI factory_QueryInterface( struct factory *f, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IWeakReferenceSource_ )) return weak_source( (IUnknown *)f, out );
    if (base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory ) || IsEqualGUID( iid, &gp_classes[f->kind].statics ))
    {
        InterlockedIncrement( &f->ref );
        *out = f;
        return S_OK;
    }
    TRACE( "%s: QueryInterface refused %s\n", class_name( f->kind ), debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

ULONG WINAPI factory_AddRef( struct factory *f )
{
    return InterlockedIncrement( &f->ref );
}

static ULONG WINAPI factory_Release( struct factory *f )
{
    ULONG ref = InterlockedDecrement( &f->ref );
    if (!ref && f->heap) free( f );
    return ref;
}

static HRESULT WINAPI factory_GetIids( struct factory *f, ULONG *count, IID **ids )
{
    if (count) *count = 0;
    if (ids) *ids = NULL;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( struct factory *f, HSTRING *name )
{
    UINT32 len = wcslen( gp_classes[f->kind].name );
    if (f->heap) len -= 8; /* "Instance" */
    return WindowsCreateString( gp_classes[f->kind].name, len, name );
}

static HRESULT WINAPI factory_GetTrustLevel( struct factory *f, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

/* "module+offset" of a caller, for the trace */
static void caller_name( void *addr, char *buf, size_t size )
{
    char path[MAX_PATH] = "?", *base;
    HMODULE module = NULL;

    if (!GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             addr, &module ))
    {
        snprintf( buf, size, "%p", addr );
        return;
    }
    GetModuleFileNameA( module, path, sizeof(path) );
    base = strrchr( path, '\\' ) ? strrchr( path, '\\' ) + 1 : path;
    snprintf( buf, size, "%s+%#Ix", base, (ULONG_PTR)((char *)addr - (char *)module) );
}

/* The generic slot: an event registration succeeds (and never fires), anything else is traced and answered as
 * the package's own client answers without the service. */
HRESULT generic_slot( struct factory *f, int n, void *a2, void *ret )
{
    const struct gp_slot *s = slot_info( f->kind, n );
    char caller[160];

    if (s && s->kind == 1)
    {
        LONGLONG *token = a2;
        if (!token) return E_POINTER;
        *token = InterlockedIncrement64( &f->next_token );
        TRACE( "%s.%s: registered (token %I64d)\n", class_name( f->kind ), s->name, *token );
        return S_OK;
    }
    if (s && s->kind == 2) return S_OK;
    caller_name( ret, caller, sizeof(caller) );
    FIXME( "%s.%s (slot %d, from %s) -> not implemented here (0x87E00196)\n", class_name( f->kind ),
           s ? s->name : "?", n, caller );
    return GP_E_NO_SERVICE;
}

/* Only the register arguments: a thunk must not write to its caller's stack, and a sibling call passing a fifth
 * argument would store it in the incoming argument area, which belongs to the caller when it passed fewer. */
#define THUNK(n) \
    static HRESULT WINAPI t_##n( struct factory *f, void *a1, void *a2 ) \
    { return generic_slot( f, n, a2, __builtin_return_address(0) ); }
THUNK(6) THUNK(7) THUNK(8) THUNK(9) THUNK(10) THUNK(11) THUNK(12) THUNK(13) THUNK(14) THUNK(15) THUNK(16) THUNK(17)
THUNK(18) THUNK(19) THUNK(20) THUNK(21) THUNK(22) THUNK(23) THUNK(24) THUNK(25) THUNK(26) THUNK(27) THUNK(28) THUNK(29)
THUNK(30) THUNK(31) THUNK(32) THUNK(33) THUNK(34) THUNK(35) THUNK(36) THUNK(37) THUNK(38) THUNK(39)
#undef THUNK

static const void *thunks[SLOTS] =
{
    NULL, NULL, NULL, NULL, NULL, NULL,
    t_6, t_7, t_8, t_9, t_10, t_11, t_12, t_13, t_14, t_15, t_16, t_17, t_18, t_19, t_20, t_21,
    t_22, t_23, t_24, t_25, t_26, t_27, t_28, t_29, t_30, t_31, t_32, t_33, t_34, t_35, t_36, t_37,
    t_38, t_39
};

/* ---- the slots answered for real ---- */

/* PlatformStatus.ApiStatus: the platform is ready */
static HRESULT WINAPI ps_get_ApiStatus( struct factory *f, INT32 *value )
{
    TRACE( "PlatformStatus.ApiStatus -> Ready\n" );
    if (!value) return E_POINTER;
    *value = 0; /* GamePlatformApiStatus.Ready */
    return S_OK;
}

/* Settings, kept per prefix in HKCU\Software\Wine\WineUWP\GamePlatform */
static const WCHAR settings_key[] = L"Software\\Wine\\WineUWP\\GamePlatform";

static DWORD setting_dword( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);
    if (RegGetValueW( HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_DWORD, NULL, &value, &size )) value = def;
    return value;
}

static void set_setting_dword( const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( HKEY_CURRENT_USER, settings_key, name, REG_DWORD, &value, sizeof(value) );
}

/* The game folder of the prefix's storage device: C:\XboxGames unless the app moved it */
static void game_folder( WCHAR *buf, DWORD size )
{
    DWORD bytes = size * sizeof(WCHAR);
    if (RegGetValueW( HKEY_CURRENT_USER, settings_key, L"GameFolder", RRF_RT_REG_SZ, NULL, buf, &bytes ))
        lstrcpynW( buf, L"C:\\XboxGames", size );
}

/* Stable stand-in volume IDs for the prefix and the host game-data drive. */
static const GUID DEVICE_ID = {0x7e1d3a52,0x5c1b,0x4f3e,{0x9a,0x52,0x77,0x69,0x6e,0x65,0x75,0x77}};
/* Installed payloads live in the host data directory, addressed through Wine's Z: drive. */
static const GUID DATA_DEVICE_ID = {0x7e1d3a53,0x5c1b,0x4f3e,{0x9a,0x52,0x77,0x69,0x6e,0x65,0x75,0x77}};

/* StorageDeviceDetails {fd57cdc7} */
static const GUID IID_IStorageDeviceDetails = {0xfd57cdc7,0xe911,0x5a3a,{0x9b,0x3f,0xb3,0xb5,0xd3,0x64,0xcd,0x3b}};

struct device_details
{
    struct object obj;
    BOOL data_device;
    ULARGE_INTEGER total, free;
};

GETTER(struct device_details, dd, CrdId, UINT32, 0)
GETTER(struct device_details, dd, TotalSize, UINT64, This->total.QuadPart)
GETTER(struct device_details, dd, FreeSpace, UINT64, This->free.QuadPart)
GETTER(struct device_details, dd, Status, INT32, 1 /* Ready */)
GETTER(struct device_details, dd, BusType, INT32, 0 /* Unknown */)
GETTER(struct device_details, dd, BusDetails, INT32, 0)
GETTER(struct device_details, dd, AssociationType, INT32, 0)
GETTER(struct device_details, dd, Flags, INT32, 0)

static HRESULT WINAPI dd_get_DriveLetter( struct device_details *This, HSTRING *v )
{
    TRACE( "StorageDeviceDetails.DriveLetter -> %s\n", This->data_device ? "Z:" : "C:" );
    return hstr( This->data_device ? L"Z:" : L"C:", v );
}

static HRESULT WINAPI dd_get_GameFolder( struct device_details *This, HSTRING *v )
{
    WCHAR path[MAX_PATH];
    if (This->data_device) data_game_folder( path, ARRAY_SIZE(path) );
    else game_folder( path, ARRAY_SIZE(path) );
    return hstr( path, v );
}

static HRESULT WINAPI dd_get_DisplayName( struct device_details *This, HSTRING *v )
{
    return hstr( This->data_device ? L"Game data" : L"Local Disk", v );
}

static const void *device_details_vtbl[] =
{
    OBJ_METHODS, dd_get_CrdId, dd_get_TotalSize, dd_get_FreeSpace, dd_get_Status, dd_get_DriveLetter, dd_get_GameFolder,
    dd_get_DisplayName, dd_get_BusType, dd_get_BusDetails, dd_get_AssociationType, dd_get_Flags
};

static IInspectable *device_details_create( BOOL data )
{
    struct device_details *d = object_create( sizeof(*d), device_details_vtbl, &IID_IStorageDeviceDetails,
                                              L"Microsoft.GamePlatform.Services.StorageDeviceDetails", NULL );
    WCHAR path[MAX_PATH], *slash;

    if (!d) return NULL;
    d->data_device = data;
    /* a fresh data directory has no stage/ until the first install: measure the nearest folder that exists */
    if (data) data_game_folder( path, ARRAY_SIZE(path) );
    else wcscpy( path, L"C:\\" );
    while (!GetDiskFreeSpaceExW( path, &d->free, &d->total, NULL ))
    {
        if (!(slash = wcsrchr( path, '\\' )) || slash - path < 3) break;
        *slash = 0;
    }
    return (IInspectable *)d;
}

static HRESULT WINAPI st_QueryDeviceDetails( struct factory *f, const GUID *id, IInspectable **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    /* Older queue records use the package instance as their device ID. Those payloads are in the
     * host data directory too; report the same drive that QueryInstallPath and enumeration expose. */
    if (id && !IsEqualGUID( id, &DEVICE_ID ) && is_package_instance( id ))
    {
        TRACE( "Storage.QueryDeviceDetails(a package instance) -> Z:\n" );
        return (*out = device_details_create( TRUE )) ? S_OK : E_OUTOFMEMORY;
    }
    if (id && IsEqualGUID( id, &DATA_DEVICE_ID ))
    {
        TRACE( "Storage.QueryDeviceDetails -> Z:\n" );
        return (*out = device_details_create( TRUE )) ? S_OK : E_OUTOFMEMORY;
    }
    if (!id || !IsEqualGUID( id, &DEVICE_ID ))
    {
        TRACE( "Storage.QueryDeviceDetails(unknown device %s, from %p)\n", debugstr_guid( id ), __builtin_return_address(0) );
        return E_INVALIDARG;
    }
    TRACE( "Storage.QueryDeviceDetails -> C:\n" );
    return (*out = device_details_create( FALSE )) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI st_EnumerateDevices( struct factory *f, IInspectable **out )
{
    struct guid_view *view;

    if (!out) return E_POINTER;
    if (!(view = guid_view_create())) return E_OUTOFMEMORY;
    guid_view_add( view, &DEVICE_ID );
    guid_view_add( view, &DATA_DEVICE_ID );
    TRACE( "Storage.EnumerateDevices -> C:, Z:\n" );
    *out = (IInspectable *)view;
    return S_OK;
}

/* Storage.EnumerateContent: the games installed through the queue (stage directories with a marker) */
HRESULT WINAPI st_EnumerateContent( struct factory *f, const GUID *device, IInspectable **out );

static HRESULT WINAPI st_QueryDefaultDevice( struct factory *f, INT32 category, boolean validate, GUID *out )
{
    if (!out) return E_POINTER;
    TRACE( "Storage.QueryDefaultDevice(category %d) -> C:\n", category );
    *out = DEVICE_ID;
    return S_OK;
}

static HRESULT WINAPI st_SetDefaultDevice( struct factory *f, INT32 category, const GUID *device )
{
    TRACE( "Storage.SetDefaultDevice(category %d): there is one device\n", category );
    return S_OK;
}

static HRESULT WINAPI gf_QueryForDevice( struct factory *f, const GUID *id, HSTRING *out )
{
    WCHAR path[MAX_PATH];

    if (!out) return E_POINTER;
    *out = NULL;
    if (!id || (!IsEqualGUID( id, &DEVICE_ID ) && !IsEqualGUID( id, &DATA_DEVICE_ID ))) return E_INVALIDARG;
    if (IsEqualGUID( id, &DATA_DEVICE_ID )) data_game_folder( path, ARRAY_SIZE(path) );
    else game_folder( path, ARRAY_SIZE(path) );
    return hstr( path, out );
}

static HRESULT WINAPI gf_SetForDevice( struct factory *f, const GUID *id, HSTRING path )
{
    UINT32 len = 0;
    const WCHAR *p = WindowsGetStringRawBuffer( path, &len );
    WCHAR data[MAX_PATH];

    if (!id || !len) return E_INVALIDARG;
    if (IsEqualGUID( id, &DATA_DEVICE_ID ))
    {
        /* WINE_UWP_GAMES_DATA owns this location. Do not accept a move we cannot perform. */
        data_game_folder( data, ARRAY_SIZE(data) );
        return !wcsicmp( p, data ) ? S_OK : HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    }
    if (!IsEqualGUID( id, &DEVICE_ID )) return E_INVALIDARG;
    TRACE( "GameFolder.SetForDevice(%s)\n", debugstr_w( p ) );
    return HRESULT_FROM_WIN32( RegSetKeyValueW( HKEY_CURRENT_USER, settings_key, L"GameFolder", REG_SZ, p,
                                                (len + 1) * sizeof(WCHAR) ) );
}

/* CustomizationGlobalProps {c2f4f8e3}: the update settings, a property bag read and written as a whole */
static const GUID IID_ICustomizationGlobalProps = {0xc2f4f8e3,0xa535,0x53ef,{0x81,0x71,0xec,0xab,0x8c,0x86,0x2c,0x56}};

enum { GP_AUTOMATIC, GP_DURING, GP_STALE, GP_POWEROFF, GP_THROTTLING, GP_MBPS, GP_WINDOW, GP_START, GP_END, GP_CAP, GP_COUNT };
static const WCHAR *global_prop_names[GP_COUNT] =
{
    L"AutomaticUpdates", L"UpdatesDuringGameplay", L"UninstallStaleGPGames", L"PowerOffAfterUpdates", L"NetworkThrottling",
    L"MaxNetworkBandwidthMbps", L"UpdateTimeWindow", L"UpdateWindowStartTimeMins", L"UpdateWindowEndTimeMins", L"ResolutionCap"
};

struct global_props
{
    struct object obj;
    DWORD v[GP_COUNT];
};

#define BOOLPROP(name, i) \
    static HRESULT WINAPI gprops_get_##name( struct global_props *p, boolean *v ) { if (!v) return E_POINTER; *v = !!p->v[i]; return S_OK; } \
    static HRESULT WINAPI gprops_put_##name( struct global_props *p, boolean v ) { p->v[i] = !!v; return S_OK; }
#define NUMPROP(name, i, type) \
    static HRESULT WINAPI gprops_get_##name( struct global_props *p, type *v ) { if (!v) return E_POINTER; *v = (type)p->v[i]; return S_OK; } \
    static HRESULT WINAPI gprops_put_##name( struct global_props *p, type v ) { p->v[i] = (DWORD)v; return S_OK; }
BOOLPROP(AutomaticUpdates, GP_AUTOMATIC) BOOLPROP(UpdatesDuringGameplay, GP_DURING) BOOLPROP(UninstallStaleGPGames, GP_STALE)
BOOLPROP(PowerOffAfterUpdates, GP_POWEROFF) BOOLPROP(NetworkThrottling, GP_THROTTLING) NUMPROP(MaxNetworkBandwidthMbps, GP_MBPS, UINT32)
BOOLPROP(UpdateTimeWindow, GP_WINDOW) NUMPROP(UpdateWindowStartTimeMins, GP_START, UINT16) NUMPROP(UpdateWindowEndTimeMins, GP_END, UINT16)
NUMPROP(ResolutionCap, GP_CAP, INT32)
#undef BOOLPROP
#undef NUMPROP

#define PROP(name) gprops_get_##name, gprops_put_##name
static const void *global_props_vtbl[] =
{
    OBJ_METHODS, PROP(AutomaticUpdates), PROP(UpdatesDuringGameplay), PROP(UninstallStaleGPGames), PROP(PowerOffAfterUpdates),
    PROP(NetworkThrottling), PROP(MaxNetworkBandwidthMbps), PROP(UpdateTimeWindow), PROP(UpdateWindowStartTimeMins),
    PROP(UpdateWindowEndTimeMins), PROP(ResolutionCap)
};
#undef PROP

/* the ABI of ICustomizationGlobalProps, to read the app's own object */
typedef HRESULT (WINAPI *get_bool_fn)( void *, boolean * );
typedef HRESULT (WINAPI *get_u32_fn)( void *, UINT32 * );
typedef HRESULT (WINAPI *get_u16_fn)( void *, UINT16 * );
typedef HRESULT (WINAPI *get_i32_fn)( void *, INT32 * );
typedef HRESULT (WINAPI *get_hstr_fn)( void *, HSTRING * );

static HRESULT WINAPI cu_QueryGlobalProps( struct factory *f, IInspectable **out )
{
    struct global_props *p;
    int i;

    if (!out) return E_POINTER;
    if (!(p = object_create( sizeof(*p), global_props_vtbl, &IID_ICustomizationGlobalProps,
                             L"Microsoft.GamePlatform.Services.CustomizationGlobalProps", NULL )))
        return E_OUTOFMEMORY;
    /* Windows' defaults: automatic updates on, updates during gameplay off */
    for (i = 0; i < GP_COUNT; i++) p->v[i] = setting_dword( global_prop_names[i], i == GP_AUTOMATIC );
    TRACE( "Customization.QueryGlobalProps (automatic updates %lu, during gameplay %lu)\n", p->v[GP_AUTOMATIC], p->v[GP_DURING] );
    *out = (IInspectable *)p;
    return S_OK;
}

static HRESULT WINAPI cu_SetGlobalProps( struct factory *f, IInspectable *props )
{
    DWORD v[GP_COUNT];
    void **in = NULL, **vtbl;
    boolean b;
    UINT32 u;
    UINT16 w;
    INT32 n;
    int i;

    if (!props || FAILED(IInspectable_QueryInterface( props, &IID_ICustomizationGlobalProps, (void **)&in ))) return E_INVALIDARG;
    for (i = 0; i < GP_COUNT; i++) v[i] = setting_dword( global_prop_names[i], i == GP_AUTOMATIC );
    /* the app may pass its own CustomizationGlobalProps (the package's class): copy it field by field; the getters
     * are slots 6, 8, ... */
    vtbl = *(void ***)in;
    for (i = 0; i < GP_COUNT; i++)
    {
        void *get = vtbl[6 + 2 * i];
        if (i == GP_MBPS) { if (SUCCEEDED(((get_u32_fn)get)( in, &u ))) v[i] = u; }
        else if (i == GP_START || i == GP_END) { if (SUCCEEDED(((get_u16_fn)get)( in, &w ))) v[i] = w; }
        else if (i == GP_CAP) { if (SUCCEEDED(((get_i32_fn)get)( in, &n ))) v[i] = n; }
        else if (SUCCEEDED(((get_bool_fn)get)( in, &b ))) v[i] = b;
    }
    IUnknown_Release( (IUnknown *)in );
    for (i = 0; i < GP_COUNT; i++) set_setting_dword( global_prop_names[i], v[i] );
    TRACE( "Customization.SetGlobalProps (automatic updates %lu, during gameplay %lu)\n", v[GP_AUTOMATIC], v[GP_DURING] );
    return S_OK;
}

/* Manage reads the per-game settings independently of the global update policy. */
static const GUID IID_ICustomizationGameProps = {0xfee91193,0x2e09,0x5fb9,{0x8f,0x59,0x03,0x5d,0x0c,0x45,0xf3,0xe1}};

struct game_props
{
    struct object obj;
    INT32 priority, cap;    /* Standard priority, Default resolution cap */
    WCHAR *args;
};

static void game_props_destroy( struct object *obj )
{
    free( ((struct game_props *)obj)->args );
}

GETTER(struct game_props, gameprops, UpdatePriority, INT32, This->priority)
GETTER(struct game_props, gameprops, ResolutionCap, INT32, This->cap)
HSTR_GETTER(struct game_props, gameprops, CommandLineArgs, This->args)

static HRESULT WINAPI gameprops_put_UpdatePriority( struct game_props *p, INT32 v )
{
    if (v < 0 || v > 2) return E_INVALIDARG;
    p->priority = v;
    return S_OK;
}

static HRESULT WINAPI gameprops_put_CommandLineArgs( struct game_props *p, HSTRING v )
{
    UINT32 n;
    const WCHAR *s = WindowsGetStringRawBuffer( v, &n );
    WCHAR *copy = malloc( (n + 1) * sizeof(WCHAR) );

    if (!copy) return E_OUTOFMEMORY;
    memcpy( copy, s, n * sizeof(WCHAR) );
    copy[n] = 0;
    free( p->args );
    p->args = copy;
    return S_OK;
}

static HRESULT WINAPI gameprops_put_ResolutionCap( struct game_props *p, INT32 v )
{
    if (v < 0 || v > 2) return E_INVALIDARG;
    p->cap = v;
    return S_OK;
}

static const void *game_props_vtbl[] =
{
    OBJ_METHODS, gameprops_get_UpdatePriority, gameprops_put_UpdatePriority, gameprops_get_CommandLineArgs,
    gameprops_put_CommandLineArgs, gameprops_get_ResolutionCap, gameprops_put_ResolutionCap
};

static HRESULT game_props_key( HSTRING family, WCHAR *key, size_t size )
{
    UINT32 n, i;
    const WCHAR *p = WindowsGetStringRawBuffer( family, &n );

    if (!n || n > 256) return E_INVALIDARG;
    for (i = 0; i < n; i++) if (p[i] == '\\' || !p[i]) return E_INVALIDARG;
    swprintf( key, size, L"%s\\Games\\%.*s", settings_key, (int)n, p );
    return S_OK;
}

static HRESULT WINAPI cu_QueryGameProps( struct factory *f, HSTRING family, IInspectable **out )
{
    struct game_props *p;
    WCHAR key[400];
    DWORD size;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = game_props_key( family, key, ARRAY_SIZE(key) ))) return hr;
    if (!(p = object_create( sizeof(*p), game_props_vtbl, &IID_ICustomizationGameProps,
                             L"Microsoft.GamePlatform.Services.CustomizationGameProps", game_props_destroy )))
        return E_OUTOFMEMORY;
    size = sizeof(p->priority);
    RegGetValueW( HKEY_CURRENT_USER, key, L"UpdatePriority", RRF_RT_REG_DWORD, NULL, &p->priority, &size );
    size = sizeof(p->cap);
    RegGetValueW( HKEY_CURRENT_USER, key, L"ResolutionCap", RRF_RT_REG_DWORD, NULL, &p->cap, &size );
    if (p->priority < 0 || p->priority > 2) p->priority = 0;
    if (p->cap < 0 || p->cap > 2) p->cap = 0;
    size = 0;
    if (!RegGetValueW( HKEY_CURRENT_USER, key, L"CommandLineArgs", RRF_RT_REG_SZ, NULL, NULL, &size ) && size &&
        (p->args = calloc( 1, size + sizeof(WCHAR) )))
    {
        if (RegGetValueW( HKEY_CURRENT_USER, key, L"CommandLineArgs", RRF_RT_REG_SZ, NULL, p->args, &size )) p->args[0] = 0;
    }
    TRACE( "Customization.QueryGameProps(%s) -> priority %d\n", debugstr_hstring( family ), p->priority );
    *out = (IInspectable *)p;
    return S_OK;
}

static HRESULT WINAPI cu_SetGameProps( struct factory *f, HSTRING family, IInspectable *props )
{
    INT32 priority = 0, cap = 0;
    HSTRING args = NULL;
    void **in = NULL, **vtbl;
    const WCHAR *s;
    WCHAR key[400];
    LSTATUS status;
    UINT32 n;
    HRESULT hr;

    if (FAILED(hr = game_props_key( family, key, ARRAY_SIZE(key) ))) return hr;
    if (!props || FAILED(IInspectable_QueryInterface( props, &IID_ICustomizationGameProps, (void **)&in ))) return E_INVALIDARG;
    vtbl = *(void ***)in;
    hr = ((get_i32_fn)vtbl[6])( in, &priority );
    if (SUCCEEDED(hr)) hr = ((get_i32_fn)vtbl[10])( in, &cap );
    if (SUCCEEDED(hr)) hr = ((get_hstr_fn)vtbl[8])( in, &args );
    IUnknown_Release( (IUnknown *)in );
    if (FAILED(hr) || priority < 0 || priority > 2 || cap < 0 || cap > 2)
    {
        WindowsDeleteString( args );
        return FAILED(hr) ? hr : E_INVALIDARG;
    }
    status = RegSetKeyValueW( HKEY_CURRENT_USER, key, L"UpdatePriority", REG_DWORD, &priority, sizeof(priority) );
    if (!status) status = RegSetKeyValueW( HKEY_CURRENT_USER, key, L"ResolutionCap", REG_DWORD, &cap, sizeof(cap) );
    s = WindowsGetStringRawBuffer( args, &n );
    if (!status) status = RegSetKeyValueW( HKEY_CURRENT_USER, key, L"CommandLineArgs", REG_SZ, s, (n + 1) * sizeof(WCHAR) );
    WindowsDeleteString( args );
    TRACE( "Customization.SetGameProps(%s) -> priority %d\n", debugstr_hstring( family ), priority );
    return HRESULT_FROM_WIN32( status );
}

/* SupportedFeatures: none of the optional Gaming Services features (streaming, remote install, ...) */
static HRESULT WINAPI sf_QueryFeatures( struct factory *f, UINT32 *out )
{
    if (!out) return E_POINTER;
    *out = 0;
    TRACE( "SupportedFeatures.Query*Features -> None\n" );
    return S_OK;
}

/* ProcessMonitor: no game is running */
static HRESULT WINAPI pm_QueryGamingServicesUsage( struct factory *f, UINT32 *out )
{
    if (!out) return E_POINTER;
    *out = 0;
    return S_OK;
}

static HRESULT WINAPI pm_EnumerateRunningGames( struct factory *f, IInspectable **out )
{
    static const GUID IID_IVectorView_RunningGame = {0xfe83e010,0x2ed8,0x5f52,{0x84,0x7a,0x4d,0x18,0xc5,0x33,0x70,0xac}};
    static const GUID IID_IIterable_RunningGame = {0x1be6bc9c,0x7b82,0x5ad7,{0x82,0xe3,0x2e,0x91,0x68,0xd7,0x32,0x69}};
    if (!out) return E_POINTER;
    *out = (IInspectable *)obj_view_create( &IID_IVectorView_RunningGame, &IID_IIterable_RunningGame, NULL,
        L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.RunningGame>" );
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* Package.Create(source, sourceType): a package object for the source, which answers for it once it is installed
 * or queued (install.c). (experiment: WINE_UWP_GP_CREATE_HR picks the HRESULT) */
static HRESULT WINAPI pk_Create( struct factory *f, HSTRING source, INT32 type, IInspectable **out )
{
    int k = kind_of( L"Microsoft.GamePlatform.Services.PackageInstance" );
    struct factory *package;
    char env[32] = "";

    if (!out) return E_POINTER;
    *out = NULL;
    if (GetEnvironmentVariableA( "WINE_UWP_GP_CREATE_HR", env, sizeof(env) ))
    {
        HRESULT hr = strtoul( env, NULL, 16 );
        TRACE( "Package.Create(%s, type %d) -> %#lx\n", debugstr_hstring( source ), type, hr );
        return hr;
    }
    if (!(package = calloc( 1, sizeof(*package) ))) return E_OUTOFMEMORY;
    package->vtbl = factory_vtbl[k];
    package->ref = 1;
    package->kind = k;
    package->heap = TRUE;
    lstrcpynW( package->source, WindowsGetStringRawBuffer( source, NULL ), ARRAY_SIZE(package->source) );
    TRACE( "Package.Create(%s, type %d) -> a package object (traced)\n", debugstr_w( package->source ), type );
    *out = (IInspectable *)package;
    return S_OK;
}

/* ---- Store.QueryProductDetails: what Gaming Services knows of a Store product before it is installed ----
 * Gaming Services reads it from the Store catalogue; so does this, from the public display catalogue
 * (displaycatalog.mp.microsoft.com, no account needed). */

static SRWLOCK products_lock = SRWLOCK_INIT;
static struct product *products;
static unsigned products_count;

static void user_market( WCHAR *market, int size )
{
    GEOID geo = GetUserGeoID( GEOCLASS_NATION );
    lstrcpynW( market, L"US", size );
    if (geo != GEOID_NOT_AVAILABLE && !GetGeoInfoW( geo, GEO_ISO2, market, size, 0 )) lstrcpynW( market, L"US", size );
}

static BOOL json_true( const struct json *value )
{
    return value->type == JSON_BOOL && value->boolean;
}

static BOOL starts_with( const char *str, const char *prefix )
{
    return !strncmp( str, prefix, strlen( prefix ) );
}

void query_product( const WCHAR *id, struct product *d )
{
    const struct json *prod, *skus, *pkgs, *pk, *deps;
    WCHAR market[8], path[512], cid[64];
    struct json *root = NULL;
    const char *kind, *fmt;
    struct product *grown;
    char *body = NULL;
    DWORD status = 0;
    unsigned i, k, j;
    BOOL desktop;

    memset( d, 0, sizeof(*d) );
    d->type = 2;
    d->uwp = TRUE;
    AcquireSRWLockShared( &products_lock );
    for (i = 0; i < products_count; i++) if (!wcscmp( products[i].id, id )) *d = products[i];
    ReleaseSRWLockShared( &products_lock );
    if (d->found) return;

    user_market( market, ARRAY_SIZE(market) );
    swprintf( path, ARRAY_SIZE(path), L"/v7.0/products?bigIds=%s&market=%s&languages=en-US", id, market );
    if (SUCCEEDED(appx_store_get( L"displaycatalog.mp.microsoft.com", path, &body, &status )) && body)
        root = json_parse( body, strlen( body ) );
    appx_free( body );
    prod = json_at( json_get( root, "Products" ), 0 );
    if (status != 200 || prod->type != JSON_OBJECT)
    {
        TRACE( "Store.QueryProductDetails(%s): catalogue HTTP %lu, no product\n", debugstr_w( id ), status );
        json_free( root );
        return;
    }
    lstrcpynW( d->id, id, ARRAY_SIZE(d->id) );
    lstrcpynW( d->store_id, id, ARRAY_SIZE(d->store_id) );
    wide_copy( d->family, ARRAY_SIZE(d->family), json_str( json_get( json_get( prod, "Properties" ), "PackageFamilyName" ) ) );
    kind = json_str( json_get( prod, "ProductKind" ) );
    d->type = !strcmp( kind, "Game" ) ? 0 : !strcmp( kind, "Durable" ) ? 1 : 2;
    if (json_true( json_get( json_get( prod, "Properties" ), "IsBundle" ) ) || !strcmp( kind, "Bundle" )) d->type = 3;
    /* the first package for the desktop, of the first SKU that has one */
    skus = json_get( prod, "DisplaySkuAvailabilities" );
    for (i = 0; i < json_count( skus ) && !d->found; i++)
    {
        const struct json *props = json_get( json_get( json_at( skus, i ), "Sku" ), "Properties" );
        pkgs = json_get( props, "Packages" );
        if (!d->wu_category[0])
            wide_copy( d->wu_category, ARRAY_SIZE(d->wu_category), json_str( json_get( json_get( props, "FulfillmentData" ), "WuCategoryId" ) ) );
        for (k = 0; k < json_count( pkgs ); k++)
        {
            pk = json_at( pkgs, k );
            deps = json_get( pk, "PlatformDependencies" );
            desktop = FALSE;
            for (j = 0; j < json_count( deps ); j++)
                desktop |= !strcmp( json_str( json_get( json_at( deps, j ), "PlatformName" ) ), "Windows.Desktop" );
            if (!desktop && json_count( pkgs ) > 1) continue;
            cid[0] = '{';
            wide_copy( cid + 1, ARRAY_SIZE(cid) - 2, json_str( json_get( pk, "ContentId" ) ) );
            wcscat( cid, L"}" );
            CLSIDFromString( cid, &d->content_id );
            if (!d->family[0]) wide_copy( d->family, ARRAY_SIZE(d->family), json_str( json_get( pk, "PackageFamilyName" ) ) );
            d->size = (UINT64)json_get( pk, "MaxInstallSizeInBytes" )->number;
            if (!d->size) d->size = (UINT64)json_get( pk, "MaxDownloadSizeInBytes" )->number;
            fmt = json_str( json_get( pk, "PackageFormat" ) );
            d->uwp = !strstr( fmt, "Xvc" ) && !strstr( fmt, "XVC" );
            /* EAppx, EAppxBundle, EMsix, EMsixBundle: DRM-encrypted, the keys come with the licence */
            d->encrypted = d->uwp && (starts_with( fmt, "EAppx" ) || starts_with( fmt, "EMsix" ));
            d->found = TRUE;
            break;
        }
    }
    if (!d->found)
    {
        d->found = TRUE;
        TRACE( "Store.QueryProductDetails(%s): no package for the desktop\n", debugstr_w( id ) );
    }
    TRACE( "Store.QueryProductDetails(%s) -> %s, family %s, %I64u bytes, %s%s\n", debugstr_w( id ), kind,
           debugstr_w( d->family ), d->size, d->uwp ? "UWP" : "not UWP", d->encrypted ? ", encrypted" : "" );
    json_free( root );
    AcquireSRWLockExclusive( &products_lock );
    if ((grown = realloc( products, (products_count + 1) * sizeof(*products) )))
    {
        products = grown;
        products[products_count++] = *d;
    }
    ReleaseSRWLockExclusive( &products_lock );
}

static const GUID IID_IStoreProductDetails = {0x92cac4ca,0x4cef,0x5585,{0xac,0xbb,0x59,0x7c,0x52,0xc4,0xf3,0xc9}};

struct product_details
{
    struct object obj;
    struct product d;
};

GETTER(struct product_details, spd, ContentId, GUID, This->d.content_id)
HSTR_GETTER(struct product_details, spd, FamilyName, This->d.family)
HSTR_GETTER(struct product_details, spd, StoreId, This->d.store_id)
GETTER(struct product_details, spd, TotalSize, UINT64, This->d.size)
GETTER(struct product_details, spd, Type, INT32, This->d.type)
GETTER(struct product_details, spd, RequiresSystemDrive, boolean, FALSE)
GETTER(struct product_details, spd, IsUwp, boolean, This->d.uwp)

static const void *product_details_vtbl[] =
{
    OBJ_METHODS, spd_get_ContentId, spd_get_FamilyName, spd_get_StoreId, spd_get_TotalSize, spd_get_Type,
    spd_get_RequiresSystemDrive, spd_get_IsUwp
};

static HRESULT WINAPI store_QueryProductDetails( struct factory *f, HSTRING id, IInspectable **out )
{
    struct product_details *details;
    struct product d;

    if (!out) return E_POINTER;
    *out = NULL;
    query_product( WindowsGetStringRawBuffer( id, NULL ), &d );
    if (!d.store_id[0]) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (!(details = object_create( sizeof(*details), product_details_vtbl, &IID_IStoreProductDetails,
                                   L"Microsoft.GamePlatform.Services.StoreProductDetails", NULL )))
        return E_OUTOFMEMORY;
    details->d = d;
    *out = (IInspectable *)details;
    return S_OK;
}

static HRESULT WINAPI store_QueryProductType( struct factory *f, HSTRING id, INT32 *out )
{
    struct product d;

    if (!out) return E_POINTER;
    query_product( WindowsGetStringRawBuffer( id, NULL ), &d );
    if (!d.store_id[0]) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    *out = d.type;
    return S_OK;
}

/* No products are blocked for migration on this local Gaming Services stand-in. Returning the empty
 * list lets the Xbox product page continue; the generic no-service HRESULT turns its content panel
 * into "Content failed to load." */
HRESULT WINAPI store_EnumerateMigrationBlockedProducts( struct factory *f, IInspectable **out )
{
    if (!out) return E_POINTER;
    TRACE( "Store.EnumerateMigrationBlockedProducts -> 0\n" );
    return (*out = (IInspectable *)hstring_view_create()) ? S_OK : E_OUTOFMEMORY;
}

/* ---- ContentAccess.GetRawContentAccessDataAsync: which games the user may play (owned, Game Pass) ----
 * Gaming Services fetches it from the Content Access Service; so does this, with the user's Xbox Live token from
 * wineuwp_xboxauth.dll: contentaccess.exp.xboxservices.com/all/v1, the endpoint the app itself uses when it does not
 * use Gaming Services as its source. The data is handed over as it arrives. */

static const GUID IID_IRawContentAccessDataResult = {0xd84407df,0xb7e6,0x58a2,{0xa1,0xe8,0x2d,0xb3,0x66,0xc4,0xff,0x11}};
static const GUID IID_AsyncOp_RawContentAccessDataResult = {0x25ece3f5,0xf64b,0x5328,{0x93,0xf4,0xfa,0xc2,0xfd,0xf3,0x27,0x3b}};

typedef HRESULT (WINAPI *xboxauth_get_fn)( const char *, const char *, const char *, unsigned char **, UINT32 *, DWORD * );

struct content_access
{
    struct object obj;
    WCHAR cv[32];
    unsigned char *data;
    UINT32 size;
};

static void content_access_destroy( struct object *obj )
{
    free( ((struct content_access *)obj)->data );
}

HSTR_GETTER(struct content_access, ca, Cv, This->cv)
GETTER(struct content_access, ca, Size, UINT64, This->size)

/* "out u1[] buffer" is a FillArray (the parameter is not by reference): the caller passes its buffer */
static HRESULT WINAPI ca_GetData( struct content_access *This, UINT32 len, BYTE *buf, UINT64 size )
{
    UINT32 n;

    if (len && !buf) return E_POINTER;
    n = min( min( (UINT64)This->size, (UINT64)len ), size );
    memcpy( buf, This->data, n );
    TRACE( "RawContentAccessDataResult.GetData(%u, %I64u) -> %u bytes\n", len, size, n );
    return S_OK;
}

static const void *content_access_vtbl[] = { OBJ_METHODS, ca_get_Cv, ca_get_Size, ca_GetData };

static SRWLOCK content_access_lock = SRWLOCK_INIT;
static unsigned char *content_access_blob;     /* the last content access data the app fetched (protobuf) */
static UINT32 content_access_len;

/* whether the subscription's catalogue names the bytes */
BOOL content_access_has( const char *needle, size_t len )
{
    BOOL found = FALSE;
    UINT32 i;

    AcquireSRWLockShared( &content_access_lock );
    for (i = 0; !found && len && i + len <= content_access_len; i++)
        found = !memcmp( content_access_blob + i, needle, len );
    ReleaseSRWLockShared( &content_access_lock );
    return found;
}

static HRESULT content_access_work( void *ctx, IUnknown **result )
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    HMODULE xboxauth = LoadLibraryW( L"wineuwp_xboxauth.dll" );
    xboxauth_get_fn get = xboxauth ? (void *)GetProcAddress( xboxauth, "XboxAuthHttpGet" ) : NULL;
    char market[8], url[256], cv[64], extra[96];
    struct content_access *r;
    unsigned char *body = NULL, *copy;
    WCHAR wmarket[8];
    UINT32 len = 0;
    DWORD status = 0;
    HRESULT hr;
    int i;

    if (!get)
    {
        WARN( "ContentAccess: no wineuwp_xboxauth.dll\n" );
        return GP_E_NO_SERVICE;
    }
    user_market( wmarket, ARRAY_SIZE(wmarket) );
    WideCharToMultiByte( CP_UTF8, 0, wmarket, -1, market, sizeof(market), NULL, NULL );
    snprintf( url, sizeof(url), "https://contentaccess.exp.xboxservices.com/all/v1?market=%s&offering=XGPUPCAPP", market );
    /* a correlation vector: 16 base64 characters and a counter */
    for (i = 0; i < 16; i++) cv[i] = b64[(GetTickCount() * 2654435761u + i * 40503u + GetCurrentThreadId() * 97u) % 64];
    memcpy( cv + 16, ".0", 3 );
    snprintf( extra, sizeof(extra), "MS-CV: %s", cv );
    hr = get( url, extra, "1", &body, &len, &status );
    if (FAILED(hr) || status != 200)
    {
        TRACE( "ContentAccess.GetRawContentAccessDataAsync: hr %#lx, HTTP %lu\n", hr, status );
        if (body) CoTaskMemFree( body );
        return FAILED(hr) ? hr : E_FAIL;
    }
    if (!(r = object_create( sizeof(*r), content_access_vtbl, &IID_IRawContentAccessDataResult,
                             L"Microsoft.GamePlatform.Services.RawContentAccessDataResult", content_access_destroy )) ||
        !(r->data = malloc( len + 1 )) || !(copy = malloc( len + 1 )))
    {
        if (r) obj_Release( &r->obj );
        CoTaskMemFree( body );
        return E_OUTOFMEMORY;
    }
    memcpy( r->data, body, len );
    r->size = len;
    /* kept: which products the subscription includes, for install features that need one (gdk.c) */
    memcpy( copy, body, len );
    AcquireSRWLockExclusive( &content_access_lock );
    free( content_access_blob );
    content_access_blob = copy;
    content_access_len = len;
    ReleaseSRWLockExclusive( &content_access_lock );
    wide_copy( r->cv, ARRAY_SIZE(r->cv), cv );
    CoTaskMemFree( body );
    TRACE( "ContentAccess.GetRawContentAccessDataAsync -> %u bytes\n", len );
    *result = (IUnknown *)r;
    return S_OK;
}

static HRESULT WINAPI ca_GetRawContentAccessDataAsync( struct factory *f, IInspectable **out )
{
    if (!out) return E_POINTER;
    return (*out = async_start( &IID_AsyncOp_RawContentAccessDataResult, content_access_work, NULL )) ? S_OK : E_OUTOFMEMORY;
}

/* ---- the table ---- */

#define GPS L"Microsoft.GamePlatform.Services."
static const struct { const WCHAR *cls; int slot; const void *fn; } specials[] =
{
    { GPS L"PackageUpdate", 6, pu_QueryUpdate },
    { GPS L"PackageUpdate", 7, pu_QueryUpdateAsync },
    { GPS L"PackageUpdate", 10, pu_QueryUpdatesAsync },
    { GPS L"PackageUpdate", 11, pu_QueryUpdatesAsyncFiltered },
    { GPS L"PlatformStatus", 6, ps_get_ApiStatus },
    { GPS L"Storage", 6, st_QueryDeviceDetails },
    { GPS L"Storage", 7, st_EnumerateDevices },
    { GPS L"Storage", 8, st_EnumerateContent },
    { GPS L"Storage", 14, st_QueryDefaultDevice },
    { GPS L"Storage", 15, st_SetDefaultDevice },
    { GPS L"GameFolder", 6, gf_QueryForDevice },
    { GPS L"GameFolder", 7, gf_SetForDevice },
    { GPS L"Customization", 6, cu_QueryGlobalProps },
    { GPS L"Customization", 7, cu_SetGlobalProps },
    { GPS L"Customization", 8, cu_QueryGameProps },
    { GPS L"Customization", 9, cu_SetGameProps },
    { GPS L"SupportedFeatures", 6, sf_QueryFeatures },
    { GPS L"SupportedFeatures", 7, sf_QueryFeatures },
    { GPS L"ProcessMonitor", 6, pm_QueryGamingServicesUsage },
    { GPS L"ProcessMonitor", 7, pm_EnumerateRunningGames },
    { GPS L"Package", 6, pk_Create },
    { GPS L"Store", 7, store_EnumerateMigrationBlockedProducts },
    { GPS L"Store", 8, store_QueryProductDetails },
    { GPS L"Store", 9, store_QueryProductType },
    { GPS L"ContentAccess", 6, ca_GetRawContentAccessDataAsync },
    { GPS L"PackageRequest", 6, rq_CreateForSource },
    { GPS L"PackageRequest", 7, rq_CreateForSourceDevice },
    { GPS L"PackageRequest", 8, rq_CreateForStoreId },
    { GPS L"PackageQueue", 6, pq_SubmitRequestAsync },
    { GPS L"PackageQueue", 8, pq_PauseInstance },
    { GPS L"PackageQueue", 9, pq_ResumeInstance },
    { GPS L"PackageQueue", 10, pq_CancelInstance },
    { GPS L"PackageQueue", 11, pq_PauseId },
    { GPS L"PackageQueue", 12, pq_ResumeId },
    { GPS L"PackageQueue", 13, pq_CancelId },
    { GPS L"PackageQueue", 14, pq_PauseAll },
    { GPS L"PackageQueue", 15, pq_ResumeAll },
    { GPS L"PackageQueue", 16, pq_CancelAll },
    { GPS L"PackageQueue", 17, pq_Enumerate },
    { GPS L"PackageQueue", 18, pq_EnumerateId },
    { GPS L"PackageQueue", 19, pq_QueryInstallationInfo },
    { GPS L"PackageQueue", 20, pq_QueryInstallationSummary },
    { GPS L"PackageQueue", 23, pq_CreateViewInstance },
    { GPS L"PackageQueue", 24, pq_CreateViewId },
    { GPS L"PackageQueue", 25, pq_add_InstallationChanged },
    { GPS L"PackageQueue", 26, pq_remove_InstallationChanged },
    { GPS L"PackageQueue", 27, pq_add_QueueChanged },
    { GPS L"PackageQueue", 28, pq_remove_QueueChanged },
    { GPS L"PackageQueue", 29, pq_add_ItemAdded },
    { GPS L"PackageQueue", 30, pq_remove_ItemAdded },
    { GPS L"PackageQueue", 31, pq_add_ItemRemoved },
    { GPS L"PackageQueue", 32, pq_remove_ItemRemoved },
    { GPS L"PackageInstance", 6, pi_QueryInstallPath },
    { GPS L"PackageInstance", 7, pi_QueryInstanceId },
    { GPS L"PackageInstance", 8, pi_QueryIdentity },
    { GPS L"PackageInstance", 9, pi_QuerySize },
    { GPS L"PackageInstance", 10, pi_QuerySize },
    { GPS L"PackageInstance", 11, pi_QueryStorageCategory },
    { GPS L"PackageInstance", 12, pi_QueryHeader },
    { GPS L"PackageInstance", 13, pi_QueryDetails },
    { GPS L"PackageInstance", 15, pi_QueryFeatures },
    { GPS L"PackageInstance", 16, pi_QueryRecipes },
    { GPS L"PackageInstance", 24, pi_IsFlatStreamed },
    { GPS L"PackageInstance", 25, pi_IsLaunchable },
    { GPS L"PackageInstance", 27, pi_GetRunningProcessId },
    { GPS L"PackageInstance", 28, pi_Activate },
    { GPS L"PackageInstance", 29, pi_ActivateAumid },
    { GPS L"PackageInstance", 37, pi_add_InstallationChanged },
    { GPS L"PackageInstance", 38, pi_remove_InstallationChanged },
    { GPS L"PackagePreview", 6, pv_Create },
};
#undef GPS

static BOOL WINAPI init_factories( INIT_ONCE *once, void *param, void **context )
{
    unsigned i;
    int k;

    for (k = 0; k < K_COUNT; k++)
    {
        memcpy( factory_vtbl[k], thunks, sizeof(thunks) );
        factory_vtbl[k][0] = factory_QueryInterface;
        factory_vtbl[k][1] = factory_AddRef;
        factory_vtbl[k][2] = factory_Release;
        factory_vtbl[k][3] = factory_GetIids;
        factory_vtbl[k][4] = factory_GetRuntimeClassName;
        factory_vtbl[k][5] = factory_GetTrustLevel;
        factories[k].vtbl = factory_vtbl[k];
        factories[k].ref = 1;
        factories[k].kind = k;
    }
    for (i = 0; i < ARRAY_SIZE(specials); i++)
        if ((k = kind_of( specials[i].cls )) >= 0) factory_vtbl[k][specials[i].slot] = specials[i].fn;
    return TRUE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );
    int k;

    InitOnceExecuteOnce( &once, init_factories, NULL, NULL );
    if ((k = kind_of( name )) >= 0)
    {
        TRACE( "%s: factory\n", class_name( k ) );
        InterlockedIncrement( &factories[k].ref );
        *factory = (IActivationFactory *)&factories[k];
        return S_OK;
    }
    TRACE( "%s: no such class\n", debugstr_w( name ) );
    *factory = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow( void )
{
    return S_FALSE;
}

/*
 * Microsoft.GamePlatform.Services: installing Store packages
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
 * PackageRequest, PackageQueue and the installed Package.
 *
 * Gaming Services installs a game the user owns from the Store's delivery service; so does this:
 *   1. the product's Windows Update category, from the public display catalogue;
 *   2. the package's download address from the Store's delivery service (appxinstall's appx_store_*), or for a
 *      GDK title its .msixvc from the Xbox CDN (packagespc.xboxlive.com, signed by wineuwp_xboxauth.dll);
 *   3. appxinstall streams the package from there into a stage directory, with its frameworks from the Windows
 *      files, decrypting it with the licence's content keys when it is encrypted, and reports progress;
 *   4. the queue reports it as Gaming Services would (ItemAdded, InstallationChanged with progress,
 *      ItemRemoved), Storage.EnumerateContent lists it, and Package answers for it: install path, identity,
 *      launchable, and Activate starts it through its stage directory's wineuwp-launch.exe.
 *
 * Where things live comes from the environment run.sh sets, all Unix paths: WINE_UWP_GAMES_DATA (the data
 * directory with stage/), WINE_UWP_WINDOWS_TREE (the harvested Windows files the frameworks come from),
 * WINE_UWP_ROOT (launch-defaults.env) and WINE_UWP_D3D_RUNTIME.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gameplatform);

static const GUID IID_IPackageRequest = {0x25198e18,0xd58c,0x5ee2,{0x9c,0x59,0x62,0xf3,0xd4,0x8d,0x84,0x0f}};
static const GUID IID_IPackageRequestSize = {0x929c3222,0x99f2,0x512b,{0x9a,0x55,0xf4,0x91,0x3b,0x1e,0xf8,0x7f}};
static const GUID IID_IPackageInstallationInfo = {0xa07926f8,0xe675,0x51e7,{0x84,0xca,0xce,0x0b,0xd8,0xd3,0xc2,0xf3}};
static const GUID IID_IPackageInstallationSummary = {0xdff0ab9f,0x5b87,0x5d0e,{0x94,0xa4,0x57,0xe2,0x68,0x86,0xa6,0xd9}};
static const GUID IID_IInstallationChangedEventArgs = {0xed49332e,0x122c,0x55ba,{0x9e,0x27,0x17,0x61,0xa9,0xa0,0xc0,0xb7}};
static const GUID IID_IPackageQueueItemEventArgs = {0xfb6c1626,0xe830,0x5f21,{0xa9,0xfb,0xed,0xec,0x84,0x13,0x78,0x07}};
static const GUID IID_IPackageQueueView = {0x7c061aed,0x9cdf,0x54b0,{0xbf,0xef,0x43,0x3f,0xed,0x4c,0x89,0x2c}};
/* IAsyncOperation<Guid>, from piid.py --signature "pinterface({9fc2b0bb-...};g16)" */
static const GUID IID_IAsyncOperation_Guid = {0x6607bc41,0x294b,0x5975,{0x9c,0x3f,0x4b,0x49,0x83,0x6d,0x09,0x16}};
static const GUID IID_IIterable_HString = {0xe2fcc7c1,0x3bfc,0x5a0b,{0xb2,0xb0,0x72,0xe7,0x69,0xd1,0xcb,0x7e}};

enum { ST_IDLE = 0, ST_PAUSED = 1, ST_STREAMING = 2, ST_COMPLETED = 3, ST_FAILED = 4, ST_CANCELED = 5, ST_WAITING = 6 };

struct install_metric { UINT64 streamed, total; };
struct install_defrag_metric { UINT64 streamed, total; INT32 state; };
struct install_progress
{
    struct install_metric package, install, launch, network, optical;
    struct install_defrag_metric defrag;
    UINT32 data_rate, disc;
};
struct install_fast_start { INT32 state; UINT32 buffer_seconds; UINT64 miss_bytes; UINT32 miss_count, miss_bytes_recent, miss_count_recent; };

/* what an install's snapshots (InstallationInfo, InstallationSummary, the event args) show */
struct install_snap
{
    GUID id;
    WCHAR instance[40], store_id[64], family[256];
    INT32 state;
    HRESULT error;
    UINT64 total, done;
    UINT32 rate;                    /* bytes a second, over the last few seconds */
};

struct install
{
    struct install_snap s;
    WCHAR stage[MAX_PATH];          /* the stage name, then once installed its directory */
    BOOL cancel;
    BOOL features_chosen;           /* the app chose features; otherwise the default recipe */
    struct strlist features;
    BOOL paused;
    BOOL running;                   /* the installer has started */
    volatile LONG pause_flag, cancel_flag;  /* what the installer polls */
    BOOL uwp;
};

static SRWLOCK installs_lock = SRWLOCK_INIT;
static struct install **installs;   /* the queue: in progress, and finished ones until they are removed */
static unsigned installs_count;

/* event handlers registered on PackageQueue: InstallationChanged, QueueChanged, ItemAdded, ItemRemoved */
enum { EV_INSTALLATION_CHANGED, EV_QUEUE_CHANGED, EV_ITEM_ADDED, EV_ITEM_REMOVED, EV_COUNT };
struct handler { LONGLONG token; IUnknown *handler; };
static SRWLOCK events_lock = SRWLOCK_INIT;
static struct handler *event_handlers[EV_COUNT];
static unsigned event_counts[EV_COUNT];
static LONGLONG next_event_token = 100;

/* IPackage.InstallationChanged: handlers registered on a package object, by the package's source */
struct package_handler { LONGLONG token; WCHAR key[64]; IUnknown *handler; IInspectable *sender; };
static struct package_handler *package_handlers;
static unsigned package_handlers_count;

static void stage_name_for_family( const WCHAR *family, WCHAR *name, size_t size )
{
    const WCHAR *end = wcschr( family, '_' ), *start;
    size_t len;

    if (!end) end = family + wcslen( family );
    for (start = end; start > family && start[-1] != '.'; start--);
    len = min( (size_t)(end - start), size - 1 );
    memcpy( name, start, len * sizeof(WCHAR) );
    name[len] = 0;
    for (; *name; name++) *name = towlower( *name );
}

static void env_w( const WCHAR *name, WCHAR *buf, size_t size )
{
    DWORD n = GetEnvironmentVariableW( name, buf, size );
    if (!n || n >= size) buf[0] = 0;
}

/* the data directory games are installed into (Unix path) */
void games_data_unix( WCHAR *buf, size_t size )
{
    env_w( L"WINE_UWP_GAMES_DATA", buf, size );
    if (!buf[0])
    {
        env_w( L"UWP_BIN", buf, size );
        if (buf[0]) wcsncat( buf, L"/xbox-games", size - wcslen( buf ) - 1 );
    }
}

void unix_to_windows( WCHAR *buf, size_t size, const WCHAR *unix_path )
{
    WCHAR *p;
    swprintf( buf, size, L"Z:%s", unix_path );
    for (p = buf; *p; p++) if (*p == '/') *p = '\\';
}

/* the data directory's stage\ as a Windows path; empty if it is not known */
static void stage_root( WCHAR *buf, size_t size )
{
    WCHAR data[MAX_PATH];
    games_data_unix( data, ARRAY_SIZE(data) );
    if (!data[0])
    {
        buf[0] = 0;
        return;
    }
    unix_to_windows( buf, size, data );
    wcsncat( buf, L"\\stage", size - wcslen( buf ) - 1 );
}

void data_game_folder( WCHAR *buf, size_t size )
{
    stage_root( buf, size );
    if (!buf[0]) lstrcpynW( buf, L"Z:\\stage", size );
}

/* A package instance id is a GUID string (the app parses it as one). Gaming Services makes one per installation;
 * this derives it from the package family name, so it is the same for the queued and the installed package. */
static void instance_for( const WCHAR *family, WCHAR *buf )
{
    UINT64 h1 = 1469598103934665603ull, h2 = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;
    const WCHAR *p;

    for (p = family; *p; p++)
    {
        WCHAR l = towlower( *p );
        h1 = (h1 ^ l) * 1099511628211ull;
        h2 = (h2 ^ (l * 31u)) * 0x100000001b3ull + 0x632be59bd9b4e019ull;
    }
    swprintf( buf, 40, L"{%08X-%04X-%04X-%04X-%012I64X}", (unsigned)(h1 >> 32), (unsigned)(h1 >> 16) & 0xffff,
              ((unsigned)h1 & 0x0fff) | 0x5000, ((unsigned)(h2 >> 48) & 0x3fff) | 0x8000, h2 & 0xffffffffffffull );
}

/* ---- the installed games: stage directories with a .wine-uwp-xbox marker ---- */

void installed_game_clear( struct installed_game *game )
{
    strlist_free( &game->features );
}

/* reads a small text file into buf; FALSE if it is not there */
static BOOL read_small_file( const WCHAR *path, char *buf, DWORD size )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    DWORD got = 0;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile( file, buf, size - 1, &got, NULL );
    CloseHandle( file );
    buf[got] = 0;
    return TRUE;
}

static BOOL read_marker( const WCHAR *dir, struct installed_game *g )
{
    char buf[4096], *line, *next, *eq;
    WCHAR path[MAX_PATH];

    memset( g, 0, sizeof(*g) );
    g->uwp = TRUE;
    swprintf( path, ARRAY_SIZE(path), L"%s\\.wine-uwp-xbox", dir );
    if (!read_small_file( path, buf, sizeof(buf) )) return FALSE;
    for (line = buf; line && *line; line = next)
    {
        next = line + strcspn( line, "\r\n" );
        if (*next) *next++ = 0;
        while (*next == '\r' || *next == '\n') next++;
        if (!(eq = strchr( line, '=' ))) continue;
        *eq++ = 0;
        if (!strcmp( line, "store_id" )) wide_copy( g->store_id, ARRAY_SIZE(g->store_id), eq );
        else if (!strcmp( line, "family" )) wide_copy( g->family, ARRAY_SIZE(g->family), eq );
        else if (!strcmp( line, "full_name" )) wide_copy( g->full_name, ARRAY_SIZE(g->full_name), eq );
        else if (!strcmp( line, "title" )) wide_copy( g->title, ARRAY_SIZE(g->title), eq );
        else if (!strcmp( line, "aumid" )) wide_copy( g->aumid, ARRAY_SIZE(g->aumid), eq );
        else if (!strcmp( line, "size" )) g->size = _strtoui64( eq, NULL, 10 );
        else if (!strcmp( line, "uwp" )) g->uwp = strcmp( eq, "false" ) && strcmp( eq, "0" );
        else if (!strcmp( line, "features" ))
        {
            g->features_known = TRUE;
            strlist_split( &g->features, eq, ';' );
        }
    }
    lstrcpynW( g->stage_dir, dir, ARRAY_SIZE(g->stage_dir) );
    instance_for( g->family, g->instance );
    if (g->family[0]) return TRUE;
    installed_game_clear( g );
    return FALSE;
}

static BOOL installed_game_matches( const struct installed_game *g, const WCHAR *key )
{
    return !wcsicmp( g->instance, key ) || !wcsicmp( g->store_id, key ) || !wcsicmp( g->full_name, key ) ||
           !wcsicmp( g->family, key );
}

/* calls found for each installed game until it returns TRUE */
static BOOL enum_installed( BOOL (*found)( struct installed_game *g, void *ctx ), void *ctx )
{
    WCHAR stage[MAX_PATH], pattern[MAX_PATH], dir[MAX_PATH];
    struct installed_game g;
    WIN32_FIND_DATAW fd;
    BOOL done = FALSE;
    HANDLE h;

    stage_root( stage, ARRAY_SIZE(stage) );
    if (!stage[0]) return FALSE;
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", stage );
    if ((h = FindFirstFileW( pattern, &fd )) == INVALID_HANDLE_VALUE) return FALSE;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        swprintf( dir, ARRAY_SIZE(dir), L"%s\\%s", stage, fd.cFileName );
        if (!read_marker( dir, &g )) continue;
        done = found( &g, ctx );
        if (!done) installed_game_clear( &g );
    } while (!done && FindNextFileW( h, &fd ));
    FindClose( h );
    return done;
}

struct find_ctx { const WCHAR *key; struct installed_game *out; };

static BOOL find_one( struct installed_game *g, void *param )
{
    struct find_ctx *ctx = param;
    if (!installed_game_matches( g, ctx->key )) return FALSE;
    *ctx->out = *g;
    return TRUE;
}

/* fills game (clear it with installed_game_clear) */
BOOL find_installed( const WCHAR *key, struct installed_game *game )
{
    struct find_ctx ctx = { key, game };
    return enum_installed( find_one, &ctx );
}

static BOOL add_content( struct installed_game *g, void *view )
{
    hstring_view_add( view, g->instance );
    installed_game_clear( g );
    return FALSE;
}

/* Storage.EnumerateContent: the games installed through the queue */
HRESULT WINAPI st_EnumerateContent( struct factory *f, const GUID *device, IInspectable **out )
{
    static const GUID DEVICE_ID = {0x7e1d3a52,0x5c1b,0x4f3e,{0x9a,0x52,0x77,0x69,0x6e,0x65,0x75,0x77}};
    static const GUID DATA_DEVICE_ID = {0x7e1d3a53,0x5c1b,0x4f3e,{0x9a,0x52,0x77,0x69,0x6e,0x65,0x75,0x77}};
    struct hstring_view *view;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || (!IsEqualGUID( device, &DEVICE_ID ) && !IsEqualGUID( device, &DATA_DEVICE_ID ))) return E_INVALIDARG;
    if (!(view = hstring_view_create())) return E_OUTOFMEMORY;
    if (IsEqualGUID( device, &DATA_DEVICE_ID )) enum_installed( add_content, view );
    TRACE( "Storage.EnumerateContent -> %u\n", hstring_view_size( view ) );
    *out = (IInspectable *)view;
    return S_OK;
}

/* ---- PackageInstallationInfo / Summary: snapshots of an install ---- */

static struct install_progress progress_of( const struct install_snap *in )
{
    struct install_progress p = {{0}};
    p.package.total = p.install.total = p.network.total = p.launch.total = in->total;
    p.package.streamed = p.install.streamed = p.network.streamed = in->done;
    /* launchable once it is all there */
    p.launch.streamed = in->state == ST_COMPLETED ? in->total : 0;
    p.data_rate = in->state == ST_STREAMING ? in->rate : 0;
    return p;
}

struct snap_object
{
    struct object obj;
    struct install_snap in;
};

static const GUID guid_null;

GETTER(struct snap_object, info, InstallationId, GUID, This->in.id)
HSTR_GETTER(struct snap_object, info, SourceInstanceId, This->in.instance)
HSTR_GETTER(struct snap_object, info, TargetInstanceId, This->in.instance)
HSTR_GETTER(struct snap_object, info, StoreId, This->in.store_id)
HSTR_GETTER(struct snap_object, info, BundleStoreId, L"")
GETTER(struct snap_object, info, Type, INT32, 0 /* Install */)
GETTER(struct snap_object, info, State, INT32, This->in.state)
GETTER(struct snap_object, info, WaitReason, INT32, 0)
GETTER(struct snap_object, info, Sources, UINT32, 2 /* Network */)
GETTER(struct snap_object, info, Launchable, boolean, This->in.state == ST_COMPLETED)
GETTER(struct snap_object, info, LaunchRegionsStreaming, boolean, FALSE)
GETTER(struct snap_object, info, EnhancedStreaming, boolean, FALSE)
GETTER(struct snap_object, info, Error, HRESULT, This->in.error)
GETTER(struct snap_object, info, DataRate, UINT32, This->in.state == ST_STREAMING ? This->in.rate : 0)
GETTER(struct snap_object, info, StorageSpaceRequired, UINT64, This->in.total)
GETTER(struct snap_object, info, PersistentLocalStorageSize, UINT64, 0)
GETTER(struct snap_object, info, PersistentLocalStorageCrdId, UINT32, 0)
GETTER(struct snap_object, info, BuildId, GUID, guid_null)
GETTER(struct snap_object, info, PlanId, GUID, guid_null)
GETTER(struct snap_object, info, Progress, struct install_progress, progress_of( &This->in ))
GETTER(struct snap_object, info, AvailableDate, INT64, 0)
GETTER(struct snap_object, info, RemediationAction, INT32, 0)

static HRESULT WINAPI info_get_FastStart( struct snap_object *This, struct install_fast_start *v )
{
    if (!v) return E_POINTER;
    memset( v, 0, sizeof(*v) );
    return S_OK;
}

static const void *installation_info_vtbl[] =
{
    OBJ_METHODS, info_get_InstallationId, info_get_SourceInstanceId, info_get_TargetInstanceId, info_get_StoreId,
    info_get_BundleStoreId, info_get_Type, info_get_State, info_get_WaitReason, info_get_Sources, info_get_FastStart,
    info_get_Launchable, info_get_LaunchRegionsStreaming, info_get_EnhancedStreaming, info_get_Error, info_get_DataRate,
    info_get_StorageSpaceRequired, info_get_PersistentLocalStorageSize, info_get_PersistentLocalStorageCrdId,
    info_get_BuildId, info_get_PlanId, info_get_Progress, info_get_AvailableDate, info_get_RemediationAction,
    info_get_Error /* ExtendedError */
};

static const void *installation_summary_vtbl[] =
{
    OBJ_METHODS, info_get_InstallationId, info_get_BundleStoreId, info_get_State, info_get_Error, info_get_Progress
};

static IInspectable *snap_create( const void **vtbl, const GUID *iid, const WCHAR *name, const struct install_snap *in )
{
    struct snap_object *obj = object_create( sizeof(*obj), vtbl, iid, name, NULL );
    if (!obj) return NULL;
    obj->in = *in;
    return (IInspectable *)obj;
}

static IInspectable *installation_info_create( const struct install_snap *in )
{
    return snap_create( installation_info_vtbl, &IID_IPackageInstallationInfo,
                        L"Microsoft.GamePlatform.Services.PackageInstallationInfo", in );
}

static IInspectable *installation_summary_create( const struct install_snap *in )
{
    return snap_create( installation_summary_vtbl, &IID_IPackageInstallationSummary,
                        L"Microsoft.GamePlatform.Services.PackageInstallationSummary", in );
}

/* event args */
static HRESULT WINAPI changed_get_InstallationInfo( struct snap_object *This, IInspectable **v )
{
    if (!v) return E_POINTER;
    return (*v = installation_info_create( &This->in )) ? S_OK : E_OUTOFMEMORY;
}

static const void *changed_args_vtbl[] = { OBJ_METHODS, changed_get_InstallationInfo };

static IInspectable *changed_args_create( const struct install_snap *in )
{
    return snap_create( changed_args_vtbl, &IID_IInstallationChangedEventArgs,
                        L"Microsoft.GamePlatform.Services.InstallationChangedEventArgs", in );
}

HSTR_GETTER(struct snap_object, item, PackageInstance, This->in.instance)
static const void *item_args_vtbl[] = { OBJ_METHODS, item_get_PackageInstance };

static void fire( int ev, IInspectable *sender, IInspectable *args )
{
    IUnknown **handlers;
    unsigned i, count;
    HRESULT hr;

    AcquireSRWLockShared( &events_lock );
    count = event_counts[ev];
    if ((handlers = malloc( max( count, 1 ) * sizeof(*handlers) )))
        for (i = 0; i < count; i++)
        {
            handlers[i] = event_handlers[ev][i].handler;
            IUnknown_AddRef( handlers[i] );
        }
    else count = 0;
    ReleaseSRWLockShared( &events_lock );
    for (i = 0; i < count; i++)
    {
        if (FAILED(hr = invoke_delegate( handlers[i], sender, args ))) TRACE( "event %d handler -> %#lx\n", ev, hr );
        IUnknown_Release( handlers[i] );
    }
    free( handlers );
}

static void fire_package( const struct install_snap *snap )
{
    struct package_handler *targets;
    unsigned i, count = 0;
    IInspectable *args;
    HRESULT hr;

    AcquireSRWLockShared( &events_lock );
    if ((targets = malloc( max( package_handlers_count, 1 ) * sizeof(*targets) )))
        for (i = 0; i < package_handlers_count; i++)
        {
            struct package_handler *h = &package_handlers[i];
            if (wcsicmp( h->key, snap->instance ) && wcsicmp( h->key, snap->store_id ) && wcsicmp( h->key, snap->family ))
                continue;
            IUnknown_AddRef( h->handler );
            IInspectable_AddRef( h->sender );
            targets[count++] = *h;
        }
    ReleaseSRWLockShared( &events_lock );
    for (i = 0; i < count; i++)
    {
        if ((args = changed_args_create( snap )))
        {
            if (FAILED(hr = invoke_delegate( targets[i].handler, targets[i].sender, args )))
                TRACE( "package InstallationChanged handler -> %#lx\n", hr );
            IInspectable_Release( args );
        }
        IUnknown_Release( targets[i].handler );
        IInspectable_Release( targets[i].sender );
    }
    free( targets );
}

static IInspectable *queue_sender( void )
{
    return (IInspectable *)factory_of( kind_of( L"Microsoft.GamePlatform.Services.PackageQueue" ) );
}

static void report_ex( struct install *in, BOOL added_or_removed, BOOL removed )
{
    struct install_snap snap;
    IInspectable *args;

    AcquireSRWLockShared( &installs_lock );
    snap = in->s;
    ReleaseSRWLockShared( &installs_lock );
    if (added_or_removed)
    {
        if ((args = snap_create( item_args_vtbl, &IID_IPackageQueueItemEventArgs,
                                 L"Microsoft.GamePlatform.Services.PackageQueueItemEventArgs", &snap )))
        {
            fire( removed ? EV_ITEM_REMOVED : EV_ITEM_ADDED, queue_sender(), args );
            IInspectable_Release( args );
        }
        fire( EV_QUEUE_CHANGED, queue_sender(), NULL );
    }
    if ((args = changed_args_create( &snap )))
    {
        fire( EV_INSTALLATION_CHANGED, queue_sender(), args );
        IInspectable_Release( args );
    }
    fire_package( &snap );
}

static void report( struct install *in )
{
    report_ex( in, FALSE, FALSE );
}

/* ---- the install worker ---- */

static void guid_text( const GUID *id, char *buf )
{
    sprintf( buf, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", id->Data1, id->Data2, id->Data3, id->Data4[0],
             id->Data4[1], id->Data4[2], id->Data4[3], id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7] );
}

/* An encrypted package's content keys, from the Store licence (wineuwp_xboxauth.dll), as the installer takes them:
 * "keyid:key;..." in hex. They go to it in memory, never into a file or a log. */
static HRESULT encrypted_package_keys( const GUID *content_id, char *keys, size_t size )
{
    typedef HRESULT (WINAPI *content_keys_fn)( const char *, const char *, char *, UINT32 );
    HMODULE auth = LoadLibraryW( L"wineuwp_xboxauth.dll" );
    content_keys_fn fn = auth ? (void *)GetProcAddress( auth, "XboxAuthContentKeys" ) : NULL;
    char id[40];
    HRESULT hr;

    if (!fn)
    {
        WARN( "install: no wineuwp_xboxauth.dll with XboxAuthContentKeys\n" );
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    }
    guid_text( content_id, id );
    hr = fn( id, "US", keys, size );
    TRACE( "install: content keys for %s -> %#lx\n", id, hr );
    if (FAILED(hr)) SecureZeroMemory( keys, size );
    return hr;
}

/* A GDK title's package (.msixvc) address: packagespc.xboxlive.com's GetBasePackage for its content id, signed and
 * authorized like any Xbox Live call (http://update.xboxlive.com). The first CDN root plus the file's relative URL. */
HRESULT msixvc_url( const GUID *content_id, char **url )
{
    typedef HRESULT (WINAPI *get_fn)( const char *, const char *, const char *, unsigned char **, UINT32 *, DWORD * );
    HMODULE xboxauth = LoadLibraryW( L"wineuwp_xboxauth.dll" );
    get_fn get = xboxauth ? (void *)GetProcAddress( xboxauth, "XboxAuthHttpGet" ) : NULL;
    const struct json *files, *file;
    unsigned char *body = NULL;
    char req[128], id[40];
    struct json *root;
    UINT32 len = 0, i;
    DWORD status = 0;
    HRESULT hr;

    *url = NULL;
    if (!get) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    guid_text( content_id, id );
    snprintf( req, sizeof(req), "https://packagespc.xboxlive.com/GetBasePackage/%s", id );
    hr = get( req, "Accept: application/json", "3", &body, &len, &status );
    if (FAILED(hr) || status != 200)
    {
        TRACE( "install: GetBasePackage -> %#lx, HTTP %lu\n", hr, status );
        if (body) CoTaskMemFree( body );
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    root = json_parse( (char *)body, len );
    CoTaskMemFree( body );
    files = json_get( root, "PackageFiles" );
    for (i = 0; i < json_count( files ); i++)
    {
        const char *name, *cdn, *rel;
        size_t n;

        file = json_at( files, i );
        name = json_str( json_get( file, "FileName" ) );
        n = strlen( name );
        if (n < 7 || stricmp( name + n - 7, ".msixvc" )) continue;
        cdn = json_str( json_at( json_get( file, "CdnRootPaths" ), 0 ) );
        rel = json_str( json_get( file, "RelativeUrl" ) );
        if ((*url = malloc( strlen( cdn ) + strlen( rel ) + 1 )))
        {
            strcpy( *url, cdn );
            strcat( *url, rel );
        }
        TRACE( "install: GDK package %s, %.0f bytes\n", name, json_get( file, "FileSize" )->number );
        json_free( root );
        if (*url && !**url)
        {
            free( *url );
            *url = NULL;
        }
        return *url ? S_OK : HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    TRACE( "install: GetBasePackage lists no .msixvc\n" );
    json_free( root );
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

struct installer_progress
{
    struct install *in;
    ULONGLONG rate_t0, report_t0;
    UINT64 rate_b0;
    BOOL installed;
    WCHAR last[512];                /* the installer's last message: why, when it fails */
};

static void CALLBACK installer_event( void *context, enum appx_event event, UINT64 completed, UINT64 total, const WCHAR *text )
{
    struct installer_progress *p = context;
    struct install *in = p->in;
    ULONGLONG now;
    UINT64 delta;

    switch (event)
    {
    case APPX_EVENT_STAGE:
        TRACE( "install: %s\n", debugstr_w( text ) );
        break;
    case APPX_EVENT_MESSAGE:
        TRACE( "install: %s\n", debugstr_w( text ) );
        lstrcpynW( p->last, text, ARRAY_SIZE(p->last) );
        break;
    case APPX_EVENT_INSTALLED:
        p->installed = TRUE;
        break;
    case APPX_EVENT_PROGRESS:
        now = GetTickCount64();
        if (!total) break;
        AcquireSRWLockExclusive( &installs_lock );
        /* byte-precise progress avoids permille quantization on very large packages */
        in->s.done = min( in->s.total, (UINT64)((long double)in->s.total * min( completed, total ) / total) );
        if (in->cancel)
        {
            ReleaseSRWLockExclusive( &installs_lock );
            break;
        }
        /* do not average a pause interval into the next speed sample */
        if (in->paused)
        {
            p->rate_t0 = 0;
            ReleaseSRWLockExclusive( &installs_lock );
            break;
        }
        if (!p->rate_t0)
        {
            p->rate_t0 = now;
            p->rate_b0 = in->s.done;
        }
        else if (now - p->rate_t0 >= 2000)
        {
            delta = in->s.done >= p->rate_b0 ? in->s.done - p->rate_b0 : 0;
            in->s.rate = min( delta * 1000 / (now - p->rate_t0), (UINT64)0xffffffff );
            p->rate_t0 = now;
            p->rate_b0 = in->s.done;
        }
        in->s.state = ST_STREAMING;
        ReleaseSRWLockExclusive( &installs_lock );
        /* keep XAML event traffic bounded while still refreshing speed smoothly */
        if (!p->report_t0 || now - p->report_t0 >= 500 || completed >= total)
        {
            p->report_t0 = now;
            report( in );
        }
        break;
    }
}

/* a Unix path from the environment as a Windows one; empty if unset */
static void env_path( const WCHAR *name, const WCHAR *suffix, WCHAR *buf, size_t size )
{
    WCHAR value[MAX_PATH];
    env_w( name, value, ARRAY_SIZE(value) );
    if (!value[0])
    {
        buf[0] = 0;
        return;
    }
    wcsncat( value, suffix, ARRAY_SIZE(value) - wcslen( value ) - 1 );
    unix_to_windows( buf, size, value );
}

static HRESULT run_installer( struct install *in, const char *url, const WCHAR *stage_name, const char *keys,
                              const WCHAR *tags, const WCHAR *languages )
{
    WCHAR data_unix[MAX_PATH], data[MAX_PATH], tree[MAX_PATH], defaults[MAX_PATH], d3d[MAX_PATH], *package;
    struct installer_progress progress = { in };
    struct appx_install_params params = {0};
    BOOL canceled;
    HRESULT hr;
    int len;

    AcquireSRWLockShared( &installs_lock );
    canceled = in->cancel;
    ReleaseSRWLockShared( &installs_lock );
    if (canceled) return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    games_data_unix( data_unix, ARRAY_SIZE(data_unix) );
    env_path( L"WINE_UWP_WINDOWS_TREE", L"", tree, ARRAY_SIZE(tree) );
    if (!data_unix[0] || !tree[0])
    {
        WARN( "install: WINE_UWP_GAMES_DATA or WINE_UWP_WINDOWS_TREE not set\n" );
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    }
    unix_to_windows( data, ARRAY_SIZE(data), data_unix );
    CreateDirectoryW( data, NULL );
    env_path( L"WINE_UWP_ROOT", L"/launch-defaults.env", defaults, ARRAY_SIZE(defaults) );
    env_path( L"WINE_UWP_D3D_RUNTIME", L"", d3d, ARRAY_SIZE(d3d) );
    if (!d3d[0]) env_path( L"UWP_BIN", L"/ext/d3d-runtime/x86_64-windows", d3d, ARRAY_SIZE(d3d) );
    len = MultiByteToWideChar( CP_UTF8, 0, url, -1, NULL, 0 );
    if (!(package = malloc( len * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, 0, url, -1, package, len );

    params.package = package;
    params.data_dir = data;
    params.windows_tree = tree;
    params.stage = stage_name;
    params.tags = tags;
    params.languages = languages;
    /* an encrypted package's content keys go to the installer in memory, never into a file or a log */
    params.content_keys = keys && *keys ? keys : NULL;
    params.launch_defaults = defaults[0] ? defaults : NULL;
    params.d3d_runtime = d3d[0] ? d3d : NULL;
    params.pause = &in->pause_flag;
    params.cancel = &in->cancel_flag;
    params.callback = installer_event;
    params.context = &progress;
    AcquireSRWLockExclusive( &installs_lock );
    in->running = TRUE;
    InterlockedExchange( &in->pause_flag, in->paused );
    ReleaseSRWLockExclusive( &installs_lock );
    TRACE( "install: installer started for %s\n", debugstr_w( stage_name ) );
    hr = appx_install( &params );
    TRACE( "install: installer -> %#lx%s\n", hr, progress.installed ? ", installed" : "" );
    if (FAILED(hr) && progress.last[0]) WARN( "install: %s\n", debugstr_w( progress.last ) );
    free( package );
    return SUCCEEDED(hr) && progress.installed ? S_OK : FAILED(hr) ? hr : E_FAIL;
}

/* the stage directory's identity.env: the package full name and AUMID the installer found */
static void read_identity( const WCHAR *dir, WCHAR *full, WCHAR *aumid, WCHAR *title, size_t size )
{
    char buf[4096], *line, *next, *eq;
    WCHAR path[MAX_PATH];
    size_t n;

    swprintf( path, ARRAY_SIZE(path), L"%s\\identity.env", dir );
    if (!read_small_file( path, buf, sizeof(buf) )) return;
    for (line = buf; line && *line; line = next)
    {
        next = line + strcspn( line, "\r\n" );
        if (*next) *next++ = 0;
        while (*next == '\r' || *next == '\n') next++;
        if (!strncmp( line, "export ", 7 )) line += 7;
        if (!(eq = strchr( line, '=' ))) continue;
        *eq++ = 0;
        n = strlen( eq );
        if (n >= 2 && (eq[0] == '\'' || eq[0] == '"'))
        {
            eq[n - 1] = 0;
            eq++;
        }
        if (!strcmp( line, "WINE_UWP_PACKAGE_FULL_NAME" )) wide_copy( full, size, eq );
        else if (!strcmp( line, "WINE_UWP_AUMID" )) wide_copy( aumid, size, eq );
        else if (!strcmp( line, "WINE_UWP_WINDOW_TITLE" )) wide_copy( title, size, eq );
    }
}

static void write_marker( struct install *in, const struct product *d, const WCHAR *dir )
{
    WCHAR full[256] = {0}, aumid[256] = {0}, title[256] = {0}, path[MAX_PATH];
    char *store_id = utf8_dup( in->s.store_id ), *family = utf8_dup( d->family ), *full8, *aumid8, *title8;
    char *features = in->features_chosen ? strlist_join( &in->features, ';' ) : NULL, *marker;
    size_t size;
    HANDLE file;
    DWORD written;

    read_identity( dir, full, aumid, title, ARRAY_SIZE(full) );
    full8 = utf8_dup( full );
    aumid8 = utf8_dup( aumid );
    title8 = utf8_dup( title );
    size = 256 + strlen( store_id ? store_id : "" ) + strlen( family ? family : "" ) + strlen( full8 ? full8 : "" ) +
           strlen( aumid8 ? aumid8 : "" ) + strlen( title8 ? title8 : "" ) + (features ? strlen( features ) : 0);
    if ((marker = malloc( size )))
    {
        int len = snprintf( marker, size, "store_id=%s\nfamily=%s\nfull_name=%s\naumid=%s\ntitle=%s\nsize=%I64u\nuwp=%s\n",
                            store_id ? store_id : "", family ? family : "", full8 ? full8 : "", aumid8 ? aumid8 : "",
                            title8 ? title8 : "", d->size, d->uwp ? "true" : "false" );
        if (features) len += snprintf( marker + len, size - len, "features=%s\n", features );
        swprintf( path, ARRAY_SIZE(path), L"%s\\.wine-uwp-xbox", dir );
        file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (file != INVALID_HANDLE_VALUE)
        {
            WriteFile( file, marker, len, &written, NULL );
            CloseHandle( file );
        }
        free( marker );
    }
    free( store_id );
    free( family );
    free( full8 );
    free( aumid8 );
    free( title8 );
    free( features );
}

static void remove_install( struct install *in )
{
    unsigned i;

    AcquireSRWLockExclusive( &installs_lock );
    for (i = 0; i < installs_count; i++)
        if (installs[i] == in)
        {
            memmove( installs + i, installs + i + 1, (installs_count - i - 1) * sizeof(*installs) );
            installs_count--;
            break;
        }
    ReleaseSRWLockExclusive( &installs_lock );
}

static DWORD WINAPI install_thread( void *param )
{
    struct install *in = param;
    WCHAR stage_name[MAX_PATH], root[MAX_PATH], dir[MAX_PATH], *tags = NULL, *languages = NULL;
    struct appx_store_package pkg;
    char *url = NULL, *category, keys[4096];
    BOOL canceled;
    struct product d;
    HRESULT hr;

    keys[0] = 0;
    query_product( in->s.store_id, &d );
    stage_name_for_family( d.family, stage_name, ARRAY_SIZE(stage_name) );

    AcquireSRWLockExclusive( &installs_lock );
    /* SubmitRequestAsync already set the selected GDK feature size. The catalog's size is the
     * full package and must not replace it when the worker starts. */
    if (d.uwp) in->s.total = d.size;
    /* Pause can be requested as soon as SubmitRequestAsync publishes the queued item, before
     * this worker gets scheduled. Do not overwrite that state while resolving the delivery URL. */
    canceled = in->cancel;
    if (!canceled) in->s.state = in->paused ? ST_PAUSED : ST_STREAMING;
    ReleaseSRWLockExclusive( &installs_lock );
    if (canceled)
    {
        hr = HRESULT_FROM_WIN32( ERROR_CANCELLED );
        goto done;
    }
    report( in );
    if (!d.uwp)
    {
        /* a GDK title: its .msixvc from the Xbox CDN, decrypted with the licence's content key */
        if (FAILED(hr = msixvc_url( &d.content_id, &url ))) goto done;
        if (FAILED(hr = encrypted_package_keys( &d.content_id, keys, sizeof(keys) ))) goto done;
        gdk_installer_choice( in->s.store_id, in->features_chosen ? &in->features : NULL, &tags, &languages );
        hr = run_installer( in, url, stage_name, keys, tags, languages );
    }
    else
    {
        if (!d.wu_category[0])
        {
            WARN( "install %s: no Store category in the catalogue\n", debugstr_w( in->s.store_id ) );
            hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
            goto done;
        }
        category = utf8_dup( d.wu_category );
        hr = category ? appx_store_find_package( category, d.family, &pkg ) : E_OUTOFMEMORY;
        free( category );
        if (FAILED(hr))
        {
            WARN( "install %s: no package for %s\n", debugstr_w( in->s.store_id ), debugstr_w( d.family ) );
            goto done;
        }
        TRACE( "install %s: package %s\n", debugstr_w( in->s.store_id ), pkg.moniker );
        if (FAILED(hr = appx_store_package_url( &pkg, &category ))) goto done;
        url = strdup( category );
        appx_free( category );
        if (!url)
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        TRACE( "install %s: download address found\n", debugstr_w( in->s.store_id ) );
        if (d.encrypted && FAILED(hr = encrypted_package_keys( &d.content_id, keys, sizeof(keys) ))) goto done;
        hr = run_installer( in, url, stage_name, keys, NULL, NULL );
    }
    SecureZeroMemory( keys, sizeof(keys) );
    if (FAILED(hr)) goto done;

    stage_root( root, ARRAY_SIZE(root) );
    swprintf( dir, ARRAY_SIZE(dir), L"%s\\%s", root, stage_name );
    write_marker( in, &d, dir );
    AcquireSRWLockExclusive( &installs_lock );
    lstrcpynW( in->stage, dir, ARRAY_SIZE(in->stage) );
    ReleaseSRWLockExclusive( &installs_lock );

done:
    SecureZeroMemory( keys, sizeof(keys) );
    free( tags );
    free( languages );
    free( url );
    AcquireSRWLockExclusive( &installs_lock );
    canceled = in->cancel;
    in->s.state = canceled ? ST_CANCELED : SUCCEEDED(hr) ? ST_COMPLETED : ST_FAILED;
    in->s.error = canceled ? HRESULT_FROM_WIN32( ERROR_CANCELLED ) : hr;
    if (SUCCEEDED(hr) && !canceled) in->s.done = in->s.total;
    ReleaseSRWLockExclusive( &installs_lock );
    TRACE( "install %s -> %s (%#lx)\n", debugstr_w( in->s.store_id ),
           canceled ? "canceled" : SUCCEEDED(hr) ? "completed" : "failed", in->s.error );
    report( in );
    if (SUCCEEDED(hr) || canceled)
    {
        /* completed and canceled items leave the queue after their terminal state is observable */
        if (!canceled) Sleep( 1000 );
        report_ex( in, TRUE, TRUE );
        remove_install( in );
        /* (the install itself is kept: event args and views may still refer to it) */
    }
    return 0;
}

/* ---- PackageRequest ---- */

struct request_size
{
    struct object obj;
    UINT64 size;
    BOOL upgrade;
};

GETTER(struct request_size, rqsize, IsUpgrade, boolean, This->upgrade)
GETTER(struct request_size, rqsize, TotalSize, UINT64, This->size)
GETTER(struct request_size, rqsize, DownloadSize, UINT64, This->size)
GETTER(struct request_size, rqsize, FreeSpaceRequired, UINT64, This->size)

static const void *request_size_vtbl[] =
{
    OBJ_METHODS, rqsize_get_IsUpgrade, rqsize_get_TotalSize, rqsize_get_DownloadSize, rqsize_get_FreeSpaceRequired
};

IInspectable *request_size_create( UINT64 size, BOOL upgrade )
{
    struct request_size *r;
    if (!(r = object_create( sizeof(*r), request_size_vtbl, &IID_IPackageRequestSize,
                             L"Microsoft.GamePlatform.Services.PackageRequestSize", NULL )))
        return NULL;
    r->size = size;
    r->upgrade = upgrade;
    return (IInspectable *)r;
}

static void request_destroy( struct object *obj )
{
    struct package_request *r = (struct package_request *)obj;
    if (r->metadata) IInspectable_Release( r->metadata );
    if (r->specs) IInspectable_Release( (IInspectable *)r->specs );
    strlist_free( &r->features );
}

static HRESULT put_str( WCHAR *dst, size_t size, HSTRING value )
{
    lstrcpynW( dst, WindowsGetStringRawBuffer( value, NULL ), size );
    return S_OK;
}

HSTR_GETTER(struct package_request, rq, Destination, This->destination)
HSTR_GETTER(struct package_request, rq, DestinationName, This->destination_name)
HSTR_GETTER(struct package_request, rq, Alias, This->alias)
GETTER(struct package_request, rq, Options, UINT32, This->options)
GETTER(struct package_request, rq, Type, INT32, This->type)

static HRESULT WINAPI rq_put_Destination( struct package_request *r, HSTRING v )
{
    put_str( r->destination, ARRAY_SIZE(r->destination), v );
    TRACE( "PackageRequest.Destination = %s\n", debugstr_w( r->destination ) );
    return S_OK;
}

static HRESULT WINAPI rq_put_DestinationName( struct package_request *r, HSTRING v )
{
    return put_str( r->destination_name, ARRAY_SIZE(r->destination_name), v );
}

static HRESULT WINAPI rq_put_Alias( struct package_request *r, HSTRING v )
{
    return put_str( r->alias, ARRAY_SIZE(r->alias), v );
}

static HRESULT WINAPI rq_get_Metadata( struct package_request *r, IInspectable **v )
{
    HSTRING cls;

    if (!v) return E_POINTER;
    if (!r->metadata && SUCCEEDED(WindowsCreateString( L"Windows.Foundation.Collections.PropertySet", 42, &cls )))
    {
        RoActivateInstance( cls, &r->metadata );
        WindowsDeleteString( cls );
    }
    if ((*v = r->metadata)) IInspectable_AddRef( r->metadata );
    return r->metadata ? S_OK : E_FAIL;
}

static HRESULT WINAPI rq_put_Metadata( struct package_request *r, IInspectable *v )
{
    if (v) IInspectable_AddRef( v );
    if (r->metadata) IInspectable_Release( r->metadata );
    r->metadata = v;
    return S_OK;
}

static HRESULT WINAPI rq_put_Options( struct package_request *r, UINT32 v )
{
    r->options = v;
    TRACE( "PackageRequest.Options = %#x\n", v );
    return S_OK;
}

static HRESULT WINAPI rq_put_Type( struct package_request *r, INT32 v )
{
    r->type = v;
    return S_OK;
}

static HRESULT WINAPI rq_get_Sources( struct package_request *r, IInspectable **v )
{
    FIXME( "PackageRequest.Sources: not implemented\n" );
    if (v) *v = NULL;
    return E_NOTIMPL;
}

/* the strings of an IIterable<String> */
void read_features( IInspectable *iterable, struct strlist *features )
{
    typedef HRESULT (WINAPI *first_fn)( void *, IInspectable ** );
    typedef HRESULT (WINAPI *bool_fn)( void *, boolean * );
    typedef HRESULT (WINAPI *current_fn)( void *, HSTRING * );
    IInspectable *first = NULL;
    void **it = NULL, **vtbl;
    boolean has = FALSE;
    HSTRING h;
    char *s;

    strlist_free( features );
    if (!iterable || FAILED(IInspectable_QueryInterface( iterable, &IID_IIterable_HString, (void **)&it )) || !it)
    {
        TRACE( "ConfigureFeatures: no IIterable<String>\n" );
        return;
    }
    if (SUCCEEDED(((first_fn)(*(void ***)it)[6])( it, &first )) && first)
    {
        vtbl = *(void ***)first;
        for (((bool_fn)vtbl[7])( first, &has ); has; ((bool_fn)vtbl[8])( first, &has ))
        {
            h = NULL;
            if (SUCCEEDED(((current_fn)vtbl[6])( first, &h )) && h)
            {
                if ((s = utf8_dup( WindowsGetStringRawBuffer( h, NULL ) ))) strlist_add( features, s );
                free( s );
                WindowsDeleteString( h );
            }
        }
        IInspectable_Release( first );
    }
    IUnknown_Release( (IUnknown *)it );
}

/* the features to install, as an IIterable<String> of feature ids */
static HRESULT WINAPI rq_ConfigureFeatures( struct package_request *r, IInspectable *iterable )
{
    char *list;

    read_features( iterable, &r->features );
    r->features_chosen = TRUE;
    list = strlist_join( &r->features, ';' );
    TRACE( "PackageRequest.ConfigureFeatures(%s)\n", debugstr_a( list ) );
    free( list );
    return S_OK;
}

static const void *request_vtbl[] =
{
    OBJ_METHODS, rq_get_Destination, rq_put_Destination, rq_get_DestinationName, rq_put_DestinationName, rq_get_Alias,
    rq_put_Alias, rq_get_Metadata, rq_put_Metadata, rq_get_Options, rq_put_Options, rq_get_Type, rq_put_Type,
    rq_get_Sources, request_get_Specifiers, rq_ConfigureFeatures, request_QuerySize
};

static struct package_request *request_create( void )
{
    struct package_request *r = object_create( sizeof(*r), request_vtbl, &IID_IPackageRequest,
                                               L"Microsoft.GamePlatform.Services.PackageRequest", request_destroy );
    if (r) r->source_type = 1;
    return r;
}

HRESULT WINAPI rq_CreateForSource( struct factory *f, HSTRING source, INT32 type, IInspectable **out )
{
    struct package_request *r;
    struct installed_game g;

    if (!out) return E_POINTER;
    if (!(r = request_create())) return E_OUTOFMEMORY;
    put_str( r->source, ARRAY_SIZE(r->source), source );
    r->source_type = type;
    if (type == 1 /* StoreId */) lstrcpynW( r->store_id, r->source, ARRAY_SIZE(r->store_id) );
    else if (find_installed( r->source, &g ))
    {
        lstrcpynW( r->store_id, g.store_id, ARRAY_SIZE(r->store_id) );
        installed_game_clear( &g );
    }
    TRACE( "PackageRequest.CreateForSource(%s, type %d)\n", debugstr_w( r->source ), type );
    *out = (IInspectable *)r;
    return S_OK;
}

HRESULT WINAPI rq_CreateForSourceDevice( struct factory *f, HSTRING source, INT32 type, GUID target, IInspectable **out )
{
    return rq_CreateForSource( f, source, type, out );
}

HRESULT WINAPI rq_CreateForStoreId( struct factory *f, HSTRING store, HSTRING sku, HSTRING avail, IInspectable **out )
{
    struct package_request *r;

    if (!out) return E_POINTER;
    if (!(r = request_create())) return E_OUTOFMEMORY;
    put_str( r->source, ARRAY_SIZE(r->source), store );
    put_str( r->store_id, ARRAY_SIZE(r->store_id), store );
    put_str( r->sku, ARRAY_SIZE(r->sku), sku );
    put_str( r->avail, ARRAY_SIZE(r->avail), avail );
    TRACE( "PackageRequest.CreateForStoreId(%s, %s, %s)\n", debugstr_w( r->store_id ), debugstr_w( r->sku ), debugstr_w( r->avail ) );
    *out = (IInspectable *)r;
    return S_OK;
}

/* ---- PackageQueue ---- */

HRESULT WINAPI pq_SubmitRequestAsync( struct factory *f, IInspectable *request, INT32 priority, HSTRING cv, IInspectable **out )
{
    struct package_request *r = NULL;
    struct install *in, **grown;
    struct async *op;
    struct product d;
    HANDLE thread;

    if (!out || !request) return E_POINTER;
    if (FAILED(IInspectable_QueryInterface( request, &IID_IPackageRequest, (void **)&r ))) return E_INVALIDARG;
    if (r->obj.vtbl != request_vtbl)
    {
        IInspectable_Release( (IInspectable *)r );
        return E_INVALIDARG;
    }
    if (!(in = calloc( 1, sizeof(*in) )) || !(op = async_create( &IID_IAsyncOperation_Guid, TRUE )))
    {
        free( in );
        IInspectable_Release( (IInspectable *)r );
        return E_OUTOFMEMORY;
    }
    CoCreateGuid( &in->s.id );
    lstrcpynW( in->s.store_id, r->store_id, ARRAY_SIZE(in->s.store_id) );
    strlist_copy( &in->features, &r->features );
    in->features_chosen = r->features_chosen;
    request_features_from_specifiers( r, &in->features, &in->features_chosen );
    IInspectable_Release( (IInspectable *)r );
    query_product( in->s.store_id, &d );
    lstrcpynW( in->s.family, d.family, ARRAY_SIZE(in->s.family) );
    in->uwp = d.uwp;
    stage_name_for_family( d.family, in->stage, ARRAY_SIZE(in->stage) );
    instance_for( d.family[0] ? d.family : in->s.store_id, in->s.instance );
    in->s.state = ST_WAITING;
    in->s.total = d.size;
    if (!d.uwp) in->s.total = gdk_request_size( in->s.store_id, in->features_chosen ? &in->features : NULL, d.size );
    TRACE( "PackageQueue.SubmitRequestAsync(%s, priority %d) -> installation %s\n", debugstr_w( in->s.store_id ), priority,
           debugstr_guid( &in->s.id ) );
    AcquireSRWLockExclusive( &installs_lock );
    if ((grown = realloc( installs, (installs_count + 1) * sizeof(*installs) )))
    {
        installs = grown;
        installs[installs_count++] = in;
    }
    ReleaseSRWLockExclusive( &installs_lock );
    /* already complete when handed out: SubmitRequestAsync answers as soon as the item is queued */
    async_finish_guid( op, S_OK, &in->s.id );
    *out = async_iface( op );
    report_ex( in, TRUE, FALSE );
    if ((thread = CreateThread( NULL, 0, install_thread, in, 0, NULL ))) CloseHandle( thread );
    return S_OK;
}

/* under installs_lock */
static struct install *find_install( const WCHAR *instance, const GUID *id )
{
    unsigned i;
    for (i = 0; i < installs_count; i++)
        if ((instance && !wcsicmp( installs[i]->s.instance, instance )) || (id && IsEqualGUID( &installs[i]->s.id, id )))
            return installs[i];
    return NULL;
}

HRESULT WINAPI pq_Enumerate( struct factory *f, IInspectable **out )
{
    struct hstring_view *view;
    unsigned i;

    if (!out) return E_POINTER;
    if (!(view = hstring_view_create())) return E_OUTOFMEMORY;
    AcquireSRWLockShared( &installs_lock );
    for (i = 0; i < installs_count; i++) hstring_view_add( view, installs[i]->s.instance );
    ReleaseSRWLockShared( &installs_lock );
    TRACE( "PackageQueue.Enumerate -> %u\n", hstring_view_size( view ) );
    *out = (IInspectable *)view;
    return S_OK;
}

HRESULT WINAPI pq_EnumerateId( struct factory *f, GUID id, IInspectable **out )
{
    struct hstring_view *view;
    struct install *in;

    if (!out) return E_POINTER;
    if (!(view = hstring_view_create())) return E_OUTOFMEMORY;
    AcquireSRWLockShared( &installs_lock );
    if ((in = find_install( NULL, &id ))) hstring_view_add( view, in->s.instance );
    ReleaseSRWLockShared( &installs_lock );
    *out = (IInspectable *)view;
    return S_OK;
}

static BOOL snapshot( const WCHAR *instance, const GUID *id, struct install_snap *snap )
{
    struct install *in;

    AcquireSRWLockShared( &installs_lock );
    if ((in = find_install( instance, id ))) *snap = in->s;
    ReleaseSRWLockShared( &installs_lock );
    return !!in;
}

HRESULT WINAPI pq_QueryInstallationInfo( struct factory *f, HSTRING instance, IInspectable **out )
{
    const WCHAR *key = WindowsGetStringRawBuffer( instance, NULL );
    struct install_snap snap;
    struct installed_game g;

    if (!out) return E_POINTER;
    if (!snapshot( key, NULL, &snap ))
    {
        /* an installed game: its installation is complete */
        if (!find_installed( key, &g ))
        {
            *out = NULL;
            return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
        }
        memset( &snap, 0, sizeof(snap) );
        lstrcpynW( snap.instance, g.instance, ARRAY_SIZE(snap.instance) );
        lstrcpynW( snap.store_id, g.store_id, ARRAY_SIZE(snap.store_id) );
        lstrcpynW( snap.family, g.family, ARRAY_SIZE(snap.family) );
        snap.state = ST_COMPLETED;
        snap.total = snap.done = g.size;
        installed_game_clear( &g );
    }
    return (*out = installation_info_create( &snap )) ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI pq_QueryInstallationSummary( struct factory *f, GUID id, IInspectable **out )
{
    struct install_snap snap;

    if (!out) return E_POINTER;
    if (!snapshot( NULL, &id, &snap ))
    {
        *out = NULL;
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    return (*out = installation_summary_create( &snap )) ? S_OK : E_OUTOFMEMORY;
}

/* under installs_lock */
static HRESULT cancel_install( struct install *in )
{
    if (!in) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (in->s.state == ST_COMPLETED || in->s.state == ST_FAILED || in->s.state == ST_CANCELED)
        return HRESULT_FROM_WIN32( ERROR_INVALID_STATE );
    TRACE( "install %s: cancel requested\n", debugstr_w( in->s.store_id ) );
    in->cancel = TRUE;
    in->paused = FALSE;
    InterlockedExchange( &in->cancel_flag, 1 );
    InterlockedExchange( &in->pause_flag, 0 );
    in->s.rate = 0;
    in->s.state = ST_CANCELED;
    in->s.error = HRESULT_FROM_WIN32( ERROR_CANCELLED );
    return S_OK;
}

static BOOL cancel_where( const WCHAR *instance, const GUID *id )
{
    struct install *in;
    HRESULT hr;

    AcquireSRWLockExclusive( &installs_lock );
    in = find_install( instance, id );
    hr = cancel_install( in );
    ReleaseSRWLockExclusive( &installs_lock );
    if (SUCCEEDED(hr) && in) report( in );
    return SUCCEEDED(hr);
}

HRESULT WINAPI pq_CancelInstance( struct factory *f, HSTRING instance, boolean nondestructive, boolean *out )
{
    BOOL ret = cancel_where( WindowsGetStringRawBuffer( instance, NULL ), NULL );
    if (out) *out = ret;
    return S_OK;
}

HRESULT WINAPI pq_CancelId( struct factory *f, GUID id, boolean nondestructive, boolean *out )
{
    BOOL ret = cancel_where( NULL, &id );
    if (out) *out = ret;
    return S_OK;
}

/* Pause and Resume: the installer waits between chunks while its pause flag is set */
static void set_paused( struct install *in, BOOL pause )
{
    INT32 old_state, new_state;
    BOOL changed;

    AcquireSRWLockExclusive( &installs_lock );
    old_state = in->s.state;
    changed = in->paused != pause && (in->s.state == ST_STREAMING || in->s.state == ST_PAUSED || in->s.state == ST_WAITING);
    if (changed)
    {
        in->paused = pause;
        in->s.rate = 0;
        in->s.state = pause ? ST_PAUSED : in->running ? ST_STREAMING : ST_WAITING;
        InterlockedExchange( &in->pause_flag, pause );
    }
    new_state = in->s.state;
    ReleaseSRWLockExclusive( &installs_lock );
    if (!changed) return;
    TRACE( "install %s: %s (state %d -> %d)\n", debugstr_w( in->s.store_id ), pause ? "paused" : "resumed", old_state, new_state );
    report( in );
}

static void set_paused_where( const WCHAR *instance, const GUID *id, BOOL pause )
{
    struct install **hits;
    unsigned i, count = 0;

    AcquireSRWLockShared( &installs_lock );
    if ((hits = malloc( max( installs_count, 1 ) * sizeof(*hits) )))
        for (i = 0; i < installs_count; i++)
        {
            struct install *in = installs[i];
            if ((!instance && !id) || (instance && (!wcsicmp( in->s.instance, instance ) || !wcsicmp( in->s.store_id, instance ))) ||
                (id && IsEqualGUID( &in->s.id, id )))
                hits[count++] = in;
        }
    ReleaseSRWLockShared( &installs_lock );
    for (i = 0; i < count; i++) set_paused( hits[i], pause );
    free( hits );
}

HRESULT WINAPI pq_PauseInstance( struct factory *f, HSTRING instance )
{
    TRACE( "PackageQueue.PauseInstance(%s)\n", debugstr_hstring( instance ) );
    set_paused_where( WindowsGetStringRawBuffer( instance, NULL ), NULL, TRUE );
    return S_OK;
}

HRESULT WINAPI pq_ResumeInstance( struct factory *f, HSTRING instance )
{
    TRACE( "PackageQueue.ResumeInstance(%s)\n", debugstr_hstring( instance ) );
    set_paused_where( WindowsGetStringRawBuffer( instance, NULL ), NULL, FALSE );
    return S_OK;
}

HRESULT WINAPI pq_PauseId( struct factory *f, GUID id ) { TRACE( "PackageQueue.PauseId\n" ); set_paused_where( NULL, &id, TRUE ); return S_OK; }
HRESULT WINAPI pq_ResumeId( struct factory *f, GUID id ) { TRACE( "PackageQueue.ResumeId\n" ); set_paused_where( NULL, &id, FALSE ); return S_OK; }
HRESULT WINAPI pq_PauseAll( struct factory *f ) { TRACE( "PackageQueue.PauseAll\n" ); set_paused_where( NULL, NULL, TRUE ); return S_OK; }
HRESULT WINAPI pq_ResumeAll( struct factory *f ) { TRACE( "PackageQueue.ResumeAll\n" ); set_paused_where( NULL, NULL, FALSE ); return S_OK; }

HRESULT WINAPI pq_CancelAll( struct factory *f, boolean nondestructive, boolean *out )
{
    struct install **canceled;
    unsigned i, count = 0;

    AcquireSRWLockExclusive( &installs_lock );
    if ((canceled = malloc( max( installs_count, 1 ) * sizeof(*canceled) )))
        for (i = 0; i < installs_count; i++) if (SUCCEEDED(cancel_install( installs[i] ))) canceled[count++] = installs[i];
    ReleaseSRWLockExclusive( &installs_lock );
    for (i = 0; i < count; i++) report( canceled[i] );
    free( canceled );
    if (out) *out = TRUE;
    return S_OK;
}

static HRESULT add_event( int ev, IUnknown *handler, LONGLONG *token )
{
    struct handler *grown;

    if (!handler || !token) return E_POINTER;
    AcquireSRWLockExclusive( &events_lock );
    if (!(grown = realloc( event_handlers[ev], (event_counts[ev] + 1) * sizeof(*grown) )))
    {
        ReleaseSRWLockExclusive( &events_lock );
        return E_OUTOFMEMORY;
    }
    IUnknown_AddRef( handler );
    *token = ++next_event_token;
    event_handlers[ev] = grown;
    grown[event_counts[ev]].token = *token;
    grown[event_counts[ev]++].handler = handler;
    ReleaseSRWLockExclusive( &events_lock );
    TRACE( "PackageQueue event %d: registered (token %I64d)\n", ev, *token );
    return S_OK;
}

static HRESULT remove_event( int ev, LONGLONG token )
{
    IUnknown *gone = NULL;
    unsigned i;

    AcquireSRWLockExclusive( &events_lock );
    for (i = 0; i < event_counts[ev]; i++)
        if (event_handlers[ev][i].token == token)
        {
            gone = event_handlers[ev][i].handler;
            memmove( event_handlers[ev] + i, event_handlers[ev] + i + 1, (event_counts[ev] - i - 1) * sizeof(struct handler) );
            event_counts[ev]--;
            break;
        }
    ReleaseSRWLockExclusive( &events_lock );
    if (gone) IUnknown_Release( gone );
    return S_OK;
}

HRESULT WINAPI pq_add_InstallationChanged( struct factory *f, IUnknown *h, LONGLONG *t ) { return add_event( EV_INSTALLATION_CHANGED, h, t ); }
HRESULT WINAPI pq_remove_InstallationChanged( struct factory *f, LONGLONG t ) { return remove_event( EV_INSTALLATION_CHANGED, t ); }
HRESULT WINAPI pq_add_QueueChanged( struct factory *f, IUnknown *h, LONGLONG *t ) { return add_event( EV_QUEUE_CHANGED, h, t ); }
HRESULT WINAPI pq_remove_QueueChanged( struct factory *f, LONGLONG t ) { return remove_event( EV_QUEUE_CHANGED, t ); }
HRESULT WINAPI pq_add_ItemAdded( struct factory *f, IUnknown *h, LONGLONG *t ) { return add_event( EV_ITEM_ADDED, h, t ); }
HRESULT WINAPI pq_remove_ItemAdded( struct factory *f, LONGLONG t ) { return remove_event( EV_ITEM_ADDED, t ); }
HRESULT WINAPI pq_add_ItemRemoved( struct factory *f, IUnknown *h, LONGLONG *t ) { return add_event( EV_ITEM_REMOVED, h, t ); }
HRESULT WINAPI pq_remove_ItemRemoved( struct factory *f, LONGLONG t ) { return remove_event( EV_ITEM_REMOVED, t ); }

/* PackageQueueView: one installation (or the installation of one package) */
struct queue_view
{
    struct object obj;
    WCHAR instance[64];
    GUID id;
    BOOL by_id;
};

static HRESULT WINAPI qv_Pause( struct queue_view *v )
{
    TRACE( "PackageQueueView.Pause(%s)\n", debugstr_w( v->instance ) );
    set_paused_where( v->by_id ? NULL : v->instance, v->by_id ? &v->id : NULL, TRUE );
    return S_OK;
}

static HRESULT WINAPI qv_Resume( struct queue_view *v )
{
    TRACE( "PackageQueueView.Resume(%s)\n", debugstr_w( v->instance ) );
    set_paused_where( v->by_id ? NULL : v->instance, v->by_id ? &v->id : NULL, FALSE );
    return S_OK;
}

static HRESULT WINAPI qv_Cancel( struct queue_view *v, boolean nondestructive, boolean *out )
{
    BOOL ret = cancel_where( v->by_id ? NULL : v->instance, v->by_id ? &v->id : NULL );
    if (out) *out = ret;
    return S_OK;
}

static HRESULT WINAPI qv_Enumerate( struct queue_view *v, IInspectable **out )
{
    struct hstring_view *view;
    struct install *in;

    if (!out) return E_POINTER;
    if (!(view = hstring_view_create())) return E_OUTOFMEMORY;
    AcquireSRWLockShared( &installs_lock );
    if ((in = find_install( v->by_id ? NULL : v->instance, v->by_id ? &v->id : NULL ))) hstring_view_add( view, in->s.instance );
    ReleaseSRWLockShared( &installs_lock );
    TRACE( "PackageQueueView.Enumerate(%s) -> %u\n", debugstr_w( v->instance ), hstring_view_size( view ) );
    *out = (IInspectable *)view;
    return S_OK;
}

static HRESULT WINAPI qv_QueryInstallationSummary( struct queue_view *v, IInspectable **out )
{
    struct install_snap snap;

    if (!out) return E_POINTER;
    if (!snapshot( v->by_id ? NULL : v->instance, v->by_id ? &v->id : NULL, &snap ))
    {
        *out = NULL;
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    return (*out = installation_summary_create( &snap )) ? S_OK : E_OUTOFMEMORY;
}

/* the view's changes are the queue's: one list of handlers */
static HRESULT WINAPI qv_add_InstallationChanged( struct queue_view *v, IUnknown *h, LONGLONG *t ) { return add_event( EV_INSTALLATION_CHANGED, h, t ); }
static HRESULT WINAPI qv_remove_InstallationChanged( struct queue_view *v, LONGLONG t ) { return remove_event( EV_INSTALLATION_CHANGED, t ); }

static const void *queue_view_vtbl[] =
{
    OBJ_METHODS, qv_Pause, qv_Resume, qv_Cancel, qv_Enumerate, qv_QueryInstallationSummary, qv_add_InstallationChanged,
    qv_remove_InstallationChanged
};

static struct queue_view *queue_view_create( void )
{
    return object_create( sizeof(struct queue_view), queue_view_vtbl, &IID_IPackageQueueView,
                          L"Microsoft.GamePlatform.Services.PackageQueueView", NULL );
}

HRESULT WINAPI pq_CreateViewInstance( struct factory *f, HSTRING instance, IInspectable **out )
{
    struct queue_view *v;

    if (!out) return E_POINTER;
    if (!(v = queue_view_create())) return E_OUTOFMEMORY;
    put_str( v->instance, ARRAY_SIZE(v->instance), instance );
    *out = (IInspectable *)v;
    return S_OK;
}

HRESULT WINAPI pq_CreateViewId( struct factory *f, GUID id, IInspectable **out )
{
    struct queue_view *v;

    if (!out) return E_POINTER;
    if (!(v = queue_view_create())) return E_OUTOFMEMORY;
    v->id = id;
    v->by_id = TRUE;
    *out = (IInspectable *)v;
    return S_OK;
}

/* ---- the installed Package ---- */

/* an installed game, or one in the queue (its identity is known before it is installed) */
BOOL is_package_instance( const GUID *id )
{
    struct installed_game g;
    WCHAR str[64];
    BOOL found;

    StringFromGUID2( id, str, ARRAY_SIZE(str) );
    if (find_installed( str, &g ))
    {
        installed_game_clear( &g );
        return TRUE;
    }
    AcquireSRWLockShared( &installs_lock );
    found = !!find_install( str, NULL );
    ReleaseSRWLockShared( &installs_lock );
    return found;
}

BOOL package_game( struct factory *f, struct installed_game *g )
{
    return find_installed( f->source, g );
}

BOOL queued_game( struct factory *f, struct installed_game *g )
{
    WCHAR root[MAX_PATH];
    BOOL found = FALSE;
    unsigned i;

    memset( g, 0, sizeof(*g) );
    AcquireSRWLockShared( &installs_lock );
    for (i = 0; i < installs_count; i++)
    {
        struct install *in = installs[i];
        if (wcsicmp( in->s.instance, f->source ) && wcsicmp( in->s.store_id, f->source )) continue;
        lstrcpynW( g->instance, in->s.instance, ARRAY_SIZE(g->instance) );
        lstrcpynW( g->family, in->s.family, ARRAY_SIZE(g->family) );
        lstrcpynW( g->store_id, in->s.store_id, ARRAY_SIZE(g->store_id) );
        lstrcpynW( g->full_name, in->s.family, ARRAY_SIZE(g->full_name) );
        /* Pending payloads are unpacked beside the final directory. QueryDetails needs a real
         * stage path so its logo properties point into the visible .new tree during download. */
        if (in->stage[0] && !wcschr( in->stage, '\\' ))
        {
            stage_root( root, ARRAY_SIZE(root) );
            swprintf( g->stage_dir, ARRAY_SIZE(g->stage_dir), L"%s\\%s.new", root, in->stage );
        }
        g->uwp = in->uwp;
        g->size = in->s.total;
        found = TRUE;
        break;
    }
    ReleaseSRWLockShared( &installs_lock );
    return found;
}

/* the store id of an install in the queue, by package instance or installation id */
BOOL queued_store_id( const WCHAR *key, const GUID *id, WCHAR *store_id, size_t size )
{
    struct install *in;
    BOOL found = FALSE;

    AcquireSRWLockShared( &installs_lock );
    if ((in = find_install( key, id )) && in->s.store_id[0])
    {
        lstrcpynW( store_id, in->s.store_id, size );
        found = TRUE;
    }
    ReleaseSRWLockShared( &installs_lock );
    return found;
}

HRESULT WINAPI pi_QueryInstallPath( struct factory *f, HSTRING *out )
{
    struct installed_game g;
    HRESULT hr;

    if (!package_game( f, &g )) return generic_slot( f, 6, NULL, __builtin_return_address(0) );
    TRACE( "Package(%s).QueryInstallPath -> %s\n", debugstr_w( f->source ), debugstr_w( g.stage_dir ) );
    hr = hstr( g.stage_dir, out );
    installed_game_clear( &g );
    return hr;
}

HRESULT WINAPI pi_QueryInstanceId( struct factory *f, HSTRING *out )
{
    struct installed_game g;
    HRESULT hr;

    if (!package_game( f, &g ) && !queued_game( f, &g )) return generic_slot( f, 7, NULL, __builtin_return_address(0) );
    hr = hstr( g.instance, out );
    installed_game_clear( &g );
    return hr;
}

HRESULT WINAPI pi_QueryIdentity( struct factory *f, INT32 type, HSTRING *out )
{
    struct installed_game g;
    HRESULT hr;

    if (!package_game( f, &g ) && !queued_game( f, &g ))
        return generic_slot( f, 8, out, __builtin_return_address(0) );
    TRACE( "Package(%s).QueryIdentity(%d)\n", debugstr_w( f->source ), type );
    hr = hstr( type == 0 ? g.family : type == 1 ? g.full_name : g.store_id, out );
    installed_game_clear( &g );
    return hr;
}

HRESULT WINAPI pi_QuerySize( struct factory *f, UINT64 *out )
{
    struct installed_game g;

    if (!package_game( f, &g )) return generic_slot( f, 10, NULL, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    *out = g.size;
    return S_OK;
}

HRESULT WINAPI pi_QueryStorageCategory( struct factory *f, INT32 *out )
{
    struct installed_game g;

    if (!package_game( f, &g )) return generic_slot( f, 11, NULL, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    *out = 0; /* App: a Store app, installed with the Windows apps */
    return S_OK;
}

HRESULT WINAPI pi_IsLaunchable( struct factory *f, boolean *out )
{
    struct installed_game g;
    BOOL installed = package_game( f, &g );

    /* still in the queue: there is a package, it just cannot be started yet */
    if (!installed && !queued_game( f, &g )) return generic_slot( f, 25, NULL, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    *out = installed;
    return S_OK;
}

HRESULT WINAPI pi_IsFlatStreamed( struct factory *f, boolean *out )
{
    struct installed_game g;

    if (!package_game( f, &g )) return generic_slot( f, 24, NULL, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    *out = FALSE;
    return S_OK;
}

/* A game gets the environment a launcher would give it, not ours: everything that run.sh exported for the app
 * that starts it (its package identity, DLL overrides, framework path, test switches) is left out, and the game's
 * wineuwp-launch.exe sets its own from identity.env and launch.env. */
static WCHAR *clean_environment( void )
{
    static const WCHAR *drop[] = { L"WINE_UWP_", L"UWP_", L"WINEDLLOVERRIDES=", L"WINEPATH=", L"WINE_D3D_CONFIG=",
                                   L"WINE_SKIP_NATIVE_THREAD_CALLS=" };
    WCHAR *env = GetEnvironmentStringsW(), *block, *p, *out;
    size_t len = 0;
    unsigned i;
    BOOL skip;

    for (p = env; p && *p; p += wcslen( p ) + 1) len += wcslen( p ) + 1;
    if (!(block = out = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        FreeEnvironmentStringsW( env );
        return NULL;
    }
    for (p = env; p && *p; p += wcslen( p ) + 1)
    {
        for (i = 0, skip = FALSE; i < ARRAY_SIZE(drop); i++) if (!wcsnicmp( p, drop[i], wcslen( drop[i] ) )) skip = TRUE;
        if (skip) continue;
        wcscpy( out, p );
        out += wcslen( p ) + 1;
    }
    *out = 0;
    if (env) FreeEnvironmentStringsW( env );
    return block;
}

static HRESULT launch_game( struct factory *f, const char *what )
{
    STARTUPINFOW si = { sizeof(si) };
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 2], *env;
    struct installed_game g;
    PROCESS_INFORMATION pi;
    BOOL ret;

    if (!package_game( f, &g )) return GP_E_NO_SERVICE;
    swprintf( exe, ARRAY_SIZE(exe), L"%s\\wineuwp-launch.exe", g.stage_dir );
    swprintf( cmd, ARRAY_SIZE(cmd), L"\"%s\"", exe );
    TRACE( "Package(%s).%s: starting %s\n", debugstr_w( f->source ), what, debugstr_w( exe ) );
    env = clean_environment();
    ret = CreateProcessW( exe, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env, g.stage_dir, &si, &pi );
    free( env );
    installed_game_clear( &g );
    if (!ret)
    {
        WARN( "CreateProcess failed %lu\n", GetLastError() );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return S_OK;
}

HRESULT WINAPI pi_Activate( struct factory *f, HSTRING exe, HSTRING params, HSTRING caller, HSTRING cv, UINT32 timeout )
{
    return launch_game( f, "Activate" );
}

HRESULT WINAPI pi_ActivateAumid( struct factory *f, HSTRING exe, HSTRING params, HSTRING caller, HSTRING cv )
{
    return launch_game( f, "ActivateAumid" );
}

HRESULT WINAPI pi_GetRunningProcessId( struct factory *f, HSTRING exe, UINT32 *out )
{
    struct installed_game g;

    if (!package_game( f, &g )) return generic_slot( f, 27, out, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    *out = 0;
    return S_OK;
}

HRESULT WINAPI pi_add_InstallationChanged( struct factory *f, IUnknown *handler, LONGLONG *token )
{
    struct package_handler *grown, *h;

    if (!handler || !token) return E_POINTER;
    AcquireSRWLockExclusive( &events_lock );
    if (!(grown = realloc( package_handlers, (package_handlers_count + 1) * sizeof(*grown) )))
    {
        ReleaseSRWLockExclusive( &events_lock );
        return E_OUTOFMEMORY;
    }
    package_handlers = grown;
    h = &grown[package_handlers_count++];
    h->handler = handler;
    IUnknown_AddRef( h->handler );
    factory_AddRef( f );
    h->sender = (IInspectable *)f;
    lstrcpynW( h->key, f->source, ARRAY_SIZE(h->key) );
    *token = h->token = ++next_event_token;
    ReleaseSRWLockExclusive( &events_lock );
    TRACE( "Package(%s).add_InstallationChanged (token %I64d)\n", debugstr_w( f->source ), *token );
    return S_OK;
}

HRESULT WINAPI pi_remove_InstallationChanged( struct factory *f, LONGLONG token )
{
    struct package_handler gone = {0};
    unsigned i;

    AcquireSRWLockExclusive( &events_lock );
    for (i = 0; i < package_handlers_count; i++)
        if (package_handlers[i].token == token)
        {
            gone = package_handlers[i];
            memmove( package_handlers + i, package_handlers + i + 1, (package_handlers_count - i - 1) * sizeof(*package_handlers) );
            package_handlers_count--;
            break;
        }
    ReleaseSRWLockExclusive( &events_lock );
    if (gone.handler)
    {
        IUnknown_Release( gone.handler );
        IInspectable_Release( gone.sender );
    }
    return S_OK;
}

/* PackageDetails and PackageHeader of an installed game */
static const GUID IID_IPackageDetails = {0x97751bca,0x2add,0x5e51,{0xb3,0x7d,0xf9,0x5c,0xc7,0x47,0xcd,0xb1}};
static const GUID IID_IPackageHeader = {0xc6e63704,0x7787,0x59af,{0xad,0x43,0xa3,0x74,0x78,0x69,0xc6,0x53}};

struct package_details
{
    struct object obj;
    struct installed_game g;    /* without its features */
    BOOL pending;
    GUID content_id;
};

static INT64 file_time_of( const WCHAR *dir, const WCHAR *name )
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    WCHAR path[MAX_PATH];

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", dir, name );
    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &a )) return 0;
    return ((INT64)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

static void package_logo_path( const struct installed_game *g, WCHAR *path, size_t size )
{
    /* Keep the actual codec in the extension. Candy Crush's package logo is JPEG; the legacy
     * .png copy is retained for wine-uwp's desktop launcher, but XAML's image loader uses the
     * extension to choose its decoder. */
    static const WCHAR *extensions[] = { L".jpg", L".jpeg", L".png" };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(extensions); i++)
    {
        swprintf( path, size, L"%s\\.wine-uwp-icon%s", g->stage_dir, extensions[i] );
        if (GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES) return;
    }
    swprintf( path, size, L"%s\\.wine-uwp-icon.png", g->stage_dir );
}

HSTR_GETTER(struct package_details, pd, InstanceId, This->g.instance)
HSTR_GETTER(struct package_details, pd, PackageFullName, This->g.full_name)
HSTR_GETTER(struct package_details, pd, DefaultDisplayName, This->g.title)
HSTR_GETTER(struct package_details, pd, SplashScreenPath, L"")
GETTER(struct package_details, pd, TitleId, UINT32, 0)
GETTER(struct package_details, pd, Registered, boolean, !This->pending)
/* installed packages are already registered; pending ones do not have all their files yet */
GETTER(struct package_details, pd, Registerable, boolean, FALSE)
GETTER(struct package_details, pd, NotFullyStreamed, boolean, This->pending)
GETTER(struct package_details, pd, UsesFeatures, boolean, FALSE)
GETTER(struct package_details, pd, IsMoving, boolean, FALSE)
GETTER(struct package_details, pd, InstallDate, INT64, file_time_of( This->g.stage_dir, L".wine-uwp-xbox" ))
GETTER(struct package_details, pd, UpdateDate, INT64, file_time_of( This->g.stage_dir, L".wine-uwp-xbox" ))
GETTER(struct package_details, pd, LaunchDate, INT64, 0)

static HRESULT logo_path( struct package_details *This, const char *what, HSTRING *v )
{
    WCHAR path[MAX_PATH];
    package_logo_path( &This->g, path, ARRAY_SIZE(path) );
    TRACE( "PackageDetails(%s).%s -> %s\n", debugstr_w( This->g.title ), what, debugstr_w( path ) );
    return hstr( path, v );
}

static HRESULT WINAPI pd_get_Square44x44LogoPath( struct package_details *This, HSTRING *v ) { return logo_path( This, "Square44x44LogoPath", v ); }
static HRESULT WINAPI pd_get_Square150x150LogoPath( struct package_details *This, HSTRING *v ) { return logo_path( This, "Square150x150LogoPath", v ); }

static HRESULT WINAPI pd_get_IsUwp( struct package_details *This, boolean *v )
{
    if (!v) return E_POINTER;
    *v = This->g.uwp;
    TRACE( "PackageDetails(%s).IsUwp -> %d\n", debugstr_w( This->g.title ), This->g.uwp );
    return S_OK;
}

static const void *package_details_vtbl[] =
{
    OBJ_METHODS, pd_get_InstanceId, pd_get_PackageFullName, pd_get_DefaultDisplayName, pd_get_Square44x44LogoPath,
    pd_get_Square150x150LogoPath, pd_get_SplashScreenPath, pd_get_TitleId, pd_get_Registered, pd_get_Registerable,
    pd_get_NotFullyStreamed, pd_get_IsUwp, pd_get_UsesFeatures, pd_get_IsMoving, pd_get_InstallDate, pd_get_UpdateDate,
    pd_get_LaunchDate
};

struct package_version { UINT64 value; UINT16 major, minor, build, revision; };

GETTER(struct package_details, ph, ContentId, GUID, This->content_id)
GETTER(struct package_details, ph, ProductId, GUID, guid_null)
GETTER(struct package_details, ph, BuildId, GUID, guid_null)
GETTER(struct package_details, ph, BlobSize, UINT64, This->g.size)

static HRESULT WINAPI ph_get_Version( struct package_details *This, struct package_version *v )
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    const WCHAR *us = wcschr( This->g.full_name, '_' );

    if (!v) return E_POINTER;
    if (us) swscanf( us + 1, L"%u.%u.%u.%u", &a, &b, &c, &d );
    v->major = a;
    v->minor = b;
    v->build = c;
    v->revision = d;
    v->value = ((UINT64)a << 48) | ((UINT64)b << 32) | ((UINT64)c << 16) | d;
    return S_OK;
}

static const void *package_header_vtbl[] =
{
    OBJ_METHODS, ph_get_ContentId, ph_get_ProductId, ph_get_BuildId, ph_get_BlobSize, ph_get_Version
};

HRESULT WINAPI pi_QueryDetails( struct factory *f, IInspectable **out )
{
    struct package_details *d;
    struct installed_game g;
    BOOL pending;

    /* Paused installs take the client's package-metadata path that Streaming bypasses. The queued
     * package exists before it is registered; returning GP_E_NO_SERVICE here drops its queue row. */
    pending = !package_game( f, &g );
    if (pending && !queued_game( f, &g )) return generic_slot( f, 13, NULL, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    TRACE( "Package(%s).QueryDetails -> %s\n", debugstr_w( f->source ), pending ? "pending" : "installed" );
    if (!(d = object_create( sizeof(*d), package_details_vtbl, &IID_IPackageDetails,
                             L"Microsoft.GamePlatform.Services.PackageDetails", NULL )))
        return E_OUTOFMEMORY;
    d->g = g;
    d->pending = pending;
    *out = (IInspectable *)d;
    return S_OK;
}

HRESULT WINAPI pi_QueryHeader( struct factory *f, IInspectable **out )
{
    struct package_details *d;
    struct installed_game g;
    struct product product;

    if (!package_game( f, &g )) return generic_slot( f, 12, NULL, __builtin_return_address(0) );
    installed_game_clear( &g );
    if (!out) return E_POINTER;
    if (!(d = object_create( sizeof(*d), package_header_vtbl, &IID_IPackageHeader,
                             L"Microsoft.GamePlatform.Services.PackageHeader", NULL )))
        return E_OUTOFMEMORY;
    d->g = g;
    query_product( g.store_id, &product );
    d->content_id = product.content_id;
    *out = (IInspectable *)d;
    return S_OK;
}

/* ---- PackageUpdate ----
 * Update-availability queries must succeed even when no update is available. Feature changes use an explicit
 * PackageRequest; a temporary PackagePreview must not advertise a package update. */

static const GUID IID_IPackageUpdate = {0x6afcfc17,0xa4a4,0x5293,{0xae,0x0d,0x32,0x38,0x0d,0x47,0x8e,0x60}};
static const GUID IID_AsyncOp_PackageUpdate = {0x297569b9,0x7e29,0x5534,{0xa9,0x93,0xf2,0x57,0x0b,0x81,0x32,0xbe}};
static const GUID IID_IVectorView_PackageUpdate = {0x63d04add,0xa47c,0x5978,{0x93,0x3a,0x58,0xc2,0x52,0x4d,0x9f,0x30}};
static const GUID IID_IIterable_PackageUpdate = {0x3b76b432,0xcc28,0x59d5,{0x8b,0xdf,0x06,0xc1,0x6f,0x2d,0x9e,0xb4}};
static const GUID IID_IIterator_PackageUpdate = {0x1e40d83e,0x7caa,0x5f07,{0xa6,0x66,0x66,0xc6,0x01,0xda,0x2f,0xa1}};
static const GUID IID_AsyncOp_PackageUpdateView = {0x939df71b,0xd0cc,0x5681,{0x8a,0x21,0xdc,0x19,0x1c,0x3b,0xe5,0xcd}};

struct package_update
{
    struct object obj;
    WCHAR store_id[64], instance[40];
    UINT64 size;
    INT32 status;
};

static HRESULT WINAPI pu_get_Status( struct package_update *u, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = u->status;
    TRACE( "PackageUpdate.Status(%s) -> %d\n", debugstr_w( u->store_id ), *v );
    return S_OK;
}

static HRESULT WINAPI pu_get_InstanceId( struct package_update *u, HSTRING *v )
{
    TRACE( "PackageUpdate.InstanceId(%s) -> %s\n", debugstr_w( u->store_id ), debugstr_w( u->instance ) );
    return hstr( u->instance, v );
}

static HRESULT WINAPI pu_get_AvailableDate( struct package_update *u, INT64 *v )
{
    if (!v) return E_POINTER;
    *v = 0;
    TRACE( "PackageUpdate.AvailableDate(%s) -> 0\n", debugstr_w( u->store_id ) );
    return S_OK;
}

static HRESULT WINAPI pu_QuerySize( struct package_update *u, IInspectable **v )
{
    if (!v) return E_POINTER;
    TRACE( "PackageUpdate.QuerySize(%s) -> %I64u\n", debugstr_w( u->store_id ), u->size );
    return (*v = request_size_create( u->size, TRUE )) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI pu_StartUpdateAsync( struct package_update *u, INT32 priority, HSTRING cv, IInspectable **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

static const void *package_update_vtbl[] =
{
    OBJ_METHODS, pu_get_Status, pu_get_InstanceId, pu_get_AvailableDate, pu_QuerySize, pu_StartUpdateAsync
};

static struct package_update *package_update_for( HSTRING source )
{
    struct package_update *u;
    struct installed_game g;

    if (!find_installed( WindowsGetStringRawBuffer( source, NULL ), &g )) return NULL;
    TRACE( "PackageUpdate.QueryUpdate(%s) -> no update\n", debugstr_w( g.store_id ) );
    if ((u = object_create( sizeof(*u), package_update_vtbl, &IID_IPackageUpdate,
                            L"Microsoft.GamePlatform.Services.PackageUpdate", NULL )))
    {
        lstrcpynW( u->store_id, g.store_id, ARRAY_SIZE(u->store_id) );
        lstrcpynW( u->instance, g.instance, ARRAY_SIZE(u->instance) );
        u->status = 2; /* NoUpdateAvailable */
    }
    installed_game_clear( &g );
    return u;
}

HRESULT WINAPI pu_QueryUpdate( struct factory *f, HSTRING source, INT32 type, boolean related, IInspectable **out )
{
    struct package_update *u;

    if (!out) return E_POINTER;
    TRACE( "PackageUpdate.QueryUpdate source=%s type=%d related=%d\n", debugstr_hstring( source ), type, related );
    if (!(u = package_update_for( source )))
    {
        *out = NULL;
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    *out = (IInspectable *)u;
    return S_OK;
}

HRESULT WINAPI pu_QueryUpdateAsync( struct factory *f, HSTRING source, INT32 type, boolean related, IInspectable **out )
{
    struct package_update *u;
    struct async *op;

    if (!out) return E_POINTER;
    *out = NULL;
    TRACE( "PackageUpdate.QueryUpdateAsync source=%s type=%d related=%d\n", debugstr_hstring( source ), type, related );
    if (!(u = package_update_for( source ))) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (!(op = async_create( &IID_AsyncOp_PackageUpdate, FALSE )))
    {
        obj_Release( &u->obj );
        return E_OUTOFMEMORY;
    }
    async_finish( op, S_OK, (IUnknown *)u );
    *out = async_iface( op );
    return S_OK;
}

static HRESULT query_updates_async( INT32 filter, IInspectable **out )
{
    struct obj_view *updates;
    struct async *op;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!(updates = obj_view_create( &IID_IVectorView_PackageUpdate, &IID_IIterable_PackageUpdate, &IID_IIterator_PackageUpdate,
                                     L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageUpdate>" )))
        return E_OUTOFMEMORY;
    if (!(op = async_create( &IID_AsyncOp_PackageUpdateView, FALSE )))
    {
        IInspectable_Release( (IInspectable *)updates );
        return E_OUTOFMEMORY;
    }
    async_finish( op, S_OK, (IUnknown *)updates );
    *out = async_iface( op );
    TRACE( "PackageUpdate.QueryUpdatesAsync(filter %d) -> 0\n", filter );
    return S_OK;
}

HRESULT WINAPI pu_QueryUpdatesAsync( struct factory *f, IInspectable **out ) { return query_updates_async( 0, out ); }
HRESULT WINAPI pu_QueryUpdatesAsyncFiltered( struct factory *f, INT32 filter, IInspectable **out ) { return query_updates_async( filter, out ); }

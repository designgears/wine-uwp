/*
 * GDK game runtime: entry points and XPackage
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
#include <wctype.h>

#include "private.h"
#include "winnls.h"
#include "wine/debug.h"
#include "wine/json.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdk);

/*
 * XPackage answers for the installed package only. Chunk availability comes from the installer's record of what it
 * laid out (.wine-uwp-chunks.json in the stage, written by appxinstall), so a title skips the optional chunks that are
 * not installed instead of reading them.
 */

static HMODULE instance;

HRESULT copy_string( const char *value, SIZE_T size, char *buffer )
{
    size_t len = strlen( value );

    if (!buffer) return E_POINTER;
    if (size <= len) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( buffer, value, len + 1 );
    return S_OK;
}

BOOL for_stage_dirs( stage_dir_callback callback, void *context )
{
    WCHAR path[MAX_PATH * 4], *slash;
    DWORD count;
    int i;

    count = GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", path, ARRAY_SIZE(path) );
    if (count && count < ARRAY_SIZE(path) && callback( path, context )) return TRUE;

    /* a copy of this DLL in stage/ or stage/game/, when the launcher's environment was lost */
    if (!GetModuleFileNameW( instance, path, ARRAY_SIZE(path) )) return FALSE;
    for (i = 0; i < 2; i++)
    {
        if (!(slash = wcsrchr( path, '\\' ))) return FALSE;
        *slash = 0;
        if (callback( path, context )) return TRUE;
    }
    return FALSE;
}

static struct json *metadata;
static char package_id[128];
static INIT_ONCE package_once = INIT_ONCE_STATIC_INIT;
static HRESULT package_status = E_UNEXPECTED;

static const char *package_name(void)
{
    return json_str( json_get( metadata, "package" ) );
}

/* A package identifier is what XPackageGetCurrentProcessPackageIdentifier answers and every other XPackage call takes
 * back: at most XPACKAGE_IDENTIFIER_MAX_LENGTH (33) bytes with the terminator, so it cannot be the package full name.
 * For an MSIXVC it is the content id, 32 hex digits. The installer records it as "identifier"; older installs have it
 * only in the layout file's name, layout_<content id>.xml. Halo's campaign asks with a 33-byte buffer, and a refusal
 * there makes it skip every availability query and then read optional HD modules that were never installed. */
static void find_content_identifier( const WCHAR *dir )
{
    const char *id = json_str( json_get( metadata, "identifier" ) );
    WIN32_FIND_DATAW data;
    WCHAR pattern[MAX_PATH * 4];
    const WCHAR *p;
    HANDLE find;
    int len = 0;

    if (!*id)
    {
        swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\layout_*.xml", dir );
        if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
        {
            for (p = data.cFileName + 7; *p && *p != '.' && len < 32; p++)
                if (iswxdigit( *p )) package_id[len++] = towupper( *p );
            FindClose( find );
        }
        package_id[len < 32 ? 0 : 32] = 0;
        id = package_id;
    }
    if (!*id) id = package_name();
    lstrcpynA( package_id, id, sizeof(package_id) );
    TRACE( "package identifier %s\n", debugstr_a(package_id) );
}

static BOOL load_chunks_json( const WCHAR *dir, void *context )
{
    WCHAR path[MAX_PATH * 4];
    LARGE_INTEGER size;
    struct json *parsed;
    HANDLE file;
    DWORD got = 0;
    char *data;
    BOOL ok;

    swprintf( path, ARRAY_SIZE(path), L"%s\\.wine-uwp-chunks.json", dir );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx( file, &size ) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024 ||
        !(data = malloc( size.QuadPart )))
    {
        CloseHandle( file );
        return FALSE;
    }
    ok = ReadFile( file, data, size.QuadPart, &got, NULL ) && got == size.QuadPart;
    CloseHandle( file );
    parsed = ok ? json_parse( data, got ) : NULL;
    free( data );
    if (!parsed) return FALSE;
    if (json_get( parsed, "version" )->number != 1 || json_get( parsed, "regions" )->type != JSON_ARRAY ||
        !*json_str( json_get( parsed, "package" ) ))
    {
        json_free( parsed );
        return FALSE;
    }
    metadata = parsed;
    find_content_identifier( dir );
    TRACE( "%u recorded regions in %s\n", json_count( json_get( metadata, "regions" ) ), debugstr_w(dir) );
    return TRUE;
}

static BOOL CALLBACK load_metadata( INIT_ONCE *once, void *param, void **context )
{
    if (for_stage_dirs( load_chunks_json, NULL )) package_status = S_OK;
    else
    {
        WARN( "no .wine-uwp-chunks.json\n" );
        package_status = HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
    }
    return TRUE;
}

HRESULT package_initialize(void)
{
    if (!InitOnceExecuteOnce( &package_once, load_metadata, NULL, NULL )) return HRESULT_FROM_WIN32( GetLastError() );
    return package_status;
}

static BOOL is_this_package( const char *id )
{
    return !id || !_stricmp( id, package_id ) || !_stricmp( id, package_name() );
}

/* Layout/XVC tag specs use "a#b" when a chunk needs both install tags. A bare Tag selector matches any component;
 * Feature matching treats the first component as the base feature and later ones as optional add-ons (so launch#hd1
 * belongs to HD1, not Launch). */
static BOOL component_matches( const char *spec, const char *wanted, BOOL optional_only )
{
    size_t len = strlen( wanted );
    const char *part = spec, *hash;

    if (!strchr( spec, '#' )) return !_stricmp( spec, wanted );
    if (!optional_only && !_stricmp( spec, wanted )) return TRUE;
    if (optional_only) part = strchr( spec, '#' ) + 1;
    for (;;)
    {
        hash = strchr( part, '#' );
        if ((hash ? hash - part : strlen( part )) == len && len && !_strnicmp( part, wanted, len )) return TRUE;
        if (!hash) return FALSE;
        part = hash + 1;
    }
}

static BOOL tag_matches( const struct json *array, const char *wanted, BOOL optional_only )
{
    unsigned int i;

    if (!wanted) return FALSE;
    for (i = 0; i < json_count( array ); i++)
        if (component_matches( json_str( json_at( array, i ) ), wanted, optional_only )) return TRUE;
    return FALSE;
}

static BOOL language_allowed( const struct json *region )
{
    const struct json *languages = json_get( region, "languages" );
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    char full[LOCALE_NAME_MAX_LENGTH * 4];
    const char *lang, *dash;
    size_t base_len;
    unsigned int i;

    if (!json_count( languages )) return TRUE;
    if (!GetUserDefaultLocaleName( locale, ARRAY_SIZE(locale) )) return TRUE;
    if (!WideCharToMultiByte( CP_UTF8, 0, locale, -1, full, sizeof(full), NULL, NULL )) return TRUE;
    base_len = (dash = strchr( full, '-' )) ? dash - full : strlen( full );
    for (i = 0; i < json_count( languages ); i++)
    {
        lang = json_str( json_at( languages, i ) );
        if (!_stricmp( lang, full )) return TRUE;
        dash = strchr( lang, '-' );
        if ((size_t)(dash ? dash - lang : strlen( lang )) == base_len && !_strnicmp( lang, full, base_len )) return TRUE;
    }
    return FALSE;
}

enum { XPackageChunkSelectorType_Language, XPackageChunkSelectorType_Tag, XPackageChunkSelectorType_Chunk,
       XPackageChunkSelectorType_Feature };
enum { XPackageChunkAvailability_Ready, XPackageChunkAvailability_Pending, XPackageChunkAvailability_Installable,
       XPackageChunkAvailability_Unavailable };
enum { XPackageKind_Game, XPackageKind_Content, XPackageKind_PublisherContent };

typedef struct
{
    UINT32 type;
    union
    {
        const char *language;
        const char *tag;
        UINT32 chunkId;
        const char *feature;
    };
} XPackageChunkSelector;

typedef struct
{
    const char *packageIdentifier;
    UINT16 version[4];
    UINT32 kind;
    const char *displayName;
    const char *description;
    const char *publisher;
    const char *storeId;
    BOOLEAN installing;
    UINT32 index;
    UINT32 count;
    BOOLEAN ageRestricted;
    const char *titleID;
} XPackageDetails;

typedef struct
{
    const char *id;
    const char *displayName;
    const char *tags;
    BOOLEAN hidden;
    UINT32 storeIdCount;
    const char **storeIds;
} XPackageFeature;

typedef BOOLEAN (CALLBACK *XPackageChunkAvailabilityCallback)( void *context, const XPackageChunkSelector *selector,
                                                               UINT32 availability );
typedef BOOLEAN (CALLBACK *XPackageEnumerationCallback)( void *context, const XPackageDetails *details );
typedef BOOLEAN (CALLBACK *XPackageFeatureEnumerationCallback)( void *context, const XPackageFeature *feature );

static BOOL region_matches( const struct json *region, const XPackageChunkSelector *selector )
{
    const struct json *features = json_get( metadata, "features" ), *feature, *tags;
    unsigned int i, j;

    switch (selector->type)
    {
    case XPackageChunkSelectorType_Chunk:
        return json_get( region, "id" )->number == selector->chunkId;
    case XPackageChunkSelectorType_Tag:
        return language_allowed( region ) && tag_matches( json_get( region, "tags" ), selector->tag, FALSE );
    case XPackageChunkSelectorType_Language:
        return tag_matches( json_get( region, "languages" ), selector->language, FALSE );
    case XPackageChunkSelectorType_Feature:
        if (!selector->feature || !language_allowed( region )) return FALSE;
        for (i = 0; i < json_count( features ); i++)
        {
            feature = json_at( features, i );
            if (_stricmp( json_str( json_get( feature, "id" ) ), selector->feature )) continue;
            tags = json_get( feature, "tags" );
            for (j = 0; j < json_count( tags ); j++)
                if (tag_matches( json_get( region, "tags" ), json_str( json_at( tags, j ) ), TRUE )) return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

static HRESULT WINAPI pkg_QueryInterface( gdk_object *iface, REFIID iid, void **out );
static ULONG WINAPI pkg_AddRef( gdk_object *iface ) { return 2; }
static ULONG WINAPI pkg_Release( gdk_object *iface ) { return 1; }

static HRESULT WINAPI pkg_GetCurrentProcessPackageIdentifier( gdk_object *iface, SIZE_T size, char *buffer )
{
    HRESULT hr = package_initialize();

    if (SUCCEEDED(hr)) hr = copy_string( package_id, size, buffer );
    TRACE( "size %Iu -> %s, hr %#lx\n", size, debugstr_a(package_id), hr );
    return hr;
}

static BOOLEAN WINAPI pkg_IsPackagedProcess( gdk_object *iface )
{
    BOOLEAN ok = SUCCEEDED(package_initialize());
    TRACE( "-> %u\n", ok );
    return ok;
}

static HRESULT WINAPI pkg_CreateInstallationMonitor( gdk_object *iface, const char *id, UINT32 count,
                                                     const XPackageChunkSelector *selectors, UINT32 interval,
                                                     XTaskQueueHandle queue, void **monitor )
{
    FIXME( "not implemented\n" );
    if (monitor) *monitor = NULL;
    return E_NOTIMPL;
}

static void WINAPI pkg_CloseInstallationMonitorHandle( gdk_object *iface, void *monitor )
{
    FIXME( "not implemented\n" );
}

static void WINAPI pkg_GetInstallationProgress( gdk_object *iface, void *monitor, void *progress )
{
    /* XPackageInstallationProgress: three UINT64 and two BOOLEANs */
    FIXME( "not implemented\n" );
    if (progress) memset( progress, 0, 32 );
}

static BOOLEAN WINAPI pkg_UpdateInstallationMonitor( gdk_object *iface, void *monitor )
{
    FIXME( "not implemented\n" );
    return FALSE;
}

static HRESULT WINAPI pkg_RegisterInstallationProgressChanged( gdk_object *iface, void *monitor, void *context,
                                                               void *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "not implemented\n" );
    if (token) token->token = 0;
    return E_NOTIMPL;
}

static BOOLEAN WINAPI pkg_UnregisterInstallationProgressChanged( gdk_object *iface, void *monitor,
                                                                 XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "not implemented\n" );
    return FALSE;
}

static HRESULT WINAPI pkg_GetUserLocale( gdk_object *iface, SIZE_T size, char *buffer )
{
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    char text[LOCALE_NAME_MAX_LENGTH * 4];

    TRACE( "size %#Ix buffer %p\n", size, buffer );
    if (!GetUserDefaultLocaleName( locale, ARRAY_SIZE(locale) )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!WideCharToMultiByte( CP_UTF8, 0, locale, -1, text, sizeof(text), NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    return copy_string( text, size, buffer );
}

static HRESULT WINAPI pkg_FindChunkAvailability( gdk_object *iface, const char *id, UINT32 count,
                                                 const XPackageChunkSelector *selectors, UINT32 *availability )
{
    const struct json *regions, *region;
    BOOL found = FALSE, ready = TRUE, selected;
    unsigned int i, j;
    HRESULT hr;

    if (!availability || (count && !selectors)) return E_POINTER;
    *availability = XPackageChunkAvailability_Unavailable;
    if (FAILED(hr = package_initialize())) return hr;
    if (!is_this_package( id )) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    regions = json_get( metadata, "regions" );
    for (i = 0; i < json_count( regions ); i++)
    {
        region = json_at( regions, i );
        selected = !count;
        for (j = 0; j < count; j++) selected |= region_matches( region, &selectors[j] );
        if (!selected) continue;
        found = TRUE;
        ready &= json_get( region, "ready" )->boolean;
    }
    if (found) *availability = ready ? XPackageChunkAvailability_Ready : XPackageChunkAvailability_Installable;

    if (TRACE_ON(gdk))
    {
        for (j = 0; j < count; j++)
        {
            const XPackageChunkSelector *s = &selectors[j];
            if (s->type == XPackageChunkSelectorType_Feature) TRACE( "  feature %s\n", debugstr_a(s->feature) );
            else if (s->type == XPackageChunkSelectorType_Tag) TRACE( "  tag %s\n", debugstr_a(s->tag) );
            else if (s->type == XPackageChunkSelectorType_Chunk) TRACE( "  chunk %u\n", s->chunkId );
            else TRACE( "  type %u\n", s->type );
        }
        TRACE( "%u selectors -> %u\n", count, *availability );
    }
    return S_OK;
}

static HRESULT WINAPI pkg_EnumerateChunkAvailability( gdk_object *iface, const char *id, UINT32 type, void *context,
                                                      XPackageChunkAvailabilityCallback callback )
{
    const struct json *regions = json_get( metadata, "regions" );
    XPackageChunkSelector selector;
    UINT32 availability;
    unsigned int i;
    HRESULT hr;

    TRACE( "type %u\n", type );
    if (!callback) return E_POINTER;
    if (type != XPackageChunkSelectorType_Chunk) return E_NOTIMPL;
    for (i = 0; i < json_count( regions ); i++)
    {
        memset( &selector, 0, sizeof(selector) );
        selector.type = type;
        selector.chunkId = json_get( json_at( regions, i ), "id" )->number;
        if (FAILED(hr = pkg_FindChunkAvailability( iface, id, 1, &selector, &availability ))) return hr;
        if (!callback( context, &selector, availability )) break;
    }
    return S_OK;
}

static HRESULT WINAPI pkg_notimpl( gdk_object *iface )
{
    FIXME( "not implemented\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI pkg_InstallChunks( gdk_object *iface, const char *id, UINT32 count,
                                         XPackageChunkSelector *selectors, UINT32 interval, BOOLEAN suppress,
                                         XTaskQueueHandle queue, void **monitor )
{
    FIXME( "not implemented\n" );
    if (monitor) *monitor = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI pkg_InstallChunksResult( gdk_object *iface, XAsyncBlock *block, void **monitor )
{
    FIXME( "not implemented\n" );
    if (monitor) *monitor = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI pkg_EstimateDownloadSize( gdk_object *iface, const char *id, UINT32 count,
                                                const XPackageChunkSelector *selectors, UINT64 *size,
                                                BOOLEAN *confirm )
{
    FIXME( "not implemented\n" );
    if (size) *size = 0;
    if (confirm) *confirm = FALSE;
    return E_NOTIMPL;
}

static BOOLEAN WINAPI pkg_UnregisterPackageInstalled( gdk_object *iface, XTaskQueueRegistrationToken token,
                                                      BOOLEAN wait )
{
    FIXME( "not implemented\n" );
    return FALSE;
}

static HRESULT WINAPI pkg_GetMountPathSize( gdk_object *iface, void *mount, SIZE_T *size )
{
    FIXME( "not implemented\n" );
    if (size) *size = 0;
    return E_NOTIMPL;
}

static void WINAPI pkg_CloseMountHandle( gdk_object *iface, void *mount )
{
    FIXME( "not implemented\n" );
}

static HRESULT WINAPI pkg_EnumeratePackages( gdk_object *iface, UINT32 kind, UINT32 scope, void *context,
                                             XPackageEnumerationCallback callback )
{
    XPackageDetails details;
    HRESULT hr;

    TRACE( "kind %u scope %u\n", kind, scope );
    if (!callback) return E_POINTER;
    if (FAILED(hr = package_initialize())) return hr;
    /* Some titles enumerate Content (DLC) at start-up. Optional HD and feature chunks are Features of the Game
     * package, not Content packages, so there are no Content rows; the Game package is reported when asked for, so
     * availability and feature probes have an identity. */
    if (kind != XPackageKind_Game && kind != XPackageKind_PublisherContent) return S_OK;
    if (!*package_id) return S_OK;
    memset( &details, 0, sizeof(details) );
    details.packageIdentifier = package_id;
    details.kind = XPackageKind_Game;
    details.displayName = package_id;
    details.description = "";
    details.publisher = "Microsoft Studios";
    details.storeId = "";
    details.count = 1;
    details.titleID = "";
    callback( context, &details );
    return S_OK;
}

static HRESULT WINAPI pkg_RegisterPackageInstalled( gdk_object *iface, XTaskQueueHandle queue, void *context,
                                                    void *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "\n" );
    if (token) token->token = 1;
    return S_OK;
}

static HRESULT WINAPI pkg_GetWriteStats( gdk_object *iface, UINT64 *stats )
{
    FIXME( "not implemented\n" );
    if (stats) memset( stats, 0, 4 * sizeof(UINT64) );
    return E_NOTIMPL;
}

static HRESULT WINAPI pkg_EnumerateFeatures( gdk_object *iface, const char *id, void *context,
                                             XPackageFeatureEnumerationCallback callback )
{
    const struct json *features = json_get( metadata, "features" ), *feature, *tags;
    XPackageFeature value;
    char buf[1024];
    unsigned int i, j;
    size_t len;

    TRACE( "%u features\n", json_count( features ) );
    if (!callback) return E_POINTER;
    for (i = 0; i < json_count( features ); i++)
    {
        feature = json_at( features, i );
        tags = json_get( feature, "tags" );
        buf[0] = 0;
        for (j = len = 0; j < json_count( tags ) && len < sizeof(buf) - 1; j++)
            len += snprintf( buf + len, sizeof(buf) - len, "%s%s", j ? ";" : "", json_str( json_at( tags, j ) ) );
        memset( &value, 0, sizeof(value) );
        value.id = json_str( json_get( feature, "id" ) );
        value.displayName = json_str( json_get( feature, "name" ) );
        value.tags = buf;
        value.hidden = json_get( feature, "hidden" )->boolean;
        TRACE( "feature %s tags %s hidden %u\n", value.id, buf, value.hidden );
        if (!callback( context, &value )) break;
    }
    return S_OK;
}

static BOOLEAN WINAPI pkg_UninstallPackage( gdk_object *iface, const char *id )
{
    FIXME( "not implemented\n" );
    return FALSE;
}

static HRESULT WINAPI pkg_MountWithUiResult( gdk_object *iface, XAsyncBlock *block, void **mount )
{
    FIXME( "not implemented\n" );
    if (mount) *mount = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI pkg_GetKind( gdk_object *iface, const char *id, UINT32 *kind )
{
    if (!kind) return E_POINTER;
    *kind = XPackageKind_Game;
    if (!is_this_package( id )) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    return S_OK;
}

/* IXPackageImpl4: IXPackageImpl's methods, then Impl2's, Impl3's and Impl4's appended */
static const void *const package_vtbl[] =
{
    pkg_QueryInterface,
    pkg_AddRef,
    pkg_Release,
    pkg_GetCurrentProcessPackageIdentifier,
    pkg_IsPackagedProcess,
    pkg_CreateInstallationMonitor,
    pkg_CloseInstallationMonitorHandle,
    pkg_GetInstallationProgress,
    pkg_UpdateInstallationMonitor,
    pkg_RegisterInstallationProgressChanged,
    pkg_UnregisterInstallationProgressChanged,
    pkg_GetUserLocale,
    pkg_FindChunkAvailability,
    pkg_EnumerateChunkAvailability,
    pkg_notimpl,                    /* ChangeChunkInstallOrder */
    pkg_InstallChunks,
    pkg_notimpl,                    /* InstallChunksAsync */
    pkg_InstallChunksResult,
    pkg_EstimateDownloadSize,
    pkg_notimpl,                    /* UninstallChunks */
    pkg_notimpl,                    /* padding */
    pkg_notimpl,                    /* padding */
    pkg_UnregisterPackageInstalled,
    pkg_notimpl,                    /* padding */
    pkg_GetMountPathSize,
    pkg_notimpl,                    /* GetMountPath */
    pkg_CloseMountHandle,
    pkg_notimpl,                    /* padding */
    pkg_EnumeratePackages,
    pkg_RegisterPackageInstalled,
    pkg_GetWriteStats,
    pkg_notimpl,                    /* padding */
    pkg_notimpl,                    /* UninstallUWPInstance */
    pkg_EnumerateFeatures,
    pkg_UninstallPackage,
    /* Impl2 */
    pkg_EnumeratePackages,
    pkg_RegisterPackageInstalled,
    pkg_notimpl,                    /* MountWithUiAsync */
    pkg_MountWithUiResult,
    /* Impl3 */
    pkg_EnumeratePackages,
    pkg_RegisterPackageInstalled,
    /* Impl4 */
    pkg_GetKind,
};

static gdk_object package = { package_vtbl };

static const GUID IID_IXPackageImpl = {0x3720de07,0xe8e4,0x44a3,{0xad,0x32,0xb3,0x59,0xe8,0xad,0xbe,0x55}};
static const GUID IID_IXPackageImpl2 = {0xf92d8712,0x2b27,0x4d8a,{0xbf,0x01,0x11,0xa6,0xf8,0xe3,0xeb,0x42}};
static const GUID IID_IXPackageImpl3 = {0xe2a4734b,0x2f4a,0x456d,{0xaa,0x8f,0xd0,0x65,0xe0,0x4f,0xb2,0x09}};
static const GUID IID_IXPackageImpl4 = {0x0b9d556e,0xab70,0x43f4,{0xba,0xa0,0x99,0x99,0xac,0x4c,0xb0,0x40}};

static HRESULT WINAPI pkg_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ) && !IsEqualGUID( iid, &IID_IXPackageImpl ) &&
        !IsEqualGUID( iid, &IID_IXPackageImpl2 ) && !IsEqualGUID( iid, &IID_IXPackageImpl3 ) &&
        !IsEqualGUID( iid, &IID_IXPackageImpl4 ))
        return E_NOINTERFACE;
    *out = iface;
    return S_OK;
}

HRESULT query_xpackage( REFIID iid, void **out )
{
    return pkg_QueryInterface( &package, iid, out );
}

/*
 * Every other service GdkSupport asks for. A CLASS_E_CLASSNOTAVAILABLE for one of them becomes 0x89240107 "missing
 * required dependencies", and the title then skips its XPackage availability checks; until a service is
 * implemented, it gets this object, which fails every method. Slot 3 must be a BOOLEAN FALSE: a service whose first
 * method returns BOOLEAN read E_NOTIMPL as TRUE. The methods take only the object pointer, so each touches nothing
 * but a register whatever the real method's arguments were.
 */

static HRESULT WINAPI stub_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = iface;
    return S_OK;
}

static ULONG WINAPI stub_AddRef( gdk_object *iface ) { return 2; }
static ULONG WINAPI stub_Release( gdk_object *iface ) { return 1; }
static HRESULT WINAPI stub_hresult( gdk_object *iface ) { return E_NOTIMPL; }
static void WINAPI stub_void( gdk_object *iface ) {}
static BOOLEAN WINAPI stub_boolean( gdk_object *iface ) { return FALSE; }

static const void *const stub_vtbl[] =
{
    stub_QueryInterface, stub_AddRef, stub_Release,
    stub_boolean, stub_hresult, stub_void, stub_hresult,
    stub_hresult, stub_hresult, stub_hresult, stub_void,
    stub_hresult, stub_hresult, stub_hresult, stub_hresult,
    stub_hresult, stub_boolean, stub_void, stub_hresult,
    stub_hresult, stub_hresult, stub_void, stub_hresult,
    stub_hresult, stub_void, stub_hresult, stub_void,
    stub_boolean, stub_void, stub_hresult, stub_boolean,
    stub_void, stub_boolean, stub_hresult, stub_hresult,
    stub_hresult, stub_hresult, stub_hresult, stub_hresult,
    stub_hresult, stub_hresult, stub_hresult, stub_hresult,
};

static gdk_object stub_object = { stub_vtbl };

static const GUID CLSID_XPackageImpl = {0xaf406016,0xe850,0x4aa8,{0xa8,0x8d,0x2f,0x3d,0xcb,0x9d,0xac,0x7e}};
static const GUID CLSID_XUserImpl = {0x01acd177,0x91f9,0x4763,{0xa3,0x8e,0xcc,0xbb,0x55,0xce,0x32,0xe0}};
static const GUID CLSID_XStoreImpl = {0x0dd112ac,0x7c24,0x448c,{0xb9,0x2b,0x39,0x60,0xfb,0x5b,0xd3,0x0c}};
static const GUID CLSID_XThreadingImpl = {0x073b7dcb,0x1fcf,0x4030,{0x94,0xbe,0xe3,0xc9,0xeb,0x62,0x34,0x28}};
static const GUID CLSID_XGameImpl = {0x973a344e,0x24bf,0x4d0f,{0x84,0x57,0x56,0xc5,0x34,0x89,0x2b,0x29}};
static const GUID CLSID_XSystemImpl = {0xe349bd1a,0xfc20,0x4e40,{0xb9,0x9c,0x41,0x78,0xcc,0x6b,0x40,0x9f}};
static const GUID CLSID_XGameRuntimeFeatureImpl = {0x8836fe87,0xedb9,0x4fe3,{0x8d,0xad,0x05,0xf0,0xd2,0xcd,0x5b,0x40}};
static const GUID CLSID_XNetworkingImpl = {0x37e56907,0x2f10,0x41e8,{0xb7,0x2f,0x36,0xed,0xb1,0x85,0x33,0x1a}};

HRESULT WINAPI QueryApiImpl( REFCLSID clsid, REFIID iid, void **out )
{
    static DWORD seen[32];
    static unsigned int seen_count;
    BOOL first = TRUE;
    unsigned int i;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = package_initialize()))
    {
        WARN( "initialization failed, hr %#lx\n", hr );
        return hr;
    }
    for (i = 0; i < seen_count; i++) if (seen[i] == clsid->Data1) first = FALSE;
    if (first && seen_count < ARRAY_SIZE(seen)) seen[seen_count++] = clsid->Data1;
    if (first || IsEqualGUID( clsid, &CLSID_XPackageImpl )) TRACE( "%s %s\n", debugstr_guid(clsid), debugstr_guid(iid) );

    if (IsEqualGUID( clsid, &CLSID_XPackageImpl )) return query_xpackage( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XUserImpl )) return query_xuser( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XStoreImpl )) return query_xstore( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XThreadingImpl )) return query_xthreading( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameImpl )) return query_xgame( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XSystemImpl )) return query_xsystem( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XGameRuntimeFeatureImpl )) return query_xfeature( iid, out );
    if (IsEqualGUID( clsid, &CLSID_XNetworkingImpl )) return query_xnetworking( iid, out );
    /* any IID: GdkSupport often passes the coclass's UUID as the IID */
    *out = &stub_object;
    return S_OK;
}

HRESULT WINAPI InitializeApiImplEx2( ULONG a, ULONG b, char c, const void *d )
{
    HRESULT hr = package_initialize();

    TRACE( "%lu, %lu, %u, %p\n", a, b, c, d );
    /* start the title's sign-in early, so the first token request is answered from the cache */
    if (SUCCEEDED(hr)) warm_xboxauth_title_context();
    return hr;
}

HRESULT WINAPI InitializeApiImplEx( ULONG a, ULONG b, char c )
{
    return InitializeApiImplEx2( a, b, c, NULL );
}

HRESULT WINAPI InitializeApiImpl( ULONG a, ULONG b )
{
    return InitializeApiImplEx( a, b, 0 );
}

void WINAPI UninitializeApiImpl(void)
{
    TRACE( "\n" );
}

/* GdkSupport looks this up in XGameRuntimeInitialize; without it, it gives up before InitializeApiImpl. */
HRESULT WINAPI XErrorReport( HRESULT status, const char *message )
{
    TRACE( "%#lx %s\n", status, debugstr_a(message) );
    return S_OK;
}

BOOLEAN WINAPI XGameRuntimeIsFeatureAvailable( UINT32 feature )
{
    BOOLEAN available = feature_available( feature );
    TRACE( "%u -> %u\n", feature, available );
    return available;
}

HRESULT WINAPI XGameRuntimeInitialize(void)
{
    HRESULT hr = package_initialize();

    TRACE( "\n" );
    if (SUCCEEDED(hr)) warm_xboxauth_title_context();
    return hr;
}

void WINAPI XGameRuntimeUninitialize(void)
{
    TRACE( "\n" );
}

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        instance = inst;
        DisableThreadLibraryCalls( inst );
        /* read the package record now, so a late or missing InitializeApiImpl still finds it */
        package_initialize();
    }
    return TRUE;
}

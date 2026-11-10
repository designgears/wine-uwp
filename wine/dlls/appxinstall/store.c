/*
 * The Store's public services: the display catalogue and the delivery service (FE3)
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
 * The catalogue maps a product or a package family to a Windows Update category; the delivery
 * service maps a category to its packages and a package to its download address. Neither needs an
 * account. The Store, the Xbox app and their stand-ins here use them to find what to install.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winhttp.h"
#include "objbase.h"

#include "wine/appxinstall.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxinstall);

struct strbuf
{
    char *data;
    size_t len, size;
    BOOL failed;
};

static void strbuf_append( struct strbuf *buf, const char *str, size_t len )
{
    char *data;
    size_t size;

    if (buf->failed) return;
    if (buf->len + len + 1 > buf->size)
    {
        size = max( buf->size * 2, buf->len + len + 1024 );
        if (!(data = realloc( buf->data, size )))
        {
            buf->failed = TRUE;
            return;
        }
        buf->data = data;
        buf->size = size;
    }
    memcpy( buf->data + buf->len, str, len );
    buf->len += len;
    buf->data[buf->len] = 0;
}

static void strbuf_cat( struct strbuf *buf, const char *str )
{
    strbuf_append( buf, str, strlen( str ) );
}

static void WINAPIV strbuf_printf( struct strbuf *buf, const char *format, ... )
{
    va_list args;
    char *str;
    int len;

    va_start( args, format );
    len = vsnprintf( NULL, 0, format, args );
    va_end( args );
    if (len < 0 || !(str = malloc( len + 1 )))
    {
        buf->failed = TRUE;
        return;
    }
    va_start( args, format );
    vsnprintf( str, len + 1, format, args );
    va_end( args );
    strbuf_append( buf, str, len );
    free( str );
}

static void strbuf_free( struct strbuf *buf )
{
    free( buf->data );
    memset( buf, 0, sizeof(*buf) );
}

/* Reads a response body; *status gets the HTTP status, 0 if there was none. */
static HRESULT http_request( const WCHAR *agent, const WCHAR *host, INTERNET_PORT port, BOOL secure, const WCHAR *verb,
                             const WCHAR *path, const WCHAR *headers, const char *body, DWORD timeout,
                             size_t limit, struct strbuf *out, DWORD *status )
{
    HINTERNET session, connection = NULL, request = NULL;
    DWORD size = sizeof(*status), avail, read;
    char buffer[8192];
    HRESULT hr = S_OK;

    *status = 0;
    if (!(session = WinHttpOpen( agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0 )) &&
        !(session = WinHttpOpen( agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0 )))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(connection = WinHttpConnect( session, host, port, 0 )) ||
        !(request = WinHttpOpenRequest( connection, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0 )))
        hr = HRESULT_FROM_WIN32( GetLastError() );
    else
    {
        WinHttpSetTimeouts( request, timeout, timeout, timeout * 3, timeout * 3 );
        if (!WinHttpSendRequest( request, headers, (DWORD)-1, (void *)body, body ? strlen( body ) : 0,
                                 body ? strlen( body ) : 0, 0 ) || !WinHttpReceiveResponse( request, NULL ))
            hr = HRESULT_FROM_WIN32( GetLastError() );
        else
        {
            WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, status, &size, NULL );
            while (WinHttpQueryDataAvailable( request, &avail ) && avail && out->len < limit)
            {
                if (!WinHttpReadData( request, buffer, min( avail, sizeof(buffer) ), &read ) || !read) break;
                strbuf_append( out, buffer, read );
            }
            if (out->failed) hr = E_OUTOFMEMORY;
        }
    }
    if (request) WinHttpCloseHandle( request );
    if (connection) WinHttpCloseHandle( connection );
    WinHttpCloseHandle( session );
    return hr;
}

HRESULT CDECL appx_store_get( const WCHAR *host, const WCHAR *path, char **body, DWORD *status )
{
    struct strbuf buf = {0};
    HRESULT hr;

    TRACE( "host %s, path %s.\n", debugstr_w( host ), debugstr_w( path ) );

    *body = NULL;
    hr = http_request( L"wineuwp-store/1.0", host, INTERNET_DEFAULT_HTTPS_PORT, TRUE, L"GET", path,
                       L"Accept: application/json\r\n", NULL, 15000, 32 << 20, &buf, status );
    if (FAILED(hr))
    {
        strbuf_free( &buf );
        return hr;
    }
    if (!buf.data) strbuf_append( &buf, "", 0 );
    if (buf.failed) return E_OUTOFMEMORY;
    *body = buf.data;
    return S_OK;
}

/* The delivery service */

static const char fe3_ns[] = "xmlns=\"http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService\"";
static const char fe3_attributes[] =
    "BranchReadinessLevel=CB;CurrentBranch=ge_release;OEMModel=Virtual Machine;FlightRing=Retail;AttrDataVer=21;"
    "SystemManufacturer=Microsoft Corporation;InstallLanguage=en-US;OSUILocale=en-US;InstallationType=Client;"
    "FlightingBranchName=external;OSSkuId=48;FlightContent=Branch;App=WU;AppVer=10.0.26100.1;OSArchitecture=AMD64;"
    "IsFlightingEnabled=0;IsDeviceRetailDemo=0;TelemetryLevel=3;OSVersion=10.0.26100.1;DeviceFamily=Windows.Desktop;";
static const char fe3_url[] = "https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx";

/* A connection to the delivery service sometimes times out (10060) where the next one goes through. */
static HRESULT fe3_post( const WCHAR *path, const char *body, struct strbuf *out, DWORD *status )
{
    HRESULT hr = E_FAIL;
    int attempt;

    for (attempt = 0; attempt < 3; attempt++)
    {
        strbuf_free( out );
        hr = http_request( L"Windows-Update-Agent/10.0.10011.16384 Client-Protocol/2.0", L"fe3.delivery.mp.microsoft.com",
                           INTERNET_DEFAULT_HTTPS_PORT, TRUE, L"POST", path,
                           L"Content-Type: application/soap+xml; charset=utf-8\r\n", body, 10000, (size_t)-1, out, status );
        if (*status) break;
        WARN( "FE3 POST %s failed, hr %#lx; retrying.\n", debugstr_w( path ), hr );
    }
    if (*status != 200) WARN( "FE3 POST %s: HTTP %lu.\n", debugstr_w( path ), *status );
    return *status == 200 ? S_OK : FAILED(hr) ? hr : E_FAIL;
}

static void soap_time( char *buffer, size_t size, int offset_minutes )
{
    ULARGE_INTEGER time;
    SYSTEMTIME st;
    FILETIME ft;

    GetSystemTimeAsFileTime( &ft );
    time.LowPart = ft.dwLowDateTime;
    time.HighPart = ft.dwHighDateTime;
    time.QuadPart += (LONGLONG)offset_minutes * 60 * 10000000;
    ft.dwLowDateTime = time.LowPart;
    ft.dwHighDateTime = time.HighPart;
    FileTimeToSystemTime( &ft, &st );
    snprintf( buffer, size, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
              st.wSecond );
}

static void soap_envelope( struct strbuf *buf, const char *action, const char *to, const char *body )
{
    char created[32], expires[32];
    GUID id;

    CoCreateGuid( &id );
    soap_time( created, sizeof(created), 0 );
    soap_time( expires, sizeof(expires), 5 );
    strbuf_printf( buf,
        "<s:Envelope xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><a:Action s:mustUnderstand=\"1\">http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService/%s"
        "</a:Action><a:MessageID>urn:uuid:%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x</a:MessageID>"
        "<a:To s:mustUnderstand=\"1\">%s</a:To>"
        "<o:Security s:mustUnderstand=\"1\" xmlns:o=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "<Created>%s</Created><Expires>%s</Expires></Timestamp>"
        "<wuws:WindowsUpdateTicketsToken wsu:id=\"ClientMSA\" "
        "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" "
        "xmlns:wuws=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\">"
        "<TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"></TicketType></wuws:WindowsUpdateTicketsToken>"
        "</o:Security></s:Header><s:Body>%s</s:Body></s:Envelope>",
        action, (unsigned long)id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
        id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7], to, created, expires, body );
}

/* Replaces the five XML entities in place. */
static void xml_unescape( char *str )
{
    static const struct { const char *name; char c; } entities[] =
    {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'},
    };
    char *out = str;
    unsigned int i;

    while (*str)
    {
        if (*str == '&')
        {
            for (i = 0; i < ARRAY_SIZE(entities); i++)
                if (!strncmp( str, entities[i].name, strlen( entities[i].name ) )) break;
            if (i < ARRAY_SIZE(entities))
            {
                *out++ = entities[i].c;
                str += strlen( entities[i].name );
                continue;
            }
        }
        *out++ = *str++;
    }
    *out = 0;
}

/* Copies the text between start and the next stop after it, or returns FALSE. */
static BOOL copy_between( const char *text, const char *start, const char *stop, char *out, size_t size )
{
    const char *begin, *end;

    if (!(begin = strstr( text, start ))) return FALSE;
    begin += strlen( start );
    if (!(end = strstr( begin, stop )) || end - begin >= size) return FALSE;
    memcpy( out, begin, end - begin );
    out[end - begin] = 0;
    return TRUE;
}

struct update_ids
{
    char (*ids)[24];
    unsigned int count, size;
};

static void add_id( struct update_ids *list, const char *id )
{
    if (list->count == list->size)
    {
        unsigned int size = max( 64, list->size * 2 );
        char (*ids)[24] = realloc( list->ids, size * sizeof(*ids) );
        if (!ids) return;
        list->ids = ids;
        list->size = size;
    }
    lstrcpynA( list->ids[list->count++], id, sizeof(list->ids[0]) );
}

static void append_ids( struct strbuf *buf, const struct update_ids *list )
{
    unsigned int i;
    for (i = 0; i < list->count; i++) strbuf_printf( buf, "<int>%s</int>", list->ids[i] );
}

static ULONGLONG moniker_version( const char *version )
{
    unsigned int v[4] = {0};
    sscanf( version, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3] );
    return ((ULONGLONG)v[0] << 48) | ((ULONGLONG)v[1] << 32) | ((ULONGLONG)v[2] << 16) | v[3];
}

/* "Name_Version_Arch_ResourceId_PublisherId": whether the package is the family's, x64 or neutral,
 * and its version. */
static BOOL moniker_matches( const char *moniker, const char *name, const char *publisher, ULONGLONG *version )
{
    size_t name_len = strlen( name ), len = strlen( moniker ), publisher_len = strlen( publisher );
    char ver[64], arch[32];
    const char *rest, *p;

    if (strncmp( moniker, name, name_len ) || moniker[name_len] != '_') return FALSE;
    if (len < publisher_len || strcmp( moniker + len - publisher_len, publisher )) return FALSE;
    rest = moniker + name_len + 1;
    if (!(p = strchr( rest, '_' )) || p - rest >= sizeof(ver)) return FALSE;
    memcpy( ver, rest, p - rest );
    ver[p - rest] = 0;
    rest = p + 1;
    if (!(p = strchr( rest, '_' )) || p - rest >= sizeof(arch)) return FALSE;
    memcpy( arch, rest, p - rest );
    arch[p - rest] = 0;
    if (strcmp( arch, "neutral" ) && strcmp( arch, "x64" )) return FALSE;
    *version = moniker_version( ver );
    return TRUE;
}

/* The packages of a category come from SyncUpdates. The protocol answers with the infrastructure
 * ("non-leaf") updates first; reporting those as installed in the next request is what makes it go
 * on to the packages. Of those, this keeps the family's newest, a bundle or x64; or, with a moniker, that package
 * (an older version the delivery service still lists). */
HRESULT CDECL appx_store_find_package_moniker( const char *category, const WCHAR *family, const char *want,
                                               struct appx_store_package *package )
{
    static const WCHAR path[] = L"/ClientWebService/client.asmx";
    struct update_ids installed = {0}, other = {0};
    char now[32], cookie[4096], name[256], publisher[64], *us, *fam = NULL;
    struct strbuf body = {0}, request = {0}, response = {0};
    ULONGLONG best = 0, version;
    BOOL found = FALSE;
    DWORD status;
    HRESULT hr;
    int pass, len;

    TRACE( "category %s, family %s, package %s.\n", debugstr_a( category ), debugstr_w( family ), debugstr_a( want ) );

    memset( package, 0, sizeof(*package) );
    len = WideCharToMultiByte( CP_UTF8, 0, family, -1, NULL, 0, NULL, NULL );
    if (!(fam = malloc( len ))) return E_OUTOFMEMORY;
    WideCharToMultiByte( CP_UTF8, 0, family, -1, fam, len, NULL, NULL );
    if (!(us = strrchr( fam, '_' )) || us - fam >= sizeof(name) || strlen( us + 1 ) >= sizeof(publisher))
    {
        free( fam );
        return E_INVALIDARG;
    }
    memcpy( name, fam, us - fam );
    name[us - fam] = 0;
    strcpy( publisher, us + 1 );
    free( fam );

    soap_time( now, sizeof(now), 0 );
    strbuf_printf( &body, "<GetCookie %s><oldCookie></oldCookie><lastChange>2015-10-21T17:01:07.1472913Z</lastChange>"
                   "<currentTime>%s</currentTime><protocolVersion>1.40</protocolVersion></GetCookie>", fe3_ns, now );
    soap_envelope( &request, "GetCookie", fe3_url, body.data );
    hr = fe3_post( path, request.data, &response, &status );
    if (SUCCEEDED(hr) && !copy_between( response.data, "<EncryptedData>", "</EncryptedData>", cookie, sizeof(cookie) ))
        hr = E_FAIL;
    if (FAILED(hr))
    {
        ERR( "FE3 GetCookie failed, HTTP %lu.\n", status );
        hr = HRESULT_FROM_WIN32( ERROR_WINHTTP_CANNOT_CONNECT );
        goto done;
    }

    for (pass = 0; pass < 8; pass++)
    {
        int leaves = 0, nonleaves = 0;
        char *p, *end;

        strbuf_free( &body );
        strbuf_free( &request );
        strbuf_printf( &body, "<SyncUpdates %s><cookie><Expiration>2045-03-11T02:02:48Z</Expiration><EncryptedData>%s"
                       "</EncryptedData></cookie><parameters><ExpressQuery>false</ExpressQuery><InstalledNonLeafUpdateIDs>",
                       fe3_ns, cookie );
        append_ids( &body, &installed );
        strbuf_cat( &body, "</InstalledNonLeafUpdateIDs><OtherCachedUpdateIDs>" );
        append_ids( &body, &other );
        strbuf_printf( &body, "</OtherCachedUpdateIDs><SkipSoftwareSync>false</SkipSoftwareSync>"
            "<NeedTwoGroupOutOfScopeUpdates>true</NeedTwoGroupOutOfScopeUpdates><FilterAppCategoryIds><CategoryIdentifier>"
            "<Id>%s</Id></CategoryIdentifier></FilterAppCategoryIds><TreatAppCategoryIdsAsInstalled>true"
            "</TreatAppCategoryIdsAsInstalled><AlsoPerformRegularSync>false</AlsoPerformRegularSync><ComputerSpec/>"
            "<ExtendedUpdateInfoParameters><XmlUpdateFragmentTypes><XmlUpdateFragmentType>Extended</XmlUpdateFragmentType>"
            "</XmlUpdateFragmentTypes><Locales><string>en-US</string><string>en</string></Locales>"
            "</ExtendedUpdateInfoParameters><ClientPreferredLanguages><string>en-US</string></ClientPreferredLanguages>"
            "<ProductsParameters><SyncCurrentVersionOnly>false</SyncCurrentVersionOnly><DeviceAttributes>%s"
            "</DeviceAttributes><CallerAttributes>Interactive=1;IsSeeker=0;</CallerAttributes><Products/>"
            "</ProductsParameters></parameters></SyncUpdates>", category, fe3_attributes );
        soap_envelope( &request, "SyncUpdates", fe3_url, body.data );
        if (body.failed || request.failed)
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        if (FAILED(hr = fe3_post( path, request.data, &response, &status ))) goto done;
        xml_unescape( response.data );

        for (p = response.data; (p = strstr( p, "<UpdateInfo><ID>" )); p = end + 1)
        {
            char id[24], moniker[256], update_id[64], revision[16];

            if (!(end = strstr( p, "</UpdateInfo>" ))) break;
            *end = 0;
            if (!copy_between( p, "<ID>", "</ID>", id, sizeof(id) )) continue;
            if (!strstr( p, "<IsLeaf>true</IsLeaf>" ))
            {
                nonleaves++;
                add_id( &installed, id );
                continue;
            }
            leaves++;
            add_id( &other, id );
            if (!strstr( p, "SecuredFragment" ) ||
                !copy_between( p, "PackageMoniker=\"", "\"", moniker, sizeof(moniker) ) ||
                !copy_between( p, "<UpdateIdentity UpdateID=\"", "\"", update_id, sizeof(update_id) ))
                continue;
            if (!copy_between( strstr( p, "<UpdateIdentity UpdateID=\"" ), "RevisionNumber=\"", "\"", revision,
                               sizeof(revision) ))
                strcpy( revision, "1" );
            if (!moniker_matches( moniker, name, publisher, &version ) || (found && version <= best)) continue;
            if (want && _stricmp( moniker, want )) continue;
            lstrcpynA( package->moniker, moniker, sizeof(package->moniker) );
            lstrcpynA( package->update_id, update_id, sizeof(package->update_id) );
            lstrcpynA( package->revision, revision, sizeof(package->revision) );
            best = version;
            found = TRUE;
        }
        TRACE( "SyncUpdates pass %d: %d infrastructure updates, %d packages.\n", pass, nonleaves, leaves );
        if (leaves || !nonleaves) break;
    }
    hr = found ? S_OK : HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (found) TRACE( "-> %s.\n", debugstr_a( package->moniker ) );

done:
    free( installed.ids );
    free( other.ids );
    strbuf_free( &body );
    strbuf_free( &request );
    strbuf_free( &response );
    return hr;
}

HRESULT CDECL appx_store_find_package( const char *category, const WCHAR *family, struct appx_store_package *package )
{
    return appx_store_find_package_moniker( category, family, NULL, package );
}

/* The first four bytes of a download: a package is a zip ("PK"), its block map a cabinet ("MSCF"). */
static BOOL is_cabinet( const char *url )
{
    WCHAR *wide, host[256], path[4096];
    URL_COMPONENTS uc = {.dwStructSize = sizeof(uc)};
    struct strbuf head = {0};
    DWORD status;
    BOOL ret;
    int len;

    len = MultiByteToWideChar( CP_UTF8, 0, url, -1, NULL, 0 );
    if (!(wide = malloc( len * sizeof(WCHAR) ))) return FALSE;
    MultiByteToWideChar( CP_UTF8, 0, url, -1, wide, len );
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAY_SIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAY_SIZE(path);
    ret = WinHttpCrackUrl( wide, 0, 0, &uc );
    free( wide );
    if (!ret) return FALSE;
    http_request( L"Microsoft-Delivery-Optimization/10.0", host, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS, L"GET",
                  path, L"Range: bytes=0-3\r\n", NULL, 20000, 4, &head, &status );
    ret = head.len >= 4 && !memcmp( head.data, "MSCF", 4 );
    TRACE( "%s starts %s.\n", debugstr_a( url ), debugstr_an( head.data, min( head.len, 4 ) ) );
    strbuf_free( &head );
    return ret;
}

HRESULT CDECL appx_store_package_url( const struct appx_store_package *package, char **url )
{
    static const WCHAR path[] = L"/ClientWebService/client.asmx/secured";
    struct strbuf body = {0}, request = {0}, response = {0};
    char secured[sizeof(fe3_url) + 8], *p, *end;
    DWORD status;
    HRESULT hr;

    TRACE( "package %s.\n", debugstr_a( package->moniker ) );

    *url = NULL;
    snprintf( secured, sizeof(secured), "%s/secured", fe3_url );
    strbuf_printf( &body, "<GetExtendedUpdateInfo2 %s><updateIDs><UpdateIdentity><UpdateID>%s</UpdateID>"
                   "<RevisionNumber>%s</RevisionNumber></UpdateIdentity></updateIDs><infoTypes>"
                   "<XmlUpdateFragmentType>FileUrl</XmlUpdateFragmentType><XmlUpdateFragmentType>FileDecryption"
                   "</XmlUpdateFragmentType></infoTypes><deviceAttributes>%s</deviceAttributes></GetExtendedUpdateInfo2>",
                   fe3_ns, package->update_id, package->revision, fe3_attributes );
    soap_envelope( &request, "GetExtendedUpdateInfo2", secured, body.data );
    if (body.failed || request.failed) hr = E_OUTOFMEMORY;
    else if (SUCCEEDED(hr = fe3_post( path, request.data, &response, &status )))
    {
        xml_unescape( response.data );
        hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
        /* Two URLs that look alike, the package's and its block map's; only their first bytes tell them apart. */
        for (p = response.data; (p = strstr( p, "<Url>" )); p = end)
        {
            p += 5;
            if (!(end = strstr( p, "</Url>" ))) break;
            *end++ = 0;
            if (!strstr( p, "filestreamingservice" ) || is_cabinet( p )) continue;
            if ((*url = strdup( p ))) hr = S_OK;
            else hr = E_OUTOFMEMORY;
            break;
        }
        if (FAILED(hr)) ERR( "GetExtendedUpdateInfo2 has no file URL for %s.\n", debugstr_a( package->moniker ) );
    }
    strbuf_free( &body );
    strbuf_free( &request );
    strbuf_free( &response );
    return hr;
}

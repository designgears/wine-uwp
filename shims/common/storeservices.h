/* The Store's public services, shared by the shims that install packages (gameplatform, appinstallmanager):
 * the display catalogue (product -> package family, Windows Update category) and the delivery service (FE3:
 * category -> packages -> download URL). No account is needed for either.
 *
 * The includer defines tracef(fmt, ...) first, and includes <windows.h>, <winhttp.h>, <string> and <vector>. */
#pragma once

static std::string utf8(const std::wstring &w)
{
    std::string s(w.size() * 3 + 1, 0);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}

static std::string http_get(const wchar_t *host, const std::wstring &path, DWORD *status_out)
{
    std::string out;
    DWORD status = 0, size = sizeof(status);
    HINTERNET s = WinHttpOpen(L"wineuwp-gameplatform/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0), c = nullptr, r = nullptr;
    if (!s) s = WinHttpOpen(L"wineuwp-gameplatform/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s && (c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)))
    {
        WinHttpSetTimeouts(r, 15000, 15000, 30000, 30000);
        if (WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
        {
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
            for (;;)
            {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                size_t old = out.size();
                out.resize(old + avail);
                if (!WinHttpReadData(r, &out[old], avail, &got)) { out.resize(old); break; }
                out.resize(old + got);
                if (out.size() > (32u << 20)) break;
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (status_out) *status_out = status;
    return out;
}

/* ---- the Store's delivery service (FE3) ---- */

static std::string http_post_soap_once(const wchar_t *path, const std::string &body, DWORD *status_out)
{
    std::string out;
    DWORD status = 0, size = sizeof(status);
    HINTERNET s = WinHttpOpen(L"Windows-Update-Agent/10.0.10011.16384 Client-Protocol/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), c = nullptr, r = nullptr;
    if (!s) s = WinHttpOpen(L"Windows-Update-Agent/10.0.10011.16384 Client-Protocol/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s && (c = WinHttpConnect(s, L"fe3.delivery.mp.microsoft.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) &&
        (r = WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)))
    {
        WinHttpSetTimeouts(r, 10000, 10000, 60000, 60000);
        if (WinHttpSendRequest(r, L"Content-Type: application/soap+xml; charset=utf-8\r\n", (DWORD)-1, (void *)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, nullptr))
        {
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
            for (;;)
            {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                size_t old = out.size();
                out.resize(old + avail);
                if (!WinHttpReadData(r, &out[old], avail, &got)) { out.resize(old); break; }
                out.resize(old + got);
            }
        }
        else tracef("FE3 POST %ls: error %lu", path, GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (status_out) *status_out = status;
    return out;
}

/* a connection to the delivery service sometimes times out (10060) where the next one goes through */
static std::string http_post_soap(const wchar_t *path, const std::string &body, DWORD *status_out)
{
    std::string out;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        *status_out = 0;
        out = http_post_soap_once(path, body, status_out);
        if (*status_out) break;
        tracef("FE3 POST %ls: retrying", path);
    }
    return out;
}

static std::string xml_unescape(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != '&') { o += s[i]; continue; }
        static const struct { const char *e; char c; } ents[] = { {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'} };
        bool done = false;
        for (auto &e : ents)
            if (!s.compare(i, strlen(e.e), e.e)) { o += e.c; i += strlen(e.e) - 1; done = true; break; }
        if (!done) o += '&';
    }
    return o;
}

static std::string soap_time(int offset_minutes)
{
    SYSTEMTIME st;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = { { ft.dwLowDateTime, ft.dwHighDateTime } };
    u.QuadPart += (LONGLONG)offset_minutes * 60 * 10000000;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static std::string soap_envelope(const char *action, const char *to, const std::string &body)
{
    GUID g;
    CoCreateGuid(&g);
    char id[64];
    snprintf(id, sizeof(id), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string("<s:Envelope xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><a:Action s:mustUnderstand=\"1\">http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService/") + action +
        "</a:Action><a:MessageID>urn:uuid:" + id + "</a:MessageID><a:To s:mustUnderstand=\"1\">" + to + "</a:To>"
        "<o:Security s:mustUnderstand=\"1\" xmlns:o=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><Created>" + soap_time(0) +
        "</Created><Expires>" + soap_time(5) + "</Expires></Timestamp>"
        "<wuws:WindowsUpdateTicketsToken wsu:id=\"ClientMSA\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" "
        "xmlns:wuws=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\"><TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"></TicketType>"
        "</wuws:WindowsUpdateTicketsToken></o:Security></s:Header><s:Body>" + body + "</s:Body></s:Envelope>";
}

static const char FE3_NS[] = "xmlns=\"http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService\"";
static const char FE3_ATTRS[] = "BranchReadinessLevel=CB;CurrentBranch=ge_release;OEMModel=Virtual Machine;FlightRing=Retail;AttrDataVer=21;"
    "SystemManufacturer=Microsoft Corporation;InstallLanguage=en-US;OSUILocale=en-US;InstallationType=Client;FlightingBranchName=external;"
    "OSSkuId=48;FlightContent=Branch;App=WU;AppVer=10.0.26100.1;OSArchitecture=AMD64;IsFlightingEnabled=0;IsDeviceRetailDemo=0;"
    "TelemetryLevel=3;OSVersion=10.0.26100.1;DeviceFamily=Windows.Desktop;";

struct Fe3Update { std::string moniker, update_id, revision; };

/* the packages of a Store category: SyncUpdates until the leaf updates (the packages) arrive. The Windows Update
 * protocol answers with the infrastructure ("non-leaf") updates first; reporting those as installed in the next
 * request is what makes it go on to the packages. */
static HRESULT fe3_packages(const std::string &category, std::vector<Fe3Update> &out)
{
    static const wchar_t PATH[] = L"/ClientWebService/client.asmx";
    static const char TO[] = "https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx";
    DWORD status = 0;
    std::string r = http_post_soap(PATH, soap_envelope("GetCookie", TO, std::string("<GetCookie ") + FE3_NS +
        "><oldCookie></oldCookie><lastChange>2015-10-21T17:01:07.1472913Z</lastChange><currentTime>" + soap_time(0) +
        "</currentTime><protocolVersion>1.40</protocolVersion></GetCookie>"), &status);
    size_t a = r.find("<EncryptedData>"), b = r.find("</EncryptedData>");
    if (status != 200 || a == std::string::npos || b == std::string::npos)
    {
        tracef("FE3 GetCookie: HTTP %lu", status);
        return HRESULT_FROM_WIN32(ERROR_WINHTTP_CANNOT_CONNECT);
    }
    std::string cookie = r.substr(a + 15, b - a - 15);
    std::vector<std::string> installed, other;
    for (int pass = 0; pass < 8; pass++)
    {
        std::string ins, oth;
        for (auto &i : installed) ins += "<int>" + i + "</int>";
        for (auto &i : other) oth += "<int>" + i + "</int>";
        std::string body = std::string("<SyncUpdates ") + FE3_NS + "><cookie><Expiration>2045-03-11T02:02:48Z</Expiration><EncryptedData>" + cookie +
            "</EncryptedData></cookie><parameters><ExpressQuery>false</ExpressQuery><InstalledNonLeafUpdateIDs>" + ins +
            "</InstalledNonLeafUpdateIDs><OtherCachedUpdateIDs>" + oth + "</OtherCachedUpdateIDs><SkipSoftwareSync>false</SkipSoftwareSync>"
            "<NeedTwoGroupOutOfScopeUpdates>true</NeedTwoGroupOutOfScopeUpdates><FilterAppCategoryIds><CategoryIdentifier><Id>" + category +
            "</Id></CategoryIdentifier></FilterAppCategoryIds><TreatAppCategoryIdsAsInstalled>true</TreatAppCategoryIdsAsInstalled>"
            "<AlsoPerformRegularSync>false</AlsoPerformRegularSync><ComputerSpec/><ExtendedUpdateInfoParameters><XmlUpdateFragmentTypes>"
            "<XmlUpdateFragmentType>Extended</XmlUpdateFragmentType></XmlUpdateFragmentTypes><Locales><string>en-US</string><string>en</string>"
            "</Locales></ExtendedUpdateInfoParameters><ClientPreferredLanguages><string>en-US</string></ClientPreferredLanguages>"
            "<ProductsParameters><SyncCurrentVersionOnly>false</SyncCurrentVersionOnly><DeviceAttributes>" + FE3_ATTRS +
            "</DeviceAttributes><CallerAttributes>Interactive=1;IsSeeker=0;</CallerAttributes><Products/></ProductsParameters></parameters></SyncUpdates>";
        r = xml_unescape(http_post_soap(PATH, soap_envelope("SyncUpdates", TO, body), &status));
        if (status != 200) { tracef("FE3 SyncUpdates: HTTP %lu", status); return E_FAIL; }
        int leaves = 0, nonleaves = 0;
        for (size_t p = 0; (p = r.find("<UpdateInfo><ID>", p)) != std::string::npos;)
        {
            size_t end = r.find("</UpdateInfo>", p);
            if (end == std::string::npos) break;
            std::string u = r.substr(p, end - p);
            p = end;
            std::string id = u.substr(16, u.find("</ID>") - 16);
            bool leaf = u.find("<IsLeaf>true</IsLeaf>") != std::string::npos;
            if (!leaf) { nonleaves++; installed.push_back(id); continue; }
            leaves++;
            other.push_back(id);
            size_t m = u.find("PackageMoniker=\""), ui = u.find("<UpdateIdentity UpdateID=\"");
            if (m == std::string::npos || ui == std::string::npos || u.find("SecuredFragment") == std::string::npos) continue;
            Fe3Update f;
            f.moniker = u.substr(m + 16, u.find('"', m + 16) - m - 16);
            f.update_id = u.substr(ui + 26, u.find('"', ui + 26) - ui - 26);
            size_t rv = u.find("RevisionNumber=\"", ui);
            f.revision = rv == std::string::npos ? "1" : u.substr(rv + 16, u.find('"', rv + 16) - rv - 16);
            out.push_back(f);
        }
        tracef("FE3 SyncUpdates pass %d: %d infrastructure updates, %d packages", pass, nonleaves, leaves);
        if (leaves || !nonleaves) break;
    }
    return out.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : S_OK;
}

/* the first bytes of a download (a ranged GET): a package is a zip ("PK"), its block map a cabinet ("MSCF") */
static std::string http_head_bytes(const std::string &url)
{
    std::wstring w(url.begin(), url.end()), host(256, 0), path(4096, 0);
    std::string out;
    URL_COMPONENTS uc = {sizeof(uc)};
    uc.lpszHostName = &host[0]; uc.dwHostNameLength = (DWORD)host.size();
    uc.lpszUrlPath = &path[0]; uc.dwUrlPathLength = (DWORD)path.size();
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return out;
    host.resize(uc.dwHostNameLength);
    path.resize(uc.dwUrlPathLength);
    HINTERNET s = WinHttpOpen(L"Microsoft-Delivery-Optimization/10.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0), c = nullptr, r = nullptr;
    if (s && (c = WinHttpConnect(s, host.c_str(), uc.nPort, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
    {
        WinHttpSetTimeouts(r, 20000, 20000, 20000, 20000);
        char buf[4];
        DWORD got = 0;
        if (WinHttpSendRequest(r, L"Range: bytes=0-3\r\n", (DWORD)-1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
            WinHttpReadData(r, buf, sizeof(buf), &got))
            out.assign(buf, got);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return out;
}

static std::string fe3_file_url(const Fe3Update &u)
{
    static const char TO[] = "https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx/secured";
    DWORD status = 0;
    std::string body = std::string("<GetExtendedUpdateInfo2 ") + FE3_NS + "><updateIDs><UpdateIdentity><UpdateID>" + u.update_id +
        "</UpdateID><RevisionNumber>" + u.revision + "</RevisionNumber></UpdateIdentity></updateIDs><infoTypes>"
        "<XmlUpdateFragmentType>FileUrl</XmlUpdateFragmentType><XmlUpdateFragmentType>FileDecryption</XmlUpdateFragmentType></infoTypes>"
        "<deviceAttributes>" + FE3_ATTRS + "</deviceAttributes></GetExtendedUpdateInfo2>";
    std::string r = xml_unescape(http_post_soap(L"/ClientWebService/client.asmx/secured", soap_envelope("GetExtendedUpdateInfo2", TO, body), &status));
    /* the package's own file: the one that is not a block map */
    for (size_t p = 0; (p = r.find("<Url>", p)) != std::string::npos; p += 5)
    {
        std::string url = r.substr(p + 5, r.find("</Url>", p) - p - 5);
        if (url.find("filestreamingservice") == std::string::npos) continue;
        /* both URLs look alike; only the first bytes tell the block map (a cabinet) from the package */
        std::string magic = http_head_bytes(url);
        tracef("FE3 file starts %02x %02x", magic.size() > 0 ? (BYTE)magic[0] : 0, magic.size() > 1 ? (BYTE)magic[1] : 0);
        if (!magic.compare(0, 4, "MSCF")) continue;
        return url;
    }
    tracef("FE3 GetExtendedUpdateInfo2: HTTP %lu, no file URL", status);
    return "";
}

/* "Name_Version_Arch_ResourceId_PublisherId": the package for this family with the newest version, a bundle or x64 */
static bool pick_package(const std::vector<Fe3Update> &all, const std::wstring &family, Fe3Update &best)
{
    std::string fam = utf8(family);
    size_t us = fam.rfind('_');
    if (us == std::string::npos) return false;
    std::string name = fam.substr(0, us), pub = fam.substr(us + 1);
    UINT64 best_version = 0;
    bool found = false;
    for (auto &u : all)
    {
        if (u.moniker.compare(0, name.size() + 1, name + "_")) continue;
        if (u.moniker.size() < pub.size() || u.moniker.compare(u.moniker.size() - pub.size(), pub.size(), pub)) continue;
        std::string rest = u.moniker.substr(name.size() + 1);
        std::string ver = rest.substr(0, rest.find('_'));
        std::string arch = rest.substr(ver.size() + 1, rest.find('_', ver.size() + 1) - ver.size() - 1);
        if (arch != "neutral" && arch != "x64") continue;
        unsigned v[4] = {};
        sscanf(ver.c_str(), "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
        UINT64 version = ((UINT64)v[0] << 48) | ((UINT64)v[1] << 32) | ((UINT64)v[2] << 16) | v[3];
        if (!found || version > best_version) { best = u; best_version = version; found = true; }
    }
    return found;
}

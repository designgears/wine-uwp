// Microsoft.GamePlatform.Services.Identity.XboxAuthManager: Xbox sign-in, as a stand-in.
//
// On Windows the Xbox app signs in through Gaming Services: Microsoft.GamePlatform.Services.dll asks the
// GamingServices NT service, which holds the user's Microsoft account and Xbox Live tokens. That service cannot
// run under Wine (kernel drivers), so the package DLL fails every Identity call with 0x87E00196 and
// GrtsAuthProvider "skips sign in due to missing auth manager": the gate's Sign in button does nothing.
//
// This answers for the Identity classes instead (registered with OverridesPackage=1, like shims/gameplatform),
// and really signs in, with no embedded browser:
//   1. Microsoft account, OAuth device-code flow: login.live.com/oauth20_connect.srf hands out a user code, the
//      user's own browser is opened at https://www.microsoft.com/link?otc=<code> (ShellExecute -> winebrowser
//      -> xdg-open; WINE_UWP_LAUNCHER=log only logs it, as shims/launcher does), and oauth20_token.srf is polled
//      until the user has approved. The refresh token is kept, DPAPI-protected, in
//      %LOCALAPPDATA%\wineuwp\xboxauth.bin, so TryAddDefaultUserSilentlyAsync signs in silently next time, in
//      every process of the package (the app and its XboxPcAppFT server each make their own manager).
//   2. Xbox Live: user.auth.xboxlive.com/user/authenticate (RPS ticket "t=<msa token>") gives the user token,
//      xsts.auth.xboxlive.com/xsts/authorize an XSTS token per relying party. Its xui claims give the user hash,
//      XUID, gamertag, age group and privileges.
//   3. User.GetTokenAndSignatureAsync picks the relying party for the URL from the title endpoints table
//      (title.mgt.xboxlive.com/titles/default/endpoints?type=1, fetched once), and answers
//      "XBL3.0 x=<uhs>;<xsts>", and a Signature whenever the endpoint has a signature policy.
//   4. The device: an ECDSA P-256 proof key made once per prefix (kept in xboxauth.bin), a device token from
//      device.auth.xboxlive.com (ProofOfPossession), SISU authorization (sisu.xboxlive.com/authorize) for user, title
//      and XSTS tokens bound to that key, and the Xbox "Signature" header on every signed request. Ported from
//      xal-rs (https://github.com/OpenXbox/xal-rs, MIT License, Copyright (c) 2023 Team OpenXbox); see "The device".
//
// Tokens are never written to the log: traces carry HTTP statuses, error codes and lengths only.
//
// Slots (Microsoft.GamePlatform.Services.winmd, ./winmd.py --sig):
//   IXboxAuthManagerFactory {0fe0a92a}: 6 CreateInstance(args)
//   IXboxAuthManagerStatics {a4d7c0e6}: 6 AddUserWithTransferTokenAsync(string) -> NotSupported
//   IXboxAuthManager        {febeda28}: 6 get_Sandbox 7 TryAddDefaultUserSilentlyAsync 8 AddUserWithOptionalUiAsync
//                                       9 AddUserWithUiAsync 10 GetDeviceUserAsync
//   IXboxAuthInitializationArgs(Factory) {61be67fb}/{67116537}: a property bag
//   IUser {703e831e}: see the User class below
// The asynchronous IIDs are from piid.py --signature, and each was found in the package's own binaries.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <map>

/* ------------------------------------------------------------------------------------------- */
/* IIDs                                                                                         */

static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IAsyncAction_ = {0x5a648006,0x843a,0x4da9,{0x86,0x5b,0x9d,0x26,0xe5,0xdf,0xad,0x7b}};
static const GUID IID_IClosable_ = {0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};
static const GUID IID_IXboxAuthManager = {0xfebeda28,0x6412,0x558d,{0x97,0x81,0xc3,0xb0,0xd3,0x57,0x22,0xc5}};
static const GUID IID_IXboxAuthManagerStatics = {0xa4d7c0e6,0xc053,0x5b25,{0xa3,0xdf,0x14,0xae,0xd1,0x49,0xf7,0x36}};
static const GUID IID_IXboxAuthManagerFactory = {0x0fe0a92a,0x8479,0x509b,{0x98,0xaf,0x99,0xfb,0xce,0xa8,0x5a,0x36}};
static const GUID IID_IXboxAuthInitializationArgs = {0x61be67fb,0xd2c6,0x5494,{0x93,0x31,0xc6,0x2c,0x9e,0x0b,0xea,0x5b}};
static const GUID IID_IXboxAuthInitializationArgsFactory = {0x67116537,0xd929,0x58da,{0xad,0x5b,0x4a,0x04,0xaa,0xd2,0xb5,0x71}};
static const GUID IID_ITokenRequest = {0x6ba19172,0xafe5,0x579d,{0xbf,0x0f,0x86,0x26,0xe7,0xc7,0x1f,0xa5}};
static const GUID IID_IUser = {0x703e831e,0xe673,0x5e9d,{0x93,0x07,0x5a,0xf2,0x84,0x71,0xd4,0x17}};
static const GUID IID_IUserStateChangedEventArgs = {0x80d9a355,0x4425,0x5663,{0xbd,0x24,0xe7,0xa2,0xe2,0xfb,0x5a,0x64}};
static const GUID IID_IUserEventArgs = {0xfd4b985a,0xf082,0x58b3,{0xba,0xf1,0x49,0x90,0x7d,0x2f,0x14,0x01}};
/* IAsyncOperation<T> */
static const GUID IID_AsyncOp_User = {0x33ba6bff,0xf4cd,0x5235,{0x8e,0x39,0xb6,0xa0,0x3b,0xf6,0x5e,0x9d}};
static const GUID IID_AsyncOp_DeviceUser = {0x18043ce3,0x4aae,0x5c88,{0x90,0x73,0x6a,0xea,0x09,0x75,0xaf,0x82}};
static const GUID IID_AsyncOp_TokenRequestResult = {0xfeaa4deb,0xd613,0x5fe2,{0x97,0xcd,0xa2,0x33,0x9a,0xa5,0x7c,0x11}};
static const GUID IID_AsyncOp_MsaTicketRequestResult = {0x9fd1fea4,0x02f6,0x5339,{0xae,0x61,0x26,0x22,0xa0,0xc3,0xf4,0x6d}};
static const GUID IID_AsyncOp_TransferTokenRequestResult = {0x3f3b1941,0x2ef2,0x5905,{0x9d,0x88,0x03,0x77,0x8a,0x1d,0xe7,0x3d}};
static const GUID IID_AsyncOp_ConsentState = {0xc92086c7,0xb593,0x50c4,{0xb9,0x31,0x8a,0x9c,0x06,0xd5,0x4e,0xd2}};

static const char MSA_CLIENT_ID[] = "000000004C12AE6F";
static const char MSA_SCOPE[] = "service::user.auth.xboxlive.com::MBI_SSL";

/* ------------------------------------------------------------------------------------------- */
/* Helpers                                                                                      */

static void trace(const char *fmt, ...)
{
    /* one write per line: stderr is unbuffered, and a line written in pieces interleaves with Wine's */
    char buf[1024] = "[xboxauth] ";
    size_t pre = strlen(buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + pre, sizeof(buf) - pre - 1, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    fwrite(buf, 1, strlen(buf), stderr);
}

static std::wstring widen(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const wchar_t *w, size_t len)
{
    if (!len) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
    return s;
}
static std::string from_hstr(HSTRING h)
{
    UINT32 len = 0;
    const wchar_t *p = WindowsGetStringRawBuffer(h, &len);
    return narrow(p, len);
}
static HRESULT to_hstr(const std::string &s, HSTRING *out)
{
    if (!out) return E_POINTER;
    std::wstring w = widen(s);
    return WindowsCreateString(w.c_str(), (UINT32)w.size(), out);
}
static HRESULT to_hstr(const wchar_t *s, HSTRING *out)
{
    if (!out) return E_POINTER;
    return WindowsCreateString(s, (UINT32)wcslen(s), out);
}
static std::string urlencode(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
static std::string json_escape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
static ULONGLONG now_s(void) { return (ULONGLONG)time(nullptr); }

/* ISO 8601 "2026-10-15T10:11:12.1234567Z" -> unix seconds; 0 if unparseable */
static ULONGLONG parse_iso(const std::string &s)
{
    SYSTEMTIME st = {};
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d; st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)se;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000000ULL - 11644473600ULL;
}

/* ------------------------------------------------------------------------------------------- */
/* A small JSON reader                                                                           */

struct Json
{
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json &operator[](const char *key) const
    {
        static const Json none;
        if (type == OBJ) for (auto &kv : o) if (kv.first == key) return kv.second;
        return none;
    }
    const Json &at(size_t i) const
    {
        static const Json none;
        return type == ARR && i < a.size() ? a[i] : none;
    }
    std::string str() const
    {
        if (type == STR) return s;
        if (type == NUM) { char b2[64]; snprintf(b2, sizeof(b2), "%.0f", n); return b2; }
        return std::string();
    }
};

struct JsonParser
{
    const char *p, *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void put_utf8(std::string &o, unsigned cp)
    {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xc0 | cp >> 6); o += (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) { o += (char)(0xe0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
        else { o += (char)(0xf0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 0x3f)); o += (char)(0x80 | (cp >> 6 & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
    }
    bool string(std::string &out)
    {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"')
        {
            if (*p == '\\' && p + 1 < end)
            {
                p++;
                switch (*p)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    if (end - p < 5) return false;
                    unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp >= 0xd800 && cp < 0xdc00 && end - p > 6 && p[1] == '\\' && p[2] == 'u')
                    {
                        unsigned lo = (unsigned)strtoul(std::string(p + 3, 4).c_str(), nullptr, 16);
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += *p; break;
                }
                p++;
            }
            else out += *p++;
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json &v, int depth)
    {
        if (depth > 64) return false;
        ws();
        if (p >= end) return false;
        if (*p == '{')
        {
            v.type = Json::OBJ;
            p++; ws();
            if (p < end && *p == '}') { p++; return true; }
            for (;;)
            {
                std::string key;
                ws();
                if (!string(key)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                p++;
                v.o.emplace_back(key, Json());
                if (!value(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[')
        {
            v.type = Json::ARR;
            p++; ws();
            if (p < end && *p == ']') { p++; return true; }
            for (;;)
            {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.type = Json::STR; return string(v.s); }
        if (!strncmp(p, "true", 4)) { v.type = Json::BOOL; v.b = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.type = Json::BOOL; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char *e;
        v.n = strtod(p, &e);
        if (e == p) return false;
        v.type = Json::NUM;
        p = e;
        return true;
    }
};

static Json parse_json(const std::string &text)
{
    Json v;
    JsonParser jp{ text.data(), text.data() + text.size() };
    if (!jp.value(v, 0)) return Json();
    return v;
}

/* ------------------------------------------------------------------------------------------- */
/* HTTP, with WinHTTP                                                                            */

static HINTERNET http_session(void)
{
    static HINTERNET session;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        session = WinHttpOpen(L"XboxAuthManager/1.0 (wine-uwp)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"XboxAuthManager/1.0 (wine-uwp)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        return TRUE;
    }, nullptr, nullptr);
    return session;
}

/* Returns the HTTP status, or 0 if the request did not complete. */
static DWORD http_request(const wchar_t *method, const std::string &url, const std::vector<std::string> &headers,
                          const std::string &body, std::string &response)
{
    response.clear();
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return 0;
    std::wstring object(path);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    HINTERNET session = http_session(), connect = nullptr, request = nullptr;
    DWORD status = 0, size = sizeof(status);
    std::wstring hdrs;
    for (auto &h : headers) { hdrs += widen(h); hdrs += L"\r\n"; }

    if (!session) return 0;
    if (!(connect = WinHttpConnect(session, host, uc.nPort, 0))) goto done;
    if (!(request = WinHttpOpenRequest(connect, method, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
        goto done;
    WinHttpSetTimeouts(request, 15000, 15000, 30000, 30000);
    if (!WinHttpSendRequest(request, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), (DWORD)-1,
                            (void *)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0))
        goto done;
    if (!WinHttpReceiveResponse(request, nullptr)) goto done;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                             nullptr))
        status = 0;
    for (;;)
    {
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || !avail) break;
        size_t old = response.size();
        response.resize(old + avail);
        if (!WinHttpReadData(request, &response[old], avail, &read)) { response.resize(old); break; }
        response.resize(old + read);
        if (response.size() > (16u << 20)) break;
    }
done:
    if (!status) trace("%ls %s: request failed, error %lu", method, url.substr(0, url.find('?')).c_str(), GetLastError());
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    return status;
}

static DWORD http_form(const std::string &url, const std::string &form, std::string &response)
{
    return http_request(L"POST", url, { "Content-Type: application/x-www-form-urlencoded", "Accept: application/json" },
                        form, response);
}

/* ------------------------------------------------------------------------------------------- */
/* Persistent state: the Microsoft account's refresh token, DPAPI-protected                     */

static std::wstring store_path(bool create_dir)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"WINE_UWP_XBOXAUTH_STORE", base, MAX_PATH);
    if (n && n < MAX_PATH) return base;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) wcscpy(base, L"C:\\users\\Public");
    std::wstring dir = std::wstring(base) + L"\\wineuwp";
    if (create_dir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\xboxauth.bin";
}

static std::map<std::string, std::string> store_read(void)
{
    std::map<std::string, std::string> kv;
    HANDLE f = CreateFileW(store_path(false).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return kv;
    std::string blob;
    DWORD size = GetFileSize(f, nullptr), read = 0;
    if (size != INVALID_FILE_SIZE && size < (1u << 20))
    {
        blob.resize(size);
        if (!ReadFile(f, &blob[0], size, &read, nullptr)) read = 0;
        blob.resize(read);
    }
    CloseHandle(f);
    DATA_BLOB in = { (DWORD)blob.size(), (BYTE *)blob.data() }, out = {};
    if (blob.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
    {
        trace("stored sign-in could not be read (error %lu)", GetLastError());
        return kv;
    }
    std::string text((char *)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        pos = nl + 1;
    }
    return kv;
}

static bool store_write(const std::map<std::string, std::string> &kv)
{
    std::wstring path = store_path(true);
    if (kv.empty()) { DeleteFileW(path.c_str()); return true; }
    std::string text;
    for (auto &e : kv) text += e.first + "=" + e.second + "\n";
    DATA_BLOB in = { (DWORD)text.size(), (BYTE *)text.data() }, out = {};
    if (!CryptProtectData(&in, L"wine-uwp Xbox sign-in", nullptr, nullptr, nullptr, 0, &out))
    {
        trace("sign-in not saved: CryptProtectData failed, error %lu", GetLastError());
        return false;
    }
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, out.pbData, out.cbData, &written, nullptr) && written == out.cbData;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    LocalFree(out.pbData);
    SecureZeroMemory(&text[0], text.size());
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) trace("sign-in not saved, error %lu", GetLastError());
    return ok;
}

/* Read-modify-write of the store, under a prefix-wide mutex: the app and its FT server share the file, and
 * each keeps only some of its keys in memory. An empty value removes the key. */
static HANDLE store_mutex(void)
{
    static HANDLE m;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        m = CreateMutexW(nullptr, FALSE, L"Global\\wineuwp-xboxauth-store");
        if (!m) m = CreateMutexW(nullptr, FALSE, L"Local\\wineuwp-xboxauth-store");
        return TRUE;
    }, nullptr, nullptr);
    return m;
}
static void store_lock(void) { if (store_mutex()) WaitForSingleObject(store_mutex(), 10000); }
static void store_unlock(void) { if (store_mutex()) ReleaseMutex(store_mutex()); }

static bool store_update(const std::map<std::string, std::string> &changes)
{
    store_lock();
    auto kv = store_read();
    for (auto &e : changes)
        if (e.second.empty()) kv.erase(e.first);
        else kv[e.first] = e.second;
    bool ok = store_write(kv);
    for (auto &e : kv) SecureZeroMemory(&e.second[0], e.second.size());
    store_unlock();
    return ok;
}

/* ------------------------------------------------------------------------------------------- */
/* The account: Microsoft account and Xbox Live tokens                                           */

/* errors the asyncs report; FACILITY_ITF so they are recognisably ours */
#define XA_E_NO_ACCOUNT     MAKE_HRESULT(1, FACILITY_ITF, 0x7801)  /* nothing stored, silent sign-in impossible */
#define XA_E_MSA            MAKE_HRESULT(1, FACILITY_ITF, 0x7802)  /* login.live.com refused */
#define XA_E_XBL            MAKE_HRESULT(1, FACILITY_ITF, 0x7803)  /* user.auth / xsts refused */
#define XA_E_NETWORK        MAKE_HRESULT(1, FACILITY_ITF, 0x7804)
#define XA_E_TIMEOUT        MAKE_HRESULT(1, FACILITY_ITF, 0x7805)  /* the device code expired */

/* ------------------------------------------------------------------------------------------- */
/* The device: proof key, request signing and the Xbox Live device token                        */
/*                                                                                               */
/* Ported from xal-rs (https://github.com/OpenXbox/xal-rs, fork xodus-gaming/xal-rs; MIT License, */
/* Copyright (c) 2023 Team OpenXbox): src/request_signer.rs (RequestSigner::prehash_message_data,  */
/* sign_raw, SignaturePolicyCache::find_policy_for_url), src/models.rs (ProofKey) and             */
/* src/authenticator.rs (get_device_token, sisu_authorize, get_xsts_token).                       */
/*                                                                                               */
/* The proof key is an ECDSA P-256 key, made once per prefix and kept in the store; Xbox Live      */
/* binds the device token (and every XSTS token made with it) to its public half, and services     */
/* with a signature policy check each request's "Signature" header against it:                    */
/*   base64( policy version (4 bytes BE) | FILETIME (8 bytes BE) | ECDSA r|s (64 bytes) )         */
/* over SHA-256 of  version\0 filetime\0 METHOD\0 path?query\0 authorization\0 body[:max]\0.      */

static std::string b64(const unsigned char *p, size_t n, bool url)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += t[v >> 18 & 63];
        o += t[v >> 12 & 63];
        o += i + 1 < n ? t[v >> 6 & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    if (url)
    {
        while (!o.empty() && o.back() == '=') o.pop_back();
        for (auto &c : o) c = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    return o;
}

static std::string unb64(const std::string &s)
{
    std::string o;
    unsigned v = 0;
    int bits = 0;
    for (char c : s)
    {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        v = v << 6 | d;
        bits += 6;
        if (bits >= 8) { bits -= 8; o += (char)(v >> bits & 0xff); }
    }
    return o;
}

static bool sha256(const std::string &data, unsigned char out[32])
{
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), out, 32));
}

/* RequestSigner::prehash_message_data */
static std::string sign_prehash_input(const unsigned char version[4], const unsigned char filetime[8], const std::string &method,
                                      const std::string &path_and_query, const std::string &authorization,
                                      const std::string &body, ULONGLONG max_body_bytes)
{
    std::string m;
    m.append((const char *)version, 4); m += '\0';
    m.append((const char *)filetime, 8); m += '\0';
    std::string up = method;
    for (auto &c : up) c = (char)toupper((unsigned char)c);
    m += up; m += '\0';
    m += path_and_query; m += '\0';
    m += authorization; m += '\0';
    m.append(body, 0, (size_t)(body.size() < max_body_bytes ? body.size() : max_body_bytes)); m += '\0';
    return m;
}

struct Device
{
    CRITICAL_SECTION cs;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::string x, y;              /* public point, base64url */
    std::string id;                /* device id, a GUID */
    std::string token;             /* device token */
    ULONGLONG token_expires = 0;
};
static Device device;

static void device_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        InitializeCriticalSection(&device.cs);
        return TRUE;
    }, nullptr, nullptr);
}

static bool device_take_key(BCRYPT_KEY_HANDLE key)
{
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG len = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &len, 0)) ||
        len != sizeof(blob)) return false;
    device.key = key;
    device.x = b64(blob + sizeof(BCRYPT_ECCKEY_BLOB), 32, true);
    device.y = b64(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32, true);
    return true;
}

/* Loads the proof key, device id and device token from the store, or makes and stores a new key. */
static bool device_load(void)
{
    device_init();
    EnterCriticalSection(&device.cs);
    if (device.key) { LeaveCriticalSection(&device.cs); return true; }
    store_lock();
    auto kv = store_read();
    BCRYPT_KEY_HANDLE key = nullptr;
    std::string priv = unb64(kv["proof_key"]);
    if (priv.size() == sizeof(BCRYPT_ECCKEY_BLOB) + 96 &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key,
                                           (PUCHAR)priv.data(), (ULONG)priv.size(), 0)) &&
        device_take_key(key))
    {
        device.id = kv["device_id"];
        device.token = kv["device_token"];
        device.token_expires = _strtoui64(kv["device_expires"].c_str(), nullptr, 10);
        trace("proof key loaded");
    }
    else
    {
        if (key) BCryptDestroyKey(key);
        key = nullptr;
        unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
        ULONG len = 0;
        GUID g;
        if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0)) ||
            !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0)) ||
            !BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof(blob), &len, 0)) ||
            len != sizeof(blob) || !device_take_key(key) || FAILED(CoCreateGuid(&g)))
        {
            trace("no proof key: ECDSA P-256 key generation failed");
            if (key) BCryptDestroyKey(key);
            store_unlock();
            LeaveCriticalSection(&device.cs);
            return false;
        }
        char id[40];
        snprintf(id, sizeof(id), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
                 g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        device.id = id;
        device.token.clear();
        device.token_expires = 0;
        kv["proof_key"] = b64(blob, sizeof(blob), false);
        kv["device_id"] = device.id;
        kv.erase("device_token");
        kv.erase("device_expires");
        store_write(kv);
        SecureZeroMemory(blob, sizeof(blob));
        trace("proof key created");
    }
    for (auto &e : kv) SecureZeroMemory(&e.second[0], e.second.size());
    SecureZeroMemory(&priv[0], priv.size());
    store_unlock();
    LeaveCriticalSection(&device.cs);
    return true;
}

/* ProofKey, as JSON (xal-rs field order) */
static std::string proof_key_json(void)
{
    return "{\"alg\":\"ES256\",\"crv\":\"P-256\",\"kty\":\"EC\",\"use\":\"sig\",\"x\":\"" + device.x + "\",\"y\":\"" + device.y + "\"}";
}

/* RequestSigner::sign_raw. filetime = 0: now. Returns the Signature header value, "" on failure. */
static std::string sign_request(int policy_version, ULONGLONG max_body_bytes, const std::string &method,
                                const std::string &path_and_query, const std::string &authorization,
                                const std::string &body, ULONGLONG filetime = 0)
{
    if (!device_load()) return std::string();
    if (!filetime)
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        filetime = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    unsigned char out[4 + 8 + 64], hash[32];
    for (int i = 0; i < 4; i++) out[i] = (unsigned char)((unsigned)policy_version >> (24 - 8 * i));
    for (int i = 0; i < 8; i++) out[4 + i] = (unsigned char)(filetime >> (56 - 8 * i));
    std::string m = sign_prehash_input(out, out + 4, method, path_and_query, authorization, body, max_body_bytes);
    ULONG len = 0;
    if (!sha256(m, hash) ||
        !BCRYPT_SUCCESS(BCryptSignHash(device.key, nullptr, hash, 32, out + 12, 64, &len, 0)) || len != 64)
    {
        trace("request signing failed");
        return std::string();
    }
    return b64(out, sizeof(out), false);
}

/* scheme, host (lower case), path and path?query of a URL */
struct UrlParts { std::string scheme, host, path, path_and_query; };
static UrlParts split_url(const std::string &url)
{
    UrlParts u;
    size_t p = url.find("://");
    if (p == std::string::npos) return u;
    u.scheme = url.substr(0, p);
    size_t h = p + 3, slash = url.find_first_of("/?#", h);
    u.host = url.substr(h, slash == std::string::npos ? std::string::npos : slash - h);
    size_t at = u.host.rfind('@');
    if (at != std::string::npos) u.host = u.host.substr(at + 1);
    size_t colon = u.host.find(':');
    if (colon != std::string::npos) u.host.resize(colon);
    for (auto &c : u.host) c = (char)tolower((unsigned char)c);
    for (auto &c : u.scheme) c = (char)tolower((unsigned char)c);
    if (u.scheme == "wss") u.scheme = "https";
    else if (u.scheme == "ws") u.scheme = "http";
    std::string rest = slash == std::string::npos ? std::string() : url.substr(slash);
    rest = rest.substr(0, rest.find('#'));
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    u.path = rest.substr(0, rest.find('?'));
    u.path_and_query = rest;
    return u;
}

struct SigningPolicy { int version = 1; ULONGLONG max_body_bytes = 8192; bool es256 = true; };
static bool find_policy(const std::string &url, struct SigningPolicy &out);

/* signs one of our own requests to *.auth.xboxlive.com / sisu, with the endpoint's policy */
static std::vector<std::string> signed_headers(const std::string &url, const std::string &body, bool contract = true)
{
    std::vector<std::string> h = { "Content-Type: application/json", "Accept: application/json" };
    if (contract) h.push_back("x-xbl-contract-version: 1");
    SigningPolicy policy; /* the table's; version 1 / 8192 bytes (xal-rs' default) without it */
    if (!find_policy(url, policy)) policy = SigningPolicy();
    std::string sig = sign_request(policy.version, policy.max_body_bytes, "POST", split_url(url).path_and_query, "", body);
    if (!sig.empty()) h.push_back("Signature: " + sig);
    return h;
}

static void log_xbl_error(const char *what, DWORD status, const std::string &resp);

/* XalAuthenticator::get_device_token: device.auth.xboxlive.com, AuthMethod ProofOfPossession */
static std::string device_token_type; /* the DeviceType the cached token was issued for, "" = loaded from the store */

static HRESULT xbl_device_token(std::string &token, bool force = false, Json *answer = nullptr)
{
    /* Android: SISU refuses this MSA client (xal-rs' APP_XBOX) with a Win32 device (401), and without SISU's title
     * token peoplehub hands back an empty "people" for the signed-in user, which the Xbox app reads as "no
     * profile" and drops back to its sign-in gate. xal-rs pairs this client with Android/iOS too. */
    char want[32] = "Android";
    GetEnvironmentVariableA("WINE_UWP_XBOX_DEVICE_TYPE", want, sizeof(want));
    if (!device_load()) return XA_E_XBL;
    EnterCriticalSection(&device.cs);
    /* a stored token's type is not recorded; a non-default type asked for in this process always gets a fresh one */
    if (!force && !device.token.empty() && device.token_expires > now_s() + 3600 &&
        (device_token_type.empty() ? !strcmp(want, "Android") : device_token_type == want))
    {
        token = device.token;
        LeaveCriticalSection(&device.cs);
        return S_OK;
    }
    char type[32] = "Android", version[64] = "13";
    GetEnvironmentVariableA("WINE_UWP_XBOX_DEVICE_TYPE", type, sizeof(type));
    GetEnvironmentVariableA("WINE_UWP_XBOX_DEVICE_VERSION", version, sizeof(version));
    /* xal-rs: Android and Nintendo want "{uuid}", iOS upper case, everything else plain */
    std::string id = device.id;
    if (!strcmp(type, "Android") || !strcmp(type, "Nintendo")) id = "{" + id + "}";
    else if (!strcmp(type, "iOS")) for (auto &c : id) c = (char)toupper((unsigned char)c);
    std::string url = "https://device.auth.xboxlive.com/device/authenticate", resp, body =
        "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":"
        "\"ProofOfPossession\",\"Id\":\"" + id + "\",\"DeviceType\":\"" + type + "\",\"Version\":\"" + version +
        "\",\"ProofKey\":" + proof_key_json() + "}}";
    DWORD status = http_request(L"POST", url, signed_headers(url, body), body, resp);
    Json j = parse_json(resp);
    if (answer) *answer = j;
    if (!status) { LeaveCriticalSection(&device.cs); return XA_E_NETWORK; }
    if (status != 200 || j["Token"].str().empty())
    {
        log_xbl_error("device/authenticate", status, resp);
        LeaveCriticalSection(&device.cs);
        return XA_E_XBL;
    }
    device.token = token = j["Token"].str();
    device_token_type = type;
    device.token_expires = parse_iso(j["NotAfter"].str());
    if (!device.token_expires) device.token_expires = now_s() + 86400;
    char exp[24];
    snprintf(exp, sizeof(exp), "%llu", device.token_expires);
    LeaveCriticalSection(&device.cs);
    store_update({ { "device_token", token }, { "device_expires", exp } });
    trace("Xbox Live device token received (%s, %zu chars, valid %llu h)", type, token.size(),
          (device.token_expires - now_s()) / 3600);
    return S_OK;
}

struct Xsts { std::string token; ULONGLONG expires = 0; };

struct Account
{
    CRITICAL_SECTION cs;          /* guards everything below */
    CRITICAL_SECTION signin_cs;   /* one sign-in (refresh or device code) at a time */
    std::string refresh_token, msa_token, msa_user_id;
    ULONGLONG msa_expires = 0;
    std::string user_token;
    ULONGLONG user_expires = 0;
    std::string title_token;      /* from SISU; empty when it was refused (then XSTS carries no title claims) */
    ULONGLONG title_expires = 0;
    std::map<std::string, Xsts> xsts;
    /* xui claims of the http://xboxlive.com XSTS token */
    std::string uhs, xid, gtg, agg, prv, mgt, mgs, umg;
    bool signed_in = false;
};
static Account account;

static void account_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        InitializeCriticalSection(&account.cs);
        InitializeCriticalSection(&account.signin_cs);
        return TRUE;
    }, nullptr, nullptr);
}

/* Takes an oauth20_token.srf success response. Caller holds signin_cs. */
static bool take_msa_tokens(const Json &j)
{
    std::string access = j["access_token"].str(), refresh = j["refresh_token"].str();
    if (access.empty()) return false;
    EnterCriticalSection(&account.cs);
    account.msa_token = access;
    account.msa_expires = now_s() + (ULONGLONG)(j["expires_in"].n > 60 ? j["expires_in"].n - 60 : 0);
    if (!refresh.empty()) account.refresh_token = refresh;
    if (!j["user_id"].str().empty()) account.msa_user_id = j["user_id"].str();
    std::map<std::string, std::string> kv = { { "refresh_token", account.refresh_token }, { "user_id", account.msa_user_id } };
    LeaveCriticalSection(&account.cs);
    store_update(kv);
    trace("Microsoft account tokens received (access %zu chars, refresh %s, expires in %.0f s)", access.size(),
          refresh.empty() ? "unchanged" : "renewed", j["expires_in"].n);
    return true;
}

static void log_oauth_error(const char *what, DWORD status, const std::string &resp)
{
    Json j = parse_json(resp);
    trace("%s: HTTP %lu, error \"%s\"", what, status, j["error"].str().c_str());
}

/* grant_type=refresh_token for a scope. Caller holds signin_cs. */
static HRESULT msa_refresh(const std::string &scope, std::string *token_out)
{
    std::string refresh;
    EnterCriticalSection(&account.cs);
    refresh = account.refresh_token;
    LeaveCriticalSection(&account.cs);
    if (refresh.empty()) return XA_E_NO_ACCOUNT;

    std::string resp;
    DWORD status = http_form("https://login.live.com/oauth20_token.srf",
                             "grant_type=refresh_token&client_id=" + std::string(MSA_CLIENT_ID) + "&scope=" +
                             urlencode(scope) + "&refresh_token=" + urlencode(refresh), resp);
    if (!status) return XA_E_NETWORK;
    Json j = parse_json(resp);
    if (status != 200 || j["access_token"].str().empty())
    {
        log_oauth_error("refresh", status, resp);
        if (j["error"].str() == "invalid_grant")
        {
            /* the stored sign-in is gone for good (password change, revoked): forget it */
            EnterCriticalSection(&account.cs);
            account.refresh_token.clear();
            LeaveCriticalSection(&account.cs);
            store_update({ { "refresh_token", "" }, { "user_id", "" } });
            return XA_E_NO_ACCOUNT;
        }
        return XA_E_MSA;
    }
    if (scope == MSA_SCOPE) take_msa_tokens(j);
    else if (!j["refresh_token"].str().empty())
    {
        EnterCriticalSection(&account.cs);
        account.refresh_token = j["refresh_token"].str();
        std::map<std::string, std::string> kv = { { "refresh_token", account.refresh_token }, { "user_id", account.msa_user_id } };
        LeaveCriticalSection(&account.cs);
        store_update(kv);
    }
    if (token_out) *token_out = j["access_token"].str();
    return S_OK;
}

static void log_xbl_error(const char *what, DWORD status, const std::string &resp)
{
    Json j = parse_json(resp);
    trace("%s: HTTP %lu, XErr %s %s", what, status, j["XErr"].str().c_str(), j["Message"].str().c_str());
    std::string x = j["XErr"].str();
    if (x == "2148916233") trace("  this Microsoft account has no Xbox profile yet: create one at https://www.xbox.com/live");
    else if (x == "2148916235") trace("  Xbox Live is not available in this account's country");
    else if (x == "2148916236" || x == "2148916237") trace("  the account needs adult verification (South Korea)");
    else if (x == "2148916238") trace("  a child account: an adult has to add it to a Microsoft family first");
}

/* user.auth.xboxlive.com: Microsoft account token -> Xbox Live user token */
static HRESULT xbl_user_authenticate(const std::string &msa_token, std::string &user_token, ULONGLONG &expires)
{
    std::string resp, body =
        "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":\"RPS\","
        "\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":\"t=" + json_escape(msa_token) + "\"}}";
    DWORD status = http_request(L"POST", "https://user.auth.xboxlive.com/user/authenticate",
                                signed_headers("https://user.auth.xboxlive.com/user/authenticate", body), body, resp);
    if (!status) return XA_E_NETWORK;
    Json j = parse_json(resp);
    if (status != 200 || j["Token"].str().empty())
    {
        log_xbl_error("user/authenticate", status, resp);
        return XA_E_XBL;
    }
    user_token = j["Token"].str();
    expires = parse_iso(j["NotAfter"].str());
    if (!expires) expires = now_s() + 3600;
    trace("Xbox Live user token received (%zu chars)", user_token.size());
    return S_OK;
}

/* xsts.auth.xboxlive.com: user token -> XSTS token for one relying party */
/* XalAuthenticator::get_xsts_token: the device token (and the title token, when SISU gave one) bind the XSTS
 * token to our proof key, which is what makes signed requests acceptable. */
static HRESULT xbl_xsts(const std::string &user_token, const std::string &rp, Xsts &out, Json *claims,
                        bool with_title = true)
{
    std::string device_token, title_token;
    if (FAILED(xbl_device_token(device_token))) device_token.clear();
    if (with_title)
    {
        EnterCriticalSection(&account.cs);
        if (account.title_expires > now_s() + 60) title_token = account.title_token;
        LeaveCriticalSection(&account.cs);
    }
    std::string resp, body =
        "{\"RelyingParty\":\"" + json_escape(rp) + "\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\"";
    if (!device_token.empty()) body += ",\"DeviceToken\":\"" + json_escape(device_token) + "\"";
    if (!title_token.empty()) body += ",\"TitleToken\":\"" + json_escape(title_token) + "\"";
    body += ",\"UserTokens\":[\"" + json_escape(user_token) + "\"]}}";
    DWORD status = http_request(L"POST", "https://xsts.auth.xboxlive.com/xsts/authorize",
                                signed_headers("https://xsts.auth.xboxlive.com/xsts/authorize", body), body, resp);
    if (!status) return XA_E_NETWORK;
    Json j = parse_json(resp);
    if (status != 200 || j["Token"].str().empty())
    {
        trace("xsts for %s:", rp.c_str());
        log_xbl_error("xsts/authorize", status, resp);
        return XA_E_XBL;
    }
    out.token = j["Token"].str();
    out.expires = parse_iso(j["NotAfter"].str());
    if (!out.expires) out.expires = now_s() + 3600;
    out.expires = out.expires > 300 ? out.expires - 300 : 0;
    if (claims) *claims = j["DisplayClaims"]["xui"].at(0);
    trace("XSTS token for %s received (%zu chars; device %s, title %s)", rp.c_str(), out.token.size(),
          device_token.empty() ? "no" : "yes", title_token.empty() ? "no" : "yes");
    return S_OK;
}

/* XalAuthenticator::sisu_authorize, without a session id (xal-rs' xbox_live_sisu_authorization_flow after a
 * device-code login): one signed call gives user, title and device-bound XSTS tokens. AppId is the MSA client
 * the access token was issued to (xal-rs' APP_XBOX, 000000004c12ae6f, title 328178078). */
static HRESULT xbl_sisu_authorize(const std::string &msa_token, std::string &user_token, ULONGLONG &user_expires,
                                  Xsts &auth, Json &xui)
{
    std::string device_token;
    HRESULT hr;
    if (FAILED(hr = xbl_device_token(device_token))) return hr;
    std::string url = "https://sisu.xboxlive.com/authorize", resp, body =
        "{\"AccessToken\":\"t=" + json_escape(msa_token) + "\",\"AppId\":\"" + std::string(MSA_CLIENT_ID) +
        "\",\"DeviceToken\":\"" + json_escape(device_token) + "\",\"Sandbox\":\"RETAIL\",\"SiteName\":"
        "\"user.auth.xboxlive.com\",\"ProofKey\":" + proof_key_json() + "}";
    /* xal-rs sends no contract version header here */
    DWORD status = http_request(L"POST", url, signed_headers(url, body, false), body, resp);
    if (!status) return XA_E_NETWORK;
    Json j = parse_json(resp);
    const Json &u = j["UserToken"], &t = j["TitleToken"], &a = j["AuthorizationToken"];
    if (status != 200 || u["Token"].str().empty() || a["Token"].str().empty())
    {
        log_xbl_error("sisu/authorize", status, resp);
        if (!a["XErr"].str().empty()) log_xbl_error("sisu/authorize (authorization)", status, "{\"XErr\":" + a["XErr"].str() + "}");
        return XA_E_XBL;
    }
    user_token = u["Token"].str();
    user_expires = parse_iso(u["NotAfter"].str());
    if (!user_expires) user_expires = now_s() + 3600;
    auth.token = a["Token"].str();
    auth.expires = parse_iso(a["NotAfter"].str());
    if (!auth.expires) auth.expires = now_s() + 3600;
    auth.expires = auth.expires > 300 ? auth.expires - 300 : 0;
    xui = a["DisplayClaims"]["xui"].at(0);
    EnterCriticalSection(&account.cs);
    account.title_token = t["Token"].str();
    account.title_expires = parse_iso(t["NotAfter"].str());
    LeaveCriticalSection(&account.cs);
    trace("SISU authorization: user, title (%s) and XSTS tokens received", t["Token"].str().empty() ? "none" : "yes");
    return S_OK;
}

/* title.auth.xboxlive.com/title/authenticate: the title token SISU would have returned, from the device token and
 * the Microsoft account ticket. Some services (peoplehub's "people" for the signed-in user) answer an empty list to
 * XSTS tokens that carry no title claims. Leaves account.title_token empty when the service refuses. */
static HRESULT xbl_title_token(const std::string &msa_token)
{
    std::string device_token, resp, url = "https://title.auth.xboxlive.com/title/authenticate";
    HRESULT hr;
    if (FAILED(hr = xbl_device_token(device_token))) return hr;
    std::string body = "{\"Properties\":{\"AuthMethod\":\"RPS\",\"DeviceToken\":\"" + json_escape(device_token) +
        "\",\"RpsTicket\":\"t=" + json_escape(msa_token) + "\",\"SiteName\":\"user.auth.xboxlive.com\",\"ProofKey\":" +
        proof_key_json() + "},\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}";
    DWORD status = http_request(L"POST", url, signed_headers(url, body), body, resp);
    if (!status) return XA_E_NETWORK;
    Json j = parse_json(resp);
    if (status != 200 || j["Token"].str().empty())
    {
        log_xbl_error("title/authenticate", status, resp);
        return XA_E_XBL;
    }
    EnterCriticalSection(&account.cs);
    account.title_token = j["Token"].str();
    account.title_expires = parse_iso(j["NotAfter"].str());
    if (!account.title_expires) account.title_expires = now_s() + 3600;
    LeaveCriticalSection(&account.cs);
    trace("Xbox Live title token received (%zu chars)", j["Token"].str().size());
    return S_OK;
}

/* Microsoft account token in hand (account.msa_token): get the Xbox identity. Caller holds signin_cs.
 * SISU first; if it refuses, the plain user/authenticate + XSTS path (still device-bound, but no title). */
static HRESULT xbl_sign_in(void)
{
    std::string msa, user_token;
    ULONGLONG user_expires;
    Xsts x;
    Json xui;
    HRESULT hr;

    EnterCriticalSection(&account.cs);
    msa = account.msa_token;
    LeaveCriticalSection(&account.cs);
    if (FAILED(hr = xbl_sisu_authorize(msa, user_token, user_expires, x, xui)))
    {
        if (hr == XA_E_NETWORK) return hr;
        EnterCriticalSection(&account.cs);
        account.title_token.clear();
        account.title_expires = 0;
        LeaveCriticalSection(&account.cs);
        trace("SISU refused; falling back to user/authenticate + XSTS");
        if (FAILED(hr = xbl_user_authenticate(msa, user_token, user_expires))) return hr;
        if (xbl_title_token(msa) == XA_E_NETWORK) return XA_E_NETWORK;
        if (FAILED(hr = xbl_xsts(user_token, "http://xboxlive.com", x, &xui))) return hr;
    }
    if (xui["uhs"].str().empty() || xui["xid"].str().empty())
    {
        trace("XSTS answer carries no xid/uhs claims");
        return XA_E_XBL;
    }
    EnterCriticalSection(&account.cs);
    account.user_token = user_token;
    account.user_expires = user_expires;
    account.xsts.clear();
    account.xsts["http://xboxlive.com"] = x;
    account.uhs = xui["uhs"].str();
    account.xid = xui["xid"].str();
    account.gtg = xui["gtg"].str();
    account.agg = xui["agg"].str();
    account.prv = xui["prv"].str();
    account.mgt = xui["mgt"].str();
    account.mgs = xui["mgs"].str();
    account.umg = xui["umg"].str();
    account.signed_in = true;
    LeaveCriticalSection(&account.cs);
    trace("signed in to Xbox Live as \"%s\" (age group %s)", xui["gtg"].str().c_str(), xui["agg"].str().c_str());
    return S_OK;
}

static HRESULT sign_in_silently(void)
{
    HRESULT hr;
    EnterCriticalSection(&account.signin_cs);
    EnterCriticalSection(&account.cs);
    bool done = account.signed_in;
    if (account.refresh_token.empty())
    {
        store_lock();
        auto kv = store_read();
        store_unlock();
        account.refresh_token = kv["refresh_token"];
        account.msa_user_id = kv["user_id"];
    }
    bool have = !account.refresh_token.empty();
    LeaveCriticalSection(&account.cs);
    if (done) hr = S_OK;
    else if (!have) hr = XA_E_NO_ACCOUNT;
    else if (SUCCEEDED(hr = msa_refresh(MSA_SCOPE, nullptr))) hr = xbl_sign_in();
    LeaveCriticalSection(&account.signin_cs);
    return hr;
}

static void open_browser(const std::string &url)
{
    char mode[16] = "";
    std::wstring w = widen(url);
    if (GetEnvironmentVariableA("WINE_UWP_LAUNCHER", mode, sizeof(mode)) && !strcmp(mode, "log"))
    {
        HANDLE file = CreateFileW(L"C:\\users\\Public\\wineuwp-launched.uri", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written;
            WriteFile(file, url.data(), (DWORD)url.size(), &written, nullptr);
            CloseHandle(file);
        }
        else trace("C:\\users\\Public\\wineuwp-launched.uri not written, error %lu", GetLastError());
        trace("sign-in page %s (dry run, WINE_UWP_LAUNCHER=log)", url.c_str());
        return;
    }
    bool ok = (INT_PTR)ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    trace("sign-in page %s -> %s", url.c_str(), ok ? "opened in the browser" : "no browser");
}

/* The device-code flow. Blocks until the user approves, refuses or the code expires. */
static HRESULT sign_in_with_device_code(volatile LONG *cancelled)
{
    std::string resp;
    DWORD status = http_form("https://login.live.com/oauth20_connect.srf",
                             "client_id=" + std::string(MSA_CLIENT_ID) + "&scope=" + urlencode(MSA_SCOPE) +
                             "&response_type=device_code", resp);
    if (!status) return XA_E_NETWORK;
    Json j = parse_json(resp);
    std::string user_code = j["user_code"].str(), device_code = j["device_code"].str();
    if (status != 200 || user_code.empty() || device_code.empty())
    {
        log_oauth_error("device code", status, resp);
        return XA_E_MSA;
    }
    int interval = j["interval"].n >= 1 ? (int)j["interval"].n : 5;
    ULONGLONG deadline = now_s() + (j["expires_in"].n > 0 ? (ULONGLONG)j["expires_in"].n : 900);
    std::string verify = j["verification_uri"].str();
    if (verify.empty()) verify = "https://www.microsoft.com/link";
    trace("device code obtained: user code %s, valid %.0f s, verification %s", user_code.c_str(), j["expires_in"].n,
          verify.c_str());
    open_browser("https://www.microsoft.com/link?otc=" + user_code);

    while (now_s() < deadline)
    {
        for (int i = 0; i < interval * 10; i++)
        {
            if (cancelled && *cancelled) { trace("device-code sign-in cancelled"); return E_ABORT; }
            Sleep(100);
        }
        status = http_form("https://login.live.com/oauth20_token.srf",
                           "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" +
                           std::string(MSA_CLIENT_ID) + "&device_code=" + urlencode(device_code), resp);
        if (!status) continue; /* transient; keep polling */
        Json t = parse_json(resp);
        if (status == 200 && !t["access_token"].str().empty())
        {
            take_msa_tokens(t);
            return xbl_sign_in();
        }
        std::string err = t["error"].str();
        if (err == "authorization_pending") continue;
        if (err == "slow_down") { interval += 5; continue; }
        log_oauth_error("device-code poll", status, resp);
        return err == "expired_token" ? XA_E_TIMEOUT : XA_E_MSA;
    }
    trace("device code expired before the sign-in was finished");
    return XA_E_TIMEOUT;
}

static void sign_out(void)
{
    EnterCriticalSection(&account.signin_cs);
    EnterCriticalSection(&account.cs);
    account.signed_in = false;
    account.refresh_token.clear();
    account.msa_token.clear();
    account.user_token.clear();
    account.xsts.clear();
    LeaveCriticalSection(&account.cs);
    /* the device's proof key and device token stay: they belong to the machine, not the user */
    store_update({ { "refresh_token", "" }, { "user_id", "" } });
    LeaveCriticalSection(&account.signin_cs);
    trace("signed out; the stored sign-in is deleted");
}

/* ------------------------------------------------------------------------------------------- */
/* Relying parties: the title endpoints table                                                    */

struct Endpoint { std::string protocol, host, path, rp; bool wildcard; int policy = -1; };
static std::vector<Endpoint> endpoints;
static std::vector<SigningPolicy> policies;
static bool endpoints_loaded;

static void load_endpoints(void)
{
    EnterCriticalSection(&account.cs);
    bool loaded = endpoints_loaded;
    LeaveCriticalSection(&account.cs);
    if (loaded) return;

    std::string resp;
    DWORD status = http_request(L"GET", "https://title.mgt.xboxlive.com/titles/default/endpoints?type=1",
                                { "Accept: application/json", "x-xbl-contract-version: 1" }, "", resp);
    std::vector<Endpoint> list;
    Json j = parse_json(resp);
    for (auto &e : j["EndPoints"].a)
    {
        Endpoint ep;
        ep.protocol = e["Protocol"].str();
        ep.host = e["Host"].str();
        ep.path = e["Path"].str();
        ep.rp = e["RelyingParty"].str();
        ep.wildcard = e["HostType"].str() == "wildcard";
        if (e["SignaturePolicyIndex"].type == Json::NUM) ep.policy = (int)e["SignaturePolicyIndex"].n;
        for (auto &c : ep.host) c = (char)tolower((unsigned char)c);
        for (auto &c : ep.protocol) c = (char)tolower((unsigned char)c);
        list.push_back(ep);
    }
    std::vector<SigningPolicy> pols;
    for (auto &p : j["SignaturePolicies"].a)
    {
        SigningPolicy sp;
        sp.version = (int)p["Version"].n;
        sp.max_body_bytes = (ULONGLONG)p["MaxBodyBytes"].n;
        sp.es256 = false;
        for (auto &alg : p["SupportedAlgorithms"].a) if (alg.str() == "ES256") sp.es256 = true;
        pols.push_back(sp);
    }
    trace("title endpoints: HTTP %lu, %zu entries, %zu signature policies", status, list.size(), pols.size());
    EnterCriticalSection(&account.cs);
    if (!endpoints_loaded && (status == 200 || !status))
    {
        endpoints = list;
        policies = pols;
        /* a failed fetch leaves the table empty and the fallback below in force; retry next time */
        endpoints_loaded = !list.empty();
    }
    LeaveCriticalSection(&account.cs);
}

/* The relying party for a URL, "" for none (the request needs no token). */
static std::string relying_party(const std::string &url)
{
    std::string u = url, scheme, host, path = "/";
    size_t p = u.find("://");
    if (p == std::string::npos) return std::string();
    scheme = u.substr(0, p);
    size_t h = p + 3, slash = u.find_first_of("/?#", h);
    host = u.substr(h, slash == std::string::npos ? std::string::npos : slash - h);
    if (slash != std::string::npos && u[slash] == '/') path = u.substr(slash, u.find_first_of("?#", slash) - slash);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host.resize(colon);
    for (auto &c : host) c = (char)tolower((unsigned char)c);
    for (auto &c : scheme) c = (char)tolower((unsigned char)c);

    load_endpoints();
    EnterCriticalSection(&account.cs);
    const Endpoint *best = nullptr;
    int best_score = -1;
    for (auto &e : endpoints)
    {
        if (!e.protocol.empty() && e.protocol != scheme) continue;
        bool match;
        if (e.wildcard)
        {
            std::string suffix = e.host.size() > 1 && e.host[0] == '*' ? e.host.substr(1) : e.host;
            match = host.size() >= suffix.size() && !host.compare(host.size() - suffix.size(), suffix.size(), suffix);
        }
        else match = host == e.host;
        if (!match) continue;
        if (!e.path.empty() && path.compare(0, e.path.size(), e.path)) continue;
        int score = (e.wildcard ? 0 : 1 << 20) + (int)e.path.size() * 2 + (int)e.host.size();
        if (score > best_score) { best_score = score; best = &e; }
    }
    std::string rp;
    bool found = best != nullptr;
    if (best) rp = best->rp;
    LeaveCriticalSection(&account.cs);
    if (!found)
    {
        /* no table (offline?): every Xbox Live service takes the default relying party */
        const char sfx[] = ".xboxlive.com";
        if (host.size() > strlen(sfx) && !host.compare(host.size() - strlen(sfx), strlen(sfx), sfx))
            rp = "http://xboxlive.com";
    }
    return rp;
}

/* SignaturePolicyCache::find_policy_for_url: among endpoints with a signature policy, the one whose protocol and
 * host match (fqdn exactly, wildcard by suffix) and whose path, if it has one, equals the URL's; the longest
 * host wins. false: the request is not signed. */
static bool find_policy(const std::string &url, SigningPolicy &out)
{
    UrlParts u = split_url(url);
    if (u.scheme != "http" && u.scheme != "https") return false;
    load_endpoints();
    EnterCriticalSection(&account.cs);
    const Endpoint *best = nullptr;
    for (auto &e : endpoints)
    {
        if (e.policy < 0 || e.protocol != u.scheme) continue;
        bool match;
        if (e.wildcard)
        {
            std::string suffix = e.host;
            while (!suffix.empty() && suffix[0] == '*') suffix.erase(0, 1);
            match = u.host.size() >= suffix.size() && !u.host.compare(u.host.size() - suffix.size(), suffix.size(), suffix);
        }
        else match = u.host == e.host;
        if (!match || (!e.path.empty() && u.path != e.path)) continue;
        if (!best || e.host.size() > best->host.size()) best = &e;
    }
    bool found = best && (size_t)best->policy < policies.size();
    if (found) out = policies[best->policy];
    LeaveCriticalSection(&account.cs);
    return found && out.es256;
}

static HRESULT get_token_for_url(const std::string &url, bool force, std::string &header, bool with_title = true)
{
    header.clear();
    std::string rp = relying_party(url);
    std::string shown = url.substr(0, url.find('?'));
    if (rp.empty())
    {
        trace("token for %s: no relying party, no token", shown.c_str());
        return S_OK;
    }
    EnterCriticalSection(&account.cs);
    bool signed_in = account.signed_in;
    std::string uhs = account.uhs, user_token = account.user_token;
    ULONGLONG user_expires = account.user_expires;
    std::string key = with_title ? rp : rp + " (no title)";
    auto it = account.xsts.find(key);
    bool cached = !force && it != account.xsts.end() && it->second.expires > now_s();
    std::string xsts = cached ? it->second.token : std::string();
    LeaveCriticalSection(&account.cs);
    if (!signed_in) return XA_E_NO_ACCOUNT;

    if (!cached)
    {
        HRESULT hr;
        Xsts x;
        if (force || user_expires <= now_s() + 60)
        {
            /* renew the whole chain */
            EnterCriticalSection(&account.signin_cs);
            hr = msa_refresh(MSA_SCOPE, nullptr);
            if (SUCCEEDED(hr))
            {
                std::string msa;
                EnterCriticalSection(&account.cs);
                msa = account.msa_token;
                LeaveCriticalSection(&account.cs);
                Xsts ax;
                Json axui;
                hr = xbl_sisu_authorize(msa, user_token, user_expires, ax, axui);
                if (FAILED(hr) && hr != XA_E_NETWORK) hr = xbl_user_authenticate(msa, user_token, user_expires);
                else if (SUCCEEDED(hr))
                {
                    EnterCriticalSection(&account.cs);
                    account.xsts["http://xboxlive.com"] = ax;
                    LeaveCriticalSection(&account.cs);
                }
                if (SUCCEEDED(hr))
                {
                    EnterCriticalSection(&account.cs);
                    account.user_token = user_token;
                    account.user_expires = user_expires;
                    LeaveCriticalSection(&account.cs);
                }
            }
            LeaveCriticalSection(&account.signin_cs);
            if (FAILED(hr)) return hr;
        }
        if (FAILED(hr = xbl_xsts(user_token, rp, x, nullptr, with_title))) return hr;
        EnterCriticalSection(&account.cs);
        account.xsts[key] = x;
        LeaveCriticalSection(&account.cs);
        xsts = x.token;
    }
    header = "XBL3.0 x=" + uhs + ";" + xsts;
    trace("token for %s: relying party %s%s", shown.c_str(), rp.c_str(), cached ? " (cached)" : "");
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
/* COM plumbing                                                                                  */

struct IInspectableImpl : IInspectable {};

#define IUNKNOWN_IINSPECTABLE_DECL                                                                        \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override;                            \
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }                      \
    ULONG STDMETHODCALLTYPE Release() override                                                            \
    { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }                              \
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override                                       \
    { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }                                              \
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override                                       \
    { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static bool is_base_iid(REFIID iid)
{
    return IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_);
}
static void trace_refused(const char *who, REFIID g)
{
    trace("%s: QueryInterface refused {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", who,
          (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4],
          g.Data4[5], g.Data4[6], g.Data4[7]);
}

struct IClosable_ : IInspectable { virtual HRESULT STDMETHODCALLTYPE Close() = 0; };

/* ------------------------------------------------------------------------------------------- */
/* IAsyncOperation<T> / IAsyncAction, completed from any thread                                  */

struct IAsyncOpAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void *result) = 0;
};
struct IAsyncInfoAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32 *id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32 *status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

enum result_kind { R_ACTION, R_IFACE, R_STRINGS1, R_STRINGS2, R_I4, R_TRANSFER };

struct AsyncOp : IAsyncOpAbi, IAsyncInfoAbi
{
    LONG ref = 1;
    GUID iid;
    result_kind kind;
    const char *name;
    CRITICAL_SECTION cs;
    INT32 status = 0; /* Started */
    HRESULT error = S_OK;
    IUnknown *handler = nullptr;
    IUnknown *iface = nullptr;
    std::string s1, s2;
    INT32 i4 = 0, i4b = 0;
    volatile LONG cancelled = 0;
    UINT32 id;

    AsyncOp(REFIID i, result_kind k, const char *n) : iid(i), kind(k), name(n)
    {
        static LONG next_id;
        id = (UINT32)InterlockedIncrement(&next_id);
        InitializeCriticalSection(&cs);
    }
    ~AsyncOp()
    {
        if (handler) handler->Release();
        if (iface) iface->Release();
        DeleteCriticalSection(&cs);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(riid) || IsEqualGUID(riid, iid)) *out = static_cast<IAsyncOpAbi *>(this);
        else if (IsEqualGUID(riid, IID_IAsyncInfo_)) *out = static_cast<IAsyncInfoAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    {
        return to_hstr(kind == R_ACTION ? L"Windows.Foundation.IAsyncAction" : L"Windows.Foundation.IAsyncOperation`1", n);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

    void invoke(IUnknown *h, INT32 st)
    {
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IAsyncOpAbi *, INT32);
        ((invoke_fn)(*(void ***)h)[3])(h, static_cast<IAsyncOpAbi *>(this), st);
    }

    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *h) override
    {
        EnterCriticalSection(&cs);
        if (handler) { LeaveCriticalSection(&cs); return E_ILLEGAL_DELEGATE_ASSIGNMENT; }
        if (h) h->AddRef();
        handler = h;
        INT32 st = status;
        LeaveCriticalSection(&cs);
        if (h && st != 0) invoke(h, st); /* already done: call it now */
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **h) override
    {
        if (!h) return E_POINTER;
        EnterCriticalSection(&cs);
        if ((*h = handler)) handler->AddRef();
        LeaveCriticalSection(&cs);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResults(void *result) override
    {
        EnterCriticalSection(&cs);
        INT32 st = status;
        HRESULT err = error;
        LeaveCriticalSection(&cs);
        if (st == 3) return err;
        if (st == 2) return E_ILLEGAL_METHOD_CALL;
        if (st != 1) return E_ILLEGAL_METHOD_CALL;
        HRESULT hr = S_OK;
        switch (kind)
        {
        case R_ACTION: break;
        case R_IFACE:
            if (!result) return E_POINTER;
            if ((*(IUnknown **)result = iface)) iface->AddRef();
            break;
        case R_STRINGS1:
            if (!result) return E_POINTER;
            hr = to_hstr(s1, (HSTRING *)result);
            break;
        case R_STRINGS2:
        {
            if (!result) return E_POINTER;
            HSTRING *pair = (HSTRING *)result;
            pair[0] = pair[1] = nullptr;
            if (SUCCEEDED(hr = to_hstr(s1, &pair[0])) && FAILED(hr = to_hstr(s2, &pair[1])))
            {
                WindowsDeleteString(pair[0]);
                pair[0] = nullptr;
            }
            break;
        }
        case R_I4:
            if (!result) return E_POINTER;
            *(INT32 *)result = i4;
            break;
        case R_TRANSFER:
            if (!result) return E_POINTER;
            ((INT32 *)result)[0] = i4;
            ((INT32 *)result)[1] = i4b;
            break;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE get_Id(UINT32 *v) override { if (!v) return E_POINTER; *v = id; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(INT32 *v) override
    {
        if (!v) return E_POINTER;
        EnterCriticalSection(&cs); *v = status; LeaveCriticalSection(&cs);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *v) override
    {
        if (!v) return E_POINTER;
        EnterCriticalSection(&cs); *v = status == 3 ? error : S_OK; LeaveCriticalSection(&cs);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Cancel() override
    {
        InterlockedExchange(&cancelled, 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

    /* finish: hr fails -> Error, E_ABORT after Cancel -> Canceled, else Completed */
    void finish(HRESULT hr)
    {
        EnterCriticalSection(&cs);
        if (status != 0) { LeaveCriticalSection(&cs); return; }
        if (hr == E_ABORT && cancelled) status = 2;
        else if (FAILED(hr)) { status = 3; error = hr; }
        else status = 1;
        INT32 st = status;
        IUnknown *h = handler;
        if (h) h->AddRef();
        LeaveCriticalSection(&cs);
        trace("%s #%u -> %s (hr %#lx)", name, id, st == 1 ? "Completed" : st == 2 ? "Canceled" : "Error", (unsigned long)hr);
        if (h) { invoke(h, st); h->Release(); }
    }
};

/* Runs work(op) on its own thread, then finishes the operation with its result. */
template <typename F> static AsyncOp *start_async(REFIID iid, result_kind kind, const char *name, F work)
{
    struct Ctx { AsyncOp *op; F work; };
    AsyncOp *op = new AsyncOp(iid, kind, name);
    Ctx *ctx = new Ctx{ op, work };
    op->AddRef(); /* for the thread */
    HANDLE t = CreateThread(nullptr, 0, [](void *param) -> DWORD {
        Ctx *c = (Ctx *)param;
        HRESULT hr = c->work(c->op);
        c->op->finish(hr);
        c->op->Release();
        delete c;
        return 0;
    }, ctx, 0, nullptr);
    if (t) CloseHandle(t);
    else
    {
        op->finish(E_OUTOFMEMORY);
        op->Release();
        delete ctx;
    }
    return op;
}

/* ------------------------------------------------------------------------------------------- */
/* Event lists                                                                                   */

struct EventList
{
    CRITICAL_SECTION cs;
    std::vector<std::pair<INT64, IUnknown *>> handlers;
    EventList() { InitializeCriticalSection(&cs); }
    ~EventList()
    {
        for (auto &h : handlers) h.second->Release();
        DeleteCriticalSection(&cs);
    }
    HRESULT add(IUnknown *h, INT64 *token)
    {
        static LONG64 next;
        if (!h || !token) return E_POINTER;
        h->AddRef();
        *token = InterlockedIncrement64(&next);
        EnterCriticalSection(&cs);
        handlers.emplace_back(*token, h);
        LeaveCriticalSection(&cs);
        return S_OK;
    }
    HRESULT remove(INT64 token)
    {
        IUnknown *h = nullptr;
        EnterCriticalSection(&cs);
        for (size_t i = 0; i < handlers.size(); i++)
            if (handlers[i].first == token) { h = handlers[i].second; handlers.erase(handlers.begin() + i); break; }
        LeaveCriticalSection(&cs);
        if (h) h->Release();
        return S_OK;
    }
    /* EventHandler<T>::Invoke(sender, args) is slot 3 */
    void raise(IInspectable *sender, IInspectable *args)
    {
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IInspectable *, IInspectable *);
        std::vector<IUnknown *> copy;
        EnterCriticalSection(&cs);
        for (auto &h : handlers) { h.second->AddRef(); copy.push_back(h.second); }
        LeaveCriticalSection(&cs);
        for (auto h : copy) { ((invoke_fn)(*(void ***)h)[3])(h, sender, args); h->Release(); }
    }
};

/* ------------------------------------------------------------------------------------------- */
/* User                                                                                          */

struct ModernGamertag { HSTRING modern, suffix, unique; };
struct PrivilegeResult { boolean has; INT32 deny; };

struct IUserAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Gamertag(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ModernGamertagProperties(ModernGamertag *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Xuid(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_XuidStr(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AgeGroup(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStoreUser(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WebAccountId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckPrivilege(INT32, PrivilegeResult *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureAsync(IInspectable *, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMsaTicketAsync(HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMsaTicketAsync2(HSTRING, boolean, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGamerPictureAsync(INT32, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResolveUserIssueWithUiAsync(HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckConsent(HSTRING, INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE ManageConsentWithUi(HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SignOutAsync(IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_UserStateChanged(IUnknown *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_UserStateChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_UserPropertiesChanged(IUnknown *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_UserPropertiesChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_UserGamerPictureChanged(IUnknown *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_UserGamerPictureChanged(INT64) = 0;
};

struct User;
static User *current_user; /* the signed-in user of this process, guarded by account.cs */

/* UserStateChangedEventArgs: IUserStateChangedEventArgs + IUserEventArgs */
struct IUserStateChangedEventArgsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Type(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDeferral(IInspectable **, boolean *) = 0;
};
struct IUserEventArgsAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE get_User(IUserAbi **) = 0; };

struct StateChangedArgs : IUserStateChangedEventArgsAbi, IUserEventArgsAbi
{
    LONG ref = 1;
    INT32 type;
    IUserAbi *user;
    StateChangedArgs(INT32 t, IUserAbi *u) : type(t), user(u) { user->AddRef(); }
    ~StateChangedArgs() { user->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IUserStateChangedEventArgs)) *out = static_cast<IUserStateChangedEventArgsAbi *>(this);
        else if (IsEqualGUID(iid, IID_IUserEventArgs)) *out = static_cast<IUserEventArgsAbi *>(this);
        else { trace_refused("UserStateChangedEventArgs", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Microsoft.GamePlatform.Services.Identity.UserStateChangedEventArgs", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Type(INT32 *v) override { if (!v) return E_POINTER; *v = type; return S_OK; }
    HRESULT STDMETHODCALLTYPE TryGetDeferral(IInspectable **d, boolean *ok) override
    {
        if (!d || !ok) return E_POINTER;
        *d = nullptr; *ok = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_User(IUserAbi **u) override { if (!u) return E_POINTER; *u = user; user->AddRef(); return S_OK; }
};

struct User : IUserAbi, IClosable_
{
    LONG ref = 1;
    INT32 state = 0; /* SignedIn */
    EventList state_changed, properties_changed, picture_changed;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IUser)) *out = static_cast<IUserAbi *>(this);
        else if (IsEqualGUID(iid, IID_IClosable_)) *out = static_cast<IClosable_ *>(this);
        else { trace_refused("User", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Microsoft.GamePlatform.Services.Identity.User", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

    static std::string field(std::string Account::*f)
    {
        EnterCriticalSection(&account.cs);
        std::string v = account.*f;
        LeaveCriticalSection(&account.cs);
        return v;
    }

    HRESULT STDMETHODCALLTYPE get_Gamertag(HSTRING *v) override { return to_hstr(field(&Account::gtg), v); }
    HRESULT STDMETHODCALLTYPE get_ModernGamertagProperties(ModernGamertag *v) override
    {
        if (!v) return E_POINTER;
        std::string gtg = field(&Account::gtg), mgt = field(&Account::mgt), mgs = field(&Account::mgs), umg = field(&Account::umg);
        if (mgt.empty()) mgt = gtg;
        if (umg.empty()) umg = mgs.empty() ? mgt : mgt + "#" + mgs;
        v->modern = v->suffix = v->unique = nullptr;
        to_hstr(mgt, &v->modern);
        to_hstr(mgs, &v->suffix);
        to_hstr(umg, &v->unique);
        trace("User.ModernGamertagProperties");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Xuid(UINT64 *v) override
    {
        if (!v) return E_POINTER;
        *v = _strtoui64(field(&Account::xid).c_str(), nullptr, 10);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_XuidStr(HSTRING *v) override { return to_hstr(field(&Account::xid), v); }
    HRESULT STDMETHODCALLTYPE get_AgeGroup(INT32 *v) override
    {
        if (!v) return E_POINTER;
        std::string a = field(&Account::agg);
        *v = a == "Adult" ? 3 : a == "Teen" ? 2 : a == "Child" ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_State(INT32 *v) override { if (!v) return E_POINTER; *v = state; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsStoreUser(boolean *v) override
    {
        /* there is no Microsoft Store account on this machine to be the same as */
        if (!v) return E_POINTER;
        *v = FALSE;
        trace("User.IsStoreUser -> false");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_WebAccountId(HSTRING *v) override
    {
        trace("User.WebAccountId");
        return to_hstr(field(&Account::msa_user_id), v);
    }
    HRESULT STDMETHODCALLTYPE CheckPrivilege(INT32 privilege, PrivilegeResult *v) override
    {
        /* the XSTS token's "prv" claim lists the privileges the account holds */
        if (!v) return E_POINTER;
        std::string prv = " " + field(&Account::prv) + " ";
        char needle[16];
        snprintf(needle, sizeof(needle), " %d ", privilege);
        v->has = prv.find(needle) != std::string::npos;
        v->deny = v->has ? 0 : 2; /* Restricted */
        trace("User.CheckPrivilege(%d) -> %s", privilege, v->has ? "yes" : "restricted");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTokenAndSignatureAsync(IInspectable *request, IAsyncOpAbi **out) override
    {
        if (!out) return E_POINTER;
        std::string url;
        boolean force = FALSE;
        IInspectable *tr = nullptr;
        if (!request || FAILED(request->QueryInterface(IID_ITokenRequest, (void **)&tr))) return E_INVALIDARG;
        /* ITokenRequest: 6 get_Method 8 get_Url 10 get_Headers 12 get_Body 14 get_ForceRefresh 16 get_IgnoreTitleIdentity */
        typedef HRESULT (STDMETHODCALLTYPE *get_str)(IInspectable *, HSTRING *);
        typedef HRESULT (STDMETHODCALLTYPE *get_bool)(IInspectable *, boolean *);
        typedef HRESULT (STDMETHODCALLTYPE *get_obj)(IInspectable *, IInspectable **);
        std::string method = "GET", body;
        boolean ignore_title = FALSE;
        HSTRING h = nullptr;
        if (SUCCEEDED(((get_str)(*(void ***)tr)[6])(tr, &h))) { if (h) method = from_hstr(h); WindowsDeleteString(h); h = nullptr; }
        if (SUCCEEDED(((get_str)(*(void ***)tr)[8])(tr, &h))) { url = from_hstr(h); WindowsDeleteString(h); }
        ((get_bool)(*(void ***)tr)[14])(tr, &force);
        ((get_bool)(*(void ***)tr)[16])(tr, &ignore_title);
        IInspectable *buf = nullptr;
        if (SUCCEEDED(((get_obj)(*(void ***)tr)[12])(tr, &buf)) && buf)
        {
            /* IBuffer: 7 get_Length; IBufferByteAccess {905a0fef-bc53-11df-8c49-001e4fc686da}: 3 Buffer */
            static const GUID IID_IBufferByteAccess_ = {0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
            typedef HRESULT (STDMETHODCALLTYPE *get_u32)(IInspectable *, UINT32 *);
            typedef HRESULT (STDMETHODCALLTYPE *get_bytes)(IUnknown *, BYTE **);
            UINT32 len = 0;
            IUnknown *access = nullptr;
            BYTE *bytes = nullptr;
            if (SUCCEEDED(((get_u32)(*(void ***)buf)[7])(buf, &len)) && len &&
                SUCCEEDED(buf->QueryInterface(IID_IBufferByteAccess_, (void **)&access)))
            {
                if (SUCCEEDED(((get_bytes)(*(void ***)access)[3])(access, &bytes)) && bytes) body.assign((char *)bytes, len);
                access->Release();
            }
            buf->Release();
        }
        tr->Release();
        *out = start_async(IID_AsyncOp_TokenRequestResult, R_STRINGS2, "GetTokenAndSignatureAsync",
            [url, method, body, force, ignore_title](AsyncOp *op) -> HRESULT {
                std::string header;
                HRESULT hr = get_token_for_url(url, force != FALSE, header, !ignore_title);
                if (FAILED(hr)) return hr;
                op->s1 = header;
                op->s2.clear();
                SigningPolicy policy;
                if (find_policy(url, policy))
                {
                    op->s2 = sign_request(policy.version, policy.max_body_bytes, method, split_url(url).path_and_query,
                                          header, body);
                    trace("signed %s %s (policy %d, body %zu bytes)", method.c_str(), url.substr(0, url.find('?')).c_str(),
                          policy.version, body.size());
                }
                return S_OK;
            });
        return S_OK;
    }
    HRESULT msa_ticket(HSTRING scope, bool force, IAsyncOpAbi **out)
    {
        if (!out) return E_POINTER;
        std::string s = from_hstr(scope);
        trace("User.GetMsaTicketAsync(\"%s\", force %d)", s.c_str(), force);
        *out = start_async(IID_AsyncOp_MsaTicketRequestResult, R_STRINGS1, "GetMsaTicketAsync", [s](AsyncOp *op) -> HRESULT {
            std::string token;
            EnterCriticalSection(&account.signin_cs);
            HRESULT hr = msa_refresh(s, &token);
            LeaveCriticalSection(&account.signin_cs);
            if (SUCCEEDED(hr)) op->s1 = token;
            return hr;
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMsaTicketAsync(HSTRING scope, IAsyncOpAbi **out) override { return msa_ticket(scope, false, out); }
    HRESULT STDMETHODCALLTYPE GetMsaTicketAsync2(HSTRING scope, boolean force, IAsyncOpAbi **out) override
    { return msa_ticket(scope, force != FALSE, out); }
    HRESULT STDMETHODCALLTYPE GetGamerPictureAsync(INT32 size, IAsyncOpAbi **out) override
    {
        trace("User.GetGamerPictureAsync(%d) -> E_NOTIMPL", size);
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ResolveUserIssueWithUiAsync(HSTRING url, IAsyncOpAbi **out) override
    {
        /* the issue is resolved on a web page: open it, as the Windows UI would show it */
        if (!out) return E_POINTER;
        std::string u = from_hstr(url);
        trace("User.ResolveUserIssueWithUiAsync");
        if (!u.empty()) open_browser(u);
        AsyncOp *op = new AsyncOp(IID_IAsyncAction_, R_ACTION, "ResolveUserIssueWithUiAsync");
        op->finish(S_OK);
        *out = op;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckConsent(HSTRING name, INT32 *v) override
    {
        /* no consent service here: the consent does not apply */
        if (!v) return E_POINTER;
        *v = 1; /* NotApplicable */
        trace("User.CheckConsent(\"%s\") -> NotApplicable", from_hstr(name).c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ManageConsentWithUi(HSTRING name, IAsyncOpAbi **out) override
    {
        if (!out) return E_POINTER;
        trace("User.ManageConsentWithUi(\"%s\") -> NotApplicable", from_hstr(name).c_str());
        AsyncOp *op = new AsyncOp(IID_AsyncOp_ConsentState, R_I4, "ManageConsentWithUi");
        op->i4 = 1;
        op->finish(S_OK);
        *out = op;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SignOutAsync(IAsyncOpAbi **out) override;
    HRESULT STDMETHODCALLTYPE add_UserStateChanged(IUnknown *h, INT64 *t) override { return state_changed.add(h, t); }
    HRESULT STDMETHODCALLTYPE remove_UserStateChanged(INT64 t) override { return state_changed.remove(t); }
    HRESULT STDMETHODCALLTYPE add_UserPropertiesChanged(IUnknown *h, INT64 *t) override { return properties_changed.add(h, t); }
    HRESULT STDMETHODCALLTYPE remove_UserPropertiesChanged(INT64 t) override { return properties_changed.remove(t); }
    HRESULT STDMETHODCALLTYPE add_UserGamerPictureChanged(IUnknown *h, INT64 *t) override { return picture_changed.add(h, t); }
    HRESULT STDMETHODCALLTYPE remove_UserGamerPictureChanged(INT64 t) override { return picture_changed.remove(t); }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

    void raise_state(INT32 type)
    {
        StateChangedArgs *args = new StateChangedArgs(type, this);
        state_changed.raise(static_cast<IUserAbi *>(this), static_cast<IUserStateChangedEventArgsAbi *>(args));
        args->Release();
    }
};

HRESULT STDMETHODCALLTYPE User::SignOutAsync(IAsyncOpAbi **out)
{
    if (!out) return E_POINTER;
    AddRef();
    User *self = this;
    *out = start_async(IID_IAsyncAction_, R_ACTION, "SignOutAsync", [self](AsyncOp *) -> HRESULT {
        self->state = 1; /* SigningOut */
        self->raise_state(1);
        sign_out();
        EnterCriticalSection(&account.cs);
        if (current_user == self) { current_user = nullptr; self->Release(); }
        LeaveCriticalSection(&account.cs);
        self->state = 2; /* SignedOut */
        self->raise_state(2);
        self->Release();
        return S_OK;
    });
    return S_OK;
}

static User *get_current_user(void)
{
    EnterCriticalSection(&account.cs);
    if (!current_user && account.signed_in) current_user = new User();
    User *u = current_user;
    if (u) u->AddRef();
    LeaveCriticalSection(&account.cs);
    return u;
}

/* ------------------------------------------------------------------------------------------- */
/* XboxAuthInitializationArgs                                                                   */

struct IXboxAuthInitializationArgsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_MsaAppId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MsaAppId(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TitleId(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_TitleId(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EnablePresence(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_EnablePresence(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EnableSpop(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_EnableSpop(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RequiredUcsConsents(UINT32 *, HSTRING **) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_RequiredUcsConsents(UINT32, HSTRING *) = 0;
};

struct InitArgs : IXboxAuthInitializationArgsAbi
{
    LONG ref = 1;
    std::string msa_app_id;
    UINT32 title_id = 0;
    boolean presence = FALSE, spop = FALSE;
    std::vector<std::string> consents;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IXboxAuthInitializationArgs)) *out = static_cast<IXboxAuthInitializationArgsAbi *>(this);
        else { trace_refused("XboxAuthInitializationArgs", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_MsaAppId(HSTRING *v) override { return to_hstr(msa_app_id, v); }
    HRESULT STDMETHODCALLTYPE put_MsaAppId(HSTRING v) override { msa_app_id = from_hstr(v); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TitleId(UINT32 *v) override { if (!v) return E_POINTER; *v = title_id; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_TitleId(UINT32 v) override { title_id = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_EnablePresence(boolean *v) override { if (!v) return E_POINTER; *v = presence; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_EnablePresence(boolean v) override { presence = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_EnableSpop(boolean *v) override { if (!v) return E_POINTER; *v = spop; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_EnableSpop(boolean v) override { spop = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_RequiredUcsConsents(UINT32 *n, HSTRING **v) override
    {
        if (!n || !v) return E_POINTER;
        *n = 0; *v = nullptr;
        if (consents.empty()) return S_OK;
        HSTRING *arr = (HSTRING *)CoTaskMemAlloc(consents.size() * sizeof(HSTRING));
        if (!arr) return E_OUTOFMEMORY;
        for (size_t i = 0; i < consents.size(); i++) { arr[i] = nullptr; to_hstr(consents[i], &arr[i]); }
        *n = (UINT32)consents.size();
        *v = arr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_RequiredUcsConsents(UINT32 n, HSTRING *v) override
    {
        consents.clear();
        for (UINT32 i = 0; i < n && v; i++) consents.push_back(from_hstr(v[i]));
        return S_OK;
    }
};

struct IXboxAuthInitializationArgsFactoryAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE CreateInstance(HSTRING, UINT32, InitArgs **) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInstance2(HSTRING, UINT32, boolean, boolean, UINT32, HSTRING *, InitArgs **) = 0;
};
struct IActivationFactoryAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **) = 0; };

struct InitArgsFactory : IXboxAuthInitializationArgsFactoryAbi, IActivationFactoryAbi
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IActivationFactory)) *out = static_cast<IActivationFactoryAbi *>(this);
        else if (IsEqualGUID(iid, IID_IXboxAuthInitializationArgsFactory)) *out = static_cast<IXboxAuthInitializationArgsFactoryAbi *>(this);
        else { trace_refused("XboxAuthInitializationArgs factory", iid); *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        *out = static_cast<IXboxAuthInitializationArgsAbi *>(new InitArgs());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(HSTRING app, UINT32 title, InitArgs **out) override
    {
        if (!out) return E_POINTER;
        InitArgs *a = new InitArgs();
        a->msa_app_id = from_hstr(app);
        a->title_id = title;
        trace("XboxAuthInitializationArgs(msa app %s, title %#x)", a->msa_app_id.c_str(), title);
        *out = a;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance2(HSTRING app, UINT32 title, boolean presence, boolean spop, UINT32 n,
                                              HSTRING *consents, InitArgs **out) override
    {
        if (!out) return E_POINTER;
        InitArgs *a = new InitArgs();
        a->msa_app_id = from_hstr(app);
        a->title_id = title;
        a->presence = presence;
        a->spop = spop;
        a->put_RequiredUcsConsents(n, consents);
        trace("XboxAuthInitializationArgs(msa app %s, title %#x, presence %d, spop %d, %u consents)",
              a->msa_app_id.c_str(), title, presence, spop, n);
        *out = a;
        return S_OK;
    }
};
static InitArgsFactory init_args_factory;

/* ------------------------------------------------------------------------------------------- */
/* XboxAuthManager                                                                               */

struct IXboxAuthManagerAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Sandbox(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryAddDefaultUserSilentlyAsync(IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddUserWithOptionalUiAsync(IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddUserWithUiAsync(IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceUserAsync(IAsyncOpAbi **) = 0;
};

static HRESULT complete_with_user(AsyncOp *op, HRESULT hr)
{
    if (FAILED(hr)) return hr;
    User *u = get_current_user();
    op->iface = u ? static_cast<IUserAbi *>(u) : nullptr;
    return S_OK;
}

struct Manager : IXboxAuthManagerAbi, IClosable_
{
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IXboxAuthManager)) *out = static_cast<IXboxAuthManagerAbi *>(this);
        else if (IsEqualGUID(iid, IID_IClosable_)) *out = static_cast<IClosable_ *>(this);
        else { trace_refused("XboxAuthManager", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

    HRESULT STDMETHODCALLTYPE get_Sandbox(HSTRING *v) override { return to_hstr(L"RETAIL", v); }
    HRESULT STDMETHODCALLTYPE TryAddDefaultUserSilentlyAsync(IAsyncOpAbi **out) override
    {
        if (!out) return E_POINTER;
        trace("XboxAuthManager.TryAddDefaultUserSilentlyAsync");
        *out = start_async(IID_AsyncOp_User, R_IFACE, "TryAddDefaultUserSilentlyAsync", [](AsyncOp *op) -> HRESULT {
            HRESULT hr = sign_in_silently();
            if (hr == XA_E_NO_ACCOUNT) { trace("no stored sign-in: no default user"); return S_OK; }
            return complete_with_user(op, hr);
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddUserWithOptionalUiAsync(IAsyncOpAbi **out) override
    {
        if (!out) return E_POINTER;
        trace("XboxAuthManager.AddUserWithOptionalUiAsync");
        *out = start_async(IID_AsyncOp_User, R_IFACE, "AddUserWithOptionalUiAsync", [](AsyncOp *op) -> HRESULT {
            HRESULT hr = sign_in_silently();
            if (FAILED(hr) && hr != XA_E_NETWORK) hr = sign_in_with_device_code(&op->cancelled);
            return complete_with_user(op, hr);
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddUserWithUiAsync(IAsyncOpAbi **out) override
    {
        if (!out) return E_POINTER;
        trace("XboxAuthManager.AddUserWithUiAsync");
        *out = start_async(IID_AsyncOp_User, R_IFACE, "AddUserWithUiAsync", [](AsyncOp *op) -> HRESULT {
            EnterCriticalSection(&account.cs);
            bool in = account.signed_in;
            LeaveCriticalSection(&account.cs);
            HRESULT hr = in ? S_OK : sign_in_with_device_code(&op->cancelled);
            return complete_with_user(op, hr);
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceUserAsync(IAsyncOpAbi **out) override
    {
        /* a device identity needs an Xbox device token and a proof key; this has neither */
        trace("XboxAuthManager.GetDeviceUserAsync -> E_NOTIMPL");
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};

struct IXboxAuthManagerFactoryAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE CreateInstance(IInspectable *, IXboxAuthManagerAbi **) = 0; };
struct IXboxAuthManagerStaticsAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE AddUserWithTransferTokenAsync(HSTRING, IAsyncOpAbi **) = 0; };

struct ManagerFactory : IXboxAuthManagerFactoryAbi, IXboxAuthManagerStaticsAbi, IActivationFactoryAbi
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IActivationFactory)) *out = static_cast<IActivationFactoryAbi *>(this);
        else if (IsEqualGUID(iid, IID_IXboxAuthManagerFactory)) *out = static_cast<IXboxAuthManagerFactoryAbi *>(this);
        else if (IsEqualGUID(iid, IID_IXboxAuthManagerStatics)) *out = static_cast<IXboxAuthManagerStaticsAbi *>(this);
        else { trace_refused("XboxAuthManager factory", iid); *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IInspectable *args, IXboxAuthManagerAbi **out) override
    {
        if (!out) return E_POINTER;
        std::string app;
        UINT32 title = 0;
        IXboxAuthInitializationArgsAbi *a = nullptr;
        if (args && SUCCEEDED(args->QueryInterface(IID_IXboxAuthInitializationArgs, (void **)&a)))
        {
            HSTRING h = nullptr;
            if (SUCCEEDED(a->get_MsaAppId(&h))) { app = from_hstr(h); WindowsDeleteString(h); }
            a->get_TitleId(&title);
            a->Release();
        }
        trace("XboxAuthManager(msa app %s, title %#x)", app.c_str(), title);
        *out = new Manager();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddUserWithTransferTokenAsync(HSTRING, IAsyncOpAbi **out) override
    {
        if (!out) return E_POINTER;
        trace("XboxAuthManager.AddUserWithTransferTokenAsync -> NotSupported");
        AsyncOp *op = new AsyncOp(IID_AsyncOp_TransferTokenRequestResult, R_TRANSFER, "AddUserWithTransferTokenAsync");
        op->i4 = 1; /* NotSupported */
        op->i4b = S_OK;
        op->finish(S_OK);
        *out = op;
        return S_OK;
    }
};
static ManagerFactory manager_factory;

/* ------------------------------------------------------------------------------------------- */

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const wchar_t *name = WindowsGetStringRawBuffer(classid, nullptr);
    account_init();
    if (!wcscmp(name, L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager"))
        *factory = (IActivationFactory *)static_cast<IActivationFactoryAbi *>(&manager_factory);
    else if (!wcscmp(name, L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs"))
        *factory = (IActivationFactory *)static_cast<IActivationFactoryAbi *>(&init_args_factory);
    else
    {
        trace("%ls: no stand-in", name);
        *factory = nullptr;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    trace("%ls: factory", name);
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

/* For probes/probe-xboxauth.cpp: exercises the HTTP paths without an app.
 *   "endpoints <url>"   relying party lookup for a URL
 *   "xbl <rps>"         user/authenticate with a deliberately invalid ticket (request shape, error path)
 *   "devicecode"        obtains a device code and stops (no browser, no polling)
 *   "silent"            silent sign-in from the stored refresh token, then the profile claims */
extern "C" __declspec(dllexport) int WINAPI XboxAuthSelfTest(const char *what, const char *arg)
{
    account_init();
    if (!strcmp(what, "sigtest"))
    {
        /* xal-rs src/request_signer.rs tests: prehashed_data and verify_real_request */
        int fails = 0;
        unsigned char ver[4] = { 0, 0, 0, 1 }, ft[8], hash[32];
        ULONGLONG t = (1586999965ULL + 11644473600ULL) * 10000000ULL;
        for (int i = 0; i < 8; i++) ft[i] = (unsigned char)(t >> (56 - 8 * i));
        sha256(sign_prehash_input(ver, ft, "POST", "/path?query=1", "XBL3.0 x=userid;jsonwebtoken", "thebodygoeshere", 8192), hash);
        static const unsigned char want[32] = { 0xf7,0xd6,0x1b,0x6f,0x8d,0x4d,0xcd,0x86,0xda,0x1a,0xa8,0x55,0x3f,0x0e,0xe7,0xc1,
                                                0x54,0x50,0x81,0x1e,0x7c,0xd2,0x75,0x93,0x64,0xe2,0x2f,0x67,0xd8,0x53,0xff,0x50 };
        bool ok = !memcmp(hash, want, 32);
        fails += !ok;
        trace("sigtest prehash vector: %s", ok ? "match" : "MISMATCH");

        std::string body = "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":\"ProofOfPossession\",\"Id\":\"{e51d4344-196a-4550-9e27-f6c5006a9949}\",\"DeviceType\":\"Android\",\"Version\":\"8.0.0\",\"ProofKey\":{\"kty\":\"EC\",\"alg\":\"ES256\",\"crv\":\"P-256\",\"x\":\"GJS1AAhiPYw0ZSQJDCF8kcZkKAc2tRWXAN6Yw-o_hMQ\",\"y\":\"UAQJHUc_yVIhUQgIvoED3kt0JDz_G8jgXys_XR2s-Xc\",\"use\":\"sig\"}}}";
        std::string sig = unb64("AAAAAQHY4xgs5DyIujFG5E5MZ4D1xjd9Up+H4AKLoyBHd95MAUZcabUN//Y/gijed4vvKtlfp4Cd4dJzVhpK0m+sYZcYRqQjBEKAZw==");
        std::string x = unb64("GJS1AAhiPYw0ZSQJDCF8kcZkKAc2tRWXAN6Yw-o_hMQ"), y = unb64("UAQJHUc_yVIhUQgIvoED3kt0JDz_G8jgXys_XR2s-Xc");
        std::vector<unsigned char> pub(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
        BCRYPT_ECCKEY_BLOB *hdr = (BCRYPT_ECCKEY_BLOB *)pub.data();
        hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        hdr->cbKey = 32;
        memcpy(&pub[sizeof(*hdr)], x.data(), 32);
        memcpy(&pub[sizeof(*hdr) + 32], y.data(), 32);
        BCRYPT_KEY_HANDLE k = nullptr;
        ok = sig.size() == 76 && x.size() == 32 && y.size() == 32 &&
             BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, pub.data(), (ULONG)pub.size(), 0));
        if (ok)
        {
            sha256(sign_prehash_input((const unsigned char *)sig.data(), (const unsigned char *)sig.data() + 4, "POST",
                                      "/device/authenticate", "", body, 8192), hash);
            ok = BCRYPT_SUCCESS(BCryptVerifySignature(k, nullptr, hash, 32, (PUCHAR)sig.data() + 12, 64, 0));
            BCryptDestroyKey(k);
        }
        fails += !ok;
        trace("sigtest real device/authenticate signature from xal-rs: %s", ok ? "verifies" : "DOES NOT VERIFY");

        /* our own key: sign, then verify through the exported public key */
        std::string mine = unb64(sign_request(1, 8192, "POST", "/path?query=1", "XBL3.0 x=userid;jsonwebtoken", "thebodygoeshere"));
        ok = mine.size() == 76 && !memcmp(mine.data(), ver, 4);
        if (ok)
        {
            sha256(sign_prehash_input((const unsigned char *)mine.data(), (const unsigned char *)mine.data() + 4, "POST",
                                      "/path?query=1", "XBL3.0 x=userid;jsonwebtoken", "thebodygoeshere", 8192), hash);
            ok = BCRYPT_SUCCESS(BCryptVerifySignature(device.key, nullptr, hash, 32, (PUCHAR)mine.data() + 12, 64, 0));
        }
        fails += !ok;
        trace("sigtest own proof key round trip: %s", ok ? "verifies" : "FAILS");
        return fails;
    }
    if (!strcmp(what, "devicetoken"))
    {
        if (arg && *arg) SetEnvironmentVariableA("WINE_UWP_XBOX_DEVICE_TYPE", arg);
        std::string token;
        Json j;
        HRESULT hr = xbl_device_token(token, true, &j);
        const Json &xdi = j["DisplayClaims"]["xdi"];
        trace("device token -> %#lx: %zu chars, NotAfter %s, claims did %s dcs %s", (unsigned long)hr, token.size(),
              j["NotAfter"].str().c_str(), xdi["did"].str().empty() ? "absent" : "present", xdi["dcs"].str().c_str());
        if (SUCCEEDED(hr))
        {
            /* an XSTS request with the device token only: proves the token and the signature are accepted together */
            Xsts x;
            std::string resp, url = "https://xsts.auth.xboxlive.com/xsts/authorize", body =
                "{\"RelyingParty\":\"http://xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\","
                "\"DeviceToken\":\"" + token + "\"}}";
            DWORD status = http_request(L"POST", url, signed_headers(url, body), body, resp);
            Json r = parse_json(resp);
            trace("xsts with the device token alone: HTTP %lu, token %zu chars, XErr %s", status, r["Token"].str().size(),
                  r["XErr"].str().c_str());
            /* and the same with a corrupted signature, as a control: it must be refused */
            auto hdrs = signed_headers(url, body);
            for (auto &hh : hdrs) if (!hh.compare(0, 11, "Signature: ")) hh[hh.size() - 5] = hh[hh.size() - 5] == 'A' ? 'B' : 'A';
            status = http_request(L"POST", url, hdrs, body, resp);
            trace("control, same request with a damaged signature: HTTP %lu", status);

            /* controls on device/authenticate itself: unsigned, and with one signature byte changed */
            std::string durl = "https://device.auth.xboxlive.com/device/authenticate", dbody =
                "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":"
                "\"ProofOfPossession\",\"Id\":\"" + device.id + "\",\"DeviceType\":\"Win32\",\"Version\":\"10.0.26100\","
                "\"ProofKey\":" + proof_key_json() + "}}";
            auto dh = signed_headers(durl, dbody);
            status = http_request(L"POST", durl, dh, dbody, resp);
            trace("device/authenticate, signed: HTTP %lu", status);
            std::vector<std::string> unsigned_h;
            for (auto &hh : dh) if (hh.compare(0, 11, "Signature: ")) unsigned_h.push_back(hh);
            status = http_request(L"POST", durl, unsigned_h, dbody, resp);
            trace("device/authenticate, no Signature: HTTP %lu, XErr %s", status, parse_json(resp)["XErr"].str().c_str());
            for (auto &hh : dh)
                if (!hh.compare(0, 11, "Signature: "))
                {
                    std::string raw = unb64(hh.substr(11));
                    raw[40] ^= 1;
                    hh = "Signature: " + b64((const unsigned char *)raw.data(), raw.size(), false);
                }
            status = http_request(L"POST", durl, dh, dbody, resp);
            trace("device/authenticate, one signature bit flipped: HTTP %lu, XErr %s", status, parse_json(resp)["XErr"].str().c_str());
        }
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (!strcmp(what, "sisu"))
    {
        /* sisu/authorize with an invalid access token: the shape of the answer, no account needed */
        std::string u;
        ULONGLONG e;
        Xsts x;
        Json xui;
        HRESULT hr = xbl_sisu_authorize("invalid", u, e, x, xui);
        trace("sisu/authorize with an invalid ticket -> %#lx", (unsigned long)hr);
        return 0;
    }
    if (!strcmp(what, "policy"))
    {
        SigningPolicy sp;
        bool f = find_policy(arg ? arg : "", sp);
        trace("signature policy for %s: %s version %d max body %llu", arg ? arg : "", f ? "yes" : "none", sp.version, sp.max_body_bytes);
        return 0;
    }
    if (!strcmp(what, "endpoints"))
    {
        std::string rp = relying_party(arg ? arg : "");
        trace("relying party for %s: \"%s\"", arg ? arg : "", rp.c_str());
        return 0;
    }
    if (!strcmp(what, "xbl"))
    {
        std::string t;
        ULONGLONG e;
        HRESULT hr = xbl_user_authenticate(arg ? arg : "invalid", t, e);
        trace("user/authenticate -> %#lx", (unsigned long)hr);
        Xsts x;
        hr = xbl_xsts("invalid", "http://xboxlive.com", x, nullptr);
        trace("xsts -> %#lx", (unsigned long)hr);
        return 0;
    }
    if (!strcmp(what, "devicecode"))
    {
        std::string resp;
        DWORD status = http_form("https://login.live.com/oauth20_connect.srf",
                                 "client_id=" + std::string(MSA_CLIENT_ID) + "&scope=" + urlencode(MSA_SCOPE) +
                                 "&response_type=device_code", resp);
        Json j = parse_json(resp);
        trace("device code: HTTP %lu, user code %s, expires %.0f s, interval %.0f s, verification %s, device code %zu chars",
              status, j["user_code"].str().c_str(), j["expires_in"].n, j["interval"].n,
              j["verification_uri"].str().c_str(), j["device_code"].str().size());
        return status == 200 ? 0 : 1;
    }
    if (!strcmp(what, "browser"))
    {
        open_browser(arg ? arg : "https://www.microsoft.com/link");
        return 0;
    }
    /* get URL [contract]: an authenticated GET the way the app would send it (token from the title endpoints table,
     * Signature when the URL has a policy); prints the status and the start of the body. Tokens are not printed. */
    if (!strcmp(what, "get") && arg)
    {
        HRESULT hr = sign_in_silently();
        if (FAILED(hr)) { printf("silent sign-in failed %#lx\n", (unsigned long)hr); return 1; }
        std::string url = arg, header, resp;
        if (FAILED(hr = get_token_for_url(url, false, header))) { printf("token failed %#lx\n", (unsigned long)hr); return 1; }
        const char *contract = getenv("XBL_CONTRACT");
        std::vector<std::string> h = { "Authorization: " + header, std::string("x-xbl-contract-version: ") + (contract ? contract : "3"),
                                       "Accept: application/json", "Accept-Language: en-US" };
        SigningPolicy policy;
        if (find_policy(url, policy))
        {
            std::string sig = sign_request(policy.version, policy.max_body_bytes, "GET", split_url(url).path_and_query, header, "");
            if (!sig.empty()) h.push_back("Signature: " + sig);
        }
        DWORD status = http_request(L"GET", url, h, "", resp);
        printf("HTTP %lu, %zu bytes\n%.600s\n", (unsigned long)status, resp.size(), resp.c_str());
        return status == 200 ? 0 : 1;
    }
    if (!strcmp(what, "silent"))
    {
        HRESULT hr = sign_in_silently();
        trace("silent sign-in -> %#lx", (unsigned long)hr);
        if (SUCCEEDED(hr))
        {
            std::string h;
            hr = get_token_for_url(arg && *arg ? arg : "https://profile.xboxlive.com/users/me/profile/settings", false, h);
            trace("token -> %#lx, header %zu chars", (unsigned long)hr, h.size());
        }
        return SUCCEEDED(hr) ? 0 : 1;
    }
    return 2;
}

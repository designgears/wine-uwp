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

#include "../common/json.h"

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
    std::string puid;             /* the account's PUID, 16 hex digits: what a Microsoft account WebAccount.Id is */
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
    bool other = !j["user_id"].str().empty() && j["user_id"].str() != account.msa_user_id && !account.msa_user_id.empty();
    if (!j["user_id"].str().empty()) account.msa_user_id = j["user_id"].str();
    std::map<std::string, std::string> kv = { { "refresh_token", account.refresh_token }, { "user_id", account.msa_user_id } };
    if (other) { kv["email"] = ""; kv["store_account_id"] = ""; kv["puid"] = ""; account.puid.clear(); } /* another account: forget the last one's */
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
        if (j["error"].str() == "invalid_grant" && scope == MSA_SCOPE)
        {
            /* (only for the sign-in scope: another scope's invalid_grant means that scope is not granted) */
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
static void fetch_account_email(void);
static void fetch_account_puid(void);
static std::string stored_msa_account(void);

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
    store_update({ { "gamertag", xui["gtg"].str() } }); /* the WebAccount's user name, before this process signs in */
    fetch_account_email();
    fetch_account_puid();
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
    account.puid.clear();
    account.msa_token.clear();
    account.user_token.clear();
    account.xsts.clear();
    LeaveCriticalSection(&account.cs);
    /* the device's proof key and device token stay: they belong to the machine, not the user */
    store_update({ { "refresh_token", "" }, { "user_id", "" }, { "gamertag", "" }, { "email", "" }, { "store_account_id", "" }, { "puid", "" } });
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
        /* the Store account is the Microsoft account WebAccount below, which is this user's own account */
        if (!v) return E_POINTER;
        *v = TRUE;
        trace("User.IsStoreUser -> true");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_WebAccountId(HSTRING *v) override
    {
        trace("User.WebAccountId");
        return to_hstr(stored_msa_account(), v);
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
/* Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager: the Microsoft account  */
/*                                                                                              */
/* On Windows the Store's account is the Microsoft account WebAccount that the Web Account       */
/* Manager (the token broker, out of process) holds for provider https://login.microsoft.com,     */
/* authority "consumers". The Xbox app's StoreAccountManager asks for that provider with          */
/* FindAccountProviderAsync(id, authority, user) (Statics2), finds the account by its id and asks  */
/* GetTokenSilentlyAsync for tokens. There is no broker here, so the manager answers from the same */
/* Microsoft account the Xbox sign-in above holds: WebAccount.Id is the account's user id, which   */
/* is also what User.WebAccountId reports, and tokens are refresh-token grants for the request's  */
/* scope. Other providers are not found (null), as before (shims/webauthcore, now retired).       */
/*                                                                                              */
/* Slots (Windows.Security.winmd, ./winmd.py --sig):                                             */
/*   Statics {6aca7c92}: 6/7 GetTokenSilentlyAsync(request[, account]) 8/9 RequestTokenAsync      */
/*                       10 FindAccountAsync(provider, id) 11/12 FindAccountProviderAsync(id[, auth]) */
/*   Statics2 {f584184a}: 6 FindAccountProviderAsync(id, authority, user)                          */
/*   Statics4 {54e633fe}: 6/7 FindAllAccountsAsync(provider[, clientId]) 8-10 FindSystemAccountProviderAsync */

static const GUID IID_IWACMStatics = {0x6aca7c92,0xa581,0x4479,{0x9c,0x10,0x75,0x2e,0xff,0x44,0xfd,0x34}};
static const GUID IID_IWACMStatics2 = {0xf584184a,0x8b57,0x4820,{0xb6,0xa4,0x70,0xa5,0xb6,0xfc,0xf4,0x4a}};
static const GUID IID_IWACMStatics4 = {0x54e633fe,0x96e0,0x41e8,{0x98,0x32,0x12,0x98,0x89,0x7c,0x2a,0xaf}};
static const GUID IID_IWebAccountProvider = {0x29dcc8c3,0x7ab9,0x4a7c,{0xa3,0x36,0xb9,0x42,0xf9,0xdb,0xf7,0xc7}};
static const GUID IID_IWebAccountProvider2 = {0x4a01eb05,0x4e42,0x41d4,{0xb5,0x18,0xe0,0x08,0xa5,0x16,0x36,0x14}};
static const GUID IID_IWebAccountProvider3 = {0xda1c518b,0x970d,0x4d49,{0x82,0x5c,0xf2,0x70,0x6f,0x8c,0xa7,0xfe}};
static const GUID IID_IWebAccountProvider4 = {0x718fd8db,0xe796,0x4210,{0xb7,0x4e,0x84,0xd2,0x98,0x94,0xb0,0x80}};
static const GUID IID_IWebAccount = {0x69473eb2,0x8031,0x49be,{0x80,0xbb,0x96,0xcb,0x46,0xd9,0x9a,0xba}};
static const GUID IID_IWebAccount2 = {0x7b56d6f8,0x990b,0x4eb5,{0x94,0xa7,0x56,0x21,0xf3,0xa8,0xb8,0x24}};
static const GUID IID_IWebTokenRequest = {0xb77b4d68,0xadcb,0x4673,{0xb3,0x64,0x0c,0xf7,0xb3,0x5c,0xaf,0x97}};
static const GUID IID_IWebTokenRequestResult = {0xc12a8305,0xd1f8,0x4483,{0x8d,0x54,0x38,0xfe,0x29,0x27,0x84,0xff}};
static const GUID IID_IWebTokenResponse = {0x67a7c5ca,0x83f6,0x44c6,{0xa3,0xb1,0x0e,0xb6,0x9e,0x41,0xfa,0x8a}};
static const GUID IID_IWebProviderError = {0xdb191bb1,0x50c5,0x4809,{0x8d,0xca,0x09,0xc9,0x94,0x10,0x24,0x5c}};
static const GUID IID_IFindAllAccountsResult = {0xa5812b5d,0xb72e,0x420c,{0x86,0xab,0xaa,0xc0,0xd7,0xb7,0x26,0x1f}};
static const GUID IID_AsyncOp_WebAccountProvider = {0x88c66009,0x12f7,0x58e2,{0x8d,0xbe,0x6e,0xfc,0x62,0x0c,0x85,0xba}};
static const GUID IID_AsyncOp_WebAccount = {0xacd76b54,0x297f,0x5a18,{0x91,0x43,0x20,0xa3,0x09,0xe2,0xdf,0xd3}};
static const GUID IID_AsyncOp_WebTokenRequestResult = {0x0a815852,0x7c44,0x5674,{0xb3,0xd2,0xfa,0x2e,0x4c,0x1e,0x46,0xc9}};
static const GUID IID_AsyncOp_FindAllAccountsResult = {0x9affb572,0x58c3,0x5c6c,{0x93,0x97,0x2b,0x77,0x04,0xaa,0x35,0xc3}};
static const GUID IID_IVectorView_WebTokenResponse = {0x199e065c,0x8195,0x55da,{0x9c,0x10,0x8a,0xea,0xf9,0xac,0x10,0x62}};
static const GUID IID_IVectorView_WebAccount = {0xe0798d3d,0x2b4a,0x589a,{0xab,0x12,0x02,0xdc,0xcc,0x15,0x8a,0xfc}};
static const GUID IID_IMap_String_String = {0xf6d1f700,0x49c2,0x52ae,{0x81,0x54,0x82,0x6f,0x99,0x08,0x77,0x3c}};
static const GUID IID_IIterable_KVP_String_String = {0xe9bdaaf0,0xcbf6,0x5c72,{0xbe,0x90,0x29,0xcb,0xf3,0xa1,0x31,0x9b}};

static const char MSA_PROVIDER_ID[] = "https://login.microsoft.com";

/* IMap<String,String>, IMapView<String,String> and their iterator, by slot */
struct IMapSSAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE Lookup(HSTRING, HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE HasKey(HSTRING, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetView(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Insert(HSTRING, HSTRING, boolean *) = 0;
};
struct IIterableAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE First(IInspectable **) = 0; };
struct IIteratorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Current(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *) = 0;
};
struct IKeyValuePairSSAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Key(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Value(HSTRING *) = 0;
};

/* A Windows.Foundation.Collections.StringMap (Windows' own, from wintypes) holding kv. */
static IMapSSAbi *make_string_map(const std::vector<std::pair<std::string, std::string>> &kv)
{
    HSTRING cls = nullptr;
    IInspectable *obj = nullptr;
    IMapSSAbi *map = nullptr;
    to_hstr(L"Windows.Foundation.Collections.StringMap", &cls);
    HRESULT hr = RoActivateInstance(cls, &obj);
    WindowsDeleteString(cls);
    if (FAILED(hr)) { trace("StringMap: activation failed, hr %#lx", (unsigned long)hr); return nullptr; }
    hr = obj->QueryInterface(IID_IMap_String_String, (void **)&map);
    obj->Release();
    if (FAILED(hr)) return nullptr;
    for (auto &p : kv)
    {
        HSTRING k = nullptr, v = nullptr;
        boolean replaced;
        to_hstr(p.first, &k);
        to_hstr(p.second, &v);
        map->Insert(k, v, &replaced);
        WindowsDeleteString(k);
        WindowsDeleteString(v);
    }
    return map;
}

/* key=value of an app's IMap<String,String>, values cut short: they are request options, but be safe */
static std::string describe_map(IInspectable *map)
{
    std::string out;
    IIterableAbi *it = nullptr;
    IInspectable *first = nullptr;
    if (!map || FAILED(map->QueryInterface(IID_IIterable_KVP_String_String, (void **)&it))) return "?";
    if (SUCCEEDED(it->First(&first)) && first)
    {
        IIteratorAbi *iter = (IIteratorAbi *)first;
        boolean has = FALSE;
        iter->get_HasCurrent(&has);
        while (has)
        {
            IInspectable *cur = nullptr;
            if (SUCCEEDED(iter->get_Current(&cur)) && cur)
            {
                HSTRING k = nullptr, v = nullptr;
                ((IKeyValuePairSSAbi *)cur)->get_Key(&k);
                ((IKeyValuePairSSAbi *)cur)->get_Value(&v);
                std::string val = from_hstr(v);
                if (val.size() > 48) val = val.substr(0, 48) + "...";
                out += (out.empty() ? "" : ", ") + from_hstr(k) + "=" + val;
                WindowsDeleteString(k);
                WindowsDeleteString(v);
                cur->Release();
            }
            if (FAILED(iter->MoveNext(&has))) break;
        }
        first->Release();
    }
    it->Release();
    return out;
}

/* The stored Microsoft account, loaded if this process has not signed in yet. Returns its user id. */
/* The id of the Microsoft account's WebAccount, or "" when nobody is signed in. */
static std::string stored_msa_account(void)
{
    EnterCriticalSection(&account.cs);
    if (account.refresh_token.empty())
    {
        store_lock();
        auto kv = store_read();
        store_unlock();
        account.refresh_token = kv["refresh_token"];
        if (account.msa_user_id.empty()) account.msa_user_id = kv["user_id"];
    }
    if (account.puid.empty() && !account.refresh_token.empty())
    {
        store_lock();
        auto kv = store_read();
        store_unlock();
        account.puid = kv["puid"];
    }
    /* the PUID once it is known (fetch_account_puid); until then the account's user id */
    std::string id = account.refresh_token.empty() ? std::string() : !account.puid.empty() ? account.puid : account.msa_user_id;
    LeaveCriticalSection(&account.cs);
    return id;
}

/* A refresh-token grant for any scope, cached until a minute before it expires. */
struct ScopedToken { std::string token, form; ULONGLONG expires = 0; };
static std::map<std::string, ScopedToken> scoped_tokens; /* guarded by account.signin_cs */

/* With oauth2_response=1 among a request's properties, WAM answers the whole OAuth response as form data (the
 * Xbox app's age verification reads an id_token out of it: "Invalid token form data" otherwise). form_out gets the
 * string and number fields of the token endpoint's JSON answer, url-encoded. */
static std::string json_to_form(const Json &j)
{
    std::string out;
    for (auto &kv : j.o)
        if (kv.second.type == Json::STR || kv.second.type == Json::NUM)
            out += (out.empty() ? "" : "&") + urlencode(kv.first) + "=" + urlencode(kv.second.str());
    return out;
}

static HRESULT msa_token_for_scope(const std::string &scope, bool force, std::string &token, std::string &error,
                                   std::string *form_out = nullptr, const std::string &extra = std::string())
{
    HRESULT hr = S_OK;
    EnterCriticalSection(&account.signin_cs);
    auto it = scoped_tokens.find(scope + extra);
    if (!force && it != scoped_tokens.end() && it->second.expires > now_s())
    {
        token = it->second.token;
        if (form_out) *form_out = it->second.form;
        LeaveCriticalSection(&account.signin_cs);
        return S_OK;
    }
    std::string refresh;
    EnterCriticalSection(&account.cs);
    refresh = account.refresh_token;
    LeaveCriticalSection(&account.cs);
    if (refresh.empty()) { LeaveCriticalSection(&account.signin_cs); return XA_E_NO_ACCOUNT; }

    std::string resp;
    DWORD status = http_form("https://login.live.com/oauth20_token.srf",
                             "grant_type=refresh_token&client_id=" + std::string(MSA_CLIENT_ID) + "&scope=" +
                             urlencode(scope) + "&refresh_token=" + urlencode(refresh) + extra, resp);
    Json j = parse_json(resp);
    if (!status) hr = XA_E_NETWORK;
    else if (status != 200 || j["access_token"].str().empty())
    {
        error = j["error"].str();
        trace("WebAuthenticationCoreManager: scope \"%s\": HTTP %lu, error \"%s\"", scope.c_str(), status, error.c_str());
        hr = XA_E_MSA;
    }
    else
    {
        token = j["access_token"].str();
        ScopedToken &t = scoped_tokens[scope + extra];
        t.token = token;
        t.form = json_to_form(j);
        if (form_out) *form_out = t.form;
        t.expires = now_s() + (ULONGLONG)(j["expires_in"].n > 120 ? j["expires_in"].n - 60 : 60);
        if (!j["refresh_token"].str().empty())
        {
            EnterCriticalSection(&account.cs);
            account.refresh_token = j["refresh_token"].str();
            std::map<std::string, std::string> kv = { { "refresh_token", account.refresh_token }, { "user_id", account.msa_user_id } };
            LeaveCriticalSection(&account.cs);
            store_update(kv);
        }
        std::string fields;
        for (auto &kv : j.o) fields += (fields.empty() ? "" : " ") + kv.first;
        trace("WebAuthenticationCoreManager: scope \"%s\": token (%zu chars, expires in %.0f s; answer fields: %s)", scope.c_str(),
              token.size(), j["expires_in"].n, fields.c_str());
    }
    LeaveCriticalSection(&account.signin_cs);
    return hr;
}

/* The Microsoft account's e-mail address, for WebAccount.UserName (what Windows shows for a Microsoft account): the
 * preferred_username claim of an "openid profile" id_token, fetched once and kept in xboxauth.bin. */
static void fetch_account_email(void)
{
    store_lock();
    auto kv = store_read();
    store_unlock();
    if (!kv["email"].empty()) return;
    std::string token, error, form;
    HRESULT hr = msa_token_for_scope("openid profile", false, token, error, &form);
    size_t at = form.find("id_token=");
    if (FAILED(hr) || at == std::string::npos) { trace("Microsoft account e-mail address: none (hr %#lx)", (unsigned long)hr); return; }
    std::string jwt = form.substr(at + 9);
    jwt = jwt.substr(0, jwt.find('&'));
    size_t d1 = jwt.find('.'), d2 = jwt.find('.', d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos) return;
    Json claims = parse_json(unb64(jwt.substr(d1 + 1, d2 - d1 - 1)));
    std::string email = claims["preferred_username"].str();
    if (email.empty()) email = claims["email"].str();
    if (email.empty()) return;
    store_update({ { "email", email } });
    trace("Microsoft account e-mail address stored (%zu chars)", email.size());
}

/* The account's PUID. A Microsoft account WebAccount's Id is the PUID in hex, and the Xbox app checks the Store
 * account against the account its content access answer is for ("ContentAccessWrongPuid"). No Microsoft account
 * token carries it (the id_token's oid is the CID, like user_id), but the content access answer does: protobuf
 * field 3 (the user), its field 1, in decimal. Fetched once, with the user's Xbox Live token, and kept. */
static bool pb_varint(const unsigned char *&p, const unsigned char *end, ULONGLONG &v)
{
    v = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7)
    {
        unsigned char c = *p++;
        v |= (ULONGLONG)(c & 0x7f) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}
/* the first length-delimited field `field` of a message */
static bool pb_field(const unsigned char *p, const unsigned char *end, unsigned field, const unsigned char *&out, size_t &len)
{
    while (p < end)
    {
        ULONGLONG key, v;
        if (!pb_varint(p, end, key)) return false;
        switch (key & 7)
        {
        case 0: if (!pb_varint(p, end, v)) return false; break;
        case 1: p += 8; break;
        case 5: p += 4; break;
        case 2:
            if (!pb_varint(p, end, v) || v > (ULONGLONG)(end - p)) return false;
            if ((key >> 3) == field) { out = p; len = (size_t)v; return true; }
            p += v;
            break;
        default: return false;
        }
    }
    return false;
}
extern "C" __declspec(dllexport) HRESULT WINAPI XboxAuthHttpGet(const char *, const char *, const char *, unsigned char **, UINT32 *, DWORD *);
static void fetch_account_puid(void)
{
    EnterCriticalSection(&account.cs);
    bool have = !account.puid.empty();
    LeaveCriticalSection(&account.cs);
    if (have) return;
    store_lock();
    auto kv = store_read();
    store_unlock();
    if (!kv["puid"].empty()) { EnterCriticalSection(&account.cs); account.puid = kv["puid"]; LeaveCriticalSection(&account.cs); return; }
    unsigned char *body = nullptr;
    UINT32 len = 0;
    DWORD status = 0;
    if (FAILED(XboxAuthHttpGet("https://contentaccess.exp.xboxservices.com/all/v1?market=US&offering=XGPUPCAPP",
                               "MS-CV: wineuwpPuidFetch.0", "1", &body, &len, &status)) || status != 200)
    {
        trace("account PUID: content access HTTP %lu", status);
        if (body) CoTaskMemFree(body);
        return;
    }
    const unsigned char *user, *id;
    size_t ulen, idlen;
    std::string dec;
    if (pb_field(body, body + len, 3, user, ulen) && pb_field(user, user + ulen, 1, id, idlen)) dec.assign((const char *)id, idlen);
    CoTaskMemFree(body);
    ULONGLONG puid = _strtoui64(dec.c_str(), nullptr, 10);
    if (!puid) { trace("account PUID: not in the content access answer"); return; }
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llX", puid);
    EnterCriticalSection(&account.cs);
    account.puid = hex;
    LeaveCriticalSection(&account.cs);
    store_update({ { "puid", hex } });
    trace("account PUID stored");
}

struct IWebAccountProviderAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IconUri(IInspectable **) = 0;
};
struct IWebAccountProvider2Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_DisplayPurpose(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Authority(HSTRING *) = 0;
};
struct IWebAccountProvider3Abi : IInspectable { virtual HRESULT STDMETHODCALLTYPE get_User(IInspectable **) = 0; };
struct IWebAccountProvider4Abi : IInspectable { virtual HRESULT STDMETHODCALLTYPE get_IsSystemProvider(boolean *) = 0; };

struct WebAccountProvider : IWebAccountProviderAbi, IWebAccountProvider2Abi, IWebAccountProvider3Abi, IWebAccountProvider4Abi
{
    LONG ref = 1;
    std::string authority;
    IInspectable *user = nullptr;
    WebAccountProvider(const std::string &a, IInspectable *u) : authority(a), user(u) { if (user) user->AddRef(); }
    ~WebAccountProvider() { if (user) user->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IWebAccountProvider)) *out = static_cast<IWebAccountProviderAbi *>(this);
        else if (IsEqualGUID(iid, IID_IWebAccountProvider2)) *out = static_cast<IWebAccountProvider2Abi *>(this);
        else if (IsEqualGUID(iid, IID_IWebAccountProvider3)) *out = static_cast<IWebAccountProvider3Abi *>(this);
        else if (IsEqualGUID(iid, IID_IWebAccountProvider4)) *out = static_cast<IWebAccountProvider4Abi *>(this);
        else { trace_refused("WebAccountProvider", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Security.Credentials.WebAccountProvider", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

    HRESULT STDMETHODCALLTYPE get_Id(HSTRING *v) override { return to_hstr(MSA_PROVIDER_ID, v); }
    HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *v) override { return to_hstr(L"Microsoft account", v); }
    HRESULT STDMETHODCALLTYPE get_IconUri(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        *v = nullptr;
        trace("WebAccountProvider.IconUri -> null");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_DisplayPurpose(HSTRING *v) override { if (!v) return E_POINTER; *v = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Authority(HSTRING *v) override { return to_hstr(authority, v); }
    HRESULT STDMETHODCALLTYPE get_User(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if ((*v = user)) user->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsSystemProvider(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
};

struct IWebAccountAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_WebAccountProvider(IWebAccountProviderAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UserName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32 *) = 0;
};
struct IWebAccount2Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPictureAsync(INT32, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SignOutAsync(IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SignOutAsync2(HSTRING, IAsyncOpAbi **) = 0;
};

struct WebAccount : IWebAccountAbi, IWebAccount2Abi
{
    LONG ref = 1;
    WebAccountProvider *provider;
    std::string id, user_name;
    WebAccount(WebAccountProvider *p, const std::string &i, const std::string &n) : provider(p), id(i), user_name(n) { provider->AddRef(); }
    ~WebAccount() { provider->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IWebAccount)) *out = static_cast<IWebAccountAbi *>(this);
        else if (IsEqualGUID(iid, IID_IWebAccount2)) *out = static_cast<IWebAccount2Abi *>(this);
        else { trace_refused("WebAccount", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Security.Credentials.WebAccount", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

    HRESULT STDMETHODCALLTYPE get_WebAccountProvider(IWebAccountProviderAbi **v) override
    {
        if (!v) return E_POINTER;
        provider->AddRef();
        *v = provider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_UserName(HSTRING *v) override { trace("WebAccount.UserName"); return to_hstr(user_name, v); }
    HRESULT STDMETHODCALLTYPE get_State(INT32 *v) override { if (!v) return E_POINTER; *v = 1; /* Connected */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Id(HSTRING *v) override { return to_hstr(id, v); }
    HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        *v = nullptr;
        trace("WebAccount.Properties");
        IMapSSAbi *map = make_string_map({ { "Authority", provider->authority }, { "UserName", user_name }, { "CID", id } });
        if (!map) return E_OUTOFMEMORY;
        HRESULT hr = map->GetView(v);
        map->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetPictureAsync(INT32 size, IAsyncOpAbi **v) override;
    HRESULT STDMETHODCALLTYPE SignOutAsync(IAsyncOpAbi **v) override
    {
        /* signing the Store out would sign Xbox out too: it is one account here. Accept and keep it. */
        trace("WebAccount.SignOutAsync -> kept");
        if (!v) return E_POINTER;
        AsyncOp *op = new AsyncOp(IID_IAsyncAction_, R_ACTION, "WebAccount.SignOutAsync");
        op->finish(S_OK);
        *v = op;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SignOutAsync2(HSTRING, IAsyncOpAbi **v) override { return SignOutAsync(v); }
};

/* The account picture: the Xbox gamer picture of the same account (profile setting GameDisplayPicRaw), as an
 * IRandomAccessStream over the downloaded PNG. The app's StoreAccountModule.GetProfile reads it, and without it
 * reports no Store account at all. If the picture cannot be fetched, a 1x1 transparent PNG stands in. */
static const GUID IID_AsyncOp_RandomAccessStream = {0x430ecece,0x1418,0x5d19,{0x81,0xb2,0x5d,0xdb,0x38,0x16,0x03,0xcc}};
static const GUID IID_IRandomAccessStream_ = {0x905a0fe1,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};

static std::string gamer_picture(int size)
{
    std::string url = "https://profile.xboxlive.com/users/me/profile/settings?settings=GameDisplayPicRaw", header, resp;
    if (FAILED(sign_in_silently()) || FAILED(get_token_for_url(url, false, header))) return std::string();
    std::vector<std::string> h = { "Authorization: " + header, "x-xbl-contract-version: 2", "Accept: application/json" };
    SigningPolicy policy;
    if (find_policy(url, policy))
    {
        std::string sig = sign_request(policy.version, policy.max_body_bytes, "GET", split_url(url).path_and_query, header, "");
        if (!sig.empty()) h.push_back("Signature: " + sig);
    }
    DWORD status = http_request(L"GET", url, h, "", resp);
    std::string pic = parse_json(resp)["profileUsers"].at(0)["settings"].at(0)["value"].str();
    if (status != 200 || pic.empty()) { trace("gamer picture: profile HTTP %lu, no picture", status); return std::string(); }
    char dims[64];
    snprintf(dims, sizeof(dims), "%sw=%d&h=%d", pic.find('?') == std::string::npos ? "?" : "&", size, size);
    status = http_request(L"GET", pic + dims, { "Accept: image/png" }, "", resp);
    trace("gamer picture: HTTP %lu, %zu bytes", status, resp.size());
    return status == 200 ? resp : std::string();
}

/* A Windows.Storage.Streams.InMemoryRandomAccessStream holding bytes, positioned at 0. Wine's completes WriteAsync inline. */
static const GUID IID_IBuffer_ = {0x905a0fe0,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IBufferByteAccess_ = {0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IOutputStream_ = {0x905a0fe6,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
struct IBufferAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32) = 0;
};
struct IBufferByteAccessAbi : IUnknown { virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE **) = 0; };
struct ByteBuffer : IBufferAbi, IBufferByteAccessAbi
{
    LONG ref = 1;
    std::string bytes;
    UINT32 length;
    ByteBuffer(const std::string &b) : bytes(b), length((UINT32)b.size()) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IBuffer_)) *out = static_cast<IBufferAbi *>(this);
        else if (IsEqualGUID(iid, IID_IBufferByteAccess_)) *out = static_cast<IBufferByteAccessAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Storage.Streams.Buffer", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Capacity(UINT32 *v) override { if (!v) return E_POINTER; *v = (UINT32)bytes.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Length(UINT32 *v) override { if (!v) return E_POINTER; *v = length; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Length(UINT32 v) override { if (v > bytes.size()) return E_INVALIDARG; length = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE Buffer(BYTE **v) override { if (!v) return E_POINTER; *v = (BYTE *)&bytes[0]; return S_OK; }
};
struct IRandomAccessStreamSeekAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Size(UINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputStreamAt(UINT64, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOutputStreamAt(UINT64, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Seek(UINT64) = 0;
};
struct IOutputStreamAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE WriteAsync(IBufferAbi *, IAsyncInfoAbi **) = 0; };

static HRESULT make_memory_stream(const std::string &bytes, IInspectable **out)
{
    HSTRING cls = nullptr;
    IInspectable *obj = nullptr;
    IOutputStreamAbi *output = nullptr;
    IRandomAccessStreamSeekAbi *ras = nullptr;
    *out = nullptr;
    to_hstr(L"Windows.Storage.Streams.InMemoryRandomAccessStream", &cls);
    HRESULT hr = RoActivateInstance(cls, &obj);
    WindowsDeleteString(cls);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = obj->QueryInterface(IID_IOutputStream_, (void **)&output)))
    {
        ByteBuffer *buf = new ByteBuffer(bytes);
        IAsyncInfoAbi *write = nullptr;
        if (SUCCEEDED(hr = output->WriteAsync(buf, &write)))
        {
            IAsyncInfoAbi *info = nullptr;
            INT32 st = 0;
            if (SUCCEEDED(write->QueryInterface(IID_IAsyncInfo_, (void **)&info)))
            {
                for (int i = 0; i < 500 && SUCCEEDED(info->get_Status(&st)) && !st; i++) Sleep(10);
                info->Release();
            }
            write->Release();
            if (st != 1) hr = E_FAIL;
        }
        buf->Release();
        output->Release();
    }
    if (SUCCEEDED(hr) && SUCCEEDED(hr = obj->QueryInterface(IID_IRandomAccessStream_, (void **)&ras)))
    {
        ras->Seek(0);
        *out = ras;
    }
    obj->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE WebAccount::GetPictureAsync(INT32 size, IAsyncOpAbi **v)
{
    if (!v) return E_POINTER;
    trace("WebAccount.GetPictureAsync(%d)", size);
    *v = start_async(IID_AsyncOp_RandomAccessStream, R_IFACE, "WebAccount.GetPictureAsync", [size](AsyncOp *op) -> HRESULT {
        static const unsigned char blank_png[] = {
            0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,
            0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,
            0x54,0x78,0x9c,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,0x0d,0x0a,0x2d,0xb4,0x00,0x00,0x00,0x00,0x49,
            0x45,0x4e,0x44,0xae,0x42,0x60,0x82 };
        std::string bytes = gamer_picture(size > 0 ? size : 208);
        if (bytes.empty()) bytes.assign((const char *)blank_png, sizeof(blank_png));
        return make_memory_stream(bytes, (IInspectable **)&op->iface);
    });
    return S_OK;
}

/* The user name a Microsoft account WebAccount shows: the account's e-mail address (fetch_account_email), or until
 * that is known the Xbox gamertag of the same account. */
static std::string web_account_user_name(void)
{
    store_lock();
    auto kv = store_read();
    store_unlock();
    if (!kv["email"].empty()) return kv["email"];
    EnterCriticalSection(&account.cs);
    std::string n = account.gtg;
    LeaveCriticalSection(&account.cs);
    if (n.empty()) n = kv["gamertag"];
    return n.empty() ? std::string("Microsoft account") : n;
}

static WebAccount *make_web_account(WebAccountProvider *provider)
{
    std::string id = stored_msa_account();
    if (id.empty()) return nullptr;
    return new WebAccount(provider, id, web_account_user_name());
}

struct IWebProviderErrorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorMessage(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **) = 0;
};
struct WebProviderError : IWebProviderErrorAbi
{
    LONG ref = 1;
    UINT32 code;
    std::string message;
    WebProviderError(UINT32 c, const std::string &m) : code(c), message(m) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IWebProviderError)) *out = static_cast<IWebProviderErrorAbi *>(this);
        else { trace_refused("WebProviderError", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Security.Authentication.Web.Core.WebProviderError", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(UINT32 *v) override { if (!v) return E_POINTER; *v = code; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ErrorMessage(HSTRING *v) override { return to_hstr(message, v); }
    HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        *v = (IInspectable *)make_string_map({});
        return *v ? S_OK : E_OUTOFMEMORY;
    }
};

struct IWebTokenResponseAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Token(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ProviderError(IWebProviderErrorAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WebAccount(IWebAccountAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **) = 0;
};
struct WebTokenResponse : IWebTokenResponseAbi
{
    LONG ref = 1;
    std::string token;
    WebAccount *web_account;
    IMapSSAbi *properties;
    WebTokenResponse(const std::string &t, WebAccount *a) : token(t), web_account(a)
    {
        if (a) a->AddRef();
        properties = make_string_map({ { "Authority", a ? a->provider->authority : "consumers" } });
    }
    ~WebTokenResponse()
    {
        if (web_account) web_account->Release();
        if (properties) properties->Release();
        SecureZeroMemory(&token[0], token.size());
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IWebTokenResponse)) *out = static_cast<IWebTokenResponseAbi *>(this);
        else { trace_refused("WebTokenResponse", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Security.Authentication.Web.Core.WebTokenResponse", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Token(HSTRING *v) override { return to_hstr(token, v); }
    HRESULT STDMETHODCALLTYPE get_ProviderError(IWebProviderErrorAbi **v) override { if (!v) return E_POINTER; *v = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_WebAccount(IWebAccountAbi **v) override
    {
        if (!v) return E_POINTER;
        if ((*v = web_account)) web_account->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if ((*v = properties)) properties->AddRef();
        return properties ? S_OK : E_OUTOFMEMORY;
    }
};

/* IVectorView<T> of interfaces, with IIterable<T> (both by slot; the iterator is IIterator<T>) */
struct IVectorViewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *, UINT32 *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IInspectable **, UINT32 *) = 0;
};
struct VectorView : IVectorViewAbi, IIterableAbi
{
    LONG ref = 1;
    GUID view_iid, iterable_iid, iterator_iid;
    const wchar_t *class_name;
    std::vector<IInspectable *> items;
    VectorView(REFIID v, REFIID it, REFIID itr, const wchar_t *n) : view_iid(v), iterable_iid(it), iterator_iid(itr), class_name(n) {}
    ~VectorView() { for (auto i : items) i->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, view_iid)) *out = static_cast<IVectorViewAbi *>(this);
        else if (IsEqualGUID(iid, iterable_iid)) *out = static_cast<IIterableAbi *>(this);
        else { trace_refused("VectorView", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(class_name, n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if (i >= items.size()) { *v = nullptr; return E_BOUNDS; }
        (*v = items[i])->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *v) override { if (!v) return E_POINTER; *v = (UINT32)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *x, UINT32 *idx, boolean *found) override
    {
        if (!idx || !found) return E_POINTER;
        *found = FALSE; *idx = 0;
        for (UINT32 i = 0; i < items.size(); i++) if (items[i] == x) { *found = TRUE; *idx = i; break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 n, IInspectable **out, UINT32 *got) override
    {
        if (!got || (n && !out)) return E_POINTER;
        *got = 0;
        for (UINT32 i = start; i < items.size() && *got < n; i++) { (out[*got] = items[i])->AddRef(); (*got)++; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE First(IInspectable **v) override;
};
struct VectorIterator : IIteratorAbi
{
    LONG ref = 1;
    VectorView *view;
    UINT32 pos = 0;
    VectorIterator(VectorView *v) : view(v) { view->AddRef(); }
    ~VectorIterator() { view->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, view->iterator_iid)) *out = static_cast<IIteratorAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Foundation.Collections.IIterator`1", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Current(IInspectable **v) override { return view->GetAt(pos, v); }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override { if (!v) return E_POINTER; *v = pos < view->items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override
    {
        if (pos < view->items.size()) pos++;
        return get_HasCurrent(v);
    }
};
HRESULT STDMETHODCALLTYPE VectorView::First(IInspectable **v)
{
    if (!v) return E_POINTER;
    *v = static_cast<IIteratorAbi *>(new VectorIterator(this));
    return S_OK;
}
static const GUID IID_IIterable_WebTokenResponse = {0x7e5bb7ec,0xbbd7,0x5575,{0x9a,0x61,0xf5,0x81,0x5f,0xa2,0x2a,0x0e}};
static const GUID IID_IIterator_WebTokenResponse = {0xf080b0c9,0xa095,0x5b3a,{0xa1,0xdc,0xd1,0x7e,0x7d,0x29,0x82,0xc7}};
static const GUID IID_IIterable_WebAccount = {0xcb15d439,0xa910,0x542a,{0x89,0xed,0x7c,0xfe,0x67,0x84,0x8a,0x83}};
static const GUID IID_IIterator_WebAccount = {0xbfb82cca,0xaebc,0x567c,{0x95,0xd9,0xeb,0xa2,0x5c,0x36,0x5f,0xaa}};

struct IWebTokenRequestResultAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_ResponseData(IVectorViewAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ResponseStatus(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ResponseError(IWebProviderErrorAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE InvalidateCacheAsync(IAsyncOpAbi **) = 0;
};
struct WebTokenRequestResult : IWebTokenRequestResultAbi
{
    LONG ref = 1;
    INT32 status;            /* WebTokenRequestStatus */
    VectorView *data;
    WebProviderError *error = nullptr;
    std::string scope;
    WebTokenRequestResult(INT32 s, const std::string &sc) : status(s), scope(sc)
    {
        data = new VectorView(IID_IVectorView_WebTokenResponse, IID_IIterable_WebTokenResponse, IID_IIterator_WebTokenResponse,
                              L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Authentication.Web.Core.WebTokenResponse>");
    }
    ~WebTokenRequestResult() { data->Release(); if (error) error->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IWebTokenRequestResult)) *out = static_cast<IWebTokenRequestResultAbi *>(this);
        else { trace_refused("WebTokenRequestResult", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Security.Authentication.Web.Core.WebTokenRequestResult", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ResponseData(IVectorViewAbi **v) override
    {
        if (!v) return E_POINTER;
        data->AddRef();
        *v = data;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ResponseStatus(INT32 *v) override { if (!v) return E_POINTER; *v = status; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ResponseError(IWebProviderErrorAbi **v) override
    {
        if (!v) return E_POINTER;
        if ((*v = error)) error->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InvalidateCacheAsync(IAsyncOpAbi **v) override
    {
        if (!v) return E_POINTER;
        EnterCriticalSection(&account.signin_cs);
        scoped_tokens.erase(scope);
        LeaveCriticalSection(&account.signin_cs);
        trace("WebTokenRequestResult.InvalidateCacheAsync(\"%s\")", scope.c_str());
        AsyncOp *op = new AsyncOp(IID_IAsyncAction_, R_ACTION, "InvalidateCacheAsync");
        op->finish(S_OK);
        *v = op;
        return S_OK;
    }
};

struct IWebTokenRequestAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_WebAccountProvider(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Scope(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ClientId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PromptType(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Properties(IInspectable **) = 0;
};

/* WebTokenRequestStatus */
enum { WTRS_SUCCESS = 0, WTRS_USER_CANCEL = 1, WTRS_USER_INTERACTION_REQUIRED = 3, WTRS_PROVIDER_NOT_AVAILABLE = 4,
       WTRS_PROVIDER_ERROR = 5 };

/* GetTokenSilentlyAsync / RequestTokenAsync: a token for the request's scope from the stored Microsoft account. */
static HRESULT get_web_token(IInspectable *request, IInspectable *account_in, bool ui, IAsyncOpAbi **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    IWebTokenRequestAbi *req = nullptr;
    if (!request || FAILED(request->QueryInterface(IID_IWebTokenRequest, (void **)&req))) return E_INVALIDARG;
    std::string scope, client, provider_id, props;
    HSTRING h = nullptr;
    INT32 prompt = 0;
    if (SUCCEEDED(req->get_Scope(&h))) { scope = from_hstr(h); WindowsDeleteString(h); h = nullptr; }
    if (SUCCEEDED(req->get_ClientId(&h))) { client = from_hstr(h); WindowsDeleteString(h); h = nullptr; }
    req->get_PromptType(&prompt);
    IInspectable *pv = nullptr;
    if (SUCCEEDED(req->get_WebAccountProvider(&pv)) && pv)
    {
        IWebAccountProviderAbi *p = nullptr;
        if (SUCCEEDED(pv->QueryInterface(IID_IWebAccountProvider, (void **)&p)))
        {
            if (SUCCEEDED(p->get_Id(&h))) { provider_id = from_hstr(h); WindowsDeleteString(h); h = nullptr; }
            p->Release();
        }
        pv->Release();
    }
    IInspectable *map = nullptr;
    std::string extra;
    if (SUCCEEDED(req->get_Properties(&map)) && map)
    {
        props = describe_map(map);
        /* "claims" (an id_token claims request, e.g. the age verification's xms_agvs) goes to the token endpoint too */
        IMapSSAbi *m = nullptr;
        if (SUCCEEDED(map->QueryInterface(IID_IMap_String_String, (void **)&m)))
        {
            HSTRING key = nullptr, value = nullptr;
            boolean has = FALSE;
            to_hstr(L"claims", &key);
            if (SUCCEEDED(m->HasKey(key, &has)) && has && SUCCEEDED(m->Lookup(key, &value)))
            {
                extra = "&claims=" + urlencode(from_hstr(value));
                WindowsDeleteString(value);
            }
            WindowsDeleteString(key);
            m->Release();
        }
        map->Release();
    }
    req->Release();
    trace("WebAuthenticationCoreManager.%s(provider \"%s\", scope \"%s\", client \"%s\", prompt %d, account %s, properties {%s})",
          ui ? "RequestTokenAsync" : "GetTokenSilentlyAsync", provider_id.c_str(), scope.c_str(), client.c_str(), prompt,
          account_in ? "given" : "none", props.c_str());

    if (account_in) account_in->AddRef();
    bool msa = !_stricmp(provider_id.c_str(), MSA_PROVIDER_ID);
    bool oauth2_response = (", " + props + ",").find(", oauth2_response=1,") != std::string::npos;
    AsyncOp *op = start_async(IID_AsyncOp_WebTokenRequestResult, R_IFACE, ui ? "RequestTokenAsync" : "GetTokenSilentlyAsync",
                              [scope, msa, account_in, ui, oauth2_response, extra](AsyncOp *op) -> HRESULT {
        WebTokenRequestResult *result;
        std::string id = stored_msa_account();
        if (!msa) result = new WebTokenRequestResult(WTRS_PROVIDER_NOT_AVAILABLE, scope);
        else if (id.empty()) result = new WebTokenRequestResult(ui ? WTRS_USER_CANCEL : WTRS_USER_INTERACTION_REQUIRED, scope);
        else
        {
            std::string token, error, form;
            HRESULT hr = msa_token_for_scope(scope, false, token, error, &form, extra);
            if (SUCCEEDED(hr) && oauth2_response) token.swap(form);
            SecureZeroMemory(&form[0], form.size());
            if (SUCCEEDED(hr))
            {
                WebAccountProvider *provider = new WebAccountProvider("consumers", nullptr);
                WebAccount *wa = new WebAccount(provider, id, web_account_user_name());
                provider->Release();
                result = new WebTokenRequestResult(WTRS_SUCCESS, scope);
                result->data->items.push_back(static_cast<IWebTokenResponseAbi *>(new WebTokenResponse(token, wa)));
                wa->Release();
                SecureZeroMemory(&token[0], token.size());
            }
            else if (error == "invalid_grant" || error == "interaction_required")
                result = new WebTokenRequestResult(WTRS_USER_INTERACTION_REQUIRED, scope);
            else
            {
                result = new WebTokenRequestResult(WTRS_PROVIDER_ERROR, scope);
                result->error = new WebProviderError((UINT32)hr, error.empty() ? "the Microsoft account service refused" : error);
            }
        }
        trace("  -> status %d", result->status);
        op->iface = static_cast<IWebTokenRequestResultAbi *>(result);
        if (account_in) account_in->Release();
        return S_OK;
    });
    *out = op;
    return S_OK;
}

static HRESULT completed_iface(REFIID iid, const char *name, IUnknown *result, IAsyncOpAbi **out)
{
    if (!out) { if (result) result->Release(); return E_POINTER; }
    AsyncOp *op = new AsyncOp(iid, R_IFACE, name);
    op->iface = result; /* takes the reference */
    op->finish(S_OK);
    *out = op;
    return S_OK;
}

static HRESULT find_provider(HSTRING id, HSTRING authority, IInspectable *user, IAsyncOpAbi **out)
{
    std::string pid = from_hstr(id), auth = authority ? from_hstr(authority) : std::string("consumers");
    bool msa = !_stricmp(pid.c_str(), MSA_PROVIDER_ID) && (auth.empty() || !_stricmp(auth.c_str(), "consumers"));
    trace("WebAuthenticationCoreManager.FindAccountProviderAsync(\"%s\", \"%s\", user %s) -> %s", pid.c_str(), auth.c_str(),
          user ? "given" : "none", msa ? "Microsoft account" : "none");
    IUnknown *p = msa ? static_cast<IWebAccountProviderAbi *>(new WebAccountProvider(auth.empty() ? "consumers" : auth, user)) : nullptr;
    return completed_iface(IID_AsyncOp_WebAccountProvider, "FindAccountProviderAsync", p, out);
}

static WebAccountProvider *our_provider(IInspectable *provider)
{
    IWebAccountProviderAbi *p = nullptr;
    if (!provider || FAILED(provider->QueryInterface(IID_IWebAccountProvider, (void **)&p))) return nullptr;
    HSTRING h = nullptr;
    std::string id;
    if (SUCCEEDED(p->get_Id(&h))) { id = from_hstr(h); WindowsDeleteString(h); }
    p->Release();
    if (_stricmp(id.c_str(), MSA_PROVIDER_ID)) return nullptr;
    /* a provider object of the app's making (WebAccountProvider's own constructor) is answered as ours */
    return new WebAccountProvider("consumers", nullptr);
}

struct IFindAllAccountsResultAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Accounts(IVectorViewAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ProviderError(IWebProviderErrorAbi **) = 0;
};
struct FindAllAccountsResult : IFindAllAccountsResultAbi
{
    LONG ref = 1;
    VectorView *accounts;
    FindAllAccountsResult()
    {
        accounts = new VectorView(IID_IVectorView_WebAccount, IID_IIterable_WebAccount, IID_IIterator_WebAccount,
                                  L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Credentials.WebAccount>");
    }
    ~FindAllAccountsResult() { accounts->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IFindAllAccountsResult)) *out = static_cast<IFindAllAccountsResultAbi *>(this);
        else { trace_refused("FindAllAccountsResult", iid); *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return to_hstr(L"Windows.Security.Authentication.Web.Core.FindAllAccountsResult", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Accounts(IVectorViewAbi **v) override { if (!v) return E_POINTER; accounts->AddRef(); *v = accounts; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(INT32 *v) override { if (!v) return E_POINTER; *v = 0; /* Success */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ProviderError(IWebProviderErrorAbi **v) override { if (!v) return E_POINTER; *v = nullptr; return S_OK; }
};

struct IWACMStaticsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetTokenSilentlyAsync(IInspectable *, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTokenSilentlyWithAccountAsync(IInspectable *, IInspectable *, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestTokenAsync(IInspectable *, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestTokenWithAccountAsync(IInspectable *, IInspectable *, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAccountAsync(IInspectable *, HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAccountProviderAsync(HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAccountProviderWithAuthorityAsync(HSTRING, HSTRING, IAsyncOpAbi **) = 0;
};
struct IWACMStatics2Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE FindAccountProviderWithAuthorityForUserAsync(HSTRING, HSTRING, IInspectable *, IAsyncOpAbi **) = 0;
};
struct IWACMStatics4Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE FindAllAccountsAsync(IInspectable *, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAllAccountsWithClientIdAsync(IInspectable *, HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindSystemAccountProviderAsync(HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindSystemAccountProviderWithAuthorityAsync(HSTRING, HSTRING, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindSystemAccountProviderWithAuthorityForUserAsync(HSTRING, HSTRING, IInspectable *, IAsyncOpAbi **) = 0;
};

struct WebAuthManagerFactory : IWACMStaticsAbi, IWACMStatics2Abi, IWACMStatics4Abi, IActivationFactoryAbi
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IActivationFactory)) *out = static_cast<IActivationFactoryAbi *>(this);
        else if (IsEqualGUID(iid, IID_IWACMStatics)) *out = static_cast<IWACMStaticsAbi *>(this);
        else if (IsEqualGUID(iid, IID_IWACMStatics2)) *out = static_cast<IWACMStatics2Abi *>(this);
        else if (IsEqualGUID(iid, IID_IWACMStatics4)) *out = static_cast<IWACMStatics4Abi *>(this);
        else { trace_refused("WebAuthenticationCoreManager", iid); *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override { if (out) *out = nullptr; return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE GetTokenSilentlyAsync(IInspectable *r, IAsyncOpAbi **o) override { return get_web_token(r, nullptr, false, o); }
    HRESULT STDMETHODCALLTYPE GetTokenSilentlyWithAccountAsync(IInspectable *r, IInspectable *a, IAsyncOpAbi **o) override { return get_web_token(r, a, false, o); }
    HRESULT STDMETHODCALLTYPE RequestTokenAsync(IInspectable *r, IAsyncOpAbi **o) override { return get_web_token(r, nullptr, true, o); }
    HRESULT STDMETHODCALLTYPE RequestTokenWithAccountAsync(IInspectable *r, IInspectable *a, IAsyncOpAbi **o) override { return get_web_token(r, a, true, o); }
    HRESULT STDMETHODCALLTYPE FindAccountAsync(IInspectable *provider, HSTRING id, IAsyncOpAbi **out) override
    {
        std::string want = from_hstr(id), have = stored_msa_account();
        WebAccountProvider *p = our_provider(provider);
        WebAccount *wa = nullptr;
        std::string cid;
        EnterCriticalSection(&account.cs);
        cid = account.msa_user_id;
        LeaveCriticalSection(&account.cs);
        if (p && !have.empty() && (!_stricmp(want.c_str(), have.c_str()) || !_stricmp(want.c_str(), cid.c_str())))
            wa = make_web_account(p);
        trace("WebAuthenticationCoreManager.FindAccountAsync(%s) -> %s", want.empty() ? "\"\"" : "an id",
              wa ? "the Microsoft account" : have.empty() ? "none (not signed in)" : p ? "none (another account)" : "none (provider)");
        if (p) p->Release();
        return completed_iface(IID_AsyncOp_WebAccount, "FindAccountAsync", wa ? static_cast<IWebAccountAbi *>(wa) : nullptr, out);
    }
    HRESULT STDMETHODCALLTYPE FindAccountProviderAsync(HSTRING id, IAsyncOpAbi **out) override { return find_provider(id, nullptr, nullptr, out); }
    HRESULT STDMETHODCALLTYPE FindAccountProviderWithAuthorityAsync(HSTRING id, HSTRING a, IAsyncOpAbi **out) override { return find_provider(id, a, nullptr, out); }
    HRESULT STDMETHODCALLTYPE FindAccountProviderWithAuthorityForUserAsync(HSTRING id, HSTRING a, IInspectable *u, IAsyncOpAbi **out) override
    { return find_provider(id, a, u, out); }
    HRESULT STDMETHODCALLTYPE FindAllAccountsAsync(IInspectable *provider, IAsyncOpAbi **out) override
    {
        FindAllAccountsResult *r = new FindAllAccountsResult();
        WebAccountProvider *p = our_provider(provider);
        WebAccount *wa = p ? make_web_account(p) : nullptr;
        if (wa) r->accounts->items.push_back(static_cast<IWebAccountAbi *>(wa));
        if (p) p->Release();
        trace("WebAuthenticationCoreManager.FindAllAccountsAsync -> %zu", r->accounts->items.size());
        return completed_iface(IID_AsyncOp_FindAllAccountsResult, "FindAllAccountsAsync", static_cast<IFindAllAccountsResultAbi *>(r), out);
    }
    HRESULT STDMETHODCALLTYPE FindAllAccountsWithClientIdAsync(IInspectable *provider, HSTRING, IAsyncOpAbi **out) override
    { return FindAllAccountsAsync(provider, out); }
    HRESULT STDMETHODCALLTYPE FindSystemAccountProviderAsync(HSTRING id, IAsyncOpAbi **out) override { return find_provider(id, nullptr, nullptr, out); }
    HRESULT STDMETHODCALLTYPE FindSystemAccountProviderWithAuthorityAsync(HSTRING id, HSTRING a, IAsyncOpAbi **out) override { return find_provider(id, a, nullptr, out); }
    HRESULT STDMETHODCALLTYPE FindSystemAccountProviderWithAuthorityForUserAsync(HSTRING id, HSTRING a, IInspectable *u, IAsyncOpAbi **out) override
    { return find_provider(id, a, u, out); }
};
static WebAuthManagerFactory web_auth_manager_factory;

/* ------------------------------------------------------------------------------------------- */
/* Windows.ApplicationModel.Store.Preview.StoreConfiguration: which WebAccount is the Store's     */
/*                                                                                              */
/* Windows' own (Windows.ApplicationModel.Store.dll) asks an out-of-process StoreConfigurationServer, */
/* which is not registered here (REGDB_E_CLASSNOTREG at IStoreConfigurationStatics4). The Xbox app   */
/* reads the Store's web account id with GetStoreWebAccountId[ForUser], finds it with                */
/* WebAuthenticationCoreManager.FindAccountAsync and compares it with the Xbox user's WebAccountId     */
/* ("Account mismatch"), and sets it with SetStoreWebAccountId[ForUser] ("trySetAccount"). The only    */
/* account here is the Microsoft account of the Xbox sign-in, so that is the Store's account unless    */
/* the app sets another id, which is kept in xboxauth.bin ("store_account_id").                       */
/*                                                                                              */
/* Slots (Windows.ApplicationModel.winmd): Statics {728f7fc0} 6 SetSystemConfiguration                */
/*   7 SetMobileOperatorConfiguration 8 SetStoreWebAccountId 9 IsStoreWebAccountId                    */
/*   10 get_HardwareManufacturerInfo 11 FilterUnsupportedSystemFeaturesAsync; Statics2 {657c4595}      */
/*   6/7 PurchasePromptingPolicy; Statics3 {6d45f57c} 6 HasStoreWebAccount 7 ..ForUser 8 GetStoreLogDataAsync */
/*   9 SetStoreWebAccountIdForUser 10 IsStoreWebAccountIdForUser 11/12 ..PurchasePromptingPolicyForUser; */
/*   Statics4 {20ff56d2} 6 GetStoreWebAccountId 7 ..ForUser 8-11 enterprise ids 12/13 ShouldRestrictToEnterpriseStoreOnly */

static const GUID IID_IStoreConfigurationStatics = {0x728f7fc0,0x8628,0x42ec,{0x84,0xa2,0x07,0x78,0x0e,0xb4,0x4d,0x8b}};
static const GUID IID_IStoreConfigurationStatics2 = {0x657c4595,0xc8b7,0x4fe9,{0x9f,0x4c,0x4d,0x71,0x02,0x7d,0x34,0x7e}};
static const GUID IID_IStoreConfigurationStatics3 = {0x6d45f57c,0xf144,0x4cb5,{0x9d,0x3f,0x4e,0xb0,0x5e,0x30,0xb6,0xd3}};
static const GUID IID_IStoreConfigurationStatics4 = {0x20ff56d2,0x4ee3,0x4cf0,{0x9b,0x12,0x55,0x2c,0x03,0x31,0x0f,0x75}};

static std::string store_web_account_id(void)
{
    std::string id = stored_msa_account();
    if (id.empty()) return id;
    store_lock();
    auto kv = store_read();
    store_unlock();
    return kv["store_account_id"].empty() ? id : kv["store_account_id"];
}
static HRESULT set_store_web_account_id(HSTRING h)
{
    std::string id = from_hstr(h), msa = stored_msa_account();
    trace("StoreConfiguration.SetStoreWebAccountId(%s)", id.empty() ? "\"\"" : !_stricmp(id.c_str(), msa.c_str()) ?
          "the Microsoft account" : "another id");
    store_update({ { "store_account_id", _stricmp(id.c_str(), msa.c_str()) ? id : std::string() } });
    return S_OK;
}
static HRESULT is_store_web_account_id(HSTRING h, boolean *v)
{
    if (!v) return E_POINTER;
    std::string id = from_hstr(h), have = store_web_account_id();
    *v = !have.empty() && !_stricmp(id.c_str(), have.c_str());
    trace("StoreConfiguration.IsStoreWebAccountId -> %d", *v);
    return S_OK;
}

struct IStoreConfigurationStaticsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE SetSystemConfiguration(HSTRING, HSTRING, INT64, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMobileOperatorConfiguration(HSTRING, UINT32, UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStoreWebAccountId(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsStoreWebAccountId(HSTRING, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HardwareManufacturerInfo(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE FilterUnsupportedSystemFeaturesAsync(IInspectable *, IAsyncOpAbi **) = 0;
};
struct IStoreConfigurationStatics2Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_PurchasePromptingPolicy(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PurchasePromptingPolicy(IInspectable *) = 0;
};
struct IStoreConfigurationStatics3Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE HasStoreWebAccount(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE HasStoreWebAccountForUser(IInspectable *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStoreLogDataAsync(UINT32, IAsyncOpAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStoreWebAccountIdForUser(IInspectable *, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsStoreWebAccountIdForUser(IInspectable *, HSTRING, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPurchasePromptingPolicyForUser(IInspectable *, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPurchasePromptingPolicyForUser(IInspectable *, IInspectable *) = 0;
};
struct IStoreConfigurationStatics4Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetStoreWebAccountId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStoreWebAccountIdForUser(IInspectable *, HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEnterpriseStoreWebAccountId(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEnterpriseStoreWebAccountIdForUser(IInspectable *, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEnterpriseStoreWebAccountId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEnterpriseStoreWebAccountIdForUser(IInspectable *, HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE ShouldRestrictToEnterpriseStoreOnly(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE ShouldRestrictToEnterpriseStoreOnlyForUser(IInspectable *, boolean *) = 0;
};

struct StoreConfigurationFactory : IStoreConfigurationStaticsAbi, IStoreConfigurationStatics2Abi, IStoreConfigurationStatics3Abi,
                                   IStoreConfigurationStatics4Abi, IActivationFactoryAbi
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (is_base_iid(iid) || IsEqualGUID(iid, IID_IActivationFactory)) *out = static_cast<IActivationFactoryAbi *>(this);
        else if (IsEqualGUID(iid, IID_IStoreConfigurationStatics)) *out = static_cast<IStoreConfigurationStaticsAbi *>(this);
        else if (IsEqualGUID(iid, IID_IStoreConfigurationStatics2)) *out = static_cast<IStoreConfigurationStatics2Abi *>(this);
        else if (IsEqualGUID(iid, IID_IStoreConfigurationStatics3)) *out = static_cast<IStoreConfigurationStatics3Abi *>(this);
        else if (IsEqualGUID(iid, IID_IStoreConfigurationStatics4)) *out = static_cast<IStoreConfigurationStatics4Abi *>(this);
        else { trace_refused("StoreConfiguration", iid); *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override
    { return to_hstr(L"Windows.ApplicationModel.Store.Preview.StoreConfiguration", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override { if (out) *out = nullptr; return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE SetSystemConfiguration(HSTRING, HSTRING, INT64, HSTRING) override
    { trace("StoreConfiguration.SetSystemConfiguration: ignored"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetMobileOperatorConfiguration(HSTRING, UINT32, UINT32) override
    { trace("StoreConfiguration.SetMobileOperatorConfiguration: ignored"); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetStoreWebAccountId(HSTRING id) override { return set_store_web_account_id(id); }
    HRESULT STDMETHODCALLTYPE IsStoreWebAccountId(HSTRING id, boolean *v) override { return is_store_web_account_id(id, v); }
    HRESULT STDMETHODCALLTYPE get_HardwareManufacturerInfo(IInspectable **v) override
    { trace("StoreConfiguration.HardwareManufacturerInfo -> E_NOTIMPL"); if (v) *v = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FilterUnsupportedSystemFeaturesAsync(IInspectable *, IAsyncOpAbi **v) override
    { trace("StoreConfiguration.FilterUnsupportedSystemFeaturesAsync -> E_NOTIMPL"); if (v) *v = nullptr; return E_NOTIMPL; }

    /* null is the documented "no policy set" */
    HRESULT STDMETHODCALLTYPE get_PurchasePromptingPolicy(IInspectable **v) override { if (!v) return E_POINTER; *v = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_PurchasePromptingPolicy(IInspectable *) override { trace("StoreConfiguration.PurchasePromptingPolicy: ignored"); return S_OK; }

    HRESULT STDMETHODCALLTYPE HasStoreWebAccount(boolean *v) override
    {
        if (!v) return E_POINTER;
        *v = !store_web_account_id().empty();
        trace("StoreConfiguration.HasStoreWebAccount -> %d", *v);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HasStoreWebAccountForUser(IInspectable *, boolean *v) override { return HasStoreWebAccount(v); }
    HRESULT STDMETHODCALLTYPE GetStoreLogDataAsync(UINT32, IAsyncOpAbi **v) override
    { trace("StoreConfiguration.GetStoreLogDataAsync -> E_NOTIMPL"); if (v) *v = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStoreWebAccountIdForUser(IInspectable *, HSTRING id) override { return set_store_web_account_id(id); }
    HRESULT STDMETHODCALLTYPE IsStoreWebAccountIdForUser(IInspectable *, HSTRING id, boolean *v) override { return is_store_web_account_id(id, v); }
    HRESULT STDMETHODCALLTYPE GetPurchasePromptingPolicyForUser(IInspectable *, IInspectable **v) override { return get_PurchasePromptingPolicy(v); }
    HRESULT STDMETHODCALLTYPE SetPurchasePromptingPolicyForUser(IInspectable *, IInspectable *p) override { return put_PurchasePromptingPolicy(p); }

    HRESULT STDMETHODCALLTYPE GetStoreWebAccountId(HSTRING *v) override
    {
        std::string id = store_web_account_id();
        trace("StoreConfiguration.GetStoreWebAccountId -> %s", id.empty() ? "none" : "the Microsoft account");
        return to_hstr(id, v);
    }
    HRESULT STDMETHODCALLTYPE GetStoreWebAccountIdForUser(IInspectable *, HSTRING *v) override { return GetStoreWebAccountId(v); }
    HRESULT STDMETHODCALLTYPE SetEnterpriseStoreWebAccountId(HSTRING) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEnterpriseStoreWebAccountIdForUser(IInspectable *, HSTRING) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEnterpriseStoreWebAccountId(HSTRING *v) override { if (!v) return E_POINTER; *v = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEnterpriseStoreWebAccountIdForUser(IInspectable *, HSTRING *v) override { return GetEnterpriseStoreWebAccountId(v); }
    HRESULT STDMETHODCALLTYPE ShouldRestrictToEnterpriseStoreOnly(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE ShouldRestrictToEnterpriseStoreOnlyForUser(IInspectable *, boolean *v) override { return ShouldRestrictToEnterpriseStoreOnly(v); }
};
static StoreConfigurationFactory store_configuration_factory;

/* ------------------------------------------------------------------------------------------- */

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const wchar_t *name = WindowsGetStringRawBuffer(classid, nullptr);
    account_init();
    if (!wcscmp(name, L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager"))
        *factory = (IActivationFactory *)static_cast<IActivationFactoryAbi *>(&manager_factory);
    else if (!wcscmp(name, L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs"))
        *factory = (IActivationFactory *)static_cast<IActivationFactoryAbi *>(&init_args_factory);
    else if (!wcscmp(name, L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager"))
        *factory = (IActivationFactory *)static_cast<IActivationFactoryAbi *>(&web_auth_manager_factory);
    else if (!wcscmp(name, L"Windows.ApplicationModel.Store.Preview.StoreConfiguration"))
        *factory = (IActivationFactory *)static_cast<IActivationFactoryAbi *>(&store_configuration_factory);
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

/* An authenticated GET for the other stand-ins (shims/gameplatform answers Gaming Services' content access with it):
 * the Xbox Live token for the URL's relying party, its Signature when the endpoint has a policy, and extra headers
 * ('|'-separated). *body is CoTaskMemAlloc'ed. Returns S_OK when a response arrived, whatever its status. */
extern "C" __declspec(dllexport) HRESULT WINAPI XboxAuthHttpGet(const char *url, const char *extra, const char *contract,
                                                                unsigned char **body, UINT32 *len, DWORD *status)
{
    std::string header, resp;
    HRESULT hr;
    account_init();
    *body = nullptr; *len = 0; *status = 0;
    if (FAILED(hr = sign_in_silently())) return hr;
    if (FAILED(hr = get_token_for_url(url, false, header))) return hr;
    std::vector<std::string> h = { "Authorization: " + header, std::string("x-xbl-contract-version: ") + (contract ? contract : "1"),
                                   "Accept-Language: en-US" };
    std::string e = extra ? extra : "";
    for (size_t a = 0, b; a < e.size(); a = b + 1)
    {
        b = e.find('|', a);
        if (b == std::string::npos) b = e.size();
        if (b > a) h.push_back(e.substr(a, b - a));
    }
    SigningPolicy policy;
    if (find_policy(url, policy))
    {
        std::string sig = sign_request(policy.version, policy.max_body_bytes, "GET", split_url(url).path_and_query, header, "");
        if (!sig.empty()) h.push_back("Signature: " + sig);
    }
    *status = http_request(L"GET", url, h, "", resp);
    if (!*status) return XA_E_NETWORK;
    if (!(*body = (unsigned char *)CoTaskMemAlloc(resp.size() ? resp.size() : 1))) return E_OUTOFMEMORY;
    memcpy(*body, resp.data(), resp.size());
    *len = (UINT32)resp.size();
    trace("GET %s: HTTP %lu, %zu bytes", url, *status, resp.size());
    return S_OK;
}

/* For probes/probe-xboxauth.cpp: exercises the HTTP paths without an app.
 *   "endpoints <url>"   relying party lookup for a URL
 *   "xbl <rps>"         user/authenticate with a deliberately invalid ticket (request shape, error path)
 *   "devicecode"        obtains a device code and stops (no browser, no polling)
 *   "silent"            silent sign-in from the stored refresh token, then the profile claims
 *   "msascope <scope>"  a Microsoft account token for a scope (WebAuthenticationCoreManager's path) */
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
        if (const char *extra = getenv("XBL_EXTRA")) /* more headers, '|'-separated */
        {
            std::string e = extra;
            for (size_t a = 0, b; a < e.size(); a = b + 1)
            {
                b = e.find('|', a);
                if (b == std::string::npos) b = e.size();
                h.push_back(e.substr(a, b - a));
            }
        }
        SigningPolicy policy;
        if (find_policy(url, policy))
        {
            std::string sig = sign_request(policy.version, policy.max_body_bytes, "GET", split_url(url).path_and_query, header, "");
            if (!sig.empty()) h.push_back("Signature: " + sig);
        }
        DWORD status = http_request(L"GET", url, h, "", resp);
        printf("HTTP %lu, %zu bytes\n%.600s\n", (unsigned long)status, resp.size(), resp.c_str());
        if (const char *out = getenv("XBL_OUT"))  /* the whole body, for binary answers */
            if (FILE *f = fopen(out, "wb")) { fwrite(resp.data(), 1, resp.size(), f); fclose(f); }
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
    if (!strcmp(what, "msascope") && arg)
    {
        /* a refresh-token grant for a scope, as WebAuthenticationCoreManager.GetTokenSilentlyAsync makes it */
        std::string token, error, scope = arg, extra;
        if (scope.find('|') != std::string::npos) { extra = "&" + scope.substr(scope.find('|') + 1); scope.resize(scope.find('|')); }
        if (stored_msa_account().empty()) { trace("msascope: no stored sign-in"); return 1; }
        HRESULT hr = msa_token_for_scope(scope, true, token, error, nullptr, extra);
        trace("msascope \"%s\" -> %#lx%s%s, token %zu chars", arg, (unsigned long)hr, error.empty() ? "" : ", error ",
              error.c_str(), token.size());
        EnterCriticalSection(&account.signin_cs);
        std::string form = scoped_tokens[scope + extra].form;
        LeaveCriticalSection(&account.signin_cs);
        size_t at = form.find("id_token=");
        if (at != std::string::npos)
        {
            /* the claim names of the id_token (never the values) */
            std::string jwt = form.substr(at + 9);
            jwt = jwt.substr(0, jwt.find('&'));
            size_t d1 = jwt.find('.'), d2 = jwt.find('.', d1 + 1);
            Json claims = parse_json(unb64(jwt.substr(d1 + 1, d2 - d1 - 1)));
            std::string names;
            for (auto &kv : claims.o) names += " " + kv.first;
            trace("  id_token claims:%s", names.c_str());
            if (const char *want = getenv("XBA_COMPARE_OID_TAIL"))
            {
                std::string oid = claims["oid"].str(), tail;
                for (char ch : oid) if (ch != '-') tail += (char)toupper((unsigned char)ch);
                if (tail.size() > 16) tail = tail.substr(tail.size() - 16);
                trace("  oid tail %s the given value (oid starts %.19s); it %s the user_id", _stricmp(tail.c_str(), want) ? "differs from" : "equals",
                      oid.c_str(), _stricmp(tail.c_str(), account.msa_user_id.c_str()) ? "differs from" : "equals");
            }
        }
        return SUCCEEDED(hr) ? 0 : 1;
    }
    return 2;
}

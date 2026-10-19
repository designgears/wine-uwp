#define _CRT_SECURE_NO_WARNINGS
/* GDK XUser + XStore + minimal XAsync/XThreading, backed by wineuwp xboxauth.bin. */
#define NOMINMAX
#include "xuser_xstore.h"
#include "xuser_xstore_query.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <wincrypt.h>

#pragma comment(lib, "crypt32")

static void gdk_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    fputs(buf, stderr);
    OutputDebugStringA(buf);
    HANDLE file = CreateFileW(L"C:\\users\\bobby\\AppData\\Local\\Temp\\gdk-content.log",
                              FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD got = 0;
        WriteFile(file, buf, (DWORD)strlen(buf), &got, nullptr);
        CloseHandle(file);
    }
}

/* -------------------- xboxauth.bin identity -------------------- */

static std::wstring store_path()
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"WINE_UWP_XBOXAUTH_STORE", base, MAX_PATH);
    if (n && n < MAX_PATH) return base;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) wcscpy(base, L"C:\\users\\Public");
    return std::wstring(base) + L"\\wineuwp\\xboxauth.bin";
}

static std::map<std::string, std::string> store_read()
{
    std::map<std::string, std::string> kv;
    HANDLE f = CreateFileW(store_path().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return kv;
    DWORD size = GetFileSize(f, nullptr), read = 0;
    std::string blob;
    if (size != INVALID_FILE_SIZE && size < (1u << 20))
    {
        blob.resize(size);
        if (!ReadFile(f, &blob[0], size, &read, nullptr)) read = 0;
        blob.resize(read);
    }
    CloseHandle(f);
    DATA_BLOB in = { (DWORD)blob.size(), (BYTE *)blob.data() }, out = {};
    if (blob.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return kv;
    std::string text((char *)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    for (size_t pos = 0; pos < text.size(); )
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

struct Identity
{
    UINT64 xuid = 0;
    std::string gamertag;
    std::string modern;
    std::string modern_suffix;
    std::string unique_modern;
    std::string privileges; /* space-separated numbers from XSTS prv */
    XUserAgeGroup age = XUserAgeGroup::Adult;
    bool ok = false;
};

static Identity load_identity()
{
    Identity id;
    auto kv = store_read();
    id.gamertag = kv["gamertag"];
    if (id.gamertag.empty()) id.gamertag = kv.count("gtg") ? kv["gtg"] : "";
    id.modern = kv["mgt"];
    if (id.modern.empty()) id.modern = id.gamertag;
    id.modern_suffix = kv["mgs"];
    id.unique_modern = kv["umg"];
    if (id.unique_modern.empty()) id.unique_modern = id.gamertag;
    id.privileges = kv["prv"];
    std::string xid = kv["xid"];
    if (!xid.empty()) id.xuid = _strtoui64(xid.c_str(), nullptr, 10);
    std::string agg = kv["agg"];
    if (agg == "Adult" || agg == "3") id.age = XUserAgeGroup::Adult;
    else if (agg == "Teen" || agg == "2") id.age = XUserAgeGroup::Teen;
    else if (agg == "Child" || agg == "1") id.age = XUserAgeGroup::Child;
    id.ok = id.xuid != 0 && !id.gamertag.empty();
    if (!id.ok && !id.gamertag.empty())
    {
        /* Older stores only kept gamertag; still allow Add with a stable local xuid. */
        id.xuid = 0x1000000000000001ull;
        id.ok = true;
        gdk_log("GDK user: xboxauth has gamertag \"%s\" but no xid; using local placeholder xuid\n",
                id.gamertag.c_str());
    }
    return id;
}

/* -------------------- minimal XAsync (immediate) -------------------- */

static constexpr uint32_t ASYNC_SIG = 0x41535942; /* ASYB */

struct AsyncState
{
    uint32_t signature = ASYNC_SIG;
    std::atomic<LONG> refs{1};
    XAsyncProvider *provider = nullptr;
    void *context = nullptr;
    const void *identity = nullptr;
    const char *identity_name = nullptr;
    XAsyncBlock *user = nullptr;
    HRESULT status = E_PENDING;
    SIZE_T result_size = 0;
    bool completed = false;
    HANDLE done_event = nullptr;
    std::mutex mu;

    void addref() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release()
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (provider)
            {
                XAsyncProviderData data{ user, 0, nullptr, context };
                provider(XAsyncOp::Cleanup, &data);
            }
            if (done_event) CloseHandle(done_event);
            delete this;
        }
    }
};

static AsyncState *async_state(XAsyncBlock *block)
{
    if (!block) return nullptr;
    AsyncState *s;
    memcpy(&s, block->internal, sizeof(s));
    if (!s || s->signature != ASYNC_SIG) return nullptr;
    return s;
}

static void async_set_state(XAsyncBlock *block, AsyncState *s)
{
    memcpy(block->internal, &s, sizeof(s));
}

static HRESULT async_begin(XAsyncBlock *asyncBlock, void *context, const void *identity,
                           const char *identityName, XAsyncProvider *provider)
{
    if (!asyncBlock || !provider) return E_INVALIDARG;
    auto *s = new (std::nothrow) AsyncState();
    if (!s) return E_OUTOFMEMORY;
    s->provider = provider;
    s->context = context;
    s->identity = identity;
    s->identity_name = identityName;
    s->user = asyncBlock;
    s->done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    async_set_state(asyncBlock, s);

    XAsyncProviderData data{ asyncBlock, 0, nullptr, context };
    HRESULT hr = provider(XAsyncOp::Begin, &data);
    if (FAILED(hr))
    {
        async_set_state(asyncBlock, nullptr);
        s->release();
        return hr;
    }
    /* Immediate DoWork on the calling thread (no task queue required). */
    hr = provider(XAsyncOp::DoWork, &data);
    if (hr == E_PENDING) return S_OK; /* provider will XAsyncComplete later */
    if (FAILED(hr))
    {
        /* Provider failed before Complete; synthesize failure. */
        s->mu.lock();
        if (!s->completed)
        {
            s->status = hr;
            s->completed = true;
            if (s->done_event) SetEvent(s->done_event);
        }
        s->mu.unlock();
        if (asyncBlock->callback) asyncBlock->callback(asyncBlock);
    }
    return S_OK;
}

struct AsyncCallback
{
    XAsyncBlock *block;
    AsyncState *state;
};

/* The title may free or reuse its XAsyncBlock inside the completion callback (that is
 * the normal pattern), so nothing here may look at the block after the call. Reading
 * block->internal afterwards released whichever async had reused the block. */
static DWORD WINAPI async_callback_thunk(void *param)
{
    auto *cb = (AsyncCallback *)param;
    XAsyncCompletionRoutine *routine = cb->block->callback;
    if (routine) routine(cb->block);
    cb->state->release();
    delete cb;
    return 0;
}

static void async_complete(XAsyncBlock *asyncBlock, HRESULT result, SIZE_T requiredBufferSize)
{
    AsyncState *s = async_state(asyncBlock);
    if (!s) return;
    bool fire = false;
    s->mu.lock();
    if (!s->completed)
    {
        s->status = result;
        s->result_size = requiredBufferSize;
        s->completed = true;
        fire = true;
        if (s->done_event) SetEvent(s->done_event);
    }
    s->mu.unlock();
    /* Defer the completion callback so XUserAddAsync can return before the
     * title's handler runs (avoids PPL "unobserved exception" re-entrancy). */
    if (fire && asyncBlock->callback)
    {
        s->addref();
        auto *cb = new (std::nothrow) AsyncCallback{ asyncBlock, s };
        if (!cb || !QueueUserWorkItem(async_callback_thunk, cb, WT_EXECUTEDEFAULT))
        {
            delete cb;
            asyncBlock->callback(asyncBlock);
            s->release();
        }
    }
}

static HRESULT async_get_status(XAsyncBlock *asyncBlock, BOOLEAN wait)
{
    AsyncState *s = async_state(asyncBlock);
    if (!s) return E_INVALIDARG;
    if (!s->completed && wait && s->done_event)
        WaitForSingleObject(s->done_event, INFINITE);
    return s->completed ? s->status : E_PENDING;
}

static HRESULT async_get_result_size(XAsyncBlock *asyncBlock, SIZE_T *bufferSize)
{
    if (!bufferSize) return E_POINTER;
    AsyncState *s = async_state(asyncBlock);
    if (!s) return E_INVALIDARG;
    HRESULT hr = async_get_status(asyncBlock, TRUE);
    if (FAILED(hr) && hr != E_PENDING) return hr;
    *bufferSize = s->result_size;
    return s->status;
}

static HRESULT async_get_result(XAsyncBlock *asyncBlock, const void *identity,
                                SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed)
{
    AsyncState *s = async_state(asyncBlock);
    if (!s) return E_INVALIDARG;
    if (identity && s->identity && identity != s->identity) return E_INVALIDARG;
    HRESULT hr = async_get_status(asyncBlock, TRUE);
    if (FAILED(hr)) return hr;
    XAsyncProviderData data{ asyncBlock, bufferSize, buffer, s->context };
    hr = s->provider(XAsyncOp::GetResult, &data);
    if (SUCCEEDED(hr) && bufferUsed) *bufferUsed = s->result_size;
    return hr;
}

static void async_cancel(XAsyncBlock *asyncBlock)
{
    AsyncState *s = async_state(asyncBlock);
    if (!s) return;
    XAsyncProviderData data{ asyncBlock, 0, nullptr, s->context };
    s->provider(XAsyncOp::Cancel, &data);
    async_complete(asyncBlock, E_ABORT, 0);
}

/* -------------------- XUser object -------------------- */

struct XUserObject
{
    LONG refs = 1;
    Identity id;
    XUserLocalId local{ 1 };
};

static XUserObject *g_default_user = nullptr;
static std::mutex g_user_mu;

static XUserObject *user_addref(XUserObject *u)
{
    if (u) InterlockedIncrement(&u->refs);
    return u;
}

static void user_release(XUserObject *u)
{
    if (u && InterlockedDecrement(&u->refs) == 0) delete u;
}

static XUserObject *ensure_default_user()
{
    std::lock_guard<std::mutex> lock(g_user_mu);
    if (g_default_user) return user_addref(g_default_user);
    Identity id = load_identity();
    if (!id.ok) return nullptr;
    g_default_user = new XUserObject();
    g_default_user->id = std::move(id);
    g_default_user->local.value = 1;
    gdk_log("GDK user: default \"%s\" xuid=%llu\n",
            g_default_user->id.gamertag.c_str(),
            (unsigned long long)g_default_user->id.xuid);
    return user_addref(g_default_user);
}

struct AddAsyncContext
{
    XUserAddOptions options;
    XUserObject *user = nullptr;
    HRESULT result = E_FAIL;
};

static HRESULT CALLBACK add_user_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *ctx = (AddAsyncContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
    {
        ctx->user = ensure_default_user();
        if (!ctx->user)
        {
            ctx->result = HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
            async_complete(data->async, ctx->result, 0);
            return ctx->result;
        }
        ctx->result = S_OK;
        async_complete(data->async, S_OK, sizeof(XUserHandle));
        return S_OK;
    }
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XUserHandle)) return E_INVALIDARG;
        *(XUserHandle *)data->buffer = (XUserHandle)user_addref(ctx->user);
        return S_OK;
    case XAsyncOp::Cancel:
        return S_OK;
    case XAsyncOp::Cleanup:
        user_release(ctx->user);
        delete ctx;
        return S_OK;
    }
    return S_OK;
}

struct LicenseAsyncContext
{
    XStoreGameLicense license{};
};

static HRESULT CALLBACK game_license_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *ctx = (LicenseAsyncContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
        memset(&ctx->license, 0, sizeof(ctx->license));
        strncpy(ctx->license.skuStoreId, "0000000000000000", sizeof(ctx->license.skuStoreId) - 1);
        ctx->license.isActive = TRUE;
        ctx->license.isTrial = FALSE;
        ctx->license.isDiscLicense = FALSE;
        ctx->license.isTrialOwnedByThisUser = FALSE;
        ctx->license.expirationDate = 0;
        async_complete(data->async, S_OK, sizeof(XStoreGameLicense));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XStoreGameLicense)) return E_INVALIDARG;
        memcpy(data->buffer, &ctx->license, sizeof(XStoreGameLicense));
        return S_OK;
    case XAsyncOp::Cancel:
        return S_OK;
    case XAsyncOp::Cleanup:
        delete ctx;
        return S_OK;
    }
    return S_OK;
}

struct AcquireLicenseContext
{
    XStoreLicenseHandle handle = (XStoreLicenseHandle)(uintptr_t)1;
};

static HRESULT CALLBACK acquire_license_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *ctx = (AcquireLicenseContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
        async_complete(data->async, S_OK, sizeof(XStoreLicenseHandle));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XStoreLicenseHandle)) return E_INVALIDARG;
        *(XStoreLicenseHandle *)data->buffer = ctx->handle;
        return S_OK;
    case XAsyncOp::Cancel:
        return S_OK;
    case XAsyncOp::Cleanup:
        delete ctx;
        return S_OK;
    }
    return S_OK;
}

struct CanAcquireContext
{
    XStoreCanAcquireLicenseResult result{};
};

static HRESULT CALLBACK can_acquire_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *ctx = (CanAcquireContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
        memset(&ctx->result, 0, sizeof(ctx->result));
        strncpy(ctx->result.licensableSku, "0000", sizeof(ctx->result.licensableSku) - 1);
        ctx->result.status = XStoreCanLicenseStatus::Licensable;
        async_complete(data->async, S_OK, sizeof(XStoreCanAcquireLicenseResult));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XStoreCanAcquireLicenseResult)) return E_INVALIDARG;
        memcpy(data->buffer, &ctx->result, sizeof(ctx->result));
        return S_OK;
    case XAsyncOp::Cancel:
        return S_OK;
    case XAsyncOp::Cleanup:
        delete ctx;
        return S_OK;
    }
    return S_OK;
}

struct EmptyAsyncContext {};

static HRESULT CALLBACK empty_ok_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *ctx = (EmptyAsyncContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
        async_complete(data->async, S_OK, 0);
        return S_OK;
    case XAsyncOp::GetResult:
        return S_OK;
    case XAsyncOp::Cancel:
        return S_OK;
    case XAsyncOp::Cleanup:
        delete ctx;
        return S_OK;
    }
    return S_OK;
}


/* -------------------- xboxauth token bridge -------------------- */

typedef HRESULT (WINAPI *XboxAuthGetTokenAndSignatureFn)(
    const char *method, const char *url, int force,
    const void *body, UINT32 body_size,
    char *token, UINT32 token_cap, UINT32 *token_len,
    char *signature, UINT32 sig_cap, UINT32 *sig_len);
typedef HRESULT (WINAPI *XboxAuthSetTitleContextFn)(const char *msa_app_id, UINT32 title_id);

/* Title identity from env and/or MicrosoftGame.Config beside the package (generic for any GDK title). */
struct TitleIdentity
{
    UINT32 title_id = 0;
    bool have_title = false;
    char msa_app_id[64] = {};
    bool have_msa = false;
};

static bool read_file_utf8(const std::wstring &path, std::string *out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    if (size == INVALID_FILE_SIZE || size > (4u << 20))
    {
        CloseHandle(f);
        return false;
    }
    out->assign(size, '\0');
    BOOL ok = ReadFile(f, out->data(), size, &got, nullptr);
    CloseHandle(f);
    if (!ok || got != size) return false;
    /* Strip UTF-8 BOM if present. */
    if (out->size() >= 3 && (unsigned char)(*out)[0] == 0xef &&
        (unsigned char)(*out)[1] == 0xbb && (unsigned char)(*out)[2] == 0xbf)
        out->erase(0, 3);
    return true;
}

static bool xml_tag_value(const std::string &xml, const char *tag, std::string *value)
{
    std::string open = std::string("<") + tag + ">";
    std::string close = std::string("</") + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos) return false;
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos || b <= a) return false;
    *value = xml.substr(a, b - a);
    while (!value->empty() && ((*value)[0] == ' ' || (*value)[0] == '\t' ||
                               (*value)[0] == '\r' || (*value)[0] == '\n'))
        value->erase(0, 1);
    while (!value->empty())
    {
        char c = value->back();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        value->pop_back();
    }
    return !value->empty();
}

static bool try_load_microsoft_game_config(const std::wstring &dir, TitleIdentity *id)
{
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"MicrosoftGame.Config";
    std::string xml;
    if (!read_file_utf8(path, &xml)) return false;

    std::string title_s, msa_s;
    if (xml_tag_value(xml, "TitleId", &title_s))
    {
        id->title_id = (UINT32)strtoul(title_s.c_str(), nullptr, 16);
        id->have_title = (id->title_id != 0);
    }
    if (xml_tag_value(xml, "MSAAppId", &msa_s) && msa_s.size() < sizeof(id->msa_app_id))
    {
        memcpy(id->msa_app_id, msa_s.c_str(), msa_s.size() + 1);
        id->have_msa = true;
    }
    if (id->have_title || id->have_msa)
    {
        gdk_log("GDK title: MicrosoftGame.Config from %ls title=%08X msa=%s\n",
                dir.c_str(), id->have_title ? id->title_id : 0,
                id->have_msa ? id->msa_app_id : "(none)");
        return true;
    }
    return false;
}

static void load_title_identity(TitleIdentity *id)
{
    *id = {};

    char env[64] = {};
    DWORD n = GetEnvironmentVariableA("WINE_UWP_XBOX_TITLE_ID", env, sizeof(env));
    if (n && n < sizeof(env))
    {
        id->title_id = (UINT32)strtoul(env, nullptr, 0);
        id->have_title = true;
    }
    n = GetEnvironmentVariableA("WINE_UWP_MSA_APP_ID", env, sizeof(env));
    if (n && n < sizeof(env))
    {
        memcpy(id->msa_app_id, env, n + 1);
        id->have_msa = true;
    }
    if (id->have_title && id->have_msa) return;

    TitleIdentity from_cfg = {};
    WCHAR path[32768];
    DWORD count = GetEnvironmentVariableW(L"WINE_UWP_PACKAGE_PATH", path, ARRAYSIZE(path));
    if (count && count < ARRAYSIZE(path))
        try_load_microsoft_game_config(std::wstring(path, count), &from_cfg);

    if (!from_cfg.have_title || !from_cfg.have_msa)
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&load_title_identity, &self) &&
            GetModuleFileNameW(self, path, ARRAYSIZE(path)))
        {
            std::wstring dll = path;
            size_t slash = dll.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                std::wstring dir = dll.substr(0, slash);
                if (!try_load_microsoft_game_config(dir, &from_cfg) &&
                    (slash = dir.find_last_of(L"\\/")) != std::wstring::npos)
                    try_load_microsoft_game_config(dir.substr(0, slash), &from_cfg);
            }
        }
    }

    if (!id->have_title && from_cfg.have_title)
    {
        id->title_id = from_cfg.title_id;
        id->have_title = true;
    }
    if (!id->have_msa && from_cfg.have_msa)
    {
        memcpy(id->msa_app_id, from_cfg.msa_app_id, sizeof(id->msa_app_id));
        id->have_msa = true;
    }
}

static XboxAuthGetTokenAndSignatureFn load_token_fn()
{
    static XboxAuthGetTokenAndSignatureFn fn;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        HMODULE mod = LoadLibraryW(L"wineuwp_xboxauth.dll");
        if (!mod)
        {
            gdk_log("GDK user: LoadLibrary wineuwp_xboxauth.dll failed %lu\n", GetLastError());
            return TRUE;
        }
        fn = (XboxAuthGetTokenAndSignatureFn)GetProcAddress(mod, "XboxAuthGetTokenAndSignature");
        if (!fn) gdk_log("GDK user: XboxAuthGetTokenAndSignature export missing\n");
        return TRUE;
    }, nullptr, nullptr);
    return fn;
}

/* First GetToken does silent MSA + title SISU/XSTS (many seconds). Run that off the
 * Live/UI thread so XUserGetTokenAndSignatureAsync returns immediately; also warm
 * title identity after SetTitleContext so Enter→presence is a cache hit. */
static DWORD WINAPI title_identity_warm_thunk(void *)
{
    auto fn = load_token_fn();
    if (!fn) return 0;
    char token_buf[8192];
    char sig_buf[4096];
    UINT32 token_len = 0, sig_len = 0;
    HRESULT hr = fn("GET", "https://xboxlive.com/", 0, nullptr, 0,
                    token_buf, sizeof(token_buf), &token_len,
                    sig_buf, sizeof(sig_buf), &sig_len);
    gdk_log("GDK user: title-identity warm GetToken -> %08lx token=%u\n", hr, token_len);
    return 0;
}

static void ensure_xboxauth_title_context();

void wineuwp_warm_xboxauth_title_context(void)
{
    ensure_xboxauth_title_context();
}

static void ensure_xboxauth_title_context()
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        TitleIdentity id = {};
        load_title_identity(&id);
        if (!id.have_title || !id.have_msa)
        {
            gdk_log("GDK user: no TitleId/MSAAppId (env or MicrosoftGame.Config); skip SetTitleContext\n");
            return TRUE;
        }
        HMODULE mod = LoadLibraryW(L"wineuwp_xboxauth.dll");
        if (!mod) return TRUE;
        auto set_ctx = (XboxAuthSetTitleContextFn)GetProcAddress(mod, "XboxAuthSetTitleContext");
        if (!set_ctx)
        {
            gdk_log("GDK user: XboxAuthSetTitleContext export missing\n");
            return TRUE;
        }
        HRESULT hr = set_ctx(id.msa_app_id, id.title_id);
        gdk_log("GDK user: SetTitleContext msa=%s title=%08X -> %08lx\n", id.msa_app_id, id.title_id, hr);
        if (SUCCEEDED(hr))
            QueueUserWorkItem(title_identity_warm_thunk, nullptr, WT_EXECUTELONGFUNCTION);
        return TRUE;
    }, nullptr, nullptr);
}

struct TokenAsyncContext
{
    std::string method;
    std::string url;
    bool force = false;
    std::string body;
    std::string token;
    std::string signature;
    HRESULT result = E_FAIL;
};

static SIZE_T token_result_bytes(const TokenAsyncContext *ctx)
{
    SIZE_T n = sizeof(XUserGetTokenAndSignatureData);
    n += ctx->token.size() + 1;
    if (!ctx->signature.empty()) n += ctx->signature.size() + 1;
    return n;
}

struct TokenWorkItem
{
    XAsyncBlock *async = nullptr;
    TokenAsyncContext *ctx = nullptr;
};

static void token_do_work(XAsyncBlock *async, TokenAsyncContext *ctx)
{
    ensure_xboxauth_title_context();
    auto fn = load_token_fn();
    if (!fn)
    {
        ctx->result = E_NOTIMPL;
        async_complete(async, ctx->result, 0);
        return;
    }
    char token_buf[8192];
    char sig_buf[4096];
    UINT32 token_len = 0, sig_len = 0;
    HRESULT hr = fn(ctx->method.c_str(), ctx->url.c_str(), ctx->force ? 1 : 0,
                    ctx->body.empty() ? nullptr : ctx->body.data(),
                    (UINT32)ctx->body.size(),
                    token_buf, sizeof(token_buf), &token_len,
                    sig_buf, sizeof(sig_buf), &sig_len);
    ctx->result = hr;
    if (SUCCEEDED(hr))
    {
        if (token_len) ctx->token.assign(token_buf, token_len ? token_len - 1 : 0);
        if (sig_len) ctx->signature.assign(sig_buf, sig_len - 1);
        gdk_log("GDK user: GetTokenAndSignature ok token=%zu sig=%zu for %s\n",
                ctx->token.size(), ctx->signature.size(), ctx->url.c_str());
        async_complete(async, S_OK, token_result_bytes(ctx));
    }
    else
    {
        gdk_log("GDK user: GetTokenAndSignature failed hr=%08lx for %s\n", hr, ctx->url.c_str());
        async_complete(async, hr, 0);
    }
}

static DWORD WINAPI token_work_thunk(void *param)
{
    auto *item = (TokenWorkItem *)param;
    token_do_work(item->async, item->ctx);
    delete item;
    return 0;
}

static HRESULT CALLBACK token_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *ctx = (TokenAsyncContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
    {
        /* Must not block the Live/UI thread: silent MSA + title SISU/XSTS can take
         * tens of seconds and races the connection timeout → offline dialog. */
        auto *item = new (std::nothrow) TokenWorkItem{ data->async, ctx };
        if (!item)
        {
            async_complete(data->async, E_OUTOFMEMORY, 0);
            return E_OUTOFMEMORY;
        }
        if (!QueueUserWorkItem(token_work_thunk, item, WT_EXECUTELONGFUNCTION))
        {
            /* Fall back to sync only if the pool rejects the work item. */
            token_do_work(item->async, item->ctx);
            delete item;
            return S_OK;
        }
        return E_PENDING;
    }
    case XAsyncOp::GetResult:
    {
        if (!data->buffer || data->bufferSize < token_result_bytes(ctx)) return E_INVALIDARG;
        auto *out = (XUserGetTokenAndSignatureData *)data->buffer;
        char *cursor = (char *)(out + 1);
        out->tokenSize = ctx->token.size() + 1;
        memcpy(cursor, ctx->token.c_str(), out->tokenSize);
        out->token = cursor;
        cursor += out->tokenSize;
        if (!ctx->signature.empty())
        {
            out->signatureSize = ctx->signature.size() + 1;
            memcpy(cursor, ctx->signature.c_str(), out->signatureSize);
            out->signature = cursor;
        }
        else
        {
            out->signatureSize = 0;
            out->signature = nullptr;
        }
        return S_OK;
    }
    case XAsyncOp::Cancel:
        return S_OK;
    case XAsyncOp::Cleanup:
        delete ctx;
        return S_OK;
    }
    return S_OK;
}

/* -------------------- IXUserImpl4 (+ Gamertag via dual QI) -------------------- */

struct IXUserImpl4 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XUserDuplicateHandle(XUserHandle, XUserHandle *) = 0;
    virtual void STDMETHODCALLTYPE XUserCloseHandle(XUserHandle) = 0;
    virtual INT32 STDMETHODCALLTYPE XUserCompare(XUserHandle, XUserHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetMaxUsers(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserAddAsync(XUserAddOptions, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserAddResult(XAsyncBlock *, XUserHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetLocalId(XUserHandle, XUserLocalId *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserFindUserByLocalId(XUserLocalId, XUserHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetId(XUserHandle, UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserFindUserById(UINT64, XUserHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetIsGuest(XUserHandle, BOOLEAN *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetState(XUserHandle, XUserState *) = 0;
    virtual HRESULT STDMETHODCALLTYPE __PADDING__() = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetGamerPictureAsync(XUserHandle, XUserGamerPictureSize, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetGamerPictureResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetGamerPictureResult(XAsyncBlock *, SIZE_T, void *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetAgeGroup(XUserHandle, XUserAgeGroup *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserCheckPrivilege(XUserHandle, XUserPrivilegeOptions, XUserPrivilege, BOOLEAN *, XUserPrivilegeDenyReason *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserResolvePrivilegeWithUiAsync(XUserHandle, XUserPrivilegeOptions, XUserPrivilege, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserResolvePrivilegeWithUiResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureAsync(XUserHandle, XUserGetTokenAndSignatureOptions, const char *, const char *, SIZE_T, const void *, SIZE_T, const void *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureResult(XAsyncBlock *, SIZE_T, void *, void **, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureUtf16Async(XUserHandle, XUserGetTokenAndSignatureOptions, const WCHAR *, const WCHAR *, SIZE_T, const void *, SIZE_T, const void *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureUtf16ResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureUtf16Result(XAsyncBlock *, SIZE_T, void *, void **, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiAsync(XUserHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiUtf16Async(XUserHandle, const WCHAR *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiUtf16Result(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserRegisterForChangeEvent(XTaskQueueHandle, void *, XUserChangeEventCallback, XTaskQueueRegistrationToken *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken, BOOLEAN) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetSignOutDeferral(XUserSignOutDeferralHandle *) = 0;
    virtual void STDMETHODCALLTYPE XUserCloseSignOutDeferralHandle(XUserSignOutDeferralHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserAddByIdWithUiAsync(UINT64, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserAddByIdWithUiResult(XAsyncBlock *, XUserHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetMsaTokenSilentlyAsync(XUserHandle, XUserGetMsaTokenSilentlyOptions, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetMsaTokenSilentlyResult(XAsyncBlock *, SIZE_T, char *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XUserGetMsaTokenSilentlyResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XUserIsStoreUser(XUserHandle) = 0;
};

struct IXUserGamertagImpl : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XUserGetGamertag(XUserHandle, XUserGamertagComponent, SIZE_T, char *, SIZE_T *) = 0;
};

/* Rebuild User as proper vtable class — replace the broken UserApi above at link time by
 * using XUserService below as the only exported instance. */

struct XUserService final : IXUserImpl4
{
    struct GT final : IXUserGamertagImpl
    {
        XUserService *parent;
        explicit GT(XUserService *p) : parent(p) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override { return parent->QueryInterface(iid, out); }
        ULONG STDMETHODCALLTYPE AddRef() override { return parent->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return parent->Release(); }
        HRESULT STDMETHODCALLTYPE XUserGetGamertag(XUserHandle user, XUserGamertagComponent component,
                                                   SIZE_T gamertagSize, char *gamertag, SIZE_T *gamertagUsed) override
        {
            if (!user || !gamertag) return E_POINTER;
            const Identity &id = ((XUserObject *)user)->id;
            const std::string *s = &id.gamertag;
            if (component == XUserGamertagComponent::Modern) s = &id.modern;
            else if (component == XUserGamertagComponent::ModernSuffix) s = &id.modern_suffix;
            else if (component == XUserGamertagComponent::UniqueModern) s = &id.unique_modern;
            if (gamertagSize <= s->size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            memcpy(gamertag, s->c_str(), s->size() + 1);
            if (gamertagUsed) *gamertagUsed = s->size() + 1;
            return S_OK;
        }
    } gt{ this };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IXUserImpl || iid == IID_IXUserImpl2 ||
            iid == IID_IXUserImpl3 || iid == IID_IXUserImpl4 || iid == IID_IXUserImpl5 ||
            iid == IID_IXUserImpl6 || iid == CLSID_XUserImpl)
        {
            *out = static_cast<IXUserImpl4 *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IXUserGamertagImpl)
        {
            *out = &gt;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE XUserDuplicateHandle(XUserHandle handle, XUserHandle *duplicated) override
    {
        if (!duplicated) return E_POINTER;
        *duplicated = nullptr;
        if (!handle) return E_INVALIDARG;
        *duplicated = (XUserHandle)user_addref((XUserObject *)handle);
        return S_OK;
    }
    void STDMETHODCALLTYPE XUserCloseHandle(XUserHandle user) override { user_release((XUserObject *)user); }
    INT32 STDMETHODCALLTYPE XUserCompare(XUserHandle user1, XUserHandle user2) override
    {
        if (user1 == user2) return 0;
        if (!user1) return -1;
        if (!user2) return 1;
        UINT64 a = ((XUserObject *)user1)->id.xuid, b = ((XUserObject *)user2)->id.xuid;
        return (a > b) - (a < b);
    }
    HRESULT STDMETHODCALLTYPE XUserGetMaxUsers(UINT32 *maxUsers) override
    {
        if (!maxUsers) return E_POINTER;
        *maxUsers = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserAddAsync(XUserAddOptions options, XAsyncBlock *async) override
    {
        gdk_log("GDK user: XUserAddAsync options=%u\n", (unsigned)options);
        auto *ctx = new (std::nothrow) AddAsyncContext{ options };
        if (!ctx) return E_OUTOFMEMORY;
        return async_begin(async, ctx, (const void *)"XUserAddAsync", "XUserAddAsync", add_user_provider);
    }
    HRESULT STDMETHODCALLTYPE XUserAddResult(XAsyncBlock *async, XUserHandle *newUser) override
    {
        if (!newUser) return E_POINTER;
        *newUser = nullptr;
        return async_get_result(async, (const void *)"XUserAddAsync", sizeof(XUserHandle), newUser, nullptr);
    }
    HRESULT STDMETHODCALLTYPE XUserGetLocalId(XUserHandle user, XUserLocalId *userLocalId) override
    {
        if (!user || !userLocalId) return E_POINTER;
        *userLocalId = ((XUserObject *)user)->local;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserFindUserByLocalId(XUserLocalId userLocalId, XUserHandle *handle) override
    {
        if (!handle) return E_POINTER;
        *handle = nullptr;
        XUserObject *u = ensure_default_user();
        if (!u) return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
        if (u->local.value != userLocalId.value) { user_release(u); return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER); }
        *handle = (XUserHandle)u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserGetId(XUserHandle user, UINT64 *userId) override
    {
        if (!user || !userId) return E_POINTER;
        *userId = ((XUserObject *)user)->id.xuid;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserFindUserById(UINT64 userId, XUserHandle *handle) override
    {
        if (!handle) return E_POINTER;
        *handle = nullptr;
        XUserObject *u = ensure_default_user();
        if (!u) return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
        if (u->id.xuid != userId) { user_release(u); return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER); }
        *handle = (XUserHandle)u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserGetIsGuest(XUserHandle user, BOOLEAN *isGuest) override
    {
        if (!user || !isGuest) return E_POINTER;
        *isGuest = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserGetState(XUserHandle user, XUserState *state) override
    {
        if (!user || !state) return E_POINTER;
        *state = XUserState::SignedIn;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE __PADDING__() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XUserGetGamerPictureAsync(XUserHandle, XUserGamerPictureSize, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XUserGetGamerPictureAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XUserGetGamerPictureResultSize(XAsyncBlock *, SIZE_T *bufferSize) override
    {
        if (!bufferSize) return E_POINTER;
        *bufferSize = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserGetGamerPictureResult(XAsyncBlock *, SIZE_T, void *, SIZE_T *used) override
    {
        if (used) *used = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserGetAgeGroup(XUserHandle user, XUserAgeGroup *ageGroup) override
    {
        if (!user || !ageGroup) return E_POINTER;
        *ageGroup = ((XUserObject *)user)->id.age;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserCheckPrivilege(XUserHandle user, XUserPrivilegeOptions, XUserPrivilege privilege,
                                                  BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason) override
    {
        if (!user || !hasPrivilege) return E_POINTER;
        auto *uo = (XUserObject *)user;
        const std::string &prv = uo->id.privileges;
        bool has = true;
        if (!prv.empty())
        {
            char needle[32];
            snprintf(needle, sizeof(needle), " %u ", (unsigned)privilege);
            std::string hay = " " + prv + " ";
            for (char &c : hay) if (c == ',') c = ' ';
            has = hay.find(needle) != std::string::npos;
            /* XSTS prv for this Adult account lists Multiplayer (254) but omits
             * Sessions (189) / Broadcast (190). Halo still checks Sessions during
             * Live connect; deny → soft-offline even when presence/RTA can work. */
            if (!has && uo->id.age == XUserAgeGroup::Adult)
            {
                UINT32 p = (UINT32)privilege;
                if (p == 189 || p == 190) has = true;
            }
        }
        *hasPrivilege = has ? TRUE : FALSE;
        if (reason) *reason = has ? XUserPrivilegeDenyReason::None : XUserPrivilegeDenyReason::Restricted;
        gdk_log("GDK user: CheckPrivilege %u -> %u\n", (unsigned)privilege, (unsigned)*hasPrivilege);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XUserResolvePrivilegeWithUiAsync(XUserHandle, XUserPrivilegeOptions, XUserPrivilege, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XUserResolvePrivilegeWithUiAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XUserResolvePrivilegeWithUiResult(XAsyncBlock *async) override
    {
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureAsync(XUserHandle, XUserGetTokenAndSignatureOptions options, const char *method, const char *url,
                                                             SIZE_T headerCount, const void *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async) override
    {
        (void)headerCount; (void)headers;
        auto *ctx = new (std::nothrow) TokenAsyncContext();
        if (!ctx) return E_OUTOFMEMORY;
        ctx->method = method ? method : "GET";
        ctx->url = url ? url : "";
        ctx->force = ((UINT32)options & (UINT32)XUserGetTokenAndSignatureOptions::ForceRefresh) != 0;
        gdk_log("GDK user: GetTokenAndSignatureAsync %s %s%s\n", method ? method : "?", url ? url : "?",
                ctx->force ? " ForceRefresh" : "");
        if (bodyBuffer && bodySize) ctx->body.assign((const char *)bodyBuffer, bodySize);
        return async_begin(async, ctx, (const void *)"XUserGetTokenAndSignatureAsync",
                           "XUserGetTokenAndSignatureAsync", token_provider);
    }
    HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureResultSize(XAsyncBlock *async, SIZE_T *bufferSize) override
    {
        return async_get_result_size(async, bufferSize);
    }
    HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureResult(XAsyncBlock *async, SIZE_T bufferSize, void *buffer,
                                                              void **ptrToBuffer, SIZE_T *bufferUsed) override
    {
        HRESULT hr = async_get_result(async, (const void *)"XUserGetTokenAndSignatureAsync",
                                      bufferSize, buffer, bufferUsed);
        if (SUCCEEDED(hr) && ptrToBuffer) *ptrToBuffer = buffer;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureUtf16Async(XUserHandle, XUserGetTokenAndSignatureOptions, const WCHAR *, const WCHAR *,
                                                                  SIZE_T, const void *, SIZE_T, const void *, XAsyncBlock *) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureUtf16ResultSize(XAsyncBlock *, SIZE_T *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XUserGetTokenAndSignatureUtf16Result(XAsyncBlock *, SIZE_T, void *, void **, SIZE_T *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiAsync(XUserHandle, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XUserResolveIssueWithUiAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiUtf16Async(XUserHandle, const WCHAR *, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XUserResolveIssueWithUiUtf16Async", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XUserResolveIssueWithUiUtf16Result(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XUserRegisterForChangeEvent(XTaskQueueHandle, void *, XUserChangeEventCallback, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken, BOOLEAN) override { return TRUE; }
    HRESULT STDMETHODCALLTYPE XUserGetSignOutDeferral(XUserSignOutDeferralHandle *deferral) override
    {
        if (!deferral) return E_POINTER;
        *deferral = (XUserSignOutDeferralHandle)(uintptr_t)1;
        return S_OK;
    }
    void STDMETHODCALLTYPE XUserCloseSignOutDeferralHandle(XUserSignOutDeferralHandle) override {}
    HRESULT STDMETHODCALLTYPE XUserAddByIdWithUiAsync(UINT64, XAsyncBlock *async) override
    {
        return XUserAddAsync(XUserAddOptions::AddDefaultUserAllowingUI, async);
    }
    HRESULT STDMETHODCALLTYPE XUserAddByIdWithUiResult(XAsyncBlock *async, XUserHandle *newUser) override
    {
        return XUserAddResult(async, newUser);
    }
    HRESULT STDMETHODCALLTYPE XUserGetMsaTokenSilentlyAsync(XUserHandle, XUserGetMsaTokenSilentlyOptions, const char *, XAsyncBlock *) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE XUserGetMsaTokenSilentlyResult(XAsyncBlock *, SIZE_T, char *, SIZE_T *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XUserGetMsaTokenSilentlyResultSize(XAsyncBlock *, SIZE_T *) override { return E_NOTIMPL; }
    BOOLEAN STDMETHODCALLTYPE XUserIsStoreUser(XUserHandle) override { return TRUE; }
};

/* -------------------- IXStoreImpl4 -------------------- */

struct IXStoreImpl4 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XStoreCreateContext(const XUserHandle user, XStoreContextHandle *storeContextHandle) = 0;
    virtual void STDMETHODCALLTYPE XStoreCloseContextHandle(XStoreContextHandle storeContextHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryAssociatedProductsAsync(const XStoreContextHandle, XStoreProductKind, UINT32, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryAssociatedProductsResult(XAsyncBlock *, XStoreProductQueryHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryProductsAsync(const XStoreContextHandle, XStoreProductKind, const char **, SIZE_T, const char **, SIZE_T, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryProductsResult(XAsyncBlock *, XStoreProductQueryHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryEntitledProductsAsync(const XStoreContextHandle, XStoreProductKind, UINT32, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryEntitledProductsResult(XAsyncBlock *, XStoreProductQueryHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryProductForCurrentGameAsync(const XStoreContextHandle, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryProductForCurrentGameResult(XAsyncBlock *, XStoreProductQueryHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryProductForPackageAsync(const XStoreContextHandle, XStoreProductKind, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryProductForPackageResult(XAsyncBlock *, XStoreProductQueryHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreEnumerateProductsQuery(const XStoreProductQueryHandle, void *, XStoreProductQueryCallback *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XStoreProductsQueryHasMorePages(const XStoreProductQueryHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreProductsQueryNextPageAsync(const XStoreProductQueryHandle, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreProductsQueryNextPageResult(XAsyncBlock *, XStoreProductQueryHandle *) = 0;
    virtual void STDMETHODCALLTYPE XStoreCloseProductsQueryHandle(XStoreProductQueryHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForPackageAsync(const XStoreProductQueryHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForPackageResult(XAsyncBlock *, XStoreLicenseHandle *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XStoreIsLicenseValid(const XStoreLicenseHandle) = 0;
    virtual void STDMETHODCALLTYPE XStoreCloseLicenseHandle(XStoreLicenseHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForStoreIdAsync(const XStoreContextHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForStoreIdResult(XAsyncBlock *, XStoreCanAcquireLicenseResult *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForPackageAsync(const XStoreContextHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForPackageResult(XAsyncBlock *, XStoreCanAcquireLicenseResult *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryGameLicenseAsync(const XStoreContextHandle, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryGameLicenseResult(XAsyncBlock *, XStoreGameLicense *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryAddOnLicensesAsync(const XStoreContextHandle, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryAddOnLicensesResultCount(XAsyncBlock *, UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryAddOnLicensesResult(XAsyncBlock *, UINT32, XStoreAddonLicense *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryConsumableBalanceRemainingAsync(const XStoreContextHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryConsumableBalanceRemainingResult(XAsyncBlock *, XStoreConsumableResult *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreReportConsumableFulfillmentAsync(const XStoreContextHandle, const char *, UINT32, GUID, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreReportConsumableFulfillmentResult(XAsyncBlock *, XStoreConsumableResult *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreGetUserCollectionsIdAsync(const XStoreContextHandle, const char *, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreGetUserCollectionsIdResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreGetUserCollectionsIdResult(XAsyncBlock *, SIZE_T, char *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreGetUserPurchaseIdAsync(const XStoreContextHandle, const char *, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreGetUserPurchaseIdResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreGetUserPurchaseIdResult(XAsyncBlock *, SIZE_T, char *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryLicenseTokenAsync(const XStoreContextHandle, const char **, SIZE_T, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryLicenseTokenResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryLicenseTokenResult(XAsyncBlock *, SIZE_T, char *) = 0;
    virtual HRESULT STDMETHODCALLTYPE __PADDING__() = 0;
    virtual HRESULT STDMETHODCALLTYPE __PADDING_2__() = 0;
    virtual HRESULT STDMETHODCALLTYPE __PADDING_3__() = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowPurchaseUIAsync(const XStoreContextHandle, const char *, const char *, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowPurchaseUIResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowRateAndReviewUIAsync(const XStoreContextHandle, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowRateAndReviewUIResult(XAsyncBlock *, XStoreRateAndReviewResult *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowRedeemTokenUIAsync(const XStoreContextHandle, const char *, const char **, SIZE_T, BOOLEAN, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowRedeemTokenUIResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryGameAndDlcPackageUpdatesAsync(const XStoreContextHandle, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryGameAndDlcPackageUpdatesResultCount(XAsyncBlock *, UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryGameAndDlcPackageUpdatesResult(XAsyncBlock *, UINT32, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadPackageUpdatesAsync(const XStoreContextHandle, const char **, SIZE_T, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadPackageUpdatesResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackageUpdatesAsync(const XStoreContextHandle, const char **, SIZE_T, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackageUpdatesResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackagesAsync(const XStoreContextHandle, const char **, SIZE_T, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackagesResultCount(XAsyncBlock *, UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackagesResult(XAsyncBlock *, UINT32, char **) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreQueryPackageIdentifier(const char *, SIZE_T, char *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreRegisterGameLicenseChanged(XStoreContextHandle, XTaskQueueHandle, void *, XStoreGameLicenseChangedCallback *, XTaskQueueRegistrationToken *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XStoreUnregisterGameLicenseChanged(XStoreContextHandle, XTaskQueueRegistrationToken, BOOLEAN) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreRegisterPackageLicenseLost(XStoreLicenseHandle, XTaskQueueHandle, void *, XStorePackageLicenseLostCallback *, XTaskQueueRegistrationToken *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XStoreUnregisterPackageLicenseLost(XStoreLicenseHandle, XTaskQueueRegistrationToken, BOOLEAN) = 0;
    /* Impl2 */
    virtual BOOLEAN STDMETHODCALLTYPE XStoreIsAvailabilityPurchasable(const void *availability) = 0;
    /* Impl3 */
    virtual HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForDurablesAsync(const XStoreContextHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForDurablesResult(XAsyncBlock *, XStoreLicenseHandle *) = 0;
    /* Impl4 */
    virtual HRESULT STDMETHODCALLTYPE XStoreShowAssociatedProductsUIAsync(const XStoreContextHandle, const char *, XStoreProductKind, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowAssociatedProductsUIResult(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowProductPageUIAsync(const XStoreContextHandle, const char *, XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XStoreShowProductPageUIResult(XAsyncBlock *) = 0;
};

struct XStoreService final : IXStoreImpl4
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IXStoreImpl || iid == IID_IXStoreImpl2 ||
            iid == IID_IXStoreImpl3 || iid == IID_IXStoreImpl4 || iid == IID_IXStoreImpl5 ||
            iid == IID_IXStoreImpl6 || iid == CLSID_XStoreImpl)
        {
            *out = static_cast<IXStoreImpl4 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE XStoreCreateContext(const XUserHandle user, XStoreContextHandle *out) override
    {
        if (!out) return E_POINTER;
        *out = (XStoreContextHandle)(user ? user : (XUserHandle)(uintptr_t)1);
        gdk_log("GDK store: CreateContext user=%p -> %p\n", user, *out);
        return S_OK;
    }
    void STDMETHODCALLTYPE XStoreCloseContextHandle(XStoreContextHandle) override {}

    HRESULT STDMETHODCALLTYPE XStoreQueryAssociatedProductsAsync(const XStoreContextHandle, XStoreProductKind, UINT32, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryAssociatedProductsAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryAssociatedProductsResult(XAsyncBlock *async, XStoreProductQueryHandle *out) override
    {
        if (out) *out = nullptr;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryProductsAsync(const XStoreContextHandle, XStoreProductKind, const char **, SIZE_T, const char **, SIZE_T, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryProductsAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryProductsResult(XAsyncBlock *async, XStoreProductQueryHandle *out) override
    {
        if (out) *out = nullptr;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryEntitledProductsAsync(const XStoreContextHandle, XStoreProductKind, UINT32, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryEntitledProductsAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryEntitledProductsResult(XAsyncBlock *async, XStoreProductQueryHandle *out) override
    {
        if (out) *out = nullptr;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryProductForCurrentGameAsync(const XStoreContextHandle, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryProductForCurrentGameAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryProductForCurrentGameResult(XAsyncBlock *async, XStoreProductQueryHandle *out) override
    {
        if (out) *out = nullptr;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryProductForPackageAsync(const XStoreContextHandle, XStoreProductKind, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryProductForPackageAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryProductForPackageResult(XAsyncBlock *async, XStoreProductQueryHandle *out) override
    {
        if (out) *out = nullptr;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreEnumerateProductsQuery(const XStoreProductQueryHandle, void *, XStoreProductQueryCallback *) override
    {
        return S_OK; /* empty catalog */
    }
    BOOLEAN STDMETHODCALLTYPE XStoreProductsQueryHasMorePages(const XStoreProductQueryHandle) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE XStoreProductsQueryNextPageAsync(const XStoreProductQueryHandle, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreProductsQueryNextPageAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreProductsQueryNextPageResult(XAsyncBlock *async, XStoreProductQueryHandle *out) override
    {
        if (out) *out = nullptr;
        return async_get_status(async, TRUE);
    }
    void STDMETHODCALLTYPE XStoreCloseProductsQueryHandle(XStoreProductQueryHandle) override {}

    HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForPackageAsync(const XStoreProductQueryHandle, const char *packageId, XAsyncBlock *async) override
    {
        gdk_log("GDK store: AcquireLicenseForPackage %s\n", packageId ? packageId : "(null)");
        return async_begin(async, new AcquireLicenseContext(), (const void *)"XStoreAcquireLicenseForPackageAsync",
                           "XStoreAcquireLicenseForPackageAsync", acquire_license_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForPackageResult(XAsyncBlock *async, XStoreLicenseHandle *out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return async_get_result(async, (const void *)"XStoreAcquireLicenseForPackageAsync",
                                sizeof(XStoreLicenseHandle), out, nullptr);
    }
    BOOLEAN STDMETHODCALLTYPE XStoreIsLicenseValid(const XStoreLicenseHandle handle) override
    {
        return handle != nullptr;
    }
    void STDMETHODCALLTYPE XStoreCloseLicenseHandle(XStoreLicenseHandle) override {}

    HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForStoreIdAsync(const XStoreContextHandle, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new CanAcquireContext(), (const void *)"XStoreCanAcquireLicenseForStoreIdAsync",
                           "XStoreCanAcquireLicenseForStoreIdAsync", can_acquire_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForStoreIdResult(XAsyncBlock *async, XStoreCanAcquireLicenseResult *out) override
    {
        if (!out) return E_POINTER;
        return async_get_result(async, (const void *)"XStoreCanAcquireLicenseForStoreIdAsync",
                                sizeof(*out), out, nullptr);
    }
    HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForPackageAsync(const XStoreContextHandle, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new CanAcquireContext(), (const void *)"XStoreCanAcquireLicenseForPackageAsync",
                           "XStoreCanAcquireLicenseForPackageAsync", can_acquire_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreCanAcquireLicenseForPackageResult(XAsyncBlock *async, XStoreCanAcquireLicenseResult *out) override
    {
        if (!out) return E_POINTER;
        return async_get_result(async, (const void *)"XStoreCanAcquireLicenseForPackageAsync",
                                sizeof(*out), out, nullptr);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryGameLicenseAsync(const XStoreContextHandle, XAsyncBlock *async) override
    {
        gdk_log("GDK store: QueryGameLicenseAsync\n");
        return async_begin(async, new LicenseAsyncContext(), (const void *)"XStoreQueryGameLicenseAsync",
                           "XStoreQueryGameLicenseAsync", game_license_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryGameLicenseResult(XAsyncBlock *async, XStoreGameLicense *license) override
    {
        if (!license) return E_POINTER;
        return async_get_result(async, (const void *)"XStoreQueryGameLicenseAsync",
                                sizeof(*license), license, nullptr);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryAddOnLicensesAsync(const XStoreContextHandle, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryAddOnLicensesAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryAddOnLicensesResultCount(XAsyncBlock *async, UINT32 *count) override
    {
        if (!count) return E_POINTER;
        *count = 0;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryAddOnLicensesResult(XAsyncBlock *async, UINT32, XStoreAddonLicense *) override
    {
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryConsumableBalanceRemainingAsync(const XStoreContextHandle, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryConsumableBalanceRemainingAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryConsumableBalanceRemainingResult(XAsyncBlock *async, XStoreConsumableResult *out) override
    {
        if (out) out->quantity = 0;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreReportConsumableFulfillmentAsync(const XStoreContextHandle, const char *, UINT32, GUID, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreReportConsumableFulfillmentAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreReportConsumableFulfillmentResult(XAsyncBlock *async, XStoreConsumableResult *out) override
    {
        if (out) out->quantity = 0;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreGetUserCollectionsIdAsync(const XStoreContextHandle, const char *, const char *, XAsyncBlock *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreGetUserCollectionsIdResultSize(XAsyncBlock *, SIZE_T *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreGetUserCollectionsIdResult(XAsyncBlock *, SIZE_T, char *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreGetUserPurchaseIdAsync(const XStoreContextHandle, const char *, const char *, XAsyncBlock *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreGetUserPurchaseIdResultSize(XAsyncBlock *, SIZE_T *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreGetUserPurchaseIdResult(XAsyncBlock *, SIZE_T, char *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreQueryLicenseTokenAsync(const XStoreContextHandle, const char **, SIZE_T, const char *, XAsyncBlock *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreQueryLicenseTokenResultSize(XAsyncBlock *, SIZE_T *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreQueryLicenseTokenResult(XAsyncBlock *, SIZE_T, char *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE __PADDING__() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE __PADDING_2__() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE __PADDING_3__() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XStoreShowPurchaseUIAsync(const XStoreContextHandle, const char *, const char *, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreShowPurchaseUIAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowPurchaseUIResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XStoreShowRateAndReviewUIAsync(const XStoreContextHandle, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreShowRateAndReviewUIAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowRateAndReviewUIResult(XAsyncBlock *async, XStoreRateAndReviewResult *out) override
    {
        if (out) out->wasUpdated = FALSE;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowRedeemTokenUIAsync(const XStoreContextHandle, const char *, const char **, SIZE_T, BOOLEAN, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreShowRedeemTokenUIAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowRedeemTokenUIResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XStoreQueryGameAndDlcPackageUpdatesAsync(const XStoreContextHandle, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreQueryGameAndDlcPackageUpdatesAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryGameAndDlcPackageUpdatesResultCount(XAsyncBlock *async, UINT32 *count) override
    {
        if (count) *count = 0;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryGameAndDlcPackageUpdatesResult(XAsyncBlock *async, UINT32, void *) override
    {
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreDownloadPackageUpdatesAsync(const XStoreContextHandle, const char **, SIZE_T, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreDownloadPackageUpdatesAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreDownloadPackageUpdatesResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackageUpdatesAsync(const XStoreContextHandle, const char **, SIZE_T, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreDownloadAndInstallPackageUpdatesAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackageUpdatesResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackagesAsync(const XStoreContextHandle, const char **, SIZE_T, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreDownloadAndInstallPackagesAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackagesResultCount(XAsyncBlock *async, UINT32 *count) override
    {
        if (count) *count = 0;
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreDownloadAndInstallPackagesResult(XAsyncBlock *async, UINT32, char **) override
    {
        return async_get_status(async, TRUE);
    }
    HRESULT STDMETHODCALLTYPE XStoreQueryPackageIdentifier(const char *, SIZE_T size, char *out) override
    {
        if (!out || size < 2) return E_INVALIDARG;
        out[0] = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XStoreRegisterGameLicenseChanged(XStoreContextHandle, XTaskQueueHandle, void *, XStoreGameLicenseChangedCallback *, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XStoreUnregisterGameLicenseChanged(XStoreContextHandle, XTaskQueueRegistrationToken, BOOLEAN) override { return TRUE; }
    HRESULT STDMETHODCALLTYPE XStoreRegisterPackageLicenseLost(XStoreLicenseHandle, XTaskQueueHandle, void *, XStorePackageLicenseLostCallback *, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XStoreUnregisterPackageLicenseLost(XStoreLicenseHandle, XTaskQueueRegistrationToken, BOOLEAN) override { return TRUE; }
    BOOLEAN STDMETHODCALLTYPE XStoreIsAvailabilityPurchasable(const void *) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForDurablesAsync(const XStoreContextHandle, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new AcquireLicenseContext(), (const void *)"XStoreAcquireLicenseForDurablesAsync",
                           "XStoreAcquireLicenseForDurablesAsync", acquire_license_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreAcquireLicenseForDurablesResult(XAsyncBlock *async, XStoreLicenseHandle *out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return async_get_result(async, (const void *)"XStoreAcquireLicenseForDurablesAsync",
                                sizeof(XStoreLicenseHandle), out, nullptr);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowAssociatedProductsUIAsync(const XStoreContextHandle, const char *, XStoreProductKind, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreShowAssociatedProductsUIAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowAssociatedProductsUIResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
    HRESULT STDMETHODCALLTYPE XStoreShowProductPageUIAsync(const XStoreContextHandle, const char *, XAsyncBlock *async) override
    {
        return async_begin(async, new EmptyAsyncContext(), nullptr, "XStoreShowProductPageUIAsync", empty_ok_provider);
    }
    HRESULT STDMETHODCALLTYPE XStoreShowProductPageUIResult(XAsyncBlock *async) override { return async_get_status(async, TRUE); }
};

static HRESULT CALLBACK async_run_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    struct Ctx { XAsyncWork *work; };
    auto *c = (Ctx *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin: return S_OK;
    case XAsyncOp::DoWork:
    {
        HRESULT hr = c->work(data->async);
        if (hr != E_PENDING) async_complete(data->async, hr, 0);
        return hr;
    }
    case XAsyncOp::GetResult: return S_OK;
    case XAsyncOp::Cancel: return S_OK;
    case XAsyncOp::Cleanup: delete c; return S_OK;
    }
    return S_OK;
}

/* -------------------- IXThreadingImpl -------------------- */

struct IXThreadingImpl : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XAsyncGetStatus(XAsyncBlock *, BOOLEAN) = 0;
    virtual HRESULT STDMETHODCALLTYPE XAsyncGetResultSize(XAsyncBlock *, SIZE_T *) = 0;
    virtual void STDMETHODCALLTYPE XAsyncCancel(XAsyncBlock *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XAsyncRun(XAsyncBlock *, XAsyncWork *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XAsyncBegin(XAsyncBlock *, void *, const void *, const char *, XAsyncProvider *) = 0;
    virtual HRESULT STDMETHODCALLTYPE __PADDING__() = 0;
    virtual HRESULT STDMETHODCALLTYPE XAsyncSchedule(XAsyncBlock *, UINT32) = 0;
    virtual void STDMETHODCALLTYPE XAsyncComplete(XAsyncBlock *, HRESULT, SIZE_T) = 0;
    virtual HRESULT STDMETHODCALLTYPE XAsyncGetResult(XAsyncBlock *, const void *, SIZE_T, void *, SIZE_T *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueCreate(XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueCreateComposite(XTaskQueuePortHandle, XTaskQueuePortHandle, XTaskQueueHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueGetPort(XTaskQueueHandle, XTaskQueuePort, XTaskQueuePortHandle *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueDuplicateHandle(XTaskQueueHandle, XTaskQueueHandle *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XTaskQueueDispatch(XTaskQueueHandle, XTaskQueuePort, UINT32) = 0;
    virtual void STDMETHODCALLTYPE XTaskQueueCloseHandle(XTaskQueueHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueSubmitCallback(XTaskQueueHandle, XTaskQueuePort, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueSubmitDelayedCallback(XTaskQueueHandle, XTaskQueuePort, UINT32, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueRegisterWaiter(XTaskQueueHandle, XTaskQueuePort, HANDLE, void *, void *, XTaskQueueRegistrationToken *) = 0;
    virtual void STDMETHODCALLTYPE XTaskQueueUnregisterWaiter(XTaskQueueHandle, XTaskQueueRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueTerminate(XTaskQueueHandle, BOOLEAN, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE XTaskQueueRegisterMonitor(XTaskQueueHandle, void *, void *, XTaskQueueRegistrationToken *) = 0;
    virtual void STDMETHODCALLTYPE XTaskQueueUnregisterMonitor(XTaskQueueHandle, XTaskQueueRegistrationToken) = 0;
    virtual void STDMETHODCALLTYPE XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle *) = 0;
    virtual void STDMETHODCALLTYPE XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle) = 0;
    virtual void STDMETHODCALLTYPE XThreadSetTimeSensitive(BOOLEAN) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XTaskQueueIsEmpty(XTaskQueueHandle, XTaskQueuePort) = 0;
    virtual void STDMETHODCALLTYPE XThreadAssertNotTimeSensitive() = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XThreadIsTimeSensitive() = 0;
};

struct XTaskQueueObject
{
    LONG refs = 1;
};

static XTaskQueueHandle g_process_queue = nullptr;

struct XThreadingService final : IXThreadingImpl
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IXThreadingImpl || iid == CLSID_XThreadingImpl)
        {
            *out = static_cast<IXThreadingImpl *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE XAsyncGetStatus(XAsyncBlock *b, BOOLEAN wait) override { return async_get_status(b, wait); }
    HRESULT STDMETHODCALLTYPE XAsyncGetResultSize(XAsyncBlock *b, SIZE_T *n) override { return async_get_result_size(b, n); }
    void STDMETHODCALLTYPE XAsyncCancel(XAsyncBlock *b) override { async_cancel(b); }
    HRESULT STDMETHODCALLTYPE XAsyncRun(XAsyncBlock *b, XAsyncWork *work) override
    {
        if (!b || !work) return E_INVALIDARG;
        struct Ctx { XAsyncWork *work; };
        auto *ctx = new Ctx{ work };
        return async_begin(b, ctx, (const void *)work, "XAsyncRun", async_run_provider);
    }
    HRESULT STDMETHODCALLTYPE XAsyncBegin(XAsyncBlock *b, void *ctx, const void *id, const char *name, XAsyncProvider *p) override
    {
        return async_begin(b, ctx, id, name, p);
    }
    HRESULT STDMETHODCALLTYPE __PADDING__() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XAsyncSchedule(XAsyncBlock *, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE XAsyncComplete(XAsyncBlock *b, HRESULT r, SIZE_T n) override { async_complete(b, r, n); }
    HRESULT STDMETHODCALLTYPE XAsyncGetResult(XAsyncBlock *b, const void *id, SIZE_T n, void *buf, SIZE_T *used) override
    {
        return async_get_result(b, id, n, buf, used);
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueCreate(XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle *out) override
    {
        if (!out) return E_POINTER;
        *out = new XTaskQueueObject();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueCreateComposite(XTaskQueuePortHandle, XTaskQueuePortHandle, XTaskQueueHandle *out) override
    {
        return XTaskQueueCreate(XTaskQueueDispatchMode::Immediate, XTaskQueueDispatchMode::Immediate, out);
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueGetPort(XTaskQueueHandle q, XTaskQueuePort, XTaskQueuePortHandle *out) override
    {
        if (!out) return E_POINTER;
        *out = (XTaskQueuePortHandle)q;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueDuplicateHandle(XTaskQueueHandle q, XTaskQueueHandle *out) override
    {
        if (!out || !q) return E_INVALIDARG;
        InterlockedIncrement(&q->refs);
        *out = q;
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XTaskQueueDispatch(XTaskQueueHandle, XTaskQueuePort, UINT32) override { return FALSE; }
    void STDMETHODCALLTYPE XTaskQueueCloseHandle(XTaskQueueHandle q) override
    {
        if (q && InterlockedDecrement(&q->refs) == 0) delete q;
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueSubmitCallback(XTaskQueueHandle, XTaskQueuePort, void *ctx, void *cb) override
    {
        using CB = void __stdcall(void *, BOOLEAN);
        if (cb) ((CB *)cb)(ctx, FALSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueSubmitDelayedCallback(XTaskQueueHandle q, XTaskQueuePort p, UINT32, void *ctx, void *cb) override
    {
        return XTaskQueueSubmitCallback(q, p, ctx, cb);
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueRegisterWaiter(XTaskQueueHandle, XTaskQueuePort, HANDLE, void *, void *, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    void STDMETHODCALLTYPE XTaskQueueUnregisterWaiter(XTaskQueueHandle, XTaskQueueRegistrationToken) override {}
    HRESULT STDMETHODCALLTYPE XTaskQueueTerminate(XTaskQueueHandle, BOOLEAN, void *ctx, void *cb) override
    {
        using CB = void __stdcall(void *);
        if (cb) ((CB *)cb)(ctx);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XTaskQueueRegisterMonitor(XTaskQueueHandle, void *, void *, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    void STDMETHODCALLTYPE XTaskQueueUnregisterMonitor(XTaskQueueHandle, XTaskQueueRegistrationToken) override {}
    void STDMETHODCALLTYPE XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle *out) override
    {
        if (!out) return;
        if (!g_process_queue) g_process_queue = new XTaskQueueObject();
        InterlockedIncrement(&g_process_queue->refs);
        *out = g_process_queue;
    }
    void STDMETHODCALLTYPE XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle q) override { g_process_queue = q; }
    void STDMETHODCALLTYPE XThreadSetTimeSensitive(BOOLEAN) override {}
    BOOLEAN STDMETHODCALLTYPE XTaskQueueIsEmpty(XTaskQueueHandle, XTaskQueuePort) override { return TRUE; }
    void STDMETHODCALLTYPE XThreadAssertNotTimeSensitive() override {}
    BOOLEAN STDMETHODCALLTYPE XThreadIsTimeSensitive() override { return FALSE; }
};


/* -------------------- XGame / XSystem -------------------- */

struct IXGameImpl3 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XGameGetXboxTitleId(UINT32 *titleId) = 0;
    virtual void STDMETHODCALLTYPE XLaunchNewGame(const char *exePath, const char *args, XUserHandle defaultUser) = 0;
    virtual HRESULT STDMETHODCALLTYPE XLaunchRestartOnCrash(const char *args, UINT32 reserved) = 0;
};

struct XGameService final : IXGameImpl3
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IXGameImpl || iid == IID_IXGameImpl2 ||
            iid == IID_IXGameImpl3 || iid == CLSID_XGameImpl)
        {
            *out = static_cast<IXGameImpl3 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE XGameGetXboxTitleId(UINT32 *titleId) override
    {
        if (!titleId) return E_POINTER;
        TitleIdentity id = {};
        load_title_identity(&id);
        if (!id.have_title)
        {
            gdk_log("GDK game: GetXboxTitleId missing (no env / MicrosoftGame.Config)\n");
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        *titleId = id.title_id;
        gdk_log("GDK game: GetXboxTitleId -> %08X\n", *titleId);
        return S_OK;
    }
    void STDMETHODCALLTYPE XLaunchNewGame(const char *, const char *, XUserHandle) override {}
    HRESULT STDMETHODCALLTYPE XLaunchRestartOnCrash(const char *, UINT32) override { return S_OK; }
};

using XSystemHandleCallback = void __stdcall(void *handle, UINT32 type, UINT32 reason, void *context);

struct IXSystemImpl3 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XSystemGetConsoleId(INT32 consoleIdSize, char *consoleId, SIZE_T *consoleIdUsed) = 0;
    virtual HRESULT STDMETHODCALLTYPE XSystemGetXboxLiveSandboxId(INT32 sandboxIdSize, char *sandboxId, SIZE_T *sandboxIdUsed) = 0;
    virtual HRESULT STDMETHODCALLTYPE XSystemGetAppSpecificDeviceId(INT32 appSpecificDeviceIdSize, char *appSpecificDeviceId, SIZE_T *appSpecificDeviceIdUsed) = 0;
    virtual HRESULT STDMETHODCALLTYPE XSystemHandleTrack(XSystemHandleCallback callback, void *context) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XSystemIsHandleValid(void *handle) = 0;
};

static HRESULT copy_id(const char *value, INT32 size, char *buffer, SIZE_T *used)
{
    if (!buffer || size <= 0) return E_INVALIDARG;
    size_t need = strlen(value) + 1;
    if ((SIZE_T)size < need) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(buffer, value, need);
    if (used) *used = need;
    return S_OK;
}

struct XSystemService final : IXSystemImpl3
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IXSystemImpl || iid == IID_IXSystemImpl2 ||
            iid == IID_IXSystemImpl3 || iid == IID_IXSystemImpl4 || iid == IID_IXSystemImpl5 ||
            iid == CLSID_XSystemImpl)
        {
            *out = static_cast<IXSystemImpl3 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE XSystemGetConsoleId(INT32 size, char *out, SIZE_T *used) override
    {
        /* Stable synthetic console id for PC. */
        HRESULT hr = copy_id("WINEUWPPC00000001", size, out, used);
        gdk_log("GDK system: GetConsoleId hr=%08lx\n", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE XSystemGetXboxLiveSandboxId(INT32 size, char *out, SIZE_T *used) override
    {
        char env[64] = {};
        const char *sandbox = "RETAIL";
        DWORD n = GetEnvironmentVariableA("WINE_UWP_XBOX_SANDBOX", env, sizeof(env));
        if (n && n < sizeof(env)) sandbox = env;
        HRESULT hr = copy_id(sandbox, size, out, used);
        gdk_log("GDK system: GetXboxLiveSandboxId -> %s hr=%08lx\n", sandbox, hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE XSystemGetAppSpecificDeviceId(INT32 size, char *out, SIZE_T *used) override
    {
        HRESULT hr = copy_id("wine-uwp-app-device-0001", size, out, used);
        gdk_log("GDK system: GetAppSpecificDeviceId hr=%08lx\n", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE XSystemHandleTrack(XSystemHandleCallback, void *) override { return S_OK; }
    BOOLEAN STDMETHODCALLTYPE XSystemIsHandleValid(void *handle) override { return handle != nullptr; }
};

/* -------------------- XNetworking -------------------- */

/* Halo's XsapiThunks/libHttpClient always calls
 * XNetworkingQuerySecurityInformationForUrlUtf16Async (no IsFeatureAvailable
 * gate). The wide stub returned E_NOTIMPL without XAsyncComplete → HC waited
 * forever on XAsyncGetStatus(..., true) → offline spinner hang. */

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1   0x00000080
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 0x00000200
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif

static constexpr UINT32 kDefaultSecurityProtocolFlags =
    WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 |
    WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
    WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;

enum class XNetworkingConnectivityCostHint : UINT32
{
    Unknown = 0,
    Unrestricted = 1,
    Fixed = 2,
    Variable = 3,
};

enum class XNetworkingConnectivityLevelHint : UINT32
{
    Unknown = 0,
    None = 1,
    LocalAccess = 2,
    InternetAccess = 3,
    ConstrainedInternetAccess = 4,
};

struct XNetworkingThumbprint
{
    UINT32 thumbprintType;
    SIZE_T thumbprintBufferByteCount;
    UINT8 *thumbprintBuffer;
};

struct XNetworkingSecurityInformation
{
    UINT32 enabledHttpSecurityProtocolFlags;
    SIZE_T thumbprintCount;
    XNetworkingThumbprint *thumbprints;
};

struct XNetworkingConnectivityHint
{
    XNetworkingConnectivityLevelHint connectivityLevel;
    XNetworkingConnectivityCostHint connectivityCost;
    UINT32 ianaInterfaceType;
    BOOLEAN networkInitialized;
    BOOLEAN approachingDataLimit;
    BOOLEAN overDataLimit;
    BOOLEAN roaming;
};

using XNetworkingConnectivityHintChangedCallback =
    void __stdcall(void *context, const XNetworkingConnectivityHint *connectivityHint);
using XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback =
    void __stdcall(void *context, UINT16 preferredLocalUdpMultiplayerPort);

static constexpr GUID IID_IXNetworkingImpl_ =
    {0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
static constexpr GUID IID_IXNetworkingImpl2_ =
    {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};

static void fill_security_info(XNetworkingSecurityInformation *info)
{
    info->enabledHttpSecurityProtocolFlags = kDefaultSecurityProtocolFlags;
    info->thumbprintCount = 0;
    info->thumbprints = nullptr;
}

static HRESULT CALLBACK security_info_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
        /* Complete inline — HC waits with XAsyncGetStatus(..., true). */
        async_complete(data->async, S_OK, sizeof(XNetworkingSecurityInformation));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XNetworkingSecurityInformation))
            return E_INVALIDARG;
        fill_security_info((XNetworkingSecurityInformation *)data->buffer);
        return S_OK;
    case XAsyncOp::Cancel:
    case XAsyncOp::Cleanup:
        return S_OK;
    }
    return S_OK;
}

static HRESULT security_info_async_begin(XAsyncBlock *asyncBlock, const char *url_for_log)
{
    if (!asyncBlock) return E_INVALIDARG;
    gdk_log("GDK net: QuerySecurityInformation url=%s\n", url_for_log ? url_for_log : "(null)");
    return async_begin(asyncBlock, nullptr, (const void *)security_info_provider,
                       "XNetworkingQuerySecurityInformation", security_info_provider);
}

static HRESULT security_info_async_result(XAsyncBlock *asyncBlock,
                                          SIZE_T bufferByteCount,
                                          SIZE_T *bufferByteCountUsed,
                                          UINT8 *buffer,
                                          XNetworkingSecurityInformation **outInfo)
{
    if (!buffer || !outInfo) return E_POINTER;
    *outInfo = nullptr;
    HRESULT hr = async_get_result(asyncBlock, nullptr, bufferByteCount, buffer, bufferByteCountUsed);
    if (FAILED(hr)) return hr;
    *outInfo = (XNetworkingSecurityInformation *)buffer;
    return S_OK;
}

struct UdpPortAsyncContext { UINT16 port = 3074; };

static HRESULT CALLBACK udp_port_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    auto *c = (UdpPortAsyncContext *)data->context;
    switch (op)
    {
    case XAsyncOp::Begin:
        return S_OK;
    case XAsyncOp::DoWork:
        async_complete(data->async, S_OK, sizeof(UINT16));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(UINT16)) return E_INVALIDARG;
        *(UINT16 *)data->buffer = c->port;
        return S_OK;
    case XAsyncOp::Cleanup:
        delete c;
        return S_OK;
    default:
        return S_OK;
    }
}

struct IXNetworkingImpl2 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPort(UINT16 *port) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(XAsyncBlock *asyncBlock) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(XAsyncBlock *asyncBlock, UINT16 *port) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(XTaskQueueHandle, void *, XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback *, XTaskQueueRegistrationToken *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(XTaskQueueRegistrationToken, BOOLEAN) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlAsync(const char *url, XAsyncBlock *asyncBlock) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlAsyncResultSize(XAsyncBlock *asyncBlock, SIZE_T *n) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlAsyncResult(XAsyncBlock *asyncBlock, SIZE_T n, SIZE_T *used, UINT8 *buf, XNetworkingSecurityInformation **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16Async(const WCHAR *url, XAsyncBlock *asyncBlock) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(XAsyncBlock *asyncBlock, SIZE_T *n) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(XAsyncBlock *asyncBlock, SIZE_T n, SIZE_T *used, UINT8 *buf, XNetworkingSecurityInformation **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingVerifyServerCertificate(void *requestHandle, const XNetworkingSecurityInformation *info) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingGetConnectivityHint(XNetworkingConnectivityHint *hint) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingRegisterConnectivityHintChanged(XTaskQueueHandle, void *, XNetworkingConnectivityHintChangedCallback *, XTaskQueueRegistrationToken *) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XNetworkingUnregisterConnectivityHintChanged(XTaskQueueRegistrationToken, BOOLEAN) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQueryConfigurationSetting(UINT32 setting, UINT64 *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingSetConfigurationSetting(UINT32 setting, UINT64 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE XNetworkingQueryStatistics(UINT32 type, void *buffer) = 0;
};

struct XNetworkingService final : IXNetworkingImpl2
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IXNetworkingImpl_ || iid == IID_IXNetworkingImpl2_ ||
            iid == CLSID_XNetworkingImpl_)
        {
            *out = static_cast<IXNetworkingImpl2 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPort(UINT16 *port) override
    {
        if (!port) return E_POINTER;
        *port = 3074;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(XAsyncBlock *asyncBlock) override
    {
        if (!asyncBlock) return E_INVALIDARG;
        struct Ctx { UINT16 port = 3074; };
        auto *ctx = new (std::nothrow) UdpPortAsyncContext();
        if (!ctx) return E_OUTOFMEMORY;
        return async_begin(asyncBlock, ctx, nullptr, "XNetworkingQueryPreferredLocalUdpMultiplayerPort",
                           udp_port_provider);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(XAsyncBlock *asyncBlock, UINT16 *port) override
    {
        if (!port) return E_POINTER;
        return async_get_result(asyncBlock, nullptr, sizeof(UINT16), port, nullptr);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(XTaskQueueHandle, void *, XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback *, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(XTaskQueueRegistrationToken, BOOLEAN) override
    {
        return TRUE;
    }

    HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlAsync(const char *url, XAsyncBlock *asyncBlock) override
    {
        return security_info_async_begin(asyncBlock, url);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlAsyncResultSize(XAsyncBlock *asyncBlock, SIZE_T *n) override
    {
        return async_get_result_size(asyncBlock, n);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlAsyncResult(XAsyncBlock *asyncBlock, SIZE_T n, SIZE_T *used, UINT8 *buf, XNetworkingSecurityInformation **out) override
    {
        return security_info_async_result(asyncBlock, n, used, buf, out);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16Async(const WCHAR *url, XAsyncBlock *asyncBlock) override
    {
        char narrow[512] = {};
        if (url) WideCharToMultiByte(CP_UTF8, 0, url, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        return security_info_async_begin(asyncBlock, narrow);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(XAsyncBlock *asyncBlock, SIZE_T *n) override
    {
        return async_get_result_size(asyncBlock, n);
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(XAsyncBlock *asyncBlock, SIZE_T n, SIZE_T *used, UINT8 *buf, XNetworkingSecurityInformation **out) override
    {
        return security_info_async_result(asyncBlock, n, used, buf, out);
    }

    HRESULT STDMETHODCALLTYPE XNetworkingVerifyServerCertificate(void *, const XNetworkingSecurityInformation *) override
    {
        /* Empty thumbprint list → accept Schannel's chain validation. */
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XNetworkingGetConnectivityHint(XNetworkingConnectivityHint *hint) override
    {
        if (!hint) return E_POINTER;
        memset(hint, 0, sizeof(*hint));
        hint->connectivityLevel = XNetworkingConnectivityLevelHint::InternetAccess;
        hint->connectivityCost = XNetworkingConnectivityCostHint::Unrestricted;
        hint->ianaInterfaceType = 6; /* ethernet */
        hint->networkInitialized = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XNetworkingRegisterConnectivityHintChanged(XTaskQueueHandle, void *, XNetworkingConnectivityHintChangedCallback *, XTaskQueueRegistrationToken *token) override
    {
        if (token) token->token = 1;
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XNetworkingUnregisterConnectivityHintChanged(XTaskQueueRegistrationToken, BOOLEAN) override
    {
        return TRUE;
    }
    HRESULT STDMETHODCALLTYPE XNetworkingQueryConfigurationSetting(UINT32, UINT64 *value) override
    {
        if (!value) return E_POINTER;
        *value = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XNetworkingSetConfigurationSetting(UINT32, UINT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE XNetworkingQueryStatistics(UINT32, void *buffer) override
    {
        if (buffer) memset(buffer, 0, sizeof(UINT64) * 5);
        return S_OK;
    }
};

/* -------------------- XGameRuntimeFeature -------------------- */

/* Matches xgameruntimefeature.idl XGameRuntimeFeature enum order. */
enum : UINT32
{
    kFeature_XAccessibility = 0,
    kFeature_XAppCapture = 1,
    kFeature_XAsync = 2,
    kFeature_XAsyncProvider = 3,
    kFeature_XDisplay = 4,
    kFeature_XGame = 5,
    kFeature_XGameInvite = 6,
    kFeature_XGameSave = 7,
    kFeature_XGameUI = 8,
    kFeature_XLauncher = 9,
    kFeature_XNetworking = 10,
    kFeature_XPackage = 11,
    kFeature_XPersistentLocalStorage = 12,
    kFeature_XSpeechSynthesizer = 13,
    kFeature_XStore = 14,
    kFeature_XSystem = 15,
    kFeature_XTaskQueue = 16,
    kFeature_XThread = 17,
    kFeature_XUser = 18,
    kFeature_XError = 19,
    kFeature_XGameEvent = 20,
    kFeature_XGameStreaming = 21,
};

/* Advertise services we implement. XNetworking must be TRUE now that
 * QuerySecurityInformation completes; Halo's HC ignores IsFeatureAvailable and
 * always calls the Utf16 security API, but other titles gate on the feature. */
static BOOLEAN feature_available(UINT32 feature)
{
    switch (feature)
    {
    case kFeature_XAsync:
    case kFeature_XAsyncProvider:
    case kFeature_XGame:
    case kFeature_XNetworking:
    case kFeature_XPackage:
    case kFeature_XStore:
    case kFeature_XSystem:
    case kFeature_XTaskQueue:
    case kFeature_XThread:
    case kFeature_XUser:
    case kFeature_XError:
        return TRUE;
    default:
        return FALSE;
    }
}

struct IXGameRuntimeFeatureImpl : IUnknown
{
    virtual BOOLEAN STDMETHODCALLTYPE XGameRuntimeIsFeatureAvailable(UINT32 feature) = 0;
};

struct XFeatureService final : IXGameRuntimeFeatureImpl
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        /* Coclass UUID == interface UUID; GdkSupport often QI's with the CLSID. */
        if (iid == IID_IUnknown || iid == CLSID_XGameRuntimeFeatureImpl_)
        {
            *out = static_cast<IXGameRuntimeFeatureImpl *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    BOOLEAN STDMETHODCALLTYPE XGameRuntimeIsFeatureAvailable(UINT32 feature) override
    {
        BOOLEAN avail = feature_available(feature);
        gdk_log("GDK feature: IsFeatureAvailable(%u) -> %u\n", feature, (unsigned)avail);
        return avail;
    }
};

/* -------------------- factories -------------------- */

static XUserService g_user;
static XStoreService g_store;
static XThreadingService g_threading;
static XGameService g_game;
static XSystemService g_system;
static XFeatureService g_feature;
static XNetworkingService g_networking;

HRESULT query_xuser(REFIID iid, void **out)
{
    return g_user.QueryInterface(iid, out);
}
HRESULT query_xstore(REFIID iid, void **out)
{
    return g_store.QueryInterface(iid, out);
}
HRESULT query_xthreading(REFIID iid, void **out)
{
    return g_threading.QueryInterface(iid, out);
}
HRESULT query_xgame(REFIID iid, void **out)
{
    return g_game.QueryInterface(iid, out);
}
HRESULT query_xsystem(REFIID iid, void **out)
{
    return g_system.QueryInterface(iid, out);
}
HRESULT query_xfeature(REFIID iid, void **out)
{
    return g_feature.QueryInterface(iid, out);
}
HRESULT query_xnetworking(REFIID iid, void **out)
{
    return g_networking.QueryInterface(iid, out);
}

/* Flat C exports — XsapiThunks GetProcAddress's these for the Utf16 security path. */
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingQuerySecurityInformationForUrlAsync(const char *url, XAsyncBlock *asyncBlock)
{
    return g_networking.XNetworkingQuerySecurityInformationForUrlAsync(url, asyncBlock);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingQuerySecurityInformationForUrlAsyncResultSize(XAsyncBlock *asyncBlock, SIZE_T *n)
{
    return g_networking.XNetworkingQuerySecurityInformationForUrlAsyncResultSize(asyncBlock, n);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingQuerySecurityInformationForUrlAsyncResult(XAsyncBlock *asyncBlock, SIZE_T n, SIZE_T *used,
                                                     UINT8 *buf, XNetworkingSecurityInformation **out)
{
    return g_networking.XNetworkingQuerySecurityInformationForUrlAsyncResult(asyncBlock, n, used, buf, out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingQuerySecurityInformationForUrlUtf16Async(const WCHAR *url, XAsyncBlock *asyncBlock)
{
    return g_networking.XNetworkingQuerySecurityInformationForUrlUtf16Async(url, asyncBlock);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(XAsyncBlock *asyncBlock, SIZE_T *n)
{
    return g_networking.XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(asyncBlock, n);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(XAsyncBlock *asyncBlock, SIZE_T n, SIZE_T *used,
                                                          UINT8 *buf, XNetworkingSecurityInformation **out)
{
    return g_networking.XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(asyncBlock, n, used, buf, out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingVerifyServerCertificate(void *requestHandle, const XNetworkingSecurityInformation *info)
{
    return g_networking.XNetworkingVerifyServerCertificate(requestHandle, info);
}
extern "C" __declspec(dllexport) HRESULT WINAPI
XNetworkingGetConnectivityHint(XNetworkingConnectivityHint *hint)
{
    return g_networking.XNetworkingGetConnectivityHint(hint);
}

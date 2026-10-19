// Local installed-content support for GDK's private package interface.
// ABI reference: https://github.com/nocts-software/xgameruntime/blob/main/xpackage.idl
// Unsupported services return errors; availability comes from installer metadata.
#define NOMINMAX
#include "package_abi.h"
#include "xuser_xstore_query.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "../common/json.h"

static std::wstring root;
static Json metadata;
static std::string package_id;
static std::string content_identifier(const std::wstring &dir);
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HRESULT init_status = E_UNEXPECTED;

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

static bool try_load_chunks_json(const std::wstring &dir)
{
    HANDLE file = CreateFileW((dir + L"\\.wine-uwp-chunks.json").c_str(), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
    { CloseHandle(file); return false; }
    std::string data((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    BOOL read = ReadFile(file, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(file);
    if (!read || got != data.size()) return false;
    Json parsed = parse_json(data);
    if (parsed["version"].n != 1 || parsed["regions"].type != Json::ARR ||
        parsed["package"].str().empty()) return false;
    root = dir;
    metadata = std::move(parsed);
    package_id = content_identifier(dir);
    return true;
}
/* A package identifier is what XPackageGetCurrentProcessPackageIdentifier answers and every other XPackage
 * call takes back: at most XPACKAGE_IDENTIFIER_MAX_LENGTH (33) bytes with the terminator, so it cannot be
 * the package full name. For an MSIXVC it is the content id, 32 hex digits. install-app.py records it as
 * "identifier"; older installs still have it in the layout file's name, layout_<content id>.xml. Halo's
 * campaign asks with a 33-byte buffer, and a refusal there makes it skip every availability query and
 * then read optional HD modules that were never installed. */
static std::string content_identifier(const std::wstring &dir)
{
    std::string id = metadata["identifier"].str();
    if (id.empty())
    {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((dir + L"\\layout_*.xml").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            for (const WCHAR *p = data.cFileName + 7; *p && *p != L'.'; p++)
                if (iswxdigit(*p)) id += (char)towupper(*p);
            FindClose(find);
        }
        if (id.size() != 32) id.clear();
    }
    if (id.empty()) id = metadata["package"].str();
    gdk_log("GDK content: package identifier %s\n", id.c_str());
    return id;
}
static bool is_this_package(const char *id)
{
    return !id || !_stricmp(id, package_id.c_str()) || !_stricmp(id, metadata["package"].str().c_str());
}
static BOOL CALLBACK load_metadata(PINIT_ONCE, void *, void **)
{
    setvbuf(stderr, nullptr, _IONBF, 0);
    WCHAR path[32768];
    DWORD count = GetEnvironmentVariableW(L"WINE_UWP_PACKAGE_PATH", path, ARRAYSIZE(path));
    if (count && count < ARRAYSIZE(path) && try_load_chunks_json(std::wstring(path, count)))
    {
        gdk_log("GDK content: %zu recorded regions (env)\n", metadata["regions"].a.size());
        init_status = S_OK;
        return TRUE;
    }
    /* Fall back: DLL may live in stage/ or stage/game/ when the launcher env was dropped. */
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&load_metadata, &self) &&
        GetModuleFileNameW(self, path, ARRAYSIZE(path)))
    {
        std::wstring dll = path;
        size_t slash = dll.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            std::wstring dir = dll.substr(0, slash);
            if (try_load_chunks_json(dir) ||
                ((slash = dir.find_last_of(L'\\')) != std::wstring::npos &&
                 try_load_chunks_json(dir.substr(0, slash))))
            {
                gdk_log("GDK content: %zu recorded regions (beside dll)\n", metadata["regions"].a.size());
                init_status = S_OK;
                return TRUE;
            }
        }
    }
    gdk_log("GDK content: no .wine-uwp-chunks.json (env=%lu)\n", count);
    init_status = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    return TRUE;
}
static HRESULT initialize()
{
    if (!InitOnceExecuteOnce(&once, load_metadata, nullptr, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    return init_status;
}
static HRESULT copy_string(const std::string &value, SIZE_T size, char *buffer)
{
    if (!buffer) return E_POINTER;
    if (size <= value.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(buffer, value.c_str(), value.size() + 1);
    return S_OK;
}
/* Layout/XVC tag specs use "a#b" when a chunk needs both install tags. A bare
 * Tag selector matches any component; Feature matching treats the first
 * component as the base feature and later ones as optional add-ons (so
 * launch#hd1 belongs to HD1, not Launch). */
static bool split_next(const std::string &spec, size_t &start, std::string &part)
{
    if (start > spec.size()) return false;
    size_t hash = spec.find('#', start);
    size_t end = hash == std::string::npos ? spec.size() : hash;
    part = spec.substr(start, end - start);
    start = hash == std::string::npos ? spec.size() + 1 : hash + 1;
    return true;
}
static bool tag_component_matches(const Json &array, const char *wanted)
{
    if (!wanted) return false;
    for (const auto &value : array.a)
    {
        const std::string &spec = value.str();
        if (!_stricmp(spec.c_str(), wanted)) return true;
        std::string part;
        for (size_t start = 0; split_next(spec, start, part); )
            if (!part.empty() && !_stricmp(part.c_str(), wanted)) return true;
    }
    return false;
}
static bool feature_tag_matches(const Json &array, const char *wanted)
{
    if (!wanted) return false;
    for (const auto &value : array.a)
    {
        const std::string &spec = value.str();
        std::string part;
        size_t start = 0;
        if (!split_next(spec, start, part)) continue;
        if (start > spec.size())
        {
            if (!_stricmp(part.c_str(), wanted)) return true;
            continue;
        }
        /* Compound spec: optional components only. */
        while (split_next(spec, start, part))
            if (!part.empty() && !_stricmp(part.c_str(), wanted)) return true;
    }
    return false;
}
static bool language_allowed(const Json &region)
{
    const Json &languages = region["languages"];
    if (languages.a.empty()) return true;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    char text[LOCALE_NAME_MAX_LENGTH * 4];
    if (!GetUserDefaultLocaleName(locale, ARRAYSIZE(locale))) return true;
    if (!WideCharToMultiByte(CP_UTF8, 0, locale, -1, text, sizeof(text), nullptr, nullptr)) return true;
    std::string full = text, base = full;
    size_t dash = base.find('-');
    if (dash != std::string::npos) base.resize(dash);
    for (const auto &value : languages.a)
    {
        std::string lang = value.str();
        if (!_stricmp(lang.c_str(), full.c_str())) return true;
        size_t lang_dash = lang.find('-');
        std::string lang_base = lang_dash == std::string::npos ? lang : lang.substr(0, lang_dash);
        if (!_stricmp(lang_base.c_str(), base.c_str())) return true;
    }
    return false;
}
static bool matches(const Json &region, const XPackageChunkSelector &selector)
{
    switch (selector.type)
    {
    case XPackageChunkSelectorType::Chunk: return region["id"].n == selector.chunkId;
    case XPackageChunkSelectorType::Tag:
        return language_allowed(region) && tag_component_matches(region["tags"], selector.tag);
    case XPackageChunkSelectorType::Language: return tag_component_matches(region["languages"], selector.language);
    case XPackageChunkSelectorType::Feature:
        if (!selector.feature || !language_allowed(region)) return false;
        for (const auto &feature : metadata["features"].a)
            if (!_stricmp(feature["id"].str().c_str(), selector.feature))
                for (const auto &tag : feature["tags"].a)
                    if (feature_tag_matches(region["tags"], tag.str().c_str())) return true;
        return false;
    }
    return false;
}

struct Package final : IXPackageImpl4
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IXPackageImpl && iid != IID_IXPackageImpl2 &&
            iid != IID_IXPackageImpl3 && iid != IID_IXPackageImpl4) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE XPackageGetCurrentProcessPackageIdentifier(SIZE_T n, char *p) override
    { gdk_log("GDK content: call %s\n", "XPackageGetCurrentProcessPackageIdentifier");
        HRESULT hr = initialize();
        if (SUCCEEDED(hr)) hr = copy_string(package_id, n, p);
        gdk_log("GDK content: GetCurrentProcessPackageIdentifier n=%Iu -> %s hr=%08lx\n", n, package_id.c_str(), hr);
        return hr;
    }
    BOOLEAN STDMETHODCALLTYPE XPackageIsPackagedProcess() override
    { gdk_log("GDK content: call %s\n", "XPackageIsPackagedProcess");
        BOOLEAN ok = SUCCEEDED(initialize());
        gdk_log("GDK content: IsPackagedProcess -> %u\n", (unsigned)ok);
        return ok;
    }
    HRESULT STDMETHODCALLTYPE XPackageCreateInstallationMonitor(const char *, UINT32, const XPackageChunkSelector *, UINT32, XTaskQueueHandle, XPackageInstallationMonitorHandle *out) override
    { gdk_log("GDK content: call %s\n", "XPackageCreateInstallationMonitor"); if (out) *out = nullptr; return E_NOTIMPL; }
    void STDMETHODCALLTYPE XPackageCloseInstallationMonitorHandle(XPackageInstallationMonitorHandle) override { gdk_log("GDK content: call %s\n", "XPackageCloseInstallationMonitorHandle");}
    void STDMETHODCALLTYPE XPackageGetInstallationProgress(XPackageInstallationMonitorHandle, XPackageInstallationProgress *out) override
    { gdk_log("GDK content: call %s\n", "XPackageGetInstallationProgress"); if (out) *out = {}; }
    BOOLEAN STDMETHODCALLTYPE XPackageUpdateInstallationMonitor(XPackageInstallationMonitorHandle) override { gdk_log("GDK content: call %s\n", "XPackageUpdateInstallationMonitor"); return FALSE; }
    HRESULT STDMETHODCALLTYPE XPackageRegisterInstallationProgressChanged(XPackageInstallationMonitorHandle, void *, XPackageInstallationProgressCallback, XTaskQueueRegistrationToken *out) override
    { gdk_log("GDK content: call %s\n", "XPackageRegisterInstallationProgressChanged"); if (out) *out = 0; return E_NOTIMPL; }
    BOOLEAN STDMETHODCALLTYPE XPackageUnregisterInstallationProgressChanged(XPackageInstallationMonitorHandle, XTaskQueueRegistrationToken, BOOLEAN) override { gdk_log("GDK content: call %s\n", "XPackageUnregisterInstallationProgressChanged"); return FALSE; }
    HRESULT STDMETHODCALLTYPE XPackageGetUserLocale(SIZE_T n, char *out) override
    { gdk_log("GDK content: call XPackageGetUserLocale n=%#Ix out=%p ret=%p\n", n, out, __builtin_return_address(0));
        WCHAR locale[LOCALE_NAME_MAX_LENGTH]; char text[LOCALE_NAME_MAX_LENGTH * 4];
        if (!GetUserDefaultLocaleName(locale, ARRAYSIZE(locale))) return HRESULT_FROM_WIN32(GetLastError());
        if (!WideCharToMultiByte(CP_UTF8, 0, locale, -1, text, sizeof(text), nullptr, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        return copy_string(text, n, out);
    }
    HRESULT STDMETHODCALLTYPE XPackageFindChunkAvailability(const char *id, UINT32 count, const XPackageChunkSelector *selectors, XPackageChunkAvailability *out) override
    { gdk_log("GDK content: call %s\n", "XPackageFindChunkAvailability");
        if (!out || (count && !selectors)) return E_POINTER;
        *out = XPackageChunkAvailability::Unavailable;
        HRESULT hr = initialize();
        if (FAILED(hr)) return hr;
        if (!is_this_package(id)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        bool found = false, ready = true;
        for (const auto &region : metadata["regions"].a)
        {
            bool selected = count == 0;
            for (UINT32 i = 0; i < count; ++i) selected |= matches(region, selectors[i]);
            if (selected) { found = true; ready &= region["ready"].b; }
        }
        if (found) *out = ready ? XPackageChunkAvailability::Ready : XPackageChunkAvailability::Installable;
        gdk_log("GDK content: availability selectors=%u", count);
        for (UINT32 i = 0; i < count; ++i)
        {
            const auto &s = selectors[i];
            if (s.type == XPackageChunkSelectorType::Feature)
                gdk_log(" feature=%s", s.feature ? s.feature : "(null)");
            else if (s.type == XPackageChunkSelectorType::Tag)
                gdk_log(" tag=%s", s.tag ? s.tag : "(null)");
            else if (s.type == XPackageChunkSelectorType::Chunk)
                gdk_log(" chunk=%u", s.chunkId);
            else
                gdk_log(" type=%u", (unsigned)s.type);
        }
        gdk_log(" result=%u\n", (unsigned)*out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XPackageEnumerateChunkAvailability(const char *id, XPackageChunkSelectorType type, void *context, XPackageChunkAvailabilityCallback callback) override
    { gdk_log("GDK content: call %s\n", "XPackageEnumerateChunkAvailability");
        gdk_log("GDK content: EnumerateChunkAvailability type=%u\n", (unsigned)type);
        if (!callback) return E_POINTER;
        if (type != XPackageChunkSelectorType::Chunk) return E_NOTIMPL;
        for (const auto &region : metadata["regions"].a)
        {
            XPackageChunkSelector selector{}; selector.type = type; selector.chunkId = (UINT32)region["id"].n;
            XPackageChunkAvailability availability;
            HRESULT hr = XPackageFindChunkAvailability(id, 1, &selector, &availability);
            if (FAILED(hr)) return hr;
            if (!callback(context, &selector, availability)) break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XPackageChangeChunkInstallOrder(const char *, UINT32, const XPackageChunkSelector *) override { gdk_log("GDK content: call %s\n", "XPackageChangeChunkInstallOrder"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageInstallChunks(const char *, UINT32, XPackageChunkSelector *, UINT32, BOOLEAN, XTaskQueueHandle, XPackageInstallationMonitorHandle *out) override
    { gdk_log("GDK content: call %s\n", "XPackageInstallChunks"); if (out) *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageInstallChunksAsync(const char *, UINT32, const XPackageChunkSelector *, UINT32, BOOLEAN, XAsyncBlock *) override { gdk_log("GDK content: call %s\n", "XPackageInstallChunksAsync"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageInstallChunksResult(XAsyncBlock *, XPackageInstallationMonitorHandle *out) override { gdk_log("GDK content: call %s\n", "XPackageInstallChunksResult"); if (out) *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageEstimateDownloadSize(const char *, UINT32, const XPackageChunkSelector *, UINT64 *out, BOOLEAN *confirm) override
    { gdk_log("GDK content: call %s\n", "XPackageEstimateDownloadSize"); if (out) *out = 0; if (confirm) *confirm = FALSE; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageUninstallChunks(const char *, UINT32, const XPackageChunkSelector *) override { gdk_log("GDK content: call %s\n", "XPackageUninstallChunks"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Padding0() override { gdk_log("GDK content: call %s\n", "Padding0"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Padding1() override { gdk_log("GDK content: call %s\n", "Padding1"); return E_NOTIMPL; }
    BOOLEAN STDMETHODCALLTYPE XPackageUnregisterPackageInstalled(XTaskQueueRegistrationToken, BOOLEAN) override { gdk_log("GDK content: call %s\n", "XPackageUnregisterPackageInstalled"); return FALSE; }
    HRESULT STDMETHODCALLTYPE Padding2() override { gdk_log("GDK content: call %s\n", "Padding2"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageGetMountPathSize(XPackageMountHandle, SIZE_T *out) override
    { gdk_log("GDK content: call %s\n", "XPackageGetMountPathSize"); if (out) *out = 0; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageGetMountPath(XPackageMountHandle, SIZE_T, char *) override
    { gdk_log("GDK content: call %s\n", "XPackageGetMountPath"); return E_NOTIMPL; }
    void STDMETHODCALLTYPE XPackageCloseMountHandle(XPackageMountHandle) override { gdk_log("GDK content: call %s\n", "XPackageCloseMountHandle");}
    HRESULT STDMETHODCALLTYPE Padding3() override { gdk_log("GDK content: call %s\n", "Padding3"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageEnumeratePackages(XPackageKind kind, XPackageEnumerationScope scope,
                                                       void *context, XPackageEnumerationCallback callback) override
    { gdk_log("GDK content: call %s\n", "XPackageEnumeratePackages");
        gdk_log("GDK content: EnumeratePackages kind=%u scope=%u\n", (unsigned)kind, (unsigned)scope);
        if (!callback) return E_POINTER;
        HRESULT hr = initialize();
        if (FAILED(hr)) return hr;
        /* Some GDK titles enumerate Content (DLC) at init. Optional HD / feature
         * chunks are Features of the Game package, not separate Content packages —
         * return success with no Content rows. Still report the Game package when
         * kind asks for it so availability / feature probes have an identity. */
        if (kind != XPackageKind::Game && kind != XPackageKind::PublisherContent)
            return S_OK;
        std::string id = package_id;
        if (id.empty()) return S_OK;
        XPackageDetails details{};
        details.packageIdentifier = id.c_str();
        details.version = {};
        details.kind = XPackageKind::Game;
        details.displayName = id.c_str();
        details.description = "";
        details.publisher = "Microsoft Studios";
        details.storeId = "";
        details.installing = FALSE;
        details.index = 0;
        details.count = 1;
        details.ageRestricted = FALSE;
        details.titleID = "";
        (void)scope;
        callback(context, &details);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XPackageRegisterPackageInstalled(XTaskQueueHandle, void *, XPackageInstalledCallback,
                                                              XTaskQueueRegistrationToken *out) override
    { gdk_log("GDK content: call %s\n", "XPackageRegisterPackageInstalled");
        gdk_log("GDK content: RegisterPackageInstalled\n");
        if (out) *out = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE XPackageGetWriteStats(XPackageWriteStats *out) override { gdk_log("GDK content: call %s\n", "XPackageGetWriteStats"); if (out) *out = {}; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Padding4() override { gdk_log("GDK content: call %s\n", "Padding4"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageUninstallUWPInstance(const char *) override { gdk_log("GDK content: call %s\n", "XPackageUninstallUWPInstance"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageEnumerateFeatures(const char *, void *context, XPackageFeatureEnumerationCallback callback) override
    { gdk_log("GDK content: call %s\n", "XPackageEnumerateFeatures");
        gdk_log("GDK content: EnumerateFeatures count=%zu\n", metadata["features"].a.size());
        if (!callback) return E_POINTER;
        for (const auto &feature : metadata["features"].a)
        {
            std::string id = feature["id"].str(), name = feature["name"].str(), tags;
            for (const auto &tag : feature["tags"].a) tags += (tags.empty() ? "" : ";") + tag.str();
            gdk_log("GDK content: feature id=%s tags=%s hidden=%u\n", id.c_str(), tags.c_str(),
                    (unsigned)feature["hidden"].b);
            XPackageFeature value{id.c_str(), name.c_str(), tags.c_str(), (BOOLEAN)feature["hidden"].b, 0, nullptr};
            if (!callback(context, &value)) break;
        }
        return S_OK;
    }
    BOOLEAN STDMETHODCALLTYPE XPackageUninstallPackage(const char *) override { gdk_log("GDK content: call %s\n", "XPackageUninstallPackage"); return FALSE; }
    HRESULT STDMETHODCALLTYPE XPackageEnumeratePackages2(XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback cb) override
    { gdk_log("GDK content: call %s\n", "XPackageEnumeratePackages2"); return XPackageEnumeratePackages(kind, scope, context, cb); }
    HRESULT STDMETHODCALLTYPE XPackageRegisterPackageInstalled2(XTaskQueueHandle queue, void *context, XPackageInstalledCallback cb, XTaskQueueRegistrationToken *token) override
    { gdk_log("GDK content: call %s\n", "XPackageRegisterPackageInstalled2"); return XPackageRegisterPackageInstalled(queue, context, cb, token); }
    HRESULT STDMETHODCALLTYPE XPackageMountWithUiAsync(const char *, XAsyncBlock *) override { gdk_log("GDK content: call %s\n", "XPackageMountWithUiAsync"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageMountWithUiResult(XAsyncBlock *, XPackageMountHandle *out) override { gdk_log("GDK content: call %s\n", "XPackageMountWithUiResult"); if (out) *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE XPackageEnumeratePackages3(XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback cb) override
    { gdk_log("GDK content: call %s\n", "XPackageEnumeratePackages3"); return XPackageEnumeratePackages(kind, scope, context, cb); }
    HRESULT STDMETHODCALLTYPE XPackageRegisterPackageInstalled3(XTaskQueueHandle queue, void *context, XPackageInstalledCallback cb, XTaskQueueRegistrationToken *token) override
    { gdk_log("GDK content: call %s\n", "XPackageRegisterPackageInstalled3"); return XPackageRegisterPackageInstalled(queue, context, cb, token); }
    HRESULT STDMETHODCALLTYPE XPackageGetKind(const char *id, XPackageKind *kind) override
    { gdk_log("GDK content: call %s\n", "XPackageGetKind");
        if (!kind) return E_POINTER;
        *kind = XPackageKind::Game;
        if (!is_this_package(id)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        return S_OK;
    }
};
static Package package;

extern "C" __declspec(dllexport) HRESULT WINAPI InitializeApiImplEx2(ULONG a, ULONG b, char c, const void *)
{
    gdk_log("GDK content: InitializeApiImplEx2(%lu,%lu,%u)\n", a, b, (unsigned)c);
    HRESULT hr = initialize();
    /* Start title SISU/XSTS warm early so Enter→presence GetToken is a cache hit. */
    if (SUCCEEDED(hr)) wineuwp_warm_xboxauth_title_context();
    return hr;
}
extern "C" __declspec(dllexport) HRESULT WINAPI InitializeApiImplEx(ULONG a, ULONG b, char c)
{
    return InitializeApiImplEx2(a, b, c, nullptr);
}
extern "C" __declspec(dllexport) HRESULT WINAPI InitializeApiImpl(ULONG a, ULONG b)
{
    gdk_log("GDK content: InitializeApiImpl(%lu,%lu)\n", a, b);
    return InitializeApiImplEx(a, b, 0);
}
extern "C" __declspec(dllexport) void WINAPI UninitializeApiImpl()
{
    gdk_log("GDK content: UninitializeApiImpl\n");
}
/* GdkSupport GetProcAddress's this during XGameRuntimeInitialize; a NULL
 * export aborts init before InitializeApiImpl / QueryApiImpl ever run. */
extern "C" __declspec(dllexport) HRESULT WINAPI XErrorReport(HRESULT status, const char *message)
{
    gdk_log("GDK content: XErrorReport hr=%08lx msg=%s\n", status, message ? message : "(null)");
    return S_OK;
}
/* GdkSupport QueryApiImpl's every GDK coclass during init. A CLASSNOTREG for
 * XThreading (073b7dcb) or friends becomes 0x89240107 "missing required
 * dependencies" and the title skips honest XPackage availability checks.
 * Until those services are implemented, hand back a wide E_NOTIMPL/FALSE stub. */
static HRESULT STDMETHODCALLTYPE stub_QueryInterface(void *self, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!self) return E_POINTER;
    *out = self;
    (void)iid;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE stub_AddRef(void *) { return 2; }
static ULONG STDMETHODCALLTYPE stub_Release(void *) { return 1; }
static HRESULT STDMETHODCALLTYPE stub_hresult(void *) { return E_NOTIMPL; }
static void STDMETHODCALLTYPE stub_void(void *) {}
static BOOLEAN STDMETHODCALLTYPE stub_boolean(void *) { return FALSE; }

/* IXThreadingImpl alone needs ~30 slots after IUnknown; pad further for other
 * coclasses until each gets a real ABI. Slot kinds alternate conservatively:
 * HRESULT failure is safe for result-bearing methods; BOOLEAN FALSE / void no-ops
 * cover the rest when the title only needed QI to clear missing-deps. */
/* Slot 3 must be BOOLEAN FALSE: Feature (and any other first-method BOOLEAN
 * coclass still on the wide stub) treated E_NOTIMPL as TRUE. */
static void *stub_vtable[] = {
    (void *)&stub_QueryInterface, (void *)&stub_AddRef, (void *)&stub_Release,
    (void *)&stub_boolean, (void *)&stub_hresult, (void *)&stub_void, (void *)&stub_hresult,
    (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_void,
    (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult,
    (void *)&stub_hresult, (void *)&stub_boolean, (void *)&stub_void, (void *)&stub_hresult,
    (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_void, (void *)&stub_hresult,
    (void *)&stub_hresult, (void *)&stub_void, (void *)&stub_hresult, (void *)&stub_void,
    (void *)&stub_boolean, (void *)&stub_void, (void *)&stub_hresult, (void *)&stub_boolean,
    (void *)&stub_void, (void *)&stub_boolean, (void *)&stub_hresult, (void *)&stub_hresult,
    (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult,
    (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult, (void *)&stub_hresult,
};
static void *stub_object = stub_vtable;

extern "C" __declspec(dllexport) HRESULT WINAPI QueryApiImpl(REFCLSID clsid, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    HRESULT hr = initialize();
    if (FAILED(hr))
    {
        gdk_log("GDK query: init failed hr=%08lx\n", hr);
        return hr;
    }
    static DWORD seen_clsid[32];
    static unsigned seen_n;
    bool first = true;
    for (unsigned i = 0; i < seen_n; ++i)
        if (seen_clsid[i] == clsid.Data1) { first = false; break; }
    if (first && seen_n < 32) seen_clsid[seen_n++] = clsid.Data1;
    if (first || clsid == CLSID_XPackageImpl)
        gdk_log("GDK query: %08lx iid %08lx\n", clsid.Data1, iid.Data1);
    if (clsid == CLSID_XPackageImpl) return package.QueryInterface(iid, out);
    if (clsid == CLSID_XUserImpl_) return query_xuser(iid, out);
    if (clsid == CLSID_XStoreImpl_) return query_xstore(iid, out);
    if (clsid == CLSID_XThreadingImpl_) return query_xthreading(iid, out);
    if (clsid == CLSID_XGameImpl_) return query_xgame(iid, out);
    if (clsid == CLSID_XSystemImpl_) return query_xsystem(iid, out);
    if (clsid == CLSID_XGameRuntimeFeatureImpl_) return query_xfeature(iid, out);
    if (clsid == CLSID_XNetworkingImpl_) return query_xnetworking(iid, out);
    /* Accept any IID the coclass asks for; GdkSupport often passes the coclass
     * UUID as the iid as well. Remaining services stay on the wide stub until
     * each gets a real ABI. */
    (void)iid;
    *out = &stub_object;
    return S_OK;
}

/* Flat C export mirrors feature_available() in xuser_xstore.cpp. */
extern "C" __declspec(dllexport) BOOLEAN WINAPI XGameRuntimeIsFeatureAvailable(UINT32 feature)
{
    BOOLEAN avail = FALSE;
    switch (feature)
    {
    case 2:  /* XAsync */
    case 3:  /* XAsyncProvider */
    case 5:  /* XGame */
    case 10: /* XNetworking — real QuerySecurityInformation completes async */
    case 11: /* XPackage */
    case 14: /* XStore */
    case 15: /* XSystem */
    case 16: /* XTaskQueue */
    case 17: /* XThread */
    case 18: /* XUser */
    case 19: /* XError */
        avail = TRUE;
        break;
    default:
        avail = FALSE;
        break;
    }
    gdk_log("GDK feature: IsFeatureAvailable(%u) -> %u\n", feature, (unsigned)avail);
    return avail;
}
extern "C" __declspec(dllexport) HRESULT WINAPI XGameRuntimeInitialize()
{
    gdk_log("GDK content: XGameRuntimeInitialize\n");
    HRESULT hr = initialize();
    if (SUCCEEDED(hr)) wineuwp_warm_xboxauth_title_context();
    return hr;
}
extern "C" __declspec(dllexport) void WINAPI XGameRuntimeUninitialize()
{
    gdk_log("GDK content: XGameRuntimeUninitialize\n");
}

/* CampaignS1 HaloInfinite.exe keeps .text encrypted on disk; soft-skip scans
 * the decrypted image and applies the parent repro20 patches at runtime. */
void wineuwp_start_hd_softskip(void);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        gdk_log("GDK content: xgameruntime loaded\n");
        /* Prefetch chunk metadata so a late/missing InitializeApi still has
         * PackagePath-backed answers if QueryApi runs afterward. */
        initialize();
        wineuwp_start_hd_softskip();
    }
    return TRUE;
}

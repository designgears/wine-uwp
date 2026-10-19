/* Smoke-test xgameruntime's XPackageFindChunkAvailability against a stage's
 * .wine-uwp-chunks.json. Set WINE_UWP_PACKAGE_PATH to the stage directory. */
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../shims/gameruntime/package_abi.h"

typedef HRESULT (WINAPI *InitializeApiImpl_t)(ULONG, ULONG);
typedef HRESULT (WINAPI *QueryApiImpl_t)(REFCLSID, REFIID, void **);

static const char *availability_name(XPackageChunkAvailability value)
{
    switch (value)
    {
    case XPackageChunkAvailability::Ready: return "Ready";
    case XPackageChunkAvailability::Pending: return "Pending";
    case XPackageChunkAvailability::Installable: return "Installable";
    case XPackageChunkAvailability::Unavailable: return "Unavailable";
    }
    return "?";
}

static int check(IXPackageImpl *pkg, const char *label, UINT32 count,
                 const XPackageChunkSelector *selectors, XPackageChunkAvailability expect)
{
    XPackageChunkAvailability got = XPackageChunkAvailability::Unavailable;
    HRESULT hr = pkg->XPackageFindChunkAvailability(nullptr, count, selectors, &got);
    printf("%s -> hr=0x%08lx %s\n", label, hr, availability_name(got));
    if (FAILED(hr) || got != expect) return 1;
    return 0;
}

int main()
{
    HMODULE dll = LoadLibraryW(L"xgameruntime.dll");
    if (!dll) { printf("LoadLibrary xgameruntime.dll failed %lu\n", GetLastError()); return 2; }
    auto init = (InitializeApiImpl_t)GetProcAddress(dll, "InitializeApiImpl");
    auto query = (QueryApiImpl_t)GetProcAddress(dll, "QueryApiImpl");
    if (!init || !query) { printf("missing exports\n"); return 2; }
    HRESULT hr = init(0, 0);
    printf("InitializeApiImpl -> 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;

    void *raw = nullptr;
    hr = query(CLSID_XPackageImpl, IID_IXPackageImpl4, &raw);
    printf("QueryApiImpl IXPackageImpl4 -> 0x%08lx\n", hr);
    if (FAILED(hr))
    {
        hr = query(CLSID_XPackageImpl, IID_IXPackageImpl, &raw);
        printf("QueryApiImpl IXPackageImpl -> 0x%08lx\n", hr);
    }
    if (FAILED(hr) || !raw) return 1;

    auto *pkg = (IXPackageImpl *)raw;
    int failures = 0;

    /* Feature Launch stays Ready when only optional HD/language chunks are missing.
     * Feature HD1 and the HD module chunk must be Installable; an installed base chunk Ready. */
    XPackageChunkSelector launch{};
    launch.type = XPackageChunkSelectorType::Feature;
    launch.feature = "Launch";
    failures += check(pkg, "feature Launch", 1, &launch, XPackageChunkAvailability::Ready);

    XPackageChunkSelector hd{};
    hd.type = XPackageChunkSelectorType::Feature;
    hd.feature = "HD1";
    failures += check(pkg, "feature HD1", 1, &hd, XPackageChunkAvailability::Installable);

    XPackageChunkSelector chunk1200{};
    chunk1200.type = XPackageChunkSelectorType::Chunk;
    chunk1200.chunkId = 1200;
    failures += check(pkg, "chunk 1200", 1, &chunk1200, XPackageChunkAvailability::Installable);

    XPackageChunkSelector chunk1000{};
    chunk1000.type = XPackageChunkSelectorType::Chunk;
    chunk1000.chunkId = 1000;
    failures += check(pkg, "chunk 1000", 1, &chunk1000, XPackageChunkAvailability::Ready);

    pkg->Release();
    return failures ? 1 : 0;
}

// Ask MRT (Windows.ApplicationModel.Resources.Core) what it can resolve for the current package
// graph, independent of XAML: which resource maps exist and whether framework-package resources
// (WinUI theme XBF, embedded in the framework's resources.pri) are reachable.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>
#include <windows.applicationmodel.resources.core.h>
#include <windows.applicationmodel.h>
#include <windows.storage.h>
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <stdio.h>

using namespace ABI::Windows::ApplicationModel::Resources::Core;
using namespace ABI::Windows::Foundation::Collections;
using Microsoft::WRL::ComPtr;

static const wchar_t *str(HSTRING h) { UINT32 n = 0; const wchar_t *p = WindowsGetStringRawBuffer(h, &n); return p ? p : L""; }

struct HString {
    HSTRING h = nullptr;
    HString() = default;
    explicit HString(const wchar_t *s) { WindowsCreateString(s, (UINT32)wcslen(s), &h); }
    ~HString() { if (h) WindowsDeleteString(h); }
    operator HSTRING() const { return h; }
};

static void try_lookup(IResourceMap *map, const wchar_t *name)
{
    HString key(name);
    ComPtr<IResourceCandidate> cand;
    HRESULT hr = map->GetValue(key, &cand);
    if (FAILED(hr) || !cand) { wprintf(L"  lookup %-58ls -> hr %#08x\n", name, (unsigned)hr); return; }
    HSTRING val = nullptr;
    HRESULT hr2 = cand->get_ValueAsString(&val);
    wprintf(L"  lookup %-58ls -> OK value(hr %#08x)=%ls\n", name, (unsigned)hr2, val ? str(val) : L"");
    if (val) WindowsDeleteString(val);
}

int main()
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    wprintf(L"RoInitialize hr %#08x\n", (unsigned)hr);

    HMODULE private_module = nullptr;
    // Exercise the private MRT entry points used by Windows.UI.Xaml (pinned build).
    {
        CLSID clsid; IID manager_iid, init_iid, map_iid;
        CLSIDFromString(L"{dbce7e40-7345-439d-b12c-114a11819a09}", &clsid);
        IIDFromString(L"{130a2f65-2be7-4309-9a58-a9052ff2b61c}", &manager_iid);
        IIDFromString(L"{439dd7c9-0eeb-4715-baa7-f0877694e616}", &init_iid);
        IIDFromString(L"{6e21e72b-b9b0-42ae-a686-983cf784edcd}", &map_iid);
        ComPtr<IUnknown> manager, init, map;
        HRESULT result = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, manager_iid, &manager);
        printf("Private MRT create %#x\n", (unsigned)result);
        if (manager && SUCCEEDED(manager->QueryInterface(init_iid, &init))) {
            auto vt = *reinterpret_cast<void ***>(init.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *)>(vt[4])(init.Get());
            printf("Private MRT initialize %#x\n", (unsigned)result);
            if (FAILED(result)) return 2;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>((*reinterpret_cast<void ***>(manager.Get()))[0]), &private_module)) return 2;
            vt = *reinterpret_cast<void ***>(manager.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *, const wchar_t *, REFIID, void **)>(vt[8])(
                manager.Get(), L"Microsoft.UI.Xaml.2.8", map_iid, reinterpret_cast<void **>(map.GetAddressOf()));
            printf("Private MRT framework map %#x\n", (unsigned)result);
            if (FAILED(result)) return 2;
            ComPtr<IUnknown> files, resource;
            vt = *reinterpret_cast<void ***>(map.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *, const wchar_t *, void **)>(vt[4])(
                map.Get(), L"Files", reinterpret_cast<void **>(files.GetAddressOf()));
            printf("Private MRT Files subtree %#x\n", (unsigned)result);
            if (FAILED(result)) return 2;
            IID resource_iid;
            IIDFromString(L"{c2ac6f97-54f2-445a-8f62-d87b9537f9b2}", &resource_iid);
            vt = *reinterpret_cast<void ***>(files.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *, const wchar_t *, REFIID, void **)>(vt[11])(
                files.Get(), L"Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf", resource_iid,
                reinterpret_cast<void **>(resource.GetAddressOf()));
            printf("Private MRT theme resource %#x\n", (unsigned)result);
            if (FAILED(result)) return 2;
            IID context_iid;
            IIDFromString(L"{e3c22b30-8502-4b2f-9133-559674587e51}", &context_iid);
            ComPtr<IUnknown> default_context, context, candidate;
            vt = *reinterpret_cast<void ***>(manager.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *, REFIID, void **)>(vt[9])(
                manager.Get(), context_iid, reinterpret_cast<void **>(default_context.GetAddressOf()));
            printf("Private MRT context %#x\n", (unsigned)result);
            if (FAILED(result)) return 2;
            vt = *reinterpret_cast<void ***>(default_context.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *, void **)>(vt[22])(
                default_context.Get(), reinterpret_cast<void **>(context.GetAddressOf()));
            printf("Private MRT clone context %#x\n", (unsigned)result);
            if (FAILED(result)) return 2;
            vt = *reinterpret_cast<void ***>(resource.Get());
            result = reinterpret_cast<HRESULT (WINAPI *)(IUnknown *, IUnknown *, void **)>(vt[7])(
                resource.Get(), context.Get(), reinterpret_cast<void **>(candidate.GetAddressOf()));
            printf("Private MRT resolve theme %#x candidate=%p\n", (unsigned)result, candidate.Get());
            if (FAILED(result) || !candidate) return 2;
        } else return 2;
    }

    ComPtr<IResourceManagerStatics> statics;
    HString cls(RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceManager);
    hr = RoGetActivationFactory(cls, IID_PPV_ARGS(&statics));
    wprintf(L"ResourceManager factory hr %#08x\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    HMODULE winrt_module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>((*reinterpret_cast<void ***>(statics.Get()))[0]), &winrt_module)) return 2;
    if (private_module != winrt_module) {
        fprintf(stderr, "FAIL: private MRT and WinRT loaded different module copies\n");
        return 2;
    }
    printf("PASS: private MRT and WinRT share one module\n");

    ComPtr<IResourceManager> mgr;
    hr = statics->get_Current(&mgr);
    wprintf(L"get_Current hr %#08x\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    ComPtr<IResourceMap> main_map;
    hr = mgr->get_MainResourceMap(&main_map);
    wprintf(L"get_MainResourceMap hr %#08x\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    ComPtr<IMapView<HSTRING, NamedResource *>> view;
    if (SUCCEEDED(main_map.As(&view))) {
        unsigned size = 0;
        view->get_Size(&size);
        wprintf(L"MainResourceMap has %u entries\n", size);
        ComPtr<IIterable<IKeyValuePair<HSTRING, NamedResource *> *>> iterable;
        ComPtr<IIterator<IKeyValuePair<HSTRING, NamedResource *> *>> it;
        if (SUCCEEDED(view.As(&iterable)) && SUCCEEDED(iterable->First(&it))) {
            boolean has = false; it->get_HasCurrent(&has);
            for (int i = 0; has && i < 40; i++) {
                ComPtr<IKeyValuePair<HSTRING, NamedResource *>> kv;
                if (FAILED(it->get_Current(&kv)) || !kv) break;
                HSTRING k = nullptr;
                if (SUCCEEDED(kv->get_Key(&k))) { wprintf(L"    [%d] %ls\n", i, str(k)); WindowsDeleteString(k); }
                it->MoveNext(&has);
            }
        }
    }

    ComPtr<IMapView<HSTRING, ResourceMap *>> all;
    if (SUCCEEDED(mgr->get_AllResourceMaps(&all)) && all) {
        unsigned n = 0; all->get_Size(&n);
        wprintf(L"AllResourceMaps has %u maps\n", n);
        ComPtr<IIterable<IKeyValuePair<HSTRING, ResourceMap *> *>> itb;
        ComPtr<IIterator<IKeyValuePair<HSTRING, ResourceMap *> *>> it;
        if (SUCCEEDED(all.As(&itb)) && SUCCEEDED(itb->First(&it))) {
            boolean has = false; it->get_HasCurrent(&has);
            while (has) {
                ComPtr<IKeyValuePair<HSTRING, ResourceMap *>> kv;
                if (FAILED(it->get_Current(&kv)) || !kv) break;
                HSTRING k = nullptr; kv->get_Key(&k);
                ComPtr<IResourceMap> m;
                kv->get_Value(&m);
                unsigned cnt = 0;
                ComPtr<IMapView<HSTRING, NamedResource *>> mv;
                if (m && SUCCEEDED(m.As(&mv))) mv->get_Size(&cnt);
                wprintf(L"    map %ls (%u entries)\n", str(k), cnt);
                if (m) try_lookup(m.Get(), L"Files/Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf");
                if (k) WindowsDeleteString(k);
                it->MoveNext(&has);
            }
        }
    } else wprintf(L"AllResourceMaps unavailable\n");

    static const wchar_t *names[] = {
        L"Files/Microsoft.UI.Xaml/Themes/generic.xaml",
        L"Files/Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf",
        L"Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf",
        L"Files/App.xbf",
        L"Files/AppxManifest.xml",
    };
    for (auto n : names) try_lookup(main_map.Get(), n);

    // Package.Current.Dependencies: the package graph as WinRT exposes it, which is what WinUI's
    // FrameworkPackageDetector reads to decide whether it is deployed as a framework package.
    {
        using namespace ABI::Windows::ApplicationModel;
        ComPtr<IPackageStatics> pkg_statics;
        HString pcls(RuntimeClass_Windows_ApplicationModel_Package);
        HRESULT phr = RoGetActivationFactory(pcls, IID_PPV_ARGS(&pkg_statics));
        wprintf(L"Package statics hr %#08x\n", (unsigned)phr);
        ComPtr<IPackage> cur;
        if (SUCCEEDED(phr)) { phr = pkg_statics->get_Current(&cur); wprintf(L"Package.Current hr %#08x\n", (unsigned)phr); }
        if (cur) {
            ComPtr<IPackageId> id;
            if (SUCCEEDED(cur->get_Id(&id)) && id) {
                HSTRING n1 = nullptr, n2 = nullptr;
                id->get_Name(&n1); id->get_FamilyName(&n2);
                wprintf(L"  current package name=%ls family=%ls\n", str(n1), str(n2));
                if (n1) WindowsDeleteString(n1);
                if (n2) WindowsDeleteString(n2);
            }
            // The IPackage2..4 getters, the first thing a page binding to Package.Current reads.
            ComPtr<IPackage2> p2;
            if (SUCCEEDED(cur.As(&p2))) {
                HSTRING v = nullptr; HRESULT h;
                h = p2->get_DisplayName(&v); wprintf(L"  IPackage2.DisplayName hr %#08x %ls\n", (unsigned)h, v ? str(v) : L""); if (v) WindowsDeleteString(v), v = nullptr;
                h = p2->get_PublisherDisplayName(&v); wprintf(L"  IPackage2.PublisherDisplayName hr %#08x %ls\n", (unsigned)h, v ? str(v) : L""); if (v) WindowsDeleteString(v), v = nullptr;
                h = p2->get_Description(&v); wprintf(L"  IPackage2.Description hr %#08x %ls\n", (unsigned)h, v ? str(v) : L""); if (v) WindowsDeleteString(v), v = nullptr;
                boolean b = 0;
                h = p2->get_IsResourcePackage(&b); wprintf(L"  IPackage2.IsResourcePackage hr %#08x %d\n", (unsigned)h, b);
                h = p2->get_IsBundle(&b); wprintf(L"  IPackage2.IsBundle hr %#08x %d\n", (unsigned)h, b);
                h = p2->get_IsDevelopmentMode(&b); wprintf(L"  IPackage2.IsDevelopmentMode hr %#08x %d\n", (unsigned)h, b);
            } else wprintf(L"  no IPackage2\n");
            ComPtr<IPackage3> p3;
            if (SUCCEEDED(cur.As(&p3))) {
                ABI::Windows::Foundation::DateTime t{}; HRESULT h = p3->get_InstalledDate(&t);
                wprintf(L"  IPackage3.InstalledDate hr %#08x %lld\n", (unsigned)h, (long long)t.UniversalTime);
                ComPtr<IPackageStatus> st; h = p3->get_Status(&st); wprintf(L"  IPackage3.Status hr %#08x\n", (unsigned)h);
            } else wprintf(L"  no IPackage3\n");
            ComPtr<IPackage4> p4;
            if (SUCCEEDED(cur.As(&p4))) {
                PackageSignatureKind k{}; HRESULT h = p4->get_SignatureKind(&k);
                wprintf(L"  IPackage4.SignatureKind hr %#08x %d\n", (unsigned)h, (int)k);
                boolean b = 0; h = p4->get_IsOptional(&b); wprintf(L"  IPackage4.IsOptional hr %#08x %d\n", (unsigned)h, b);
            } else wprintf(L"  no IPackage4\n");
            ComPtr<IPackageId> id2;
            if (SUCCEEDED(cur->get_Id(&id2)) && id2) {
                PackageVersion ver{}; HRESULT h = id2->get_Version(&ver);
                wprintf(L"  IPackageId.Version hr %#08x %u.%u.%u.%u\n", (unsigned)h, ver.Major, ver.Minor, ver.Build, ver.Revision);
            }
            ComPtr<IVectorView<Package *>> deps;
            HRESULT dhr = cur->get_Dependencies(&deps);
            unsigned dn = 0;
            if (SUCCEEDED(dhr) && deps) deps->get_Size(&dn);
            wprintf(L"  Dependencies hr %#08x count %u\n", (unsigned)dhr, dn);
            for (unsigned i = 0; i < dn; i++) {
                ComPtr<IPackage> d;
                if (FAILED(deps->GetAt(i, &d)) || !d) continue;
                ComPtr<IPackageId> did;
                HSTRING dn1 = nullptr;
                if (SUCCEEDED(d->get_Id(&did)) && did) did->get_Name(&dn1);
                ComPtr<ABI::Windows::Storage::IStorageFolder> loc;
                HSTRING path = nullptr;
                if (SUCCEEDED(d->get_InstalledLocation(&loc)) && loc) {
                    ComPtr<ABI::Windows::Storage::IStorageItem> item;
                    if (SUCCEEDED(loc.As(&item)) && item) item->get_Path(&path);
                }
                wprintf(L"    dep[%u] %ls path=%ls\n", i, dn1 ? str(dn1) : L"?", path ? str(path) : L"");
                if (dn1) WindowsDeleteString(dn1);
                if (path) WindowsDeleteString(path);
            }
        }
    }
    // ms-appx://<package>/<path>: XAML resolves the authority to a package resource map, so the
    // authority has to survive parsing.
    {
        using namespace ABI::Windows::Foundation;
        ComPtr<IUriRuntimeClassFactory> uri_factory;
        HString ucls(RuntimeClass_Windows_Foundation_Uri);
        HRESULT uhr = RoGetActivationFactory(ucls, IID_PPV_ARGS(&uri_factory));
        wprintf(L"Uri factory hr %#08x\n", (unsigned)uhr);
        static const wchar_t *uris[] = {
            L"ms-appx://Microsoft.UI.Xaml.2.8/Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf",
            L"ms-appx:///App.xbf",
        };
        for (auto u : uris) {
            HString raw(u);
            ComPtr<IUriRuntimeClass> uri;
            HRESULT hr2 = uri_factory ? uri_factory->CreateUri(raw, &uri) : E_FAIL;
            if (FAILED(hr2) || !uri) { wprintf(L"  CreateUri(%ls) hr %#08x\n", u, (unsigned)hr2); continue; }
            HSTRING scheme = nullptr, host = nullptr, path = nullptr, abs = nullptr;
            uri->get_SchemeName(&scheme); uri->get_Host(&host); uri->get_Path(&path); uri->get_AbsoluteUri(&abs);
            wprintf(L"  %ls\n    scheme=%ls host=[%ls] path=%ls abs=%ls\n", u, str(scheme), str(host), str(path), str(abs));
            if (scheme) WindowsDeleteString(scheme);
            if (host) WindowsDeleteString(host);
            if (path) WindowsDeleteString(path);
            if (abs) WindowsDeleteString(abs);
        }
    }
    // API sets added for the package/resource stack
    {
        static const wchar_t *sets[][2] = {
            { L"ext-ms-win-resources-deployment-l1-1-0.dll", L"GetInitInfoByPackageFullName" },
            { L"ext-ms-onecore-appmodel-staterepository-cache-l1-1-0.dll", L"SRCacheManager_Open" },
            { L"ext-ms-win-appmodel-state-ext-l1-2-0.dll", L"OpenState" },
        };
        for (auto &e : sets) {
            HMODULE m = LoadLibraryW(e[0]);
            char narrow[128]; int i = 0;
            for (; e[1][i] && i < 127; i++) narrow[i] = (char)e[1][i];
            narrow[i] = 0;
            void *fn = m ? (void *)GetProcAddress(m, narrow) : nullptr;
            wprintf(L"  apiset %-52ls module=%p %ls=%p\n", e[0], (void *)m, e[1], fn);
        }
    }
    return 0;
}

// Regression probe, adapted from wine-uwp/probes/probe-play.cpp.
// ABI checked against Microsoft's Windows metadata projection at:
// https://github.com/microsoft/windows-rs/blob/1a68bf39ce628b10adc5cee5754a47061eaba8db/crates/libs/windows/src/Windows/Media/Playback/mod.rs
// IMediaPlayer Play/Pause/SetUriSource slots are 45/46/47; Source2 get/set are 6/7.
// Build: x86_64-w64-mingw32-g++ -std=c++11 -municode -static-libgcc -static-libstdc++
//        probe-mediaplayer-empty-play.cpp -o probe-mediaplayer-empty-play.exe -lruntimeobject -lole32
// Run under an external timeout too: a synchronous DLL call can block or crash.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cwchar>

static const GUID iid_player =
    {0x381a83cb,0x6fff,0x499b,{0x8d,0x64,0x28,0x85,0xdf,0xc1,0x24,0x9e}};
static const GUID iid_source2 =
    {0x82449b9f,0x7322,0x4c0b,{0xb0,0x3b,0x3e,0x69,0xa4,0x82,0x60,0xc5}};
static const GUID iid_playback_source =
    {0xef9dc2bc,0x9317,0x4696,{0xb0,0x51,0x2b,0xad,0x64,0x31,0x77,0xb5}};
static const GUID iid_uri_factory =
    {0x44a9796f,0x723e,0x4fdf,{0xa2,0x18,0x03,0x3e,0x75,0xb0,0xc0,0x84}};
static const GUID iid_closable =
    {0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};
using NoArgs = HRESULT (WINAPI *)(IUnknown *);
using OneObject = HRESULT (WINAPI *)(IUnknown *, IUnknown *);
using GetSourceMethod = HRESULT (WINAPI *)(IUnknown *, IUnknown **);
using CreateUriMethod = HRESULT (WINAPI *)(IUnknown *, HSTRING, IUnknown **);
using GetFactory = HRESULT (WINAPI *)(HSTRING, IActivationFactory **);

template<typename F> static F method(IUnknown *object, unsigned slot)
{
    return reinterpret_cast<F>((*reinterpret_cast<void ***>(object))[slot]);
}
static unsigned failures;
static void expect_ok(const wchar_t *label, HRESULT hr)
{
    wprintf(L"%ls %ls: hr=0x%08lx expected S_OK\n", hr == S_OK ? L"PASS" : L"FAIL", label, static_cast<unsigned long>(hr));
    if (hr != S_OK) ++failures;
}
static void report(const wchar_t *label, HRESULT hr, const void *object = nullptr)
{
    wprintf(L"%ls: hr=0x%08lx object=%p\n", label, static_cast<unsigned long>(hr), object);
}
static void module_for(const wchar_t *label, IUnknown *object, unsigned slot)
{
    HMODULE module = nullptr;
    wchar_t path[32768] = {};
    void *address = (*reinterpret_cast<void ***>(object))[slot];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, path, ARRAYSIZE(path));
    wprintf(L"%ls: object=%p slot=%u address=%p module=%ls\n", label, object, slot, address, path);
}
static HRESULT activate(const wchar_t *name, GetFactory direct, IInspectable **out)
{
    HSTRING class_name = nullptr;
    IActivationFactory *factory = nullptr;
    HRESULT hr = WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &class_name);
    report(L"WindowsCreateString(class)", hr, class_name);
    if (FAILED(hr)) return hr;
    wprintf(L"CALL activate %ls via %ls\n", name, direct ? L"explicit DLL" : L"registered class");
    if (direct)
    {
        hr = direct(class_name, &factory);
        report(L"DllGetActivationFactory", hr, factory);
        if (SUCCEEDED(hr) && factory)
        {
            hr = factory->ActivateInstance(out);
            factory->Release();
        }
    }
    else hr = RoActivateInstance(class_name, out);
    WindowsDeleteString(class_name);
    report(L"ActivateInstance", hr, *out);
    return hr;
}
static void inspect_source(IUnknown *source2)
{
    IUnknown *value = nullptr;
    HRESULT hr = method<GetSourceMethod>(source2, 6)(source2, &value);
    report(L"Source.get", hr, value);
    if (value) value->Release();
}
static bool run_case(unsigned kind, const wchar_t *file_uri, GetFactory direct)
{
    const wchar_t *names[] = {L"new player, no source", L"Source=NULL", L"empty MediaPlaybackList", L"valid file URI"};
    IInspectable *instance = nullptr, *list = nullptr;
    IUnknown *player = nullptr, *source2 = nullptr, *source = nullptr, *closable = nullptr;
    HRESULT hr;
    bool setup = false;
    wprintf(L"\nCASE %u: %ls\n", kind, names[kind]);
    hr = activate(L"Windows.Media.Playback.MediaPlayer", direct, &instance);
    if (FAILED(hr) || !instance) goto done;
    hr = instance->QueryInterface(iid_player, reinterpret_cast<void **>(&player));
    report(L"QI IMediaPlayer", hr, player);
    if (FAILED(hr) || !player) goto done;
    module_for(L"player implementation", player, 45);
    hr = player->QueryInterface(iid_source2, reinterpret_cast<void **>(&source2));
    report(L"QI IMediaPlayerSource2", hr, source2);
    if (source2) inspect_source(source2);
    if (kind == 1 || kind == 2)
    {
        if (!source2) goto done;
        if (kind == 2)
        {
            // List activation uses its registered class. Check its printed module when comparing native runs.
            hr = activate(L"Windows.Media.Playback.MediaPlaybackList", nullptr, &list);
            if (FAILED(hr) || !list) goto done;
            hr = list->QueryInterface(iid_playback_source, reinterpret_cast<void **>(&source));
            report(L"QI list IMediaPlaybackSource", hr, source);
            if (FAILED(hr) || !source) goto done;
            module_for(L"list implementation", source, 0);
        }
        wprintf(L"CALL Source.put value=%p\n", source);
        hr = method<OneObject>(source2, 7)(source2, source);
        report(L"Source.put", hr, source2);
        inspect_source(source2);
        if (FAILED(hr)) goto done;
    }
    if (kind == 3)
    {
        HSTRING class_name = nullptr, uri_text = nullptr;
        IUnknown *factory = nullptr, *uri = nullptr;
        hr = WindowsCreateString(L"Windows.Foundation.Uri", 22, &class_name);
        report(L"WindowsCreateString(Uri class)", hr, class_name);
        if (SUCCEEDED(hr)) hr = WindowsCreateString(file_uri, static_cast<UINT32>(wcslen(file_uri)), &uri_text);
        report(L"WindowsCreateString(file URI)", hr, uri_text);
        if (SUCCEEDED(hr)) hr = RoGetActivationFactory(class_name, iid_uri_factory, reinterpret_cast<void **>(&factory));
        report(L"RoGetActivationFactory(Uri)", hr, factory);
        if (SUCCEEDED(hr) && factory) hr = method<CreateUriMethod>(factory, 6)(factory, uri_text, &uri);
        report(L"CreateUriMethod", hr, uri);
        if (SUCCEEDED(hr) && uri)
        {
            wprintf(L"CALL SetUriSource uri=%p\n", uri);
            hr = method<OneObject>(player, 47)(player, uri);
            report(L"SetUriSource", hr, player);
        }
        else if (SUCCEEDED(hr)) hr = E_FAIL;
        if (uri) uri->Release();
        if (factory) factory->Release();
        WindowsDeleteString(uri_text);
        WindowsDeleteString(class_name);
        if (FAILED(hr)) goto done;
    }
    setup = true;
    wprintf(L"CALL Play player=%p\n", player);
    expect_ok(L"Play immediate", method<NoArgs>(player, 45)(player));
    expect_ok(L"Pause after Play", method<NoArgs>(player, 46)(player));
    expect_ok(L"Play after Pause", method<NoArgs>(player, 45)(player));
    expect_ok(L"Play repeated", method<NoArgs>(player, 45)(player));
    Sleep(kind == 2 ? 3250 : 250);
    if (kind == 2)
    {
        inspect_source(source2);
        wprintf(L"CALL Play after 3250ms player=%p\n", player);
        expect_ok(L"Play after 3250ms", method<NoArgs>(player, 45)(player));
    }
done:
    if (player)
    {
        expect_ok(L"Pause cleanup", method<NoArgs>(player, 46)(player));
        hr = player->QueryInterface(iid_closable, reinterpret_cast<void **>(&closable));
        report(L"QI IClosable", hr, closable);
        if (closable)
        {
            report(L"Close cleanup", method<NoArgs>(closable, 6)(closable), closable);
            closable->Release();
        }
    }
    if (source) source->Release();
    if (list) list->Release();
    if (source2) source2->Release();
    if (player) player->Release();
    if (instance) instance->Release();
    wprintf(L"CASE %u setup=%ls; objects released\n", kind, setup ? L"complete" : L"incomplete");
    return setup;
}
int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const wchar_t *dll_path = nullptr, *file_uri = nullptr;
    HMODULE module = nullptr;
    GetFactory direct = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--factory-dll") && i + 1 < argc) dll_path = argv[++i];
        else if (!file_uri && wcsncmp(argv[i], L"--", 2)) file_uri = argv[i];
        else { wprintf(L"usage: probe [--factory-dll DLL_PATH] [file:///C:/path/test.wav]\n"); return 2; }
    }
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    report(L"RoInitialize MTA", hr);
    if (FAILED(hr)) return 1;
    if (dll_path)
    {
        wprintf(L"CALL LoadLibraryExW %ls\n", dll_path);
        module = LoadLibraryExW(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module) direct = reinterpret_cast<GetFactory>(GetProcAddress(module, "DllGetActivationFactory"));
        report(L"Load explicit factory", direct ? S_OK : HRESULT_FROM_WIN32(GetLastError()), module);
        if (!direct) { if (module) FreeLibrary(module); RoUninitialize(); return 1; }
    }
    unsigned complete = 0;
    for (unsigned kind = 0; kind < (file_uri ? 4u : 3u); ++kind)
        if (run_case(kind, file_uri, direct)) ++complete;
    wprintf(L"Completed %u case setups, %u failed assertions.\n", complete, failures);
    RoUninitialize();
    // Keep the explicit module loaded until process exit, including any pending native work.
    return complete == (file_uri ? 4u : 3u) && !failures ? 0 : 1;
}

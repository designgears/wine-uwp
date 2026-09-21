// probe-play: activate Windows.Media.Playback.MediaPlayer, point it at a file and Play().
// This is the path a UWP app takes to produce sound, with none of the app's UI in the way.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <cstdio>

static const GUID IID_IMediaPlayer_ =
    {0x381a83cb,0x6fff,0x499b,{0x8d,0x64,0x28,0x85,0xdf,0xc1,0x24,0x9e}};
static const GUID IID_IUriRuntimeClassFactory_ =
    {0x44a9796f,0x723e,0x4fdf,{0xa2,0x18,0x03,0x3e,0x75,0xb0,0xc0,0x84}};

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { wprintf(L"usage: probe-play <uri-or-path> [seconds]\n"); return 2; }
    DWORD seconds = argc > 2 ? _wtoi(argv[2]) : 10;

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    wprintf(L"RoInitialize: %#lx\n", hr);

    // Windows.Foundation.Uri, which is how MediaPlayer takes a source without MediaSource
    HSTRING uri_class = nullptr, uri_str = nullptr;
    static const wchar_t *uri_name = L"Windows.Foundation.Uri";
    WindowsCreateString(uri_name, (UINT32)wcslen(uri_name), &uri_class);
    WindowsCreateString(argv[1], (UINT32)wcslen(argv[1]), &uri_str);

    IUnknown *uri_factory = nullptr;
    hr = RoGetActivationFactory(uri_class, IID_IUriRuntimeClassFactory_, (void **)&uri_factory);
    wprintf(L"Uri factory: %#lx\n", hr);
    if (FAILED(hr)) return 1;

    // IUriRuntimeClassFactory::CreateUri is slot 6
    IUnknown *uri = nullptr;
    typedef HRESULT (WINAPI *create_uri)(IUnknown *, HSTRING, IUnknown **);
    hr = ((create_uri *)(*(void ***)uri_factory))[6](uri_factory, uri_str, &uri);
    wprintf(L"CreateUri(%ls): %#lx\n", argv[1], hr);
    if (FAILED(hr)) return 1;

    HSTRING player_class = nullptr;
    static const wchar_t *player_name = L"Windows.Media.Playback.MediaPlayer";
    WindowsCreateString(player_name, (UINT32)wcslen(player_name), &player_class);
    IInspectable *player_insp = nullptr;
    hr = RoActivateInstance(player_class, &player_insp);
    wprintf(L"RoActivateInstance(MediaPlayer): %#lx\n", hr);
    if (FAILED(hr)) return 1;

    IUnknown *player = nullptr;
    hr = player_insp->QueryInterface(IID_IMediaPlayer_, (void **)&player);
    wprintf(L"QI IMediaPlayer: %#lx\n", hr);
    if (FAILED(hr)) return 1;

    // IMediaPlayer's slots, counted off media_player_vtbl in
    // dlls/windows.media.playback.mediaplayer/main.c: 0-5 are IUnknown/IInspectable, 6 onwards are
    // the properties and the event add/remove pairs, and Play/Pause/SetUriSource are the last
    // three of the interface.
    void **vtbl = *(void ***)player;
    typedef HRESULT (WINAPI *no_args)(IUnknown *);
    typedef HRESULT (WINAPI *one_ptr)(IUnknown *, IUnknown *);

    const unsigned play_slot = 45, seturi_slot = 47;
    hr = ((one_ptr *)vtbl)[seturi_slot](player, uri);
    wprintf(L"SetUriSource: %#lx\n", hr);
    hr = ((no_args *)vtbl)[play_slot](player);
    wprintf(L"Play: %#lx\n", hr);

    wprintf(L"playing for %lu s...\n", seconds);
    Sleep(seconds * 1000);
    wprintf(L"done\n");
    return 0;
}

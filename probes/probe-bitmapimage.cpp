// Exercise the same file-backed BitmapImage path Xbox's FileImageElement reports failing.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <cstdio>

static const GUID IID_IStorageFileStatics_ = {0x5984c710,0xdaf2,0x43c8,{0x8b,0xb4,0xa4,0xd3,0xea,0xcf,0xd0,0x3f}};
static const GUID IID_IStorageFile_ = {0xfa3f6186,0x4214,0x428c,{0xa6,0x4c,0x14,0xc9,0xac,0x73,0x15,0xea}};
static const GUID IID_IRandomAccessStream_ = {0x905a0fe1,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IBitmapSource_ = {0x23d86411,0x202f,0x41b2,{0x8c,0x5b,0xa8,0xa3,0xb3,0x33,0x80,0x0b}};
static const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static HRESULT get_file(const wchar_t *path, IUnknown **file)
{
    HSTRING cls = nullptr, hpath = nullptr;
    IUnknown *statics = nullptr, *operation = nullptr;
    HRESULT hr;
    *file = nullptr;
    if (FAILED(hr = WindowsCreateString(L"Windows.Storage.StorageFile", 27, &cls))) return hr;
    hr = RoGetActivationFactory(cls, IID_IStorageFileStatics_, (void **)&statics);
    WindowsDeleteString(cls);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = WindowsCreateString(path, (UINT32)wcslen(path), &hpath))) { statics->Release(); return hr; }
    using FromPath = HRESULT (WINAPI *)(IUnknown *, HSTRING, IUnknown **);
    hr = ((FromPath *)(*(void ***)statics))[6](statics, hpath, &operation);
    WindowsDeleteString(hpath);
    statics->Release();
    if (FAILED(hr)) return hr;
    using GetResults = HRESULT (WINAPI *)(IUnknown *, IUnknown **);
    hr = ((GetResults *)(*(void ***)operation))[8](operation, file);
    operation->Release();
    return hr;
}

static HRESULT wait_action(IUnknown *action)
{
    IUnknown *info = nullptr;
    HRESULT hr = action->QueryInterface(IID_IAsyncInfo_, (void **)&info);
    if (FAILED(hr)) return hr;
    using GetStatus = HRESULT (WINAPI *)(IUnknown *, int *);
    using GetResults = HRESULT (WINAPI *)(IUnknown *);
    auto get_status = ((GetStatus *)(*(void ***)info))[7];
    auto get_results = ((GetResults *)(*(void ***)action))[8];
    int status = 0;
    for (unsigned i = 0; i < 1000; ++i)
    {
        hr = get_status(info, &status);
        if (FAILED(hr)) break;
        if (status == 1) { hr = get_results(action); break; }
        if (status == 2) { hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
        if (status == 3)
        {
            using GetError = HRESULT (WINAPI *)(IUnknown *, HRESULT *);
            HRESULT error = E_FAIL;
            ((GetError *)(*(void ***)info))[8](info, &error);
            hr = error;
            break;
        }
        Sleep(10);
    }
    if (status == 0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    info->Release();
    return hr;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) { fwprintf(stderr, L"usage: probe-bitmapimage <image-file>\n"); return 2; }
    HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
    wprintf(L"RoInitialize -> %#lx\n", hr);
    IUnknown *file = nullptr, *storage_file = nullptr, *stream_op = nullptr, *stream = nullptr;
    IUnknown *random_access_stream = nullptr;
    IInspectable *bitmap = nullptr;
    IUnknown *bitmap_source = nullptr, *action = nullptr;
    hr = get_file(argv[1], &file);
    wprintf(L"GetFileFromPathAsync/GetResults -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    hr = file->QueryInterface(IID_IStorageFile_, (void **)&storage_file);
    wprintf(L"QI IStorageFile -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    using OpenAsync = HRESULT (WINAPI *)(IUnknown *, int, IUnknown **);
    hr = ((OpenAsync *)(*(void ***)storage_file))[8](storage_file, 0, &stream_op);
    wprintf(L"StorageFile.OpenAsync -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    using GetStream = HRESULT (WINAPI *)(IUnknown *, IUnknown **);
    hr = ((GetStream *)(*(void ***)stream_op))[8](stream_op, &stream);
    wprintf(L"OpenAsync.GetResults -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    hr = stream->QueryInterface(IID_IRandomAccessStream_, (void **)&random_access_stream);
    wprintf(L"QI IRandomAccessStream -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    using GetSize = HRESULT (WINAPI *)(IUnknown *, UINT64 *);
    UINT64 size = 0;
    hr = ((GetSize *)(*(void ***)random_access_stream))[6](random_access_stream, &size);
    wprintf(L"stream get_Size -> %#lx (%llu bytes)\n", hr, size);
    if (FAILED(hr)) goto done;
    {
        HSTRING cls = nullptr;
        const wchar_t *name = L"Windows.UI.Xaml.Media.Imaging.BitmapImage";
        WindowsCreateString(name, (UINT32)wcslen(name), &cls);
        hr = RoActivateInstance(cls, &bitmap);
        WindowsDeleteString(cls);
    }
    wprintf(L"RoActivateInstance BitmapImage -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    hr = bitmap->QueryInterface(IID_IBitmapSource_, (void **)&bitmap_source);
    wprintf(L"QI IBitmapSource -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    using SetSourceAsync = HRESULT (WINAPI *)(IUnknown *, IUnknown *, IUnknown **);
    hr = ((SetSourceAsync *)(*(void ***)bitmap_source))[10](bitmap_source, random_access_stream, &action);
    wprintf(L"BitmapSource.SetSourceAsync -> %#lx\n", hr);
    if (FAILED(hr)) goto done;
    hr = wait_action(action);
    wprintf(L"SetSourceAsync completion -> %#lx\n", hr);

done:
    if (action) action->Release();
    if (bitmap_source) bitmap_source->Release();
    if (bitmap) bitmap->Release();
    if (random_access_stream) random_access_stream->Release();
    if (stream) stream->Release();
    if (stream_op) stream_op->Release();
    if (storage_file) storage_file->Release();
    if (file) file->Release();
    RoUninitialize();
    return FAILED(hr) ? 1 : 0;
}

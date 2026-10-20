// Windows.System.Launcher through the platform: LaunchUriAsync, QueryUriSupportAsync for a web and an
// unknown scheme, and LaunchUriForUserAsync, which returns a LaunchUriStatus. Run with
// WINE_UWP_LAUNCHER=log so nothing reaches the real desktop.
#include <windows.h>
#include <roapi.h>
#include <windows.system.h>
#include <windows.foundation.h>
#include <wrl.h>
#include <cstdio>

using namespace Microsoft::WRL;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::System;

template <class Op> static HRESULT wait(Op *op)
{
    ComPtr<IAsyncInfo> info;
    AsyncStatus status = AsyncStatus::Started;
    HRESULT hr = op->QueryInterface(IID_PPV_ARGS(&info));
    for (int i = 0; SUCCEEDED(hr) && status == AsyncStatus::Started && i < 500; i++)
    {
        Sleep(10);
        hr = info->get_Status(&status);
    }
    return SUCCEEDED(hr) && status != AsyncStatus::Completed ? E_FAIL : hr;
}

static HSTRING str(const WCHAR *s)
{
    HSTRING h = nullptr;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

int main()
{
    int bad = 0;
    RoInitialize(RO_INIT_MULTITHREADED);
    {
        ComPtr<ILauncherStatics> launcher;
        ComPtr<ILauncherStatics2> launcher2;
        ComPtr<ILauncherStatics4> launcher4;
        ComPtr<IUriRuntimeClassFactory> uris;
        HRESULT hr = RoGetActivationFactory(str(L"Windows.System.Launcher"), IID_PPV_ARGS(&launcher));
        printf("Launcher factory: %#lx\n", hr);
        if (FAILED(hr)) return 1;
        launcher.As(&launcher2);
        launcher.As(&launcher4);
        RoGetActivationFactory(str(L"Windows.Foundation.Uri"), IID_PPV_ARGS(&uris));

        ComPtr<IUriRuntimeClass> web, odd;
        uris->CreateUri(str(L"https://example.com/probe-launcher"), &web);
        uris->CreateUri(str(L"wineprobe-nosuchscheme:x"), &odd);

        ComPtr<IAsyncOperation<bool>> op;
        boolean ok = false;
        hr = launcher->LaunchUriAsync(web.Get(), &op);
        if (SUCCEEDED(hr)) hr = wait(op.Get());
        if (SUCCEEDED(hr)) hr = op->GetResults(&ok);
        printf("LaunchUriAsync: %#lx -> %d\n", hr, ok);
        bad += FAILED(hr) || !ok;

        const WCHAR *names[] = { L"https", L"unknown scheme" };
        IUriRuntimeClass *targets[] = { web.Get(), odd.Get() };
        LaunchQuerySupportStatus want[] = { LaunchQuerySupportStatus_Available, LaunchQuerySupportStatus_NotSupported };
        for (int i = 0; i < 2; i++)
        {
            ComPtr<IAsyncOperation<LaunchQuerySupportStatus>> q;
            LaunchQuerySupportStatus st = LaunchQuerySupportStatus_Unknown;
            hr = launcher2->QueryUriSupportAsync(targets[i], LaunchQuerySupportType_Uri, &q);
            if (SUCCEEDED(hr)) hr = wait(q.Get());
            if (SUCCEEDED(hr)) hr = q->GetResults(&st);
            printf("QueryUriSupportAsync(%ls): %#lx -> %d (want %d)\n", names[i], hr, st, want[i]);
            bad += FAILED(hr) || st != want[i];
        }

        ComPtr<IAsyncOperation<LaunchUriStatus>> u;
        LaunchUriStatus us = LaunchUriStatus_Unknown;
        hr = launcher4 ? launcher4->LaunchUriForUserAsync(nullptr, web.Get(), &u) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = wait(u.Get());
        if (SUCCEEDED(hr)) hr = u->GetResults(&us);
        printf("LaunchUriForUserAsync: %#lx -> %d (want %d)\n", hr, us, LaunchUriStatus_Success);
        bad += FAILED(hr) || us != LaunchUriStatus_Success;
    }
    RoUninitialize();
    printf("%s: launcher\n", bad ? "RED" : "GREEN");
    return bad;
}

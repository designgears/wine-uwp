// Reproduce the default-channel acquisition used by Solitaire's startup subscriber.
#include <windows.h>
#include <roapi.h>
#include <windows.applicationmodel.useractivities.h>
#include <wrl.h>
#include <cstdio>

using namespace Microsoft::WRL;
using namespace ABI::Windows::ApplicationModel::UserActivities;

int main()
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    int result = 1;
    {
        const WCHAR cls[] = L"Windows.ApplicationModel.UserActivities.UserActivityChannel";
        HSTRING name = nullptr;
        hr = WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name);
        ComPtr<IUserActivityChannelStatics> factory;
        if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, IID_PPV_ARGS(&factory));
        WindowsDeleteString(name);
        printf("UserActivityChannel factory: %#lx\n", hr);
        if (SUCCEEDED(hr))
        {
            ComPtr<IUserActivityChannel> channel;
            hr = factory->GetDefault(&channel);
            printf("GetDefault: %#lx channel=%p\n", hr, channel.Get());
            if (SUCCEEDED(hr) && channel) result = 0;
        }
    }
    RoUninitialize();
    printf("%s: default activity channel\n", result ? "RED" : "GREEN");
    return result;
}

// Exercise the notification service boundary used during Solitaire's launch.
#include <windows.h>
#include <roapi.h>
#include <windows.ui.notifications.h>
#include <wrl.h>
#include <cstdio>
using namespace Microsoft::WRL;
using namespace ABI::Windows::UI::Notifications;
int main()
{
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
    int result = 1;
    {
        const WCHAR cls[] = L"Windows.UI.Notifications.ToastNotificationManager";
        HSTRING name = nullptr;
        HRESULT hr = WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name);
        ComPtr<IToastNotificationManagerStatics> factory;
        if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, IID_PPV_ARGS(&factory));
        WindowsDeleteString(name);
        printf("ToastNotificationManager factory: %#lx\n", hr);
        ComPtr<IToastNotifier> notifier;
        if (SUCCEEDED(hr)) hr = factory->CreateToastNotifier(&notifier);
        printf("CreateToastNotifier: %#lx notifier=%p\n", hr, notifier.Get());
        NotificationSetting setting = NotificationSetting_Enabled;
        if (SUCCEEDED(hr) && notifier) hr = notifier->get_Setting(&setting);
        printf("Setting: %#lx value=%d\n", hr, setting);
        if (SUCCEEDED(hr) && notifier) result = 0;
    }
    RoUninitialize();
    printf("%s: acquire notifier and read availability\n", result ? "RED" : "GREEN");
    return result;
}

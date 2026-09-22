// Notifications are unavailable without the Windows notification service.
// Return a notifier whose Setting reports DisabledForUser, so apps can test
// availability without CreateToastNotifier throwing WPN_E_PLATFORM_UNAVAILABLE.
// Delivery and scheduling remain explicitly unsupported.
#include <windows.h>
#include <roapi.h>
#include <windows.ui.notifications.h>
#include <wrl.h>
#include <cstdio>
#include <cwchar>

using namespace Microsoft::WRL;
using namespace ABI::Windows::UI::Notifications;


static void trace(const char *method)
{
    if (GetEnvironmentVariableW(L"WINE_UWP_TOAST_TRACE", nullptr, 0))
    {
        fprintf(stderr, "[toast] %s\n", method);
        fflush(stderr);
    }
}

/* Nothing is ever scheduled, but the list has to be a real IVectorView of the right
 * parameterised type: .NET Native types the RCW from GetRuntimeClassName. */
class ScheduledList final : public RuntimeClass<RuntimeClassFlags<WinRt>,
        __FIVectorView_1_Windows__CUI__CNotifications__CScheduledToastNotification, FtmBase>
{
    InspectableClass(L"Windows.Foundation.Collections.IVectorView`1<Windows.UI.Notifications.ScheduledToastNotification>", BaseTrust);
public:
    HRESULT STDMETHODCALLTYPE GetAt(unsigned, IScheduledToastNotification **out) override
    { if (out) *out = nullptr; return E_BOUNDS; }
    HRESULT STDMETHODCALLTYPE get_Size(unsigned *out) override
    { if (!out) return E_POINTER; *out = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IScheduledToastNotification *, unsigned *index, boolean *found) override
    { if (index) *index = 0; if (found) *found = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(unsigned, unsigned, IScheduledToastNotification **, unsigned *copied) override
    { if (copied) *copied = 0; return S_OK; }
};

class Notifier final : public RuntimeClass<RuntimeClassFlags<WinRt>, IToastNotifier, FtmBase>
{
    InspectableClass(L"Windows.UI.Notifications.ToastNotifier", BaseTrust);
public:
    /* Accept and drop. A refusal here is fatal: the caller is usually C++/CX or .NET
     * Native, which turns a failed HRESULT into a throw on a path that never completes
     * the task it was part of. There is nothing to deliver to, but saying so is safe. */
    HRESULT STDMETHODCALLTYPE Show(IToastNotification *) override { trace("Show: accepted and dropped"); return S_OK; }
    HRESULT STDMETHODCALLTYPE Hide(IToastNotification *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Setting(NotificationSetting *out) override
    {
        trace("Setting: DisabledForUser");
        if (!out) return E_POINTER;
        *out = NotificationSetting_DisabledForUser;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddToSchedule(IScheduledToastNotification *) override
    { trace("AddToSchedule: accepted and dropped"); return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoveFromSchedule(IScheduledToastNotification *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetScheduledToastNotifications(
            __FIVectorView_1_Windows__CUI__CNotifications__CScheduledToastNotification **out) override
    {
        trace("GetScheduledToastNotifications: empty");
        if (!out) return E_POINTER;
        return MakeAndInitialize<ScheduledList>(out);
    }
};

class Factory final : public RuntimeClass<RuntimeClassFlags<WinRt>, IActivationFactory,
                                         IToastNotificationManagerStatics, FtmBase>
{
    ComPtr<IToastNotifier> notifier = Make<Notifier>();
public:
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *out) noexcept override
    {
        const WCHAR cls[] = L"Windows.UI.Notifications.ToastNotificationManager";
        return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, out);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *out) noexcept override
    {
        if (!out) return E_POINTER;
        *out = BaseTrust;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL; // Static-only class.
    }
    HRESULT STDMETHODCALLTYPE CreateToastNotifier(IToastNotifier **out) override
    {
        trace("CreateToastNotifier");
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!notifier) return E_OUTOFMEMORY;
        return notifier.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(HSTRING, IToastNotifier **out) override
    { return CreateToastNotifier(out); }
    HRESULT STDMETHODCALLTYPE GetTemplateContent(ToastTemplateType,
            ABI::Windows::Data::Xml::Dom::IXmlDocument **out) override
    {
        trace("GetTemplateContent: unsupported");
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
};

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    const WCHAR cls[] = L"Windows.UI.Notifications.ToastNotificationManager";
    UINT32 length;
    const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
    if (length != ARRAYSIZE(cls) - 1 || wmemcmp(text, cls, length)) return CLASS_E_CLASSNOTAVAILABLE;
    static ComPtr<Factory> factory = Make<Factory>();
    if (!factory) return E_OUTOFMEMORY;
    return factory.CopyTo(out);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }

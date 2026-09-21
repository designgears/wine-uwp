// Service-free UserActivityChannel acquisition. Solitaire acquires the default
// channel in an EngineAppStartup subscriber. Native cdprt needs the absent cdp.dll;
// that exception escapes the subscriber and terminates the engine thread.
// Only channel acquisition is provided here. Timeline operations fail explicitly
// until a local activity store exists; never return a successful null activity.
#include <windows.h>
#include <roapi.h>
#include <windows.applicationmodel.useractivities.h>
#include <wrl.h>
#include <cstdio>
#include <cwchar>

using namespace Microsoft::WRL;
using namespace ABI::Windows::ApplicationModel::UserActivities;
using ABI::Windows::Foundation::IAsyncAction;

static void trace(const char *method)
{
    if (GetEnvironmentVariableW(L"WINE_UWP_USERACTIVITY_TRACE", nullptr, 0))
    {
        fprintf(stderr, "[useractivity] %s\n", method);
        fflush(stderr);
    }
}

class Channel final : public RuntimeClass<RuntimeClassFlags<WinRt>, IUserActivityChannel, FtmBase>
{
    InspectableClass(L"Windows.ApplicationModel.UserActivities.UserActivityChannel", BaseTrust);
public:
    HRESULT STDMETHODCALLTYPE GetOrCreateUserActivityAsync(HSTRING,
            __FIAsyncOperation_1_Windows__CApplicationModel__CUserActivities__CUserActivity **out) override
    {
        trace("GetOrCreateUserActivityAsync: unsupported");
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DeleteActivityAsync(HSTRING, IAsyncAction **out) override
    {
        trace("DeleteActivityAsync: unsupported");
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DeleteAllActivitiesAsync(IAsyncAction **out) override
    {
        trace("DeleteAllActivitiesAsync: unsupported");
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
};

class Factory final : public RuntimeClass<RuntimeClassFlags<WinRt>, IActivationFactory,
                                         IUserActivityChannelStatics, FtmBase>
{
    ComPtr<IUserActivityChannel> channel = Make<Channel>();
public:
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *out) noexcept override
    {
        const WCHAR cls[] = L"Windows.ApplicationModel.UserActivities.UserActivityChannel";
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
        return E_NOTIMPL; // Channels are obtained with GetDefault.
    }
    HRESULT STDMETHODCALLTYPE GetDefault(IUserActivityChannel **out) override
    {
        trace("GetDefault");
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!channel) return E_OUTOFMEMORY;
        return channel.CopyTo(out);
    }
};

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    const WCHAR cls[] = L"Windows.ApplicationModel.UserActivities.UserActivityChannel";
    UINT32 length;
    const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
    if (length != ARRAYSIZE(cls) - 1 || wmemcmp(text, cls, length)) return CLASS_E_CLASSNOTAVAILABLE;
    static ComPtr<Factory> factory = Make<Factory>();
    if (!factory) return E_OUTOFMEMORY;
    return factory.CopyTo(out);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }

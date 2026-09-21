// MemoryManager without the Windows resource-manager RPC service.
// Solitaire subscribes to AppMemoryUsageIncreased in its page constructor. The
// native rmclient returns EPT_S_NOT_REGISTERED here, aborting OnLaunched silently.
// This host has no UWP memory quota or pressure-event source. Report process
// private usage and physical capacity; retain registrations until removal.
#include <windows.h>
#include <roapi.h>
#include <windows.system.h>
#include <psapi.h>
#include <cstdio>
#include <cwchar>

using namespace ABI::Windows::System;

class MemoryManager final : public IActivationFactory, public IMemoryManagerStatics,
                            public IMemoryManagerStatics2
{
    struct Subscription
    {
        Subscription *next;
        IUnknown *handler;
        INT64 token;
        unsigned kind;
    };
    LONG refs = 1;
    LONG64 next_token = 0;
    SRWLOCK lock = SRWLOCK_INIT;
    Subscription *subscriptions = nullptr;

    // .NET Native needs Statics2 during DLL initialization. Its native reports
    // already work without the RPC service; preserve those objects and semantics.
    static HRESULT native_reports(IMemoryManagerStatics2 **out)
    {
        static HMODULE module = LoadLibraryW(L"rmclient.dll");
        if (!module) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        auto get_factory = reinterpret_cast<HRESULT (WINAPI *)(HSTRING, IActivationFactory **)>(
            GetProcAddress(module, "DllGetActivationFactory"));
        if (!get_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        const WCHAR cls[] = L"Windows.System.MemoryManager";
        HSTRING name = nullptr;
        HRESULT hr = WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name);
        if (FAILED(hr)) return hr;
        IActivationFactory *factory = nullptr;
        hr = get_factory(name, &factory);
        WindowsDeleteString(name);
        if (FAILED(hr)) return hr;
        hr = factory->QueryInterface(__uuidof(IMemoryManagerStatics2), reinterpret_cast<void **>(out));
        factory->Release();
        return hr;
    }

    HRESULT add(unsigned kind, IUnknown *handler, EventRegistrationToken *token)
    {
        if (!token) return E_POINTER;
        token->value = 0;
        if (!handler) return E_INVALIDARG;
        auto *entry = static_cast<Subscription *>(HeapAlloc(GetProcessHeap(), 0, sizeof(Subscription)));
        if (!entry) return E_OUTOFMEMORY;
        entry->handler = handler;
        entry->kind = kind;
        entry->token = InterlockedIncrement64(&next_token);
        handler->AddRef();
        AcquireSRWLockExclusive(&lock);
        entry->next = subscriptions;
        subscriptions = entry;
        token->value = entry->token;
        ReleaseSRWLockExclusive(&lock);
        if (GetEnvironmentVariableW(L"WINE_UWP_MEMORYMANAGER_TRACE", nullptr, 0))
            fprintf(stderr, "[memorymanager] subscribed event %u token %lld\n", kind, token->value);
        return S_OK;
    }
    HRESULT remove(unsigned kind, EventRegistrationToken token)
    {
        Subscription *removed = nullptr;
        AcquireSRWLockExclusive(&lock);
        for (auto **at = &subscriptions; *at; at = &(*at)->next)
        {
            if ((*at)->kind != kind || (*at)->token != token.value) continue;
            removed = *at;
            *at = removed->next;
            break;
        }
        ReleaseSRWLockExclusive(&lock);
        if (removed)
        {
            removed->handler->Release();
            HeapFree(GetProcessHeap(), 0, removed);
        }
        return S_OK;
    }
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IInspectable ||
            iid == __uuidof(IActivationFactory) || iid == __uuidof(IAgileObject))
            *out = static_cast<IActivationFactory *>(this);
        else if (iid == __uuidof(IMemoryManagerStatics))
            *out = static_cast<IMemoryManagerStatics *>(this);
        else if (iid == __uuidof(IMemoryManagerStatics2))
            *out = static_cast<IMemoryManagerStatics2 *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override
    {
        if (!count || !iids) return E_POINTER;
        *count = 0;
        *iids = static_cast<IID *>(CoTaskMemAlloc(2 * sizeof(IID)));
        if (!*iids) return E_OUTOFMEMORY;
        **iids = __uuidof(IMemoryManagerStatics);
        (*iids)[1] = __uuidof(IMemoryManagerStatics2);
        *count = 2;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override
    {
        const WCHAR cls[] = L"Windows.System.MemoryManager";
        return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *value) override
    {
        if (!value) return E_POINTER;
        *value = BaseTrust;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL; // Static-only runtime class.
    }
    HRESULT STDMETHODCALLTYPE get_AppMemoryUsage(UINT64 *value) override
    {
        if (!value) return E_POINTER;
        *value = 0;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
            return HRESULT_FROM_WIN32(GetLastError());
        *value = counters.PrivateUsage;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AppMemoryUsageLimit(UINT64 *value) override
    {
        if (!value) return E_POINTER;
        *value = 0;
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status)) return HRESULT_FROM_WIN32(GetLastError());
        *value = status.ullTotalPhys;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AppMemoryUsageLevel(AppMemoryUsageLevel *value) override
    {
        if (!value) return E_POINTER;
        UINT64 usage, limit;
        HRESULT hr = get_AppMemoryUsage(&usage);
        if (FAILED(hr)) return hr;
        hr = get_AppMemoryUsageLimit(&limit);
        if (FAILED(hr)) return hr;
        *value = usage >= limit ? AppMemoryUsageLevel_OverLimit :
                 usage >= limit - limit / 5 ? AppMemoryUsageLevel_High :
                 usage >= limit / 2 ? AppMemoryUsageLevel_Medium : AppMemoryUsageLevel_Low;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_AppMemoryUsageIncreased(__FIEventHandler_1_IInspectable *handler,
            EventRegistrationToken *token) override { return add(0, handler, token); }
    HRESULT STDMETHODCALLTYPE remove_AppMemoryUsageIncreased(EventRegistrationToken token) override
        { return remove(0, token); }
    HRESULT STDMETHODCALLTYPE add_AppMemoryUsageDecreased(__FIEventHandler_1_IInspectable *handler,
            EventRegistrationToken *token) override { return add(1, handler, token); }
    HRESULT STDMETHODCALLTYPE remove_AppMemoryUsageDecreased(EventRegistrationToken token) override
        { return remove(1, token); }
    HRESULT STDMETHODCALLTYPE add_AppMemoryUsageLimitChanging(
            __FIEventHandler_1_Windows__CSystem__CAppMemoryUsageLimitChangingEventArgs *handler,
            EventRegistrationToken *token) override { return add(2, handler, token); }
    HRESULT STDMETHODCALLTYPE remove_AppMemoryUsageLimitChanging(EventRegistrationToken token) override
        { return remove(2, token); }
    HRESULT STDMETHODCALLTYPE GetAppMemoryReport(IAppMemoryReport **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        IMemoryManagerStatics2 *native = nullptr;
        HRESULT hr = native_reports(&native);
        if (FAILED(hr)) return hr;
        hr = native->GetAppMemoryReport(out);
        native->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetProcessMemoryReport(IProcessMemoryReport **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        IMemoryManagerStatics2 *native = nullptr;
        HRESULT hr = native_reports(&native);
        if (FAILED(hr)) return hr;
        hr = native->GetProcessMemoryReport(out);
        native->Release();
        return hr;
    }
};

static MemoryManager manager;

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    UINT32 length;
    const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
    const WCHAR cls[] = L"Windows.System.MemoryManager";
    if (length != ARRAYSIZE(cls) - 1 || wmemcmp(text, cls, length)) return CLASS_E_CLASSNOTAVAILABLE;
    return manager.QueryInterface(__uuidof(IActivationFactory), reinterpret_cast<void **>(out));
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }

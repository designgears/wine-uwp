// Exercise the event registration that aborts Solitaire's page constructor.
#include <windows.h>
#include <roapi.h>
#include <windows.system.h>
#include <cstdio>

using ABI::Windows::System::IMemoryManagerStatics;

class Handler final : public __FIEventHandler_1_IInspectable
{
public:
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(__FIEventHandler_1_IInspectable))
            return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable *, IInspectable *) override { return S_OK; }
};

int main()
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    HSTRING name;
    const WCHAR cls[] = L"Windows.System.MemoryManager";
    WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name);
    IMemoryManagerStatics *manager = nullptr;
    hr = RoGetActivationFactory(name, __uuidof(IMemoryManagerStatics), (void **)&manager);
    WindowsDeleteString(name);
    if (FAILED(hr)) { printf("RED: activation %#lx\n", hr); return 1; }
    Handler handler;
    EventRegistrationToken token{};
    hr = manager->add_AppMemoryUsageIncreased(&handler, &token);
    printf("add_AppMemoryUsageIncreased: %#lx token=%lld refs=%ld\n", hr, token.value, handler.refs);
    if (FAILED(hr)) return 1;
    bool retained = handler.refs > 1;
    hr = manager->remove_AppMemoryUsageIncreased(token);
    if (FAILED(hr) || !retained || handler.refs != 1) return 1;
    UINT64 usage = 0, limit = 0;
    if (FAILED(manager->get_AppMemoryUsage(&usage)) ||
        FAILED(manager->get_AppMemoryUsageLimit(&limit)) || !limit) return 1;
    printf("GREEN: subscription retained/released; usage=%llu limit=%llu\n", usage, limit);
    manager->Release();
    RoUninitialize();
    return 0;
}

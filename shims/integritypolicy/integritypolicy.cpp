// Windows.System.Profile.WindowsIntegrityPolicy: is this PC in S mode? It is not.
//
// Windows implements the class in Windows.Security.Integrity.dll, which asks wldp.dll, which makes an RPC call to the
// lockdown-policy service. There is no such service in a prefix, so the call raises EPT_S_NOT_REGISTERED (0x6d9)
// through the caller: the Microsoft Store reads IsEnabled at start-up from managed code and fail-fasts on it.
// A Wine prefix enforces no code-integrity policy, so every property is false, and PolicyChanged never fires (the
// registration is still accepted: refusing one is fatal, AGENTS.md).
//
//   IWindowsIntegrityPolicyStatics {7d1d81db-8d63-4789-9ea5-ddcf65a94f3c}:
//     6 get_IsEnabled  7 get_IsEnabledForTrial  8 get_CanDisable  9 get_IsDisableSupported
//     10 add_PolicyChanged  11 remove_PolicyChanged
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <eventtoken.h>

static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IWindowsIntegrityPolicyStatics_ = {0x7d1d81db,0x8d63,0x4789,{0x9e,0xa5,0xdd,0xcf,0x65,0xa9,0x4f,0x3c}};
static const wchar_t CLASS_NAME[] = L"Windows.System.Profile.WindowsIntegrityPolicy";

struct IWindowsIntegrityPolicyStaticsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_IsEnabled(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsEnabledForTrial(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanDisable(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsDisableSupported(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PolicyChanged(IUnknown *, EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PolicyChanged(EventRegistrationToken) = 0;
};

static HRESULT no(boolean *v) { if (!v) return E_POINTER; *v = FALSE; return S_OK; }

struct Statics final : IWindowsIntegrityPolicyStaticsAbi, IActivationFactory
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
            IsEqualGUID(iid, IID_IWindowsIntegrityPolicyStatics_))
            *out = static_cast<IWindowsIntegrityPolicyStaticsAbi *>(this);
        else if (IsEqualGUID(iid, IID_IActivationFactory)) *out = static_cast<IActivationFactory *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return WindowsCreateString(CLASS_NAME, ARRAYSIZE(CLASS_NAME) - 1, n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override { if (out) *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_IsEnabled(boolean *v) override { return no(v); }
    HRESULT STDMETHODCALLTYPE get_IsEnabledForTrial(boolean *v) override { return no(v); }
    HRESULT STDMETHODCALLTYPE get_CanDisable(boolean *v) override { return no(v); }
    HRESULT STDMETHODCALLTYPE get_IsDisableSupported(boolean *v) override { return no(v); }
    HRESULT STDMETHODCALLTYPE add_PolicyChanged(IUnknown *, EventRegistrationToken *t) override { if (!t) return E_POINTER; t->value = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE remove_PolicyChanged(EventRegistrationToken) override { return S_OK; }
};
static Statics statics;

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING cls, IActivationFactory **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (wcscmp(WindowsGetStringRawBuffer(cls, nullptr), CLASS_NAME)) return CLASS_E_CLASSNOTAVAILABLE;
    *out = static_cast<IActivationFactory *>(&statics);
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

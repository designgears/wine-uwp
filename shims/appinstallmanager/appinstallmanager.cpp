// Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager: a stand-in for the Microsoft Store.
//
// Windows implements the class in InstallService.dll, which is only a client of the InstallService *service* (an
// out-of-process broker that downloads and deploys packages). Neither exists in a prefix: the DLL does not even load
// (UMPDC.dll, api-ms-win-ham-apphistory), and the Store fail-fasts during start-up when `new AppInstallManager()`
// throws. This stand-in answers what the Store asks at start-up honestly: no installs in progress, automatic updates
// as configured, nothing blocked by policy. Every other method is traced and refused, so the log says which ones
// the Store needs next (AGENTS.md: give every slot a traced thunk before implementing anything).
//
// Slots from Windows.ApplicationModel.winmd (./winmd.py --sig); IIDs of parameterised types from ./piid.py.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <eventtoken.h>
#include <cstdio>
#include <cstdarg>

static void trace(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[appinstallmanager] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IAsyncOperation_bool_ = {0xcdb5efb3,0x5788,0x509d,{0x9b,0xe1,0x71,0xcc,0xb8,0xa3,0x36,0x2a}};
static const GUID IID_IAppInstallManager_  = {0x9353e170,0x8441,0x4b45,{0xbd,0x72,0x7c,0x2f,0xa9,0x25,0xbe,0xee}};
static const GUID IID_IAppInstallManager2_ = {0x16937851,0xed37,0x480d,{0x83,0x14,0x52,0xe2,0x7c,0x03,0xf0,0x4a}};
static const GUID IID_IAppInstallManager3_ = {0x95b24b17,0xe96a,0x4d0e,{0x84,0xe1,0xc8,0xcb,0x41,0x7a,0x01,0x78}};
static const GUID IID_IAppInstallManager4_ = {0x260a2a16,0x5a9e,0x4ebd,{0xb9,0x44,0xf2,0xba,0x75,0xc3,0x11,0x59}};
static const GUID IID_IAppInstallManager5_ = {0x3cd7be4c,0x1be9,0x4f7f,{0xb6,0x75,0xaa,0x1d,0x64,0xa5,0x29,0xb2}};
static const GUID IID_IAppInstallManager6_ = {0xc9e7d408,0xf27a,0x4471,{0xb2,0xf4,0xe7,0x6e,0xfc,0xbe,0xbc,0xca}};
static const GUID IID_IAppInstallManager7_ = {0xa5ee7b30,0xd5e4,0x49a3,{0x93,0x53,0x3d,0xb0,0x32,0x03,0x32,0x1d}};
/* IVectorView<AppInstallItem>, IIterable<AppInstallItem>, IIterator<AppInstallItem> */
static const GUID IID_IVectorView_Item_ = {0x48d7f874,0xa83c,0x55db,{0xb2,0xe6,0x94,0x0b,0xe9,0x56,0x98,0x69}};
static const GUID IID_IIterable_Item_   = {0x93e1cdc8,0x503f,0x55b1,{0x91,0x5b,0xc0,0xdc,0x78,0x88,0xce,0x31}};
static const GUID IID_IIterator_Item_   = {0xccaca81b,0x6cf7,0x56f0,{0xb7,0xff,0x8a,0xc5,0x19,0x1e,0x79,0xbf}};

static bool base_iid(REFIID iid)
{
    return IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_);
}
static HRESULT hstr(const wchar_t *s, HSTRING *out) { return WindowsCreateString(s, (UINT32)wcslen(s), out); }

#define INSPECTABLE(cls)                                                                                          \
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }                               \
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; } \
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; } \
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(cls, n); }                    \
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* ---- IAsyncOperation<bool>, born completed; put_Completed invokes the handler at once ---- */
struct IAsyncInfoAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncBoolAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(boolean *) = 0;
};
struct AsyncInfoPart : IAsyncInfoAbi
{
    struct AsyncBool *outer;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.Foundation.IAsyncOperation`1<Boolean>", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Id(UINT32 *v) override { if (!v) return E_POINTER; *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(int *v) override { if (!v) return E_POINTER; *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *v) override { if (!v) return E_POINTER; *v = S_OK; return S_OK; }
    HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};
struct AsyncBool : IAsyncBoolAbi
{
    LONG ref = 1;
    boolean result;
    IUnknown *handler = nullptr;
    AsyncInfoPart info;
    explicit AsyncBool(bool r) : result(r) { info.outer = this; }
    ~AsyncBool() { if (handler) handler->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IAsyncOperation_bool_)) *out = static_cast<IAsyncBoolAbi *>(this);
        else if (IsEqualGUID(iid, IID_IAsyncInfo_)) *out = static_cast<IAsyncInfoAbi *>(&info);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.Foundation.IAsyncOperation`1<Boolean>")
    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *h) override
    {
        if (handler) { handler->Release(); handler = nullptr; }
        if (!h) return S_OK;
        h->AddRef();
        handler = h;
        /* a delegate: Invoke is slot 3; AsyncStatus::Completed == 1 */
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IAsyncBoolAbi *, int);
        return ((invoke_fn)(*(void ***)h)[3])(h, this, 1);
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **h) override { if (!h) return E_POINTER; if ((*h = handler)) handler->AddRef(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetResults(boolean *r) override { if (!r) return E_POINTER; *r = result; return S_OK; }
};
HRESULT AsyncInfoPart::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); }
ULONG AsyncInfoPart::AddRef() { return outer->AddRef(); }
ULONG AsyncInfoPart::Release() { return outer->Release(); }

static HRESULT async_bool(bool value, void **out, const char *what)
{
    trace("%s -> %s", what, value ? "true" : "false");
    if (!out) return E_POINTER;
    *out = static_cast<IAsyncBoolAbi *>(new AsyncBool(value));
    return S_OK;
}

/* ---- an empty IVectorView<AppInstallItem>, with its IIterable ---- */
struct IIteratorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Current(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, IInspectable **, UINT32 *) = 0;
};
struct EmptyIterator : IIteratorAbi
{
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (!base_iid(iid) && !IsEqualGUID(iid, IID_IIterator_Item_)) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IIteratorAbi *>(this); AddRef(); return S_OK;
    }
    INSPECTABLE(L"Windows.Foundation.Collections.IIterator`1<Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem>")
    HRESULT STDMETHODCALLTYPE get_Current(IInspectable **v) override { if (v) *v = nullptr; return E_BOUNDS; }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32, IInspectable **, UINT32 *n) override { if (!n) return E_POINTER; *n = 0; return S_OK; }
};
struct IVectorViewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *, UINT32 *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IInspectable **, UINT32 *) = 0;
};
struct IIterableAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE First(IIteratorAbi **) = 0;
};
struct EmptyView;
struct ViewIterable : IIterableAbi
{
    EmptyView *outer;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.Foundation.Collections.IVectorView`1<Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem>", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE First(IIteratorAbi **out) override { if (!out) return E_POINTER; *out = new EmptyIterator(); return S_OK; }
};
struct EmptyView : IVectorViewAbi
{
    LONG ref = 1;
    ViewIterable iterable;
    EmptyView() { iterable.outer = this; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IVectorView_Item_)) *out = static_cast<IVectorViewAbi *>(this);
        else if (IsEqualGUID(iid, IID_IIterable_Item_)) *out = static_cast<IIterableAbi *>(&iterable);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.Foundation.Collections.IVectorView`1<Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem>")
    HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable **v) override { if (v) *v = nullptr; return E_BOUNDS; }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *n) override { if (!n) return E_POINTER; *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *, UINT32 *i, boolean *f) override { if (!i || !f) return E_POINTER; *i = 0; *f = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IInspectable **, UINT32 *n) override { if (!n) return E_POINTER; *n = 0; return S_OK; }
};
HRESULT ViewIterable::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); }
ULONG ViewIterable::AddRef() { return outer->AddRef(); }
ULONG ViewIterable::Release() { return outer->Release(); }

static HRESULT empty_items(void **out, const char *what)
{
    trace("%s -> no installs in progress", what);
    if (!out) return E_POINTER;
    *out = static_cast<IVectorViewAbi *>(new EmptyView());
    return S_OK;
}

/* ---- AppInstallManager ---- */
#define REFUSE(name) { trace(name " -> E_NOTIMPL"); return E_NOTIMPL; }
typedef void *P; /* HSTRING, objects and out-pointers alike: only traced */

struct IAIM1 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_AppInstallItems(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE Restart(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ItemCompleted(IUnknown *, EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ItemCompleted(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ItemStatusChanged(IUnknown *, EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ItemStatusChanged(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AutoUpdateSetting(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutoUpdateSetting(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AcquisitionIdentity(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AcquisitionIdentity(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsApplicableAsync(HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartAppInstallAsync(HSTRING, HSTRING, boolean, boolean, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameAsync(HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsStoreBlockedByPolicyAsync(HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallAsync(HSTRING, void **) = 0;
};
struct IAIM2 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE StartAppInstallAsync(HSTRING, HSTRING, boolean, boolean, HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameAsync(HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel(HSTRING, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause(HSTRING, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE Restart(HSTRING, HSTRING) = 0;
};
struct IAIM3 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE StartProductInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, boolean, boolean, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartProductInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, boolean, boolean, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameForUserAsync(P, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForUpdatesForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForAllUpdatesForUserAsync(P, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsApplicableForUserAsync(P, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveToFrontOfDownloadQueue(HSTRING, HSTRING) = 0;
};
struct IAIM4 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetFreeUserEntitlementAsync(HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFreeUserEntitlementForUserAsync(P, HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFreeDeviceEntitlementAsync(HSTRING, HSTRING, HSTRING, void **) = 0;
};
struct IAIM5 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_AppInstallItemsWithGroupSupport(void **) = 0;
};
struct IAIM6 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(HSTRING, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForAllUpdatesForUserAsync(P, HSTRING, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, HSTRING, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SearchForUpdatesForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartProductInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartProductInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, P, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsPackageIdentityAllowedToInstallAsync(HSTRING, HSTRING, HSTRING, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsPackageIdentityAllowedToInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, void **) = 0;
};
struct IAIM7 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_CanInstallForAllUsers(boolean *) = 0;
};

#define FORWARD_INSPECTABLE                                                                                       \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override { return outer->QueryInterface(iid, out); } \
    ULONG STDMETHODCALLTYPE AddRef() override { return outer->AddRef(); }                                         \
    ULONG STDMETHODCALLTYPE Release() override { return outer->Release(); }                                       \
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; } \
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager", n); } \
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

static LONG next_token = 1;
static int auto_update_setting = 1; /* AutoUpdateSetting.Enabled */

struct Manager;
struct M2 : IAIM2
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE StartAppInstallAsync(HSTRING, HSTRING, boolean, boolean, HSTRING, HSTRING, HSTRING, void **) override REFUSE("StartAppInstallAsync(2)")
    HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameAsync(HSTRING, HSTRING, void **) override REFUSE("UpdateAppByPackageFamilyNameAsync(2)")
    HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, HSTRING, HSTRING, void **) override REFUSE("SearchForUpdatesAsync(2)")
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(HSTRING, void **) override REFUSE("SearchForAllUpdatesAsync(2)")
    HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsAppAllowedToInstallAsync(2)"); }
    HRESULT STDMETHODCALLTYPE Cancel(HSTRING, HSTRING) override REFUSE("Cancel(2)")
    HRESULT STDMETHODCALLTYPE Pause(HSTRING, HSTRING) override REFUSE("Pause(2)")
    HRESULT STDMETHODCALLTYPE Restart(HSTRING, HSTRING) override REFUSE("Restart(2)")
};
struct M3 : IAIM3
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE StartProductInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, boolean, boolean, HSTRING, P, void **) override REFUSE("StartProductInstallAsync(3)")
    HRESULT STDMETHODCALLTYPE StartProductInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, boolean, boolean, HSTRING, P, void **) override REFUSE("StartProductInstallForUserAsync(3)")
    HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameForUserAsync(P, HSTRING, HSTRING, void **) override REFUSE("UpdateAppByPackageFamilyNameForUserAsync")
    HRESULT STDMETHODCALLTYPE SearchForUpdatesForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, void **) override REFUSE("SearchForUpdatesForUserAsync(3)")
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesForUserAsync(P, HSTRING, void **) override REFUSE("SearchForAllUpdatesForUserAsync(3)")
    HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsAppAllowedToInstallForUserAsync"); }
    HRESULT STDMETHODCALLTYPE GetIsApplicableForUserAsync(P, HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsApplicableForUserAsync"); }
    HRESULT STDMETHODCALLTYPE MoveToFrontOfDownloadQueue(HSTRING, HSTRING) override REFUSE("MoveToFrontOfDownloadQueue")
};
struct M4 : IAIM4
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE GetFreeUserEntitlementAsync(HSTRING, HSTRING, HSTRING, void **) override REFUSE("GetFreeUserEntitlementAsync")
    HRESULT STDMETHODCALLTYPE GetFreeUserEntitlementForUserAsync(P, HSTRING, HSTRING, HSTRING, void **) override REFUSE("GetFreeUserEntitlementForUserAsync")
    HRESULT STDMETHODCALLTYPE GetFreeDeviceEntitlementAsync(HSTRING, HSTRING, HSTRING, void **) override REFUSE("GetFreeDeviceEntitlementAsync")
};
struct M5 : IAIM5
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE get_AppInstallItemsWithGroupSupport(void **out) override { return empty_items(out, "AppInstallItemsWithGroupSupport"); }
};
struct M6 : IAIM6
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(HSTRING, HSTRING, P, void **) override REFUSE("SearchForAllUpdatesAsync(6)")
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesForUserAsync(P, HSTRING, HSTRING, P, void **) override REFUSE("SearchForAllUpdatesForUserAsync(6)")
    HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, HSTRING, HSTRING, P, void **) override REFUSE("SearchForUpdatesAsync(6)")
    HRESULT STDMETHODCALLTYPE SearchForUpdatesForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, P, void **) override REFUSE("SearchForUpdatesForUserAsync(6)")
    HRESULT STDMETHODCALLTYPE StartProductInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, P, void **) override REFUSE("StartProductInstallAsync(6)")
    HRESULT STDMETHODCALLTYPE StartProductInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, P, void **) override REFUSE("StartProductInstallForUserAsync(6)")
    HRESULT STDMETHODCALLTYPE GetIsPackageIdentityAllowedToInstallAsync(HSTRING, HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsPackageIdentityAllowedToInstallAsync"); }
    HRESULT STDMETHODCALLTYPE GetIsPackageIdentityAllowedToInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsPackageIdentityAllowedToInstallForUserAsync"); }
};
struct M7 : IAIM7
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE get_CanInstallForAllUsers(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
};

struct Manager : IAIM1
{
    LONG ref = 1;
    HSTRING acquisition = nullptr;
    M2 m2; M3 m3; M4 m4; M5 m5; M6 m6; M7 m7;
    Manager() { m2.outer = m3.outer = m4.outer = m5.outer = m6.outer = m7.outer = this; }
    ~Manager() { WindowsDeleteString(acquisition); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IAppInstallManager_)) *out = static_cast<IAIM1 *>(this);
        else if (IsEqualGUID(iid, IID_IAppInstallManager2_)) *out = static_cast<IAIM2 *>(&m2);
        else if (IsEqualGUID(iid, IID_IAppInstallManager3_)) *out = static_cast<IAIM3 *>(&m3);
        else if (IsEqualGUID(iid, IID_IAppInstallManager4_)) *out = static_cast<IAIM4 *>(&m4);
        else if (IsEqualGUID(iid, IID_IAppInstallManager5_)) *out = static_cast<IAIM5 *>(&m5);
        else if (IsEqualGUID(iid, IID_IAppInstallManager6_)) *out = static_cast<IAIM6 *>(&m6);
        else if (IsEqualGUID(iid, IID_IAppInstallManager7_)) *out = static_cast<IAIM7 *>(&m7);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager")
    HRESULT STDMETHODCALLTYPE get_AppInstallItems(void **out) override { return empty_items(out, "AppInstallItems"); }
    HRESULT STDMETHODCALLTYPE Cancel(HSTRING) override REFUSE("Cancel")
    HRESULT STDMETHODCALLTYPE Pause(HSTRING) override REFUSE("Pause")
    HRESULT STDMETHODCALLTYPE Restart(HSTRING) override REFUSE("Restart")
    /* events are accepted although nothing raises them yet: refusing a registration is fatal (AGENTS.md) */
    HRESULT STDMETHODCALLTYPE add_ItemCompleted(IUnknown *, EventRegistrationToken *t) override { if (!t) return E_POINTER; t->value = InterlockedIncrement(&next_token); return S_OK; }
    HRESULT STDMETHODCALLTYPE remove_ItemCompleted(EventRegistrationToken) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE add_ItemStatusChanged(IUnknown *, EventRegistrationToken *t) override { if (!t) return E_POINTER; t->value = InterlockedIncrement(&next_token); return S_OK; }
    HRESULT STDMETHODCALLTYPE remove_ItemStatusChanged(EventRegistrationToken) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AutoUpdateSetting(int *v) override { if (!v) return E_POINTER; *v = auto_update_setting; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AutoUpdateSetting(int v) override { auto_update_setting = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AcquisitionIdentity(HSTRING *v) override { if (!v) return E_POINTER; return WindowsDuplicateString(acquisition, v); }
    HRESULT STDMETHODCALLTYPE put_AcquisitionIdentity(HSTRING v) override { WindowsDeleteString(acquisition); return WindowsDuplicateString(v, &acquisition); }
    HRESULT STDMETHODCALLTYPE GetIsApplicableAsync(HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsApplicableAsync"); }
    HRESULT STDMETHODCALLTYPE StartAppInstallAsync(HSTRING, HSTRING, boolean, boolean, void **) override REFUSE("StartAppInstallAsync")
    HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameAsync(HSTRING, void **) override REFUSE("UpdateAppByPackageFamilyNameAsync")
    HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, void **) override REFUSE("SearchForUpdatesAsync")
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(void **) override REFUSE("SearchForAllUpdatesAsync")
    HRESULT STDMETHODCALLTYPE IsStoreBlockedByPolicyAsync(HSTRING, HSTRING, void **out) override { return async_bool(false, out, "IsStoreBlockedByPolicyAsync"); }
    HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallAsync(HSTRING, void **out) override { return async_bool(true, out, "GetIsAppAllowedToInstallAsync"); }
};

struct Factory : IActivationFactory
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IActivationFactory)) { *out = static_cast<IActivationFactory *>(this); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        trace("new AppInstallManager");
        *out = static_cast<IAIM1 *>(new Manager());
        return S_OK;
    }
};
static Factory factory;

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING cls, IActivationFactory **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    const wchar_t *name = WindowsGetStringRawBuffer(cls, nullptr);
    if (wcscmp(name, L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager")) return CLASS_E_CLASSNOTAVAILABLE;
    *out = &factory;
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

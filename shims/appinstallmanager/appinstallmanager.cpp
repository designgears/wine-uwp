// Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager: a stand-in for the Microsoft Store.
//
// Windows implements the class in InstallService.dll, which is only a client of the InstallService *service* (an
// out-of-process broker that downloads and deploys packages). Neither exists in a prefix: the DLL does not even load
// (UMPDC.dll, api-ms-win-ham-apphistory), and the Store fail-fasts during start-up when `new AppInstallManager()`
// throws. This stand-in answers what the Store asks at start-up honestly: no installs in progress, automatic updates
// as configured, nothing blocked by policy, and (until installs go through FE3 here) no updates found. Every other method is traced and refused, so the log says which ones
// the Store needs next (AGENTS.md: give every slot a traced thunk before implementing anything).
//
// Slots from Windows.ApplicationModel.winmd (./winmd.py --sig); IIDs of parameterised types from ./piid.py.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <eventtoken.h>
#include <winhttp.h>
#include APPXINSTALL_H
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "../common/json.h"

static void trace(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[appinstallmanager] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}
#define tracef trace
#include "../common/storeservices.h"

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
struct ItemsIterator : IIteratorAbi
{
    LONG ref = 1;
    std::vector<IInspectable *> items;
    size_t pos = 0;
    explicit ItemsIterator(const std::vector<IInspectable *> &v) : items(v) { for (auto *i : items) i->AddRef(); }
    ~ItemsIterator() { for (auto *i : items) i->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (!base_iid(iid) && !IsEqualGUID(iid, IID_IIterator_Item_)) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IIteratorAbi *>(this); AddRef(); return S_OK;
    }
    INSPECTABLE(L"Windows.Foundation.Collections.IIterator`1<Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem>")
    HRESULT STDMETHODCALLTYPE get_Current(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if (pos >= items.size()) { *v = nullptr; return E_BOUNDS; }
        (*v = items[pos])->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override { if (!v) return E_POINTER; *v = pos < items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override { if (!v) return E_POINTER; if (pos < items.size()) pos++; *v = pos < items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 cap, IInspectable **out, UINT32 *n) override
    {
        if (!n) return E_POINTER;
        UINT32 k = 0;
        for (; k < cap && pos < items.size(); k++, pos++) (out[k] = items[pos])->AddRef();
        *n = k;
        return S_OK;
    }
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
struct ItemsView;
struct ViewIterable : IIterableAbi
{
    ItemsView *outer;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.Foundation.Collections.IVectorView`1<Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem>", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE First(IIteratorAbi **out) override;
};
struct ItemsView : IVectorViewAbi
{
    LONG ref = 1;
    ViewIterable iterable;
    std::vector<IInspectable *> items;
    ItemsView() { iterable.outer = this; }
    explicit ItemsView(const std::vector<IInspectable *> &v) : items(v) { iterable.outer = this; for (auto *i : items) i->AddRef(); }
    ~ItemsView() { for (auto *i : items) i->Release(); }
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
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if (i >= items.size()) { *v = nullptr; return E_BOUNDS; }
        (*v = items[i])->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *n) override { if (!n) return E_POINTER; *n = (UINT32)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *v, UINT32 *i, boolean *f) override
    {
        if (!i || !f) return E_POINTER;
        *i = 0; *f = FALSE;
        for (size_t k = 0; k < items.size(); k++)
            if (items[k] == v) { *i = (UINT32)k; *f = TRUE; break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 cap, IInspectable **out, UINT32 *n) override
    {
        if (!n) return E_POINTER;
        UINT32 k = 0;
        for (; k < cap && start + k < items.size(); k++) (out[k] = items[start + k])->AddRef();
        *n = k;
        return S_OK;
    }
};
HRESULT ViewIterable::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); }
ULONG ViewIterable::AddRef() { return outer->AddRef(); }
ULONG ViewIterable::Release() { return outer->Release(); }
HRESULT ViewIterable::First(IIteratorAbi **out) { if (!out) return E_POINTER; *out = new ItemsIterator(outer->items); return S_OK; }


/* ---- IAsyncOperation<T> for an object T, born completed (Completed may be NULL: "no update for that product") ---- */
static const GUID IID_IAsyncOperation_Items_ = {0x9267e107,0x2ac6,0x5e0d,{0x86,0xe9,0x31,0x54,0xf6,0x16,0xc6,0x8b}};
static const GUID IID_IAsyncOperation_Item_  = {0x83b51cbf,0x35e0,0x59ad,{0xab,0x3e,0xff,0xb3,0xf0,0x37,0x04,0xf9}};
struct IAsyncObjAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IInspectable **) = 0;
};
struct AsyncObj;
struct AsyncObjInfo : IAsyncInfoAbi
{
    AsyncObj *outer;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.Foundation.IAsyncOperation`1", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Id(UINT32 *v) override { if (!v) return E_POINTER; *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(int *v) override;
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *v) override;
    HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};
/* Completed at once, or Started and completed later by complete() from a worker thread */
struct AsyncObj : IAsyncObjAbi
{
    LONG ref = 1;
    GUID iid;
    IInspectable *result;
    IUnknown *handler = nullptr;
    int status;        /* AsyncStatus: 0 Started, 1 Completed, 3 Error */
    HRESULT error = S_OK;
    SRWLOCK lock = SRWLOCK_INIT;
    AsyncObjInfo info;
    AsyncObj(REFIID i, IInspectable *r, bool pending = false) : iid(i), result(r), status(pending ? 0 : 1) { info.outer = this; }
    ~AsyncObj() { if (handler) handler->Release(); if (result) result->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(i) || IsEqualGUID(i, iid)) *out = static_cast<IAsyncObjAbi *>(this);
        else if (IsEqualGUID(i, IID_IAsyncInfo_)) *out = static_cast<IAsyncInfoAbi *>(&info);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.Foundation.IAsyncOperation`1")
    void invoke(IUnknown *h, int st)
    {
        /* a delegate: Invoke is slot 3 */
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IAsyncObjAbi *, int);
        ((invoke_fn)(*(void ***)h)[3])(h, this, st);
    }
    void complete(IInspectable *r, HRESULT hr)
    {
        AcquireSRWLockExclusive(&lock);
        result = r;
        error = hr;
        status = FAILED(hr) ? 3 : 1;
        IUnknown *h = handler;
        if (h) h->AddRef();
        int st = status;
        ReleaseSRWLockExclusive(&lock);
        if (h) { invoke(h, st); h->Release(); }
    }
    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *h) override
    {
        AcquireSRWLockExclusive(&lock);
        if (handler) handler->Release();
        if ((handler = h)) h->AddRef();
        int st = status;
        ReleaseSRWLockExclusive(&lock);
        if (h && st) invoke(h, st);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **h) override
    {
        if (!h) return E_POINTER;
        AcquireSRWLockShared(&lock);
        if ((*h = handler)) handler->AddRef();
        ReleaseSRWLockShared(&lock);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResults(IInspectable **r) override
    {
        if (!r) return E_POINTER;
        AcquireSRWLockShared(&lock);
        HRESULT hr = status == 1 ? S_OK : status == 3 ? error : E_ILLEGAL_METHOD_CALL;
        if ((*r = hr == S_OK ? result : nullptr)) result->AddRef();
        ReleaseSRWLockShared(&lock);
        return hr;
    }
};
HRESULT AsyncObjInfo::get_Status(int *v) { if (!v) return E_POINTER; AcquireSRWLockShared(&outer->lock); *v = outer->status; ReleaseSRWLockShared(&outer->lock); return S_OK; }
HRESULT AsyncObjInfo::get_ErrorCode(HRESULT *v) { if (!v) return E_POINTER; AcquireSRWLockShared(&outer->lock); *v = outer->error; ReleaseSRWLockShared(&outer->lock); return S_OK; }
HRESULT AsyncObjInfo::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); }
ULONG AsyncObjInfo::AddRef() { return outer->AddRef(); }
ULONG AsyncObjInfo::Release() { return outer->Release(); }

/* ---- AppUpdateOptions: a plain property bag ---- */
static const GUID IID_IAppUpdateOptions_  = {0x26f0b02f,0xc2f3,0x4aea,{0xaf,0x8c,0x63,0x08,0xdd,0x9d,0xb8,0x5f}};
static const GUID IID_IAppUpdateOptions2_ = {0xf4646e08,0xed26,0x4bf9,{0x96,0x79,0x48,0xf6,0x28,0xe5,0x3d,0xf8}};
struct IAUO1 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_CatalogId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_CatalogId(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AllowForcedAppRestart(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AllowForcedAppRestart(boolean) = 0;
};
struct IAUO2 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_AutomaticallyDownloadAndInstallUpdateIfFound(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutomaticallyDownloadAndInstallUpdateIfFound(boolean) = 0;
};
struct UpdateOptions;
struct UO2 : IAUO2
{
    UpdateOptions *outer;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppUpdateOptions", n); }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AutomaticallyDownloadAndInstallUpdateIfFound(boolean *v) override;
    HRESULT STDMETHODCALLTYPE put_AutomaticallyDownloadAndInstallUpdateIfFound(boolean v) override;
};
struct UpdateOptions : IAUO1
{
    LONG ref = 1;
    HSTRING catalog = nullptr;
    boolean forced_restart = FALSE, auto_install = FALSE;
    UO2 o2;
    UpdateOptions() { o2.outer = this; }
    ~UpdateOptions() { WindowsDeleteString(catalog); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IAppUpdateOptions_)) *out = static_cast<IAUO1 *>(this);
        else if (IsEqualGUID(iid, IID_IAppUpdateOptions2_)) *out = static_cast<IAUO2 *>(&o2);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppUpdateOptions")
    HRESULT STDMETHODCALLTYPE get_CatalogId(HSTRING *v) override { if (!v) return E_POINTER; return WindowsDuplicateString(catalog, v); }
    HRESULT STDMETHODCALLTYPE put_CatalogId(HSTRING v) override { WindowsDeleteString(catalog); return WindowsDuplicateString(v, &catalog); }
    HRESULT STDMETHODCALLTYPE get_AllowForcedAppRestart(boolean *v) override { if (!v) return E_POINTER; *v = forced_restart; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AllowForcedAppRestart(boolean v) override { forced_restart = v; return S_OK; }
};
HRESULT UO2::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); }
ULONG UO2::AddRef() { return outer->AddRef(); }
ULONG UO2::Release() { return outer->Release(); }
HRESULT UO2::get_AutomaticallyDownloadAndInstallUpdateIfFound(boolean *v) { if (!v) return E_POINTER; *v = outer->auto_install; return S_OK; }
HRESULT UO2::put_AutomaticallyDownloadAndInstallUpdateIfFound(boolean v) { outer->auto_install = v; return S_OK; }

/* ---- updates: what the Store's delivery service has that is newer than what is staged ----
 * The catalogue maps a package family to its Windows Update category, FE3 lists that category's packages, and
 * wine-uwp's installer (Wine's appxinstall.dll) stages the newest one over the running package's own
 * stage directory, the way shims/gameplatform installs a game. Only the calling package is looked at: the
 * Store's Library lists nothing else here. */
static const GUID IID_IAppInstallItem_  = {0x49d3dfab,0x168a,0x4cbf,{0xa9,0x3a,0x9e,0x44,0x8c,0x82,0x73,0x7d}};
static const GUID IID_IAppInstallItem2_ = {0xd3972af8,0x40c0,0x4fd7,{0xaa,0x6c,0x0a,0xa1,0x3c,0xa6,0x18,0x8c}};
static const GUID IID_IAppInstallItem3_ = {0x6f3dc998,0xdd47,0x433c,{0x92,0x34,0x56,0x01,0x72,0xd6,0x7a,0x45}};
static const GUID IID_IAppInstallItem4_ = {0xc2d1ce12,0x71ff,0x4fc8,{0xb5,0x40,0x45,0x3d,0x4b,0x37,0xe1,0xd1}};
static const GUID IID_IAppInstallItem5_ = {0x5510e7cc,0x4076,0x4a0b,{0x94,0x72,0xc2,0x1d,0x9d,0x38,0x0e,0x55}};
static const GUID IID_IAppInstallStatus_  = {0x936dccfa,0x2450,0x4126,{0x88,0xb1,0x61,0x27,0xa6,0x44,0xdd,0x5c}};
static const GUID IID_IAppInstallStatus2_ = {0x96e7818a,0x5e92,0x4aa9,{0x8e,0xdc,0x58,0xfe,0xd4,0xb8,0x7e,0x00}};
static const GUID IID_IAppInstallStatus3_ = {0xcb880c56,0x837b,0x4b4c,{0x9e,0xbb,0x6d,0x44,0xa0,0xa9,0x63,0x07}};
static const GUID IID_IAppInstallManagerItemEventArgs_ = {0xbc505743,0x4674,0x4dd1,{0x95,0x7e,0xc2,0x56,0x82,0x08,0x6a,0x14}};

enum { AIS_PENDING = 0, AIS_STARTING = 1, AIS_DOWNLOADING = 3, AIS_INSTALLING = 5, AIS_COMPLETED = 6, AIS_CANCELED = 7,
       AIS_PAUSED = 8, AIS_ERROR = 9, AIS_READY_TO_DOWNLOAD = 13 };

static std::wstring wide(const std::string &s)
{
    std::wstring w(s.size(), 0);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? n : 0);
    return w;
}
static std::wstring env_w(const wchar_t *name)
{
    WCHAR buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : std::wstring();
}
static UINT64 version_number(const std::string &v)
{
    unsigned p[4] = {};
    sscanf(v.c_str(), "%u.%u.%u.%u", &p[0], &p[1], &p[2], &p[3]);
    return ((UINT64)p[0] << 48) | ((UINT64)p[1] << 32) | ((UINT64)p[2] << 16) | p[3];
}

struct IAppInstallStatusAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_InstallState(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DownloadSizeInBytes(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BytesDownloaded(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PercentComplete(double *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *) = 0;
};
struct IAppInstallStatus2Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_User(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ReadyForLaunch(boolean *) = 0;
};
struct IAppInstallStatus3Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_IsStaged(boolean *) = 0;
};
struct InstallStatus;
#define STATUS_PART(n, abi)                                                                                        \
    struct n : abi                                                                                                 \
    {                                                                                                              \
        InstallStatus *outer;                                                                                      \
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;                                        \
        ULONG STDMETHODCALLTYPE AddRef() override;                                                                 \
        ULONG STDMETHODCALLTYPE Release() override;                                                                \
        HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; } \
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *s) override { return hstr(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallStatus", s); } \
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
STATUS_PART(Status2, IAppInstallStatus2Abi)
    HRESULT STDMETHODCALLTYPE get_User(IInspectable **u) override { if (!u) return E_POINTER; *u = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ReadyForLaunch(boolean *v) override;
};
STATUS_PART(Status3, IAppInstallStatus3Abi)
    HRESULT STDMETHODCALLTYPE get_IsStaged(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
};
/* a snapshot, as Windows' GetCurrentStatus hands out */
struct InstallStatus : IAppInstallStatusAbi
{
    LONG ref = 1;
    int state; UINT64 total, done; HRESULT error;
    Status2 s2; Status3 s3;
    InstallStatus(int st, UINT64 t, UINT64 d, HRESULT e) : state(st), total(t), done(d), error(e) { s2.outer = s3.outer = this; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IAppInstallStatus_)) *out = static_cast<IAppInstallStatusAbi *>(this);
        else if (IsEqualGUID(iid, IID_IAppInstallStatus2_)) *out = static_cast<IAppInstallStatus2Abi *>(&s2);
        else if (IsEqualGUID(iid, IID_IAppInstallStatus3_)) *out = static_cast<IAppInstallStatus3Abi *>(&s3);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallStatus")
    HRESULT STDMETHODCALLTYPE get_InstallState(int *v) override { if (!v) return E_POINTER; *v = state; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DownloadSizeInBytes(UINT64 *v) override { if (!v) return E_POINTER; *v = total; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BytesDownloaded(UINT64 *v) override { if (!v) return E_POINTER; *v = done; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_PercentComplete(double *v) override
    {
        if (!v) return E_POINTER;
        *v = state == AIS_COMPLETED ? 100.0 : total ? 100.0 * (double)done / (double)total : 0.0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *v) override { if (!v) return E_POINTER; *v = error; return S_OK; }
};
#define STATUS_FORWARD(n) \
    HRESULT n::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); } \
    ULONG n::AddRef() { return outer->AddRef(); } \
    ULONG n::Release() { return outer->Release(); }
STATUS_FORWARD(Status2)
STATUS_FORWARD(Status3)
HRESULT Status2::get_ReadyForLaunch(boolean *v) { if (!v) return E_POINTER; *v = outer->state == AIS_COMPLETED; return S_OK; }

struct IAppInstallItemAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_ProductId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PackageFamilyName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InstallType(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentStatus(IAppInstallStatusAbi **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause() = 0;
    virtual HRESULT STDMETHODCALLTYPE Restart() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Completed(IUnknown *, EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Completed(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_StatusChanged(IUnknown *, EventRegistrationToken *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StatusChanged(EventRegistrationToken) = 0;
};
struct IAppInstallItem2Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE Cancel(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE Restart(HSTRING) = 0;
};
struct IAppInstallItem3Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Children(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ItemOperationsMightAffectOtherItems(boolean *) = 0;
};
struct IAppInstallItem4Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_LaunchAfterInstall(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LaunchAfterInstall(boolean) = 0;
};
struct IAppInstallItem5Abi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_PinToDesktopAfterInstall(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PinToDesktopAfterInstall(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PinToStartAfterInstall(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PinToStartAfterInstall(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PinToTaskbarAfterInstall(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PinToTaskbarAfterInstall(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CompletedInstallToastNotificationMode(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_CompletedInstallToastNotificationMode(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InstallInProgressToastNotificationMode(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_InstallInProgressToastNotificationMode(int) = 0;
};

/* a registered handler; for the manager's events, with the manager that registered it (the sender) */
static LONG next_token = 1;
struct Handler { IUnknown *handler; IInspectable *sender; INT64 token; };
static SRWLOCK events_lock = SRWLOCK_INIT;
static std::vector<Handler> manager_completed, manager_status;

static HRESULT add_handler(std::vector<Handler> &list, IUnknown *h, IInspectable *sender, EventRegistrationToken *t)
{
    if (!t) return E_POINTER;
    if (!h) return E_INVALIDARG;
    t->value = InterlockedIncrement(&next_token);
    h->AddRef();
    if (sender) sender->AddRef();
    AcquireSRWLockExclusive(&events_lock);
    list.push_back({h, sender, t->value});
    ReleaseSRWLockExclusive(&events_lock);
    return S_OK;
}
static HRESULT remove_handler(std::vector<Handler> &list, EventRegistrationToken t)
{
    Handler gone = {};
    AcquireSRWLockExclusive(&events_lock);
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->token == t.value) { gone = *it; list.erase(it); break; }
    ReleaseSRWLockExclusive(&events_lock);
    if (gone.handler) gone.handler->Release();
    if (gone.sender) gone.sender->Release();
    return S_OK;
}
/* TypedEventHandler<S, A>::Invoke(sender, args), slot 3 */
static void raise(std::vector<Handler> &list, IInspectable *sender, IInspectable *args)
{
    AcquireSRWLockShared(&events_lock);
    std::vector<Handler> copy = list;
    for (auto &h : copy) { h.handler->AddRef(); if (h.sender) h.sender->AddRef(); }
    ReleaseSRWLockShared(&events_lock);
    typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IInspectable *, IInspectable *);
    for (auto &h : copy)
    {
        ((invoke_fn)(*(void ***)h.handler)[3])(h.handler, h.sender ? h.sender : sender, args);
        h.handler->Release();
        if (h.sender) h.sender->Release();
    }
}

struct UpdateItem;
static UpdateItem *find_item(const std::wstring &family);

#define ITEM_PART(n, abi)                                                                                          \
    struct n : abi                                                                                                 \
    {                                                                                                              \
        UpdateItem *outer;                                                                                         \
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;                                        \
        ULONG STDMETHODCALLTYPE AddRef() override;                                                                 \
        ULONG STDMETHODCALLTYPE Release() override;                                                                \
        HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **ids) override { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; } \
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *s) override { return hstr(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem", s); } \
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }
ITEM_PART(Item2, IAppInstallItem2Abi)
    HRESULT STDMETHODCALLTYPE Cancel(HSTRING) override;
    HRESULT STDMETHODCALLTYPE Pause(HSTRING) override;
    HRESULT STDMETHODCALLTYPE Restart(HSTRING) override;
};
ITEM_PART(Item3, IAppInstallItem3Abi)
    HRESULT STDMETHODCALLTYPE get_Children(void **out) override { if (!out) return E_POINTER; *out = static_cast<IVectorViewAbi *>(new ItemsView()); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ItemOperationsMightAffectOtherItems(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
};
ITEM_PART(Item4, IAppInstallItem4Abi)
    HRESULT STDMETHODCALLTYPE get_LaunchAfterInstall(boolean *v) override;
    HRESULT STDMETHODCALLTYPE put_LaunchAfterInstall(boolean v) override;
};
ITEM_PART(Item5, IAppInstallItem5Abi)
    HRESULT STDMETHODCALLTYPE get_PinToDesktopAfterInstall(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_PinToDesktopAfterInstall(boolean) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_PinToStartAfterInstall(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_PinToStartAfterInstall(boolean) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_PinToTaskbarAfterInstall(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_PinToTaskbarAfterInstall(boolean) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_CompletedInstallToastNotificationMode(int *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_CompletedInstallToastNotificationMode(int) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_InstallInProgressToastNotificationMode(int *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_InstallInProgressToastNotificationMode(int) override { return S_OK; }
};

struct UpdateItem : IAppInstallItemAbi
{
    LONG ref = 1;
    std::wstring product_id, family, stage_unix;
    std::string url, version;
    /* under lock */
    SRWLOCK lock = SRWLOCK_INIT;
    int state = AIS_READY_TO_DOWNLOAD;
    UINT64 total = 0, done = 0;
    HRESULT error = S_OK;
    bool started = false, cancel = false, launch_after = false;
    volatile LONG cancel_flag = 0;      /* what the installer polls */
    std::vector<Handler> completed, status_changed;
    Item2 i2; Item3 i3; Item4 i4; Item5 i5;
    UpdateItem() { i2.outer = i3.outer = i4.outer = i5.outer = this; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IAppInstallItem_)) *out = static_cast<IAppInstallItemAbi *>(this);
        else if (IsEqualGUID(iid, IID_IAppInstallItem2_)) *out = static_cast<IAppInstallItem2Abi *>(&i2);
        else if (IsEqualGUID(iid, IID_IAppInstallItem3_)) *out = static_cast<IAppInstallItem3Abi *>(&i3);
        else if (IsEqualGUID(iid, IID_IAppInstallItem4_)) *out = static_cast<IAppInstallItem4Abi *>(&i4);
        else if (IsEqualGUID(iid, IID_IAppInstallItem5_)) *out = static_cast<IAppInstallItem5Abi *>(&i5);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    INSPECTABLE(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallItem")
    HRESULT STDMETHODCALLTYPE get_ProductId(HSTRING *v) override { if (!v) return E_POINTER; return hstr(product_id.c_str(), v); }
    HRESULT STDMETHODCALLTYPE get_PackageFamilyName(HSTRING *v) override { if (!v) return E_POINTER; return hstr(family.c_str(), v); }
    HRESULT STDMETHODCALLTYPE get_InstallType(int *v) override { if (!v) return E_POINTER; *v = 1; /* Update */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsUserInitiated(boolean *v) override { if (!v) return E_POINTER; *v = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentStatus(IAppInstallStatusAbi **out) override
    {
        if (!out) return E_POINTER;
        AcquireSRWLockShared(&lock);
        *out = new InstallStatus(state, total, done, error);
        ReleaseSRWLockShared(&lock);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Cancel() override;
    HRESULT STDMETHODCALLTYPE Pause() override { trace("item %ls: Pause (not supported, keeps going)", family.c_str()); return S_OK; }
    HRESULT STDMETHODCALLTYPE Restart() override { trace("item %ls: Restart", family.c_str()); start(); return S_OK; }
    HRESULT STDMETHODCALLTYPE add_Completed(IUnknown *h, EventRegistrationToken *t) override { return add_handler(completed, h, nullptr, t); }
    HRESULT STDMETHODCALLTYPE remove_Completed(EventRegistrationToken t) override { return remove_handler(completed, t); }
    HRESULT STDMETHODCALLTYPE add_StatusChanged(IUnknown *h, EventRegistrationToken *t) override { return add_handler(status_changed, h, nullptr, t); }
    HRESULT STDMETHODCALLTYPE remove_StatusChanged(EventRegistrationToken t) override { return remove_handler(status_changed, t); }

    void set_state(int st, HRESULT hr = S_OK);
    void start();
    static DWORD WINAPI install_thread(void *param);
};
#define ITEM_FORWARD(n) \
    HRESULT n::QueryInterface(REFIID iid, void **out) { return outer->QueryInterface(iid, out); } \
    ULONG n::AddRef() { return outer->AddRef(); } \
    ULONG n::Release() { return outer->Release(); }
ITEM_FORWARD(Item2)
ITEM_FORWARD(Item3)
ITEM_FORWARD(Item4)
ITEM_FORWARD(Item5)
HRESULT Item2::Cancel(HSTRING) { return outer->Cancel(); }
HRESULT Item2::Pause(HSTRING) { return outer->Pause(); }
HRESULT Item2::Restart(HSTRING) { return outer->Restart(); }
HRESULT Item4::get_LaunchAfterInstall(boolean *v) { if (!v) return E_POINTER; *v = outer->launch_after; return S_OK; }
HRESULT Item4::put_LaunchAfterInstall(boolean v) { outer->launch_after = v; return S_OK; }

/* AppInstallManagerItemEventArgs */
struct IItemEventArgsAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE get_Item(IAppInstallItemAbi **) = 0; };
struct ItemEventArgs : IItemEventArgsAbi
{
    LONG ref = 1;
    UpdateItem *item;
    explicit ItemEventArgs(UpdateItem *i) : item(i) { item->AddRef(); }
    ~ItemEventArgs() { item->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (!base_iid(iid) && !IsEqualGUID(iid, IID_IAppInstallManagerItemEventArgs_)) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IItemEventArgsAbi *>(this); AddRef(); return S_OK;
    }
    INSPECTABLE(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManagerItemEventArgs")
    HRESULT STDMETHODCALLTYPE get_Item(IAppInstallItemAbi **v) override { if (!v) return E_POINTER; (*v = item)->AddRef(); return S_OK; }
};

/* the items the manager knows of: found updates and running installs, shared by every AppInstallManager */
static SRWLOCK items_lock = SRWLOCK_INIT;
static std::vector<UpdateItem *> items;

static UpdateItem *find_item(const std::wstring &family)
{
    UpdateItem *found = nullptr;
    AcquireSRWLockShared(&items_lock);
    for (auto *i : items)
        if (i->family == family) { (found = i)->AddRef(); break; }
    ReleaseSRWLockShared(&items_lock);
    return found;
}
static std::vector<IInspectable *> active_items(void)
{
    std::vector<IInspectable *> out;
    AcquireSRWLockShared(&items_lock);
    for (auto *i : items)
    {
        AcquireSRWLockShared(&i->lock);
        bool active = i->started && i->state != AIS_COMPLETED && i->state != AIS_CANCELED && i->state != AIS_ERROR;
        ReleaseSRWLockShared(&i->lock);
        if (active) out.push_back(static_cast<IAppInstallItemAbi *>(i));
    }
    ReleaseSRWLockShared(&items_lock);
    return out;
}

void UpdateItem::set_state(int st, HRESULT hr)
{
    AcquireSRWLockExclusive(&lock);
    state = st;
    if (FAILED(hr)) error = hr;
    ReleaseSRWLockExclusive(&lock);
    ItemEventArgs *args = new ItemEventArgs(this);
    IInspectable *self = static_cast<IAppInstallItemAbi *>(this);
    raise(status_changed, self, nullptr);
    raise(manager_status, nullptr, static_cast<IItemEventArgsAbi *>(args));
    if (st == AIS_COMPLETED || st == AIS_CANCELED || st == AIS_ERROR)
    {
        raise(completed, self, nullptr);
        raise(manager_completed, nullptr, static_cast<IItemEventArgsAbi *>(args));
    }
    args->Release();
}

HRESULT UpdateItem::Cancel()
{
    trace("item %ls: Cancel", family.c_str());
    AcquireSRWLockExclusive(&lock);
    cancel = true;
    InterlockedExchange(&cancel_flag, 1);
    bool running = started && state != AIS_COMPLETED && state != AIS_ERROR && state != AIS_CANCELED;
    ReleaseSRWLockExclusive(&lock);
    if (!running) set_state(AIS_CANCELED);
    return S_OK;
}

void UpdateItem::start()
{
    AcquireSRWLockExclusive(&lock);
    bool go = !started || state == AIS_ERROR || state == AIS_CANCELED;
    if (go) { started = true; cancel = false; cancel_flag = 0; state = AIS_PENDING; done = 0; error = S_OK; }
    ReleaseSRWLockExclusive(&lock);
    if (!go) return;
    AddRef();
    HANDLE t = CreateThread(nullptr, 0, install_thread, this, 0, nullptr);
    if (t) CloseHandle(t);
    else { Release(); set_state(AIS_ERROR, HRESULT_FROM_WIN32(GetLastError())); }
}

struct InstallerProgress
{
    UpdateItem *it;
    int last_state;
    ULONGLONG report_t0;
    bool installed;
    std::string last;               /* the installer's last message: why, when it fails */
};

static void CALLBACK installer_event(void *context, enum appx_event event, UINT64 completed, UINT64 total, const WCHAR *text)
{
    InstallerProgress *p = (InstallerProgress *)context;
    UpdateItem *it = p->it;

    switch (event)
    {
    case APPX_EVENT_STAGE:
        trace("install: %ls", text);
        if (p->last_state == AIS_DOWNLOADING && wcsstr(text, L"framework")) it->set_state(p->last_state = AIS_INSTALLING);
        break;
    case APPX_EVENT_MESSAGE: trace("install: %ls", text); p->last = utf8(text); break;
    case APPX_EVENT_INSTALLED: p->installed = true; break;
    case APPX_EVENT_PROGRESS:
    {
        if (!total) break;
        AcquireSRWLockExclusive(&it->lock);
        it->total = total;
        it->done = (std::min)(completed, total);
        ReleaseSRWLockExclusive(&it->lock);
        ULONGLONG now = GetTickCount64();
        if (p->last_state != AIS_DOWNLOADING || now - p->report_t0 >= 500 || completed >= total)
        {
            p->report_t0 = now;
            p->last_state = AIS_DOWNLOADING;
            it->set_state(AIS_DOWNLOADING);
        }
        break;
    }
    }
}

/* a Unix path as a Windows one */
static std::wstring dos_path(std::wstring unix_path)
{
    std::replace(unix_path.begin(), unix_path.end(), L'/', L'\\');
    return L"Z:" + unix_path;
}

/* wine-uwp's installer stages the package over our own stage directory (stage directories are DATADIR\stage\NAME) */
DWORD WINAPI UpdateItem::install_thread(void *param)
{
    typedef HRESULT (CDECL *install_fn)(const struct appx_install_params *);
    UpdateItem *it = (UpdateItem *)param;
    std::wstring tree = env_w(L"WINE_UWP_WINDOWS_TREE"), root = env_w(L"WINE_UWP_ROOT");
    std::wstring shim_dir = env_w(L"SHIM_DIR"), uwp_bin = env_w(L"UWP_BIN");
    size_t slash = it->stage_unix.rfind(L'/');
    std::wstring stage_name = it->stage_unix.substr(slash + 1), stage_root = it->stage_unix.substr(0, slash);
    std::wstring data = stage_root.substr(0, stage_root.rfind(L'/'));
    HMODULE lib = LoadLibraryW(L"appxinstall.dll");
    install_fn appx_install = lib ? (install_fn)GetProcAddress(lib, "appx_install") : nullptr;
    if (!appx_install || tree.empty() || slash == std::wstring::npos || stage_root.compare(stage_root.size() - 6, 6, L"/stage"))
    {
        trace("install: no appxinstall.dll, Windows tree or stage directory (%ls)", it->stage_unix.c_str());
        it->set_state(AIS_ERROR, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        it->Release();
        return 0;
    }
    std::wstring package = wide(it->url), data_dos = dos_path(data), tree_dos = dos_path(tree);
    std::wstring defaults = root.empty() ? L"" : dos_path(root + L"/launch-defaults.env");
    std::wstring launcher = !shim_dir.empty() ? dos_path(shim_dir + L"/wineuwp-launch.exe")
                          : !uwp_bin.empty() ? dos_path(uwp_bin + L"/build-probes/wineuwp-launch.exe") : L"";
    InstallerProgress progress = { it, AIS_STARTING };
    struct appx_install_params params = {};
    params.package = package.c_str();
    params.data_dir = data_dos.c_str();
    params.windows_tree = tree_dos.c_str();
    params.stage = stage_name.c_str();
    params.launch_defaults = defaults.empty() ? nullptr : defaults.c_str();
    params.launcher = launcher.empty() ? nullptr : launcher.c_str();
    params.cancel = &it->cancel_flag;
    params.callback = installer_event;
    params.context = &progress;
    it->set_state(AIS_STARTING);
    trace("install: %ls %s into stage %ls", it->family.c_str(), it->version.c_str(), stage_name.c_str());
    HRESULT hr = appx_install(&params);
    AcquireSRWLockExclusive(&it->lock);
    bool canceled = it->cancel;
    ReleaseSRWLockExclusive(&it->lock);
    trace("install: installer -> %#lx%s", (unsigned long)hr, progress.installed ? ", installed" : "");
    if (SUCCEEDED(hr) && progress.installed) it->set_state(AIS_COMPLETED);
    else if (canceled) it->set_state(AIS_CANCELED);
    else
    {
        if (!progress.last.empty()) trace("install: %s", progress.last.c_str());
        it->set_state(AIS_ERROR, FAILED(hr) ? hr : E_FAIL);
    }
    it->Release();
    return 0;
}

/* the running package: family and version from its full name, stage directory from WINE_UWP_PACKAGE_PATH */
static bool own_package(std::wstring &family, std::string &version, std::wstring &stage_unix)
{
    std::wstring full = env_w(L"WINE_UWP_PACKAGE_FULL_NAME"), path = env_w(L"WINE_UWP_PACKAGE_PATH");
    /* Name_Version_Arch_ResourceId_PublisherId */
    size_t a = full.find(L'_'), b = a == std::wstring::npos ? a : full.find(L'_', a + 1), p = full.rfind(L'_');
    if (b == std::wstring::npos || path.size() < 3 || path.compare(0, 2, L"Z:")) return false;
    family = full.substr(0, a) + full.substr(p);
    version = utf8(full.substr(a + 1, b - a - 1));
    stage_unix = path.substr(2);
    std::replace(stage_unix.begin(), stage_unix.end(), L'\\', L'/');
    while (!stage_unix.empty() && stage_unix.back() == L'/') stage_unix.pop_back();
    return true;
}

/* the catalogue: a package family's product id and Windows Update category */
static bool catalog_lookup(const std::wstring &family, std::wstring &product_id, std::string &category)
{
    DWORD status = 0;
    std::string body = http_get(L"displaycatalog.mp.microsoft.com", L"/v7.0/products/lookup?alternateId=PackageFamilyName&value=" + family +
                                L"&market=US&languages=en-US&fieldsTemplate=details", &status);
    Json j = parse_json(body);
    const Json &prod = j["Products"].at(0);
    if (status != 200 || prod.type != Json::OBJ) { trace("catalogue: HTTP %lu, no product for %ls", status, family.c_str()); return false; }
    product_id = wide(prod["ProductId"].str());
    const Json &skus = prod["DisplaySkuAvailabilities"];
    for (size_t i = 0; i < skus.a.size() && category.empty(); i++)
        category = skus.at(i)["Sku"]["Properties"]["FulfillmentData"]["WuCategoryId"].str();
    return !category.empty();
}

/* a search: the item for an update newer than what is staged, or nothing */
static UpdateItem *search_update(bool auto_install)
{
    std::wstring family, stage, product;
    std::string installed, category;
    if (!own_package(family, installed, stage)) { trace("search: no package identity"); return nullptr; }
    if (UpdateItem *existing = find_item(family))
    {
        if (auto_install) existing->start();
        return existing;
    }
    if (!catalog_lookup(family, product, category)) return nullptr;
    std::vector<Fe3Update> all;
    Fe3Update best;
    HRESULT hr = fe3_packages(category, all);
    if (FAILED(hr) || !pick_package(all, family, best)) { trace("search: %ls: FE3 has no package (hr %#lx)", family.c_str(), hr); return nullptr; }
    std::string rest = best.moniker.substr(best.moniker.find('_') + 1);
    std::string newest = rest.substr(0, rest.find('_'));
    trace("search: %ls installed %s, Store has %s (%s)", family.c_str(), installed.c_str(), newest.c_str(), best.moniker.c_str());
    if (version_number(newest) <= version_number(installed)) return nullptr;
    std::string url = fe3_file_url(best);
    if (url.empty()) return nullptr;
    UpdateItem *it = new UpdateItem();
    it->product_id = product;
    it->family = family;
    it->stage_unix = stage;
    it->url = url;
    it->version = newest;
    AcquireSRWLockExclusive(&items_lock);
    items.push_back(it);
    it->AddRef();
    ReleaseSRWLockExclusive(&items_lock);
    if (auto_install) it->start();
    return it;
}

struct SearchJob { AsyncObj *op; bool all, auto_install; };
static DWORD WINAPI search_thread(void *param)
{
    SearchJob *job = (SearchJob *)param;
    UpdateItem *it = search_update(job->auto_install);
    IInspectable *result;
    if (job->all)
    {
        std::vector<IInspectable *> found;
        if (it) found.push_back(static_cast<IAppInstallItemAbi *>(it));
        result = static_cast<IVectorViewAbi *>(new ItemsView(found));
        if (it) it->Release();
    }
    else result = it ? static_cast<IAppInstallItemAbi *>(it) : nullptr;
    trace("search: %s", it ? "an update" : "no updates");
    job->op->complete(result, S_OK);
    job->op->Release();
    delete job;
    return 0;
}
static bool options_auto_install(void *options)
{
    if (!options) return false;
    IAUO2 *o2 = nullptr;
    boolean v = FALSE;
    if (SUCCEEDED(((IInspectable *)options)->QueryInterface(IID_IAppUpdateOptions2_, (void **)&o2)))
    {
        o2->get_AutomaticallyDownloadAndInstallUpdateIfFound(&v);
        o2->Release();
    }
    return v;
}
static HRESULT search_async(void **out, bool all, void *options, const char *what)
{
    if (!out) return E_POINTER;
    bool auto_install = options_auto_install(options);
    trace("%s%s", what, auto_install ? " (install if found)" : "");
    AsyncObj *op = new AsyncObj(all ? IID_IAsyncOperation_Items_ : IID_IAsyncOperation_Item_, nullptr, true);
    op->AddRef();
    SearchJob *job = new SearchJob{op, all, auto_install};
    HANDLE t = CreateThread(nullptr, 0, search_thread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else { op->complete(all ? static_cast<IVectorViewAbi *>(new ItemsView()) : nullptr, S_OK); op->Release(); delete job; }
    *out = static_cast<IAsyncObjAbi *>(op);
    return S_OK;
}
static HRESULT update_family_async(HSTRING family, void **out, const char *what)
{
    if (!out) return E_POINTER;
    std::wstring own, stage; std::string version;
    const wchar_t *asked = WindowsGetStringRawBuffer(family, nullptr);
    if (!own_package(own, version, stage) || own != asked)
    {
        trace("%s(%ls): not the running package, no update", what, asked);
        *out = static_cast<IAsyncObjAbi *>(new AsyncObj(IID_IAsyncOperation_Item_, nullptr));
        return S_OK;
    }
    return search_async(out, false, nullptr, what) == S_OK ? S_OK : E_FAIL;
}

static HRESULT current_items(void **out, const char *what)
{
    if (!out) return E_POINTER;
    std::vector<IInspectable *> v = active_items();
    trace("%s -> %u in progress", what, (unsigned)v.size());
    *out = static_cast<IVectorViewAbi *>(new ItemsView(v));
    return S_OK;
}

/* the manager's Cancel/Pause/Restart name an item by product id */
static HRESULT by_product(HSTRING id, int op, const char *what)
{
    const wchar_t *pid = WindowsGetStringRawBuffer(id, nullptr);
    UpdateItem *found = nullptr;
    AcquireSRWLockShared(&items_lock);
    for (auto *i : items)
        if (!_wcsicmp(i->product_id.c_str(), pid)) { (found = i)->AddRef(); break; }
    ReleaseSRWLockShared(&items_lock);
    trace("%s(%ls)%s", what, pid, found ? "" : ": no such item");
    if (!found) return S_OK;
    if (op == 0) found->Cancel(); else if (op == 1) found->Pause(); else found->Restart();
    found->Release();
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

static int auto_update_setting = 1; /* AutoUpdateSetting.Enabled */

struct Manager;
struct M2 : IAIM2
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE StartAppInstallAsync(HSTRING, HSTRING, boolean, boolean, HSTRING, HSTRING, HSTRING, void **) override REFUSE("StartAppInstallAsync(2)")
    HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameAsync(HSTRING f, HSTRING, void **out) override { return update_family_async(f, out, "UpdateAppByPackageFamilyNameAsync(2)"); }
    HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, HSTRING, HSTRING, void **out) override { return search_async(out, false, nullptr, "SearchForUpdatesAsync(2)"); }
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(HSTRING, void **out) override { return search_async(out, true, nullptr, "SearchForAllUpdatesAsync(2)"); }
    HRESULT STDMETHODCALLTYPE GetIsAppAllowedToInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsAppAllowedToInstallAsync(2)"); }
    HRESULT STDMETHODCALLTYPE Cancel(HSTRING id, HSTRING) override { return by_product(id, 0, "Cancel(2)"); }
    HRESULT STDMETHODCALLTYPE Pause(HSTRING id, HSTRING) override { return by_product(id, 1, "Pause(2)"); }
    HRESULT STDMETHODCALLTYPE Restart(HSTRING id, HSTRING) override { return by_product(id, 2, "Restart(2)"); }
};
struct M3 : IAIM3
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE StartProductInstallAsync(HSTRING, HSTRING, HSTRING, HSTRING, boolean, boolean, HSTRING, P, void **) override REFUSE("StartProductInstallAsync(3)")
    HRESULT STDMETHODCALLTYPE StartProductInstallForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, boolean, boolean, HSTRING, P, void **) override REFUSE("StartProductInstallForUserAsync(3)")
    HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameForUserAsync(P, HSTRING f, HSTRING, void **out) override { return update_family_async(f, out, "UpdateAppByPackageFamilyNameForUserAsync"); }
    HRESULT STDMETHODCALLTYPE SearchForUpdatesForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, void **out) override { return search_async(out, false, nullptr, "SearchForUpdatesForUserAsync(3)"); }
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesForUserAsync(P, HSTRING, void **out) override { return search_async(out, true, nullptr, "SearchForAllUpdatesForUserAsync(3)"); }
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
    HRESULT STDMETHODCALLTYPE get_AppInstallItemsWithGroupSupport(void **out) override { return current_items(out, "AppInstallItemsWithGroupSupport"); }
};
struct M6 : IAIM6
{
    IAIM1 *outer;
    FORWARD_INSPECTABLE
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(HSTRING, HSTRING, P opt, void **out) override { return search_async(out, true, opt, "SearchForAllUpdatesAsync(6)"); }
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesForUserAsync(P, HSTRING, HSTRING, P opt, void **out) override { return search_async(out, true, opt, "SearchForAllUpdatesForUserAsync(6)"); }
    HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, HSTRING, HSTRING, P opt, void **out) override { return search_async(out, false, opt, "SearchForUpdatesAsync(6)"); }
    HRESULT STDMETHODCALLTYPE SearchForUpdatesForUserAsync(P, HSTRING, HSTRING, HSTRING, HSTRING, P opt, void **out) override { return search_async(out, false, opt, "SearchForUpdatesForUserAsync(6)"); }
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
    HRESULT STDMETHODCALLTYPE get_AppInstallItems(void **out) override { return current_items(out, "AppInstallItems"); }
    HRESULT STDMETHODCALLTYPE Cancel(HSTRING id) override { return by_product(id, 0, "Cancel"); }
    HRESULT STDMETHODCALLTYPE Pause(HSTRING id) override { return by_product(id, 1, "Pause"); }
    HRESULT STDMETHODCALLTYPE Restart(HSTRING id) override { return by_product(id, 2, "Restart"); }
        HRESULT STDMETHODCALLTYPE add_ItemCompleted(IUnknown *h, EventRegistrationToken *t) override { return add_handler(manager_completed, h, static_cast<IAIM1 *>(this), t); }
    HRESULT STDMETHODCALLTYPE remove_ItemCompleted(EventRegistrationToken t) override { return remove_handler(manager_completed, t); }
    HRESULT STDMETHODCALLTYPE add_ItemStatusChanged(IUnknown *h, EventRegistrationToken *t) override { return add_handler(manager_status, h, static_cast<IAIM1 *>(this), t); }
    HRESULT STDMETHODCALLTYPE remove_ItemStatusChanged(EventRegistrationToken t) override { return remove_handler(manager_status, t); }
    HRESULT STDMETHODCALLTYPE get_AutoUpdateSetting(int *v) override { if (!v) return E_POINTER; *v = auto_update_setting; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AutoUpdateSetting(int v) override { auto_update_setting = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AcquisitionIdentity(HSTRING *v) override { if (!v) return E_POINTER; return WindowsDuplicateString(acquisition, v); }
    HRESULT STDMETHODCALLTYPE put_AcquisitionIdentity(HSTRING v) override { WindowsDeleteString(acquisition); return WindowsDuplicateString(v, &acquisition); }
    HRESULT STDMETHODCALLTYPE GetIsApplicableAsync(HSTRING, HSTRING, void **out) override { return async_bool(true, out, "GetIsApplicableAsync"); }
    HRESULT STDMETHODCALLTYPE StartAppInstallAsync(HSTRING, HSTRING, boolean, boolean, void **) override REFUSE("StartAppInstallAsync")
    HRESULT STDMETHODCALLTYPE UpdateAppByPackageFamilyNameAsync(HSTRING f, void **out) override { return update_family_async(f, out, "UpdateAppByPackageFamilyNameAsync"); }
    HRESULT STDMETHODCALLTYPE SearchForUpdatesAsync(HSTRING, HSTRING, void **out) override { return search_async(out, false, nullptr, "SearchForUpdatesAsync"); }
    HRESULT STDMETHODCALLTYPE SearchForAllUpdatesAsync(void **out) override { return search_async(out, true, nullptr, "SearchForAllUpdatesAsync"); }
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

struct OptionsFactory : Factory
{
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { return hstr(L"Windows.ApplicationModel.Store.Preview.InstallControl.AppUpdateOptions", n); }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        trace("new AppUpdateOptions");
        *out = static_cast<IAUO1 *>(new UpdateOptions());
        return S_OK;
    }
};
static OptionsFactory options_factory;

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING cls, IActivationFactory **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    const wchar_t *name = WindowsGetStringRawBuffer(cls, nullptr);
    if (!wcscmp(name, L"Windows.ApplicationModel.Store.Preview.InstallControl.AppUpdateOptions")) { *out = &options_factory; return S_OK; }
    if (wcscmp(name, L"Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager")) return CLASS_E_CLASSNOTAVAILABLE;
    *out = &factory;
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

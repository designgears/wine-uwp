// Windows.System.User: the local user, as a stand-in.
//
// On Windows the User class is served out of process by the UserManager service ("Server":
// "UserManager" in the hive), which does not exist under Wine, so RoGetActivationFactory failed
// with REGDB_E_CLASSNOTREG. The Xbox app asks for it to find the shell user before signing in
// ("GetShellUserAsync failed"), and without it its app configuration never loads.
//
// There is exactly one user here, the one running the prefix, signed in locally. That is what
// this answers:
//   IUserStatics  {155eb23b-...}: 7/8/9 FindAllAsync -> [user], 10 GetFromId -> user,
//                                 6 CreateWatcher -> E_NOTIMPL (traced)
//   IUserStatics2 {74a37e11-...}: 6 GetDefault -> user
//   IUser         {df9a26c6-...}: NonRoamableId = the account's SID string, AuthenticationStatus =
//                                 LocallyAuthenticated, Type = LocalUser, GetPropertyAsync /
//                                 GetPropertiesAsync from the account name, GetPictureAsync -> null
//   IUser2        {98ba5628-...}: CheckUserAgeConsentGroupAsync -> E_NOTIMPL (traced)
// The asynchronous answers are born completed (shims/taskbarmanager explains why).
//
// PIIDs come from piid.py --signature (control: IVector<String> = 98b9acc1-..., correct).
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <sddl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IUserStatics = {0x155eb23b,0x242a,0x45e0,{0xa2,0xe9,0x31,0x71,0xfc,0x6a,0x7f,0xdd}};
static const GUID IID_IUserStatics2 = {0x74a37e11,0x2eb5,0x4487,{0xb0,0xd5,0x2c,0x67,0x90,0xe0,0x13,0xe9}};
static const GUID IID_IUser = {0xdf9a26c6,0xe746,0x4bcd,{0xb5,0xd4,0x12,0x01,0x03,0xc4,0x20,0x9b}};
static const GUID IID_IUser2 = {0x98ba5628,0xa6e3,0x518e,{0x89,0xd9,0xd3,0xb2,0xb1,0x99,0x1a,0x10}};
static const GUID IID_IVectorView_User = {0x8cbd762a,0x1222,0x5ee5,{0xb7,0x45,0x48,0x9e,0x7a,0x42,0xc6,0xec}};
static const GUID IID_IIterable_User = {0xd1bacd1f,0x0376,0x5823,{0x8c,0x29,0x1d,0x45,0xb9,0xf4,0xc1,0x91}};
static const GUID IID_IIterator_User = {0x326fe162,0x582b,0x5659,{0xb8,0xa4,0x68,0xff,0x0f,0x52,0x57,0x45}};
static const GUID IID_IAsyncOperation_IVectorView_User = {0xe44ea1df,0xbb85,0x5a8c,{0xbd,0xdc,0xc8,0xe9,0x60,0xc3,0x55,0xc9}};
static const GUID IID_IAsyncOperation_Object = {0xabf53c57,0xee50,0x5342,{0xb5,0x2a,0x26,0xe3,0xb8,0xcc,0x02,0x4f}};
static const GUID IID_IAsyncOperation_IPropertySet = {0x490b0686,0xafd7,0x5037,{0x96,0x47,0xd8,0xfe,0x24,0x8f,0x18,0x2c}};
static const GUID IID_IPropertyValueStatics_ = {0x629bdbc8,0xd932,0x4ff4,{0x96,0xb9,0x8d,0x96,0xc5,0xc1,0xe8,0x58}};
static const GUID IID_IVectorView_String = {0x2f13c006,0xa03a,0x5f69,{0xb0,0x90,0x75,0xa4,0x3e,0x33,0x42,0x3e}};
static const GUID IID_IMap_String_Object = {0x1b0d3570,0x0877,0x5ec2,{0x8a,0x2c,0x3b,0x95,0x39,0x50,0x6a,0xca}};

static void trace(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[user] %s\n", buf);
    fflush(stderr);
}

static HRESULT hstr(const WCHAR *s, HSTRING *out) { return WindowsCreateString(s, (UINT32)wcslen(s), out); }
static HRESULT WINAPI common_GetIids(void *, ULONG *c, IID **ids) { if (c) *c = 0; if (ids) *ids = nullptr; return S_OK; }
static HRESULT WINAPI common_GetTrustLevel(void *, TrustLevel *l) { if (!l) return E_POINTER; *l = BaseTrust; return S_OK; }

/* ------------------------------------------------------------------------------------------- */
/* A completed IAsyncOperation<T> for an interface-typed T                                     */

struct Async
{
    const void **op_vtbl;
    const void **info_vtbl;
    LONG ref;
    GUID iid;
    IUnknown *result;
    IUnknown *handler;
};
static const void *async_op_vtbl[9];
static const void *async_info_vtbl[11];
static Async *from_op(void *i) { return (Async *)i; }
static Async *from_info(void *i) { return (Async *)((char *)i - sizeof(void *)); }

static HRESULT async_qi(Async *a, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, a->iid))
    {
        InterlockedIncrement(&a->ref);
        *out = &a->op_vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IAsyncInfo_))
    {
        InterlockedIncrement(&a->ref);
        *out = &a->info_vtbl;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG async_release(Async *a)
{
    ULONG ref = InterlockedDecrement(&a->ref);
    if (!ref)
    {
        if (a->handler) a->handler->Release();
        if (a->result) a->result->Release();
        free(a);
    }
    return ref;
}
static HRESULT WINAPI aop_QueryInterface(void *i, REFIID iid, void **o) { return async_qi(from_op(i), iid, o); }
static ULONG WINAPI aop_AddRef(void *i) { return InterlockedIncrement(&from_op(i)->ref); }
static ULONG WINAPI aop_Release(void *i) { return async_release(from_op(i)); }
static HRESULT WINAPI ainfo_QueryInterface(void *i, REFIID iid, void **o) { return async_qi(from_info(i), iid, o); }
static ULONG WINAPI ainfo_AddRef(void *i) { return InterlockedIncrement(&from_info(i)->ref); }
static ULONG WINAPI ainfo_Release(void *i) { return async_release(from_info(i)); }
static HRESULT WINAPI async_GetRuntimeClassName(void *, HSTRING *n) { return hstr(L"Windows.Foundation.IAsyncOperation`1", n); }

typedef HRESULT (WINAPI *completed_invoke)(void *handler, void *async, int status);
static HRESULT WINAPI aop_put_Completed(void *iface, IUnknown *handler)
{
    Async *a = from_op(iface);
    if (a->handler) { a->handler->Release(); a->handler = nullptr; }
    if (handler)
    {
        handler->AddRef();
        a->handler = handler;
        ((completed_invoke)(*(void ***)handler)[3])(handler, &a->op_vtbl, 1 /* Completed */);
    }
    return S_OK;
}
static HRESULT WINAPI aop_get_Completed(void *iface, IUnknown **handler)
{
    Async *a = from_op(iface);
    if (!handler) return E_POINTER;
    if ((*handler = a->handler)) a->handler->AddRef();
    return S_OK;
}
static HRESULT WINAPI aop_GetResults(void *iface, IUnknown **result)
{
    Async *a = from_op(iface);
    if (!result) return E_POINTER;
    if ((*result = a->result)) a->result->AddRef();
    return S_OK;
}
static HRESULT WINAPI ainfo_get_Id(void *, UINT32 *id) { *id = 1; return S_OK; }
static HRESULT WINAPI ainfo_get_Status(void *, int *s) { *s = 1; return S_OK; }
static HRESULT WINAPI ainfo_get_ErrorCode(void *, HRESULT *e) { *e = S_OK; return S_OK; }
static HRESULT WINAPI ainfo_Cancel(void *) { return S_OK; }
static HRESULT WINAPI ainfo_Close(void *) { return S_OK; }

static HRESULT completed(REFIID iid, IUnknown *result, void **out)
{
    Async *a = (Async *)calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    if (!async_op_vtbl[0])
    {
        const void *op[] = { (void *)aop_QueryInterface, (void *)aop_AddRef, (void *)aop_Release, (void *)common_GetIids,
            (void *)async_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)aop_put_Completed,
            (void *)aop_get_Completed, (void *)aop_GetResults };
        const void *info[] = { (void *)ainfo_QueryInterface, (void *)ainfo_AddRef, (void *)ainfo_Release, (void *)common_GetIids,
            (void *)async_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)ainfo_get_Id, (void *)ainfo_get_Status,
            (void *)ainfo_get_ErrorCode, (void *)ainfo_Cancel, (void *)ainfo_Close };
        memcpy(async_op_vtbl, op, sizeof(op));
        memcpy(async_info_vtbl, info, sizeof(info));
    }
    a->op_vtbl = async_op_vtbl;
    a->info_vtbl = async_info_vtbl;
    a->ref = 1;
    a->iid = iid;
    if ((a->result = result)) result->AddRef();
    *out = &a->op_vtbl;
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
/* The user                                                                                     */

struct User { const void **vtbl; const void **vtbl2; LONG ref; };
static const void *user_vtbl[12];
static const void *user2_vtbl[7];
static User the_user = { user_vtbl, user2_vtbl, 1 };

static HRESULT WINAPI user_QueryInterface(void *, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IUser))
    {
        *out = &the_user.vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IUser2))
    {
        *out = &the_user.vtbl2;
        return S_OK;
    }
    trace("User: QueryInterface refused {%08lx-%04x-%04x-...}", (unsigned long)iid.Data1, iid.Data2, iid.Data3);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI user_AddRef(void *) { return 2; }
static ULONG WINAPI user_Release(void *) { return 1; }
static HRESULT WINAPI user_GetRuntimeClassName(void *, HSTRING *n) { return hstr(L"Windows.System.User", n); }

static HRESULT WINAPI user_get_NonRoamableId(void *, HSTRING *value)
{
    WCHAR *sid_str = nullptr;
    BYTE buf[256];
    DWORD len;
    HANDLE token;
    HRESULT hr = E_FAIL;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len) &&
            ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid_str))
        {
            hr = hstr(sid_str, value);
            LocalFree(sid_str);
        }
        CloseHandle(token);
    }
    trace("User.NonRoamableId -> %#lx", hr);
    return hr;
}
static HRESULT WINAPI user_get_AuthenticationStatus(void *, INT32 *value) { *value = 1; /* LocallyAuthenticated */ return S_OK; }
static HRESULT WINAPI user_get_Type(void *, INT32 *value) { *value = 0; /* LocalUser */ return S_OK; }

/* the value of a KnownUserProperties name for the local account; NULL when there is none */
static HRESULT property_value(const WCHAR *name, IInspectable **out)
{
    WCHAR account[256] = L"";
    DWORD len = ARRAYSIZE(account);
    const WCHAR *value = nullptr;
    IActivationFactory *factory;
    HSTRING cls, str;
    HRESULT hr;

    *out = nullptr;
    GetUserNameW(account, &len);
    if (!wcscmp(name, L"AccountName") || !wcscmp(name, L"DisplayName") || !wcscmp(name, L"FirstName")) value = account;
    else if (!wcscmp(name, L"LastName") || !wcscmp(name, L"ProviderName") || !wcscmp(name, L"PrincipalName") ||
             !wcscmp(name, L"DomainName") || !wcscmp(name, L"GuestHost") || !wcscmp(name, L"SessionInitiationProtocolUri"))
        value = L"";
    trace("User property %ls -> %ls", name, value ? value : L"(none)");
    if (!value) return S_OK;

    hstr(L"Windows.Foundation.PropertyValue", &cls);
    hr = RoGetActivationFactory(cls, IID_IPropertyValueStatics_, (void **)&factory);
    WindowsDeleteString(cls);
    if (FAILED(hr)) return hr;
    hstr(value, &str);
    /* IPropertyValueStatics::CreateString is slot 16 */
    hr = ((HRESULT (WINAPI *)(void *, HSTRING, IInspectable **))(*(void ***)factory)[16])(factory, str, out);
    WindowsDeleteString(str);
    factory->Release();
    return hr;
}

static HRESULT WINAPI user_GetPropertyAsync(void *, HSTRING name, void **operation)
{
    IInspectable *value;
    HRESULT hr = property_value(WindowsGetStringRawBuffer(name, nullptr), &value);
    if (FAILED(hr)) return hr;
    hr = completed(IID_IAsyncOperation_Object, value, operation);
    if (value) value->Release();
    return hr;
}

static HRESULT WINAPI user_GetPropertiesAsync(void *, IInspectable *names, void **operation)
{
    IInspectable *set = nullptr, *names_view = nullptr;
    IUnknown *map = nullptr;
    HSTRING cls;
    UINT32 count = 0, i;
    HRESULT hr;

    hstr(L"Windows.Foundation.Collections.PropertySet", &cls);
    hr = RoActivateInstance(cls, &set);
    WindowsDeleteString(cls);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = set->QueryInterface(IID_IMap_String_Object, (void **)&map)) &&
        SUCCEEDED(hr = names->QueryInterface(IID_IVectorView_String, (void **)&names_view)))
    {
        void **view = *(void ***)names_view;
        ((HRESULT (WINAPI *)(void *, UINT32 *))view[7])(names_view, &count);          /* get_Size */
        for (i = 0; i < count; i++)
        {
            HSTRING name = nullptr;
            IInspectable *value;
            boolean replaced;
            ((HRESULT (WINAPI *)(void *, UINT32, HSTRING *))view[6])(names_view, i, &name);   /* GetAt */
            if (SUCCEEDED(property_value(WindowsGetStringRawBuffer(name, nullptr), &value)) && value)
            {
                /* IMap<String, Object>::Insert is slot 9 */
                ((HRESULT (WINAPI *)(void *, HSTRING, IInspectable *, boolean *))(*(void ***)map)[9])(map, name, value, &replaced);
                value->Release();
            }
            WindowsDeleteString(name);
        }
        names_view->Release();
    }
    if (map) map->Release();
    if (SUCCEEDED(hr)) hr = completed(IID_IAsyncOperation_IPropertySet, set, operation);
    set->Release();
    return hr;
}

static HRESULT WINAPI user_GetPictureAsync(void *, INT32, void **operation)
{
    trace("User.GetPictureAsync -> no picture");
    return completed(IID_IUnknown /* unused by callers that check the result */, nullptr, operation);
}

static HRESULT WINAPI user2_QueryInterface(void *, REFIID iid, void **out) { return user_QueryInterface(nullptr, iid, out); }
static HRESULT WINAPI user2_CheckUserAgeConsentGroupAsync(void *, INT32, void **)
{
    trace("User.CheckUserAgeConsentGroupAsync -> E_NOTIMPL");
    return E_NOTIMPL;
}

/* ------------------------------------------------------------------------------------------- */
/* [user]: an IVectorView<User> and IIterable<User> with one element, and its iterator            */

struct List { const void **view; const void **iterable; LONG ref; };
struct Iter { const void **vtbl; LONG ref; UINT32 pos; };
static const void *list_view_vtbl[10], *list_iterable_vtbl[7], *iter_vtbl[10];

static HRESULT list_qi(List *l, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IVectorView_User))
    {
        InterlockedIncrement(&l->ref);
        *out = &l->view;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IIterable_User))
    {
        InterlockedIncrement(&l->ref);
        *out = &l->iterable;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG list_release(List *l) { ULONG r = InterlockedDecrement(&l->ref); if (!r) free(l); return r; }
static List *from_view(void *i) { return (List *)i; }
static List *from_iterable(void *i) { return (List *)((char *)i - sizeof(void *)); }
static HRESULT WINAPI lv_QueryInterface(void *i, REFIID iid, void **o) { return list_qi(from_view(i), iid, o); }
static ULONG WINAPI lv_AddRef(void *i) { return InterlockedIncrement(&from_view(i)->ref); }
static ULONG WINAPI lv_Release(void *i) { return list_release(from_view(i)); }
static HRESULT WINAPI li_QueryInterface(void *i, REFIID iid, void **o) { return list_qi(from_iterable(i), iid, o); }
static ULONG WINAPI li_AddRef(void *i) { return InterlockedIncrement(&from_iterable(i)->ref); }
static ULONG WINAPI li_Release(void *i) { return list_release(from_iterable(i)); }
static HRESULT WINAPI list_GetRuntimeClassName(void *, HSTRING *n)
{
    return hstr(L"Windows.Foundation.Collections.IVectorView`1<Windows.System.User>", n);
}
static HRESULT WINAPI lv_GetAt(void *, UINT32 index, void **user)
{
    if (index) { *user = nullptr; return E_BOUNDS; }
    *user = &the_user.vtbl;
    return S_OK;
}
static HRESULT WINAPI lv_get_Size(void *, UINT32 *size) { *size = 1; return S_OK; }
static HRESULT WINAPI lv_IndexOf(void *, void *user, UINT32 *index, boolean *found)
{
    *found = user == &the_user.vtbl;
    *index = 0;
    return S_OK;
}
static HRESULT WINAPI lv_GetMany(void *, UINT32 start, UINT32 size, void **items, UINT32 *count)
{
    *count = (start == 0 && size >= 1) ? 1 : 0;
    if (*count) items[0] = &the_user.vtbl;
    return S_OK;
}

static ULONG WINAPI it_AddRef(void *i) { return InterlockedIncrement(&((Iter *)i)->ref); }
static ULONG WINAPI it_Release(void *i) { ULONG r = InterlockedDecrement(&((Iter *)i)->ref); if (!r) free(i); return r; }
static HRESULT WINAPI it_QueryInterface(void *i, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IIterator_User))
    {
        it_AddRef(i);
        *out = i;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static HRESULT WINAPI it_GetRuntimeClassName(void *, HSTRING *n)
{
    return hstr(L"Windows.Foundation.Collections.IIterator`1<Windows.System.User>", n);
}
static HRESULT WINAPI it_get_Current(void *i, void **user)
{
    if (((Iter *)i)->pos) { *user = nullptr; return E_BOUNDS; }
    *user = &the_user.vtbl;
    return S_OK;
}
static HRESULT WINAPI it_get_HasCurrent(void *i, boolean *v) { *v = !((Iter *)i)->pos; return S_OK; }
static HRESULT WINAPI it_MoveNext(void *i, boolean *v) { ((Iter *)i)->pos = 1; *v = FALSE; return S_OK; }
static HRESULT WINAPI it_GetMany(void *i, UINT32 size, void **items, UINT32 *count)
{
    Iter *it = (Iter *)i;
    *count = (!it->pos && size) ? 1 : 0;
    if (*count) { items[0] = &the_user.vtbl; it->pos = 1; }
    return S_OK;
}
static HRESULT WINAPI li_First(void *, void **out)
{
    Iter *it = (Iter *)calloc(1, sizeof(*it));
    if (!it) return E_OUTOFMEMORY;
    it->vtbl = iter_vtbl;
    it->ref = 1;
    *out = it;
    return S_OK;
}

static HRESULT make_list(IUnknown **out)
{
    List *l = (List *)calloc(1, sizeof(*l));
    if (!l) return E_OUTOFMEMORY;
    l->view = list_view_vtbl;
    l->iterable = list_iterable_vtbl;
    l->ref = 1;
    *out = (IUnknown *)&l->view;
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
/* The statics                                                                                  */

struct Statics { const void **vtbl; const void **vtbl2; LONG ref; };
static const void *statics_vtbl[11], *statics2_vtbl[7];
static Statics statics = { statics_vtbl, statics2_vtbl, 1 };

static HRESULT WINAPI st_QueryInterface(void *, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) || IsEqualGUID(iid, IID_IAgileObject_) ||
        IsEqualGUID(iid, IID_IActivationFactory) || IsEqualGUID(iid, IID_IUserStatics))
    {
        *out = &statics.vtbl;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IUserStatics2))
    {
        *out = &statics.vtbl2;
        return S_OK;
    }
    trace("statics: QueryInterface refused {%08lx-%04x-%04x-...}", (unsigned long)iid.Data1, iid.Data2, iid.Data3);
    *out = nullptr;
    return E_NOINTERFACE;
}
static HRESULT WINAPI st_GetRuntimeClassName(void *, HSTRING *n) { return hstr(L"Windows.System.User", n); }
static HRESULT WINAPI st_CreateWatcher(void *, void **)
{
    trace("User.CreateWatcher -> E_NOTIMPL");
    return E_NOTIMPL;
}
static HRESULT find_all(void **operation)
{
    IUnknown *list;
    HRESULT hr = make_list(&list);
    trace("User.FindAllAsync -> [the local user]");
    if (FAILED(hr)) return hr;
    hr = completed(IID_IAsyncOperation_IVectorView_User, list, operation);
    list->Release();
    return hr;
}
static HRESULT WINAPI st_FindAllAsync(void *, void **operation) { return find_all(operation); }
static HRESULT WINAPI st_FindAllAsyncByType(void *, INT32, void **operation) { return find_all(operation); }
static HRESULT WINAPI st_FindAllAsyncByTypeAndStatus(void *, INT32, INT32, void **operation) { return find_all(operation); }
static HRESULT WINAPI st_GetFromId(void *, HSTRING, void **user) { *user = &the_user.vtbl; return S_OK; }
static HRESULT WINAPI st2_GetDefault(void *, void **user)
{
    trace("User.GetDefault -> the local user");
    *user = &the_user.vtbl;
    return S_OK;
}

static void init(void)
{
    if (statics_vtbl[0]) return;
    const void *u[] = { (void *)user_QueryInterface, (void *)user_AddRef, (void *)user_Release, (void *)common_GetIids,
        (void *)user_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)user_get_NonRoamableId,
        (void *)user_get_AuthenticationStatus, (void *)user_get_Type, (void *)user_GetPropertyAsync,
        (void *)user_GetPropertiesAsync, (void *)user_GetPictureAsync };
    const void *u2[] = { (void *)user2_QueryInterface, (void *)user_AddRef, (void *)user_Release, (void *)common_GetIids,
        (void *)user_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)user2_CheckUserAgeConsentGroupAsync };
    const void *lv[] = { (void *)lv_QueryInterface, (void *)lv_AddRef, (void *)lv_Release, (void *)common_GetIids,
        (void *)list_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)lv_GetAt, (void *)lv_get_Size,
        (void *)lv_IndexOf, (void *)lv_GetMany };
    const void *li[] = { (void *)li_QueryInterface, (void *)li_AddRef, (void *)li_Release, (void *)common_GetIids,
        (void *)list_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)li_First };
    const void *it[] = { (void *)it_QueryInterface, (void *)it_AddRef, (void *)it_Release, (void *)common_GetIids,
        (void *)it_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)it_get_Current, (void *)it_get_HasCurrent,
        (void *)it_MoveNext, (void *)it_GetMany };
    const void *st[] = { (void *)st_QueryInterface, (void *)user_AddRef, (void *)user_Release, (void *)common_GetIids,
        (void *)st_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)st_CreateWatcher, (void *)st_FindAllAsync,
        (void *)st_FindAllAsyncByType, (void *)st_FindAllAsyncByTypeAndStatus, (void *)st_GetFromId };
    const void *st2[] = { (void *)st_QueryInterface, (void *)user_AddRef, (void *)user_Release, (void *)common_GetIids,
        (void *)st_GetRuntimeClassName, (void *)common_GetTrustLevel, (void *)st2_GetDefault };
    memcpy(user_vtbl, u, sizeof(u));
    memcpy(user2_vtbl, u2, sizeof(u2));
    memcpy(list_view_vtbl, lv, sizeof(lv));
    memcpy(list_iterable_vtbl, li, sizeof(li));
    memcpy(iter_vtbl, it, sizeof(it));
    memcpy(statics2_vtbl, st2, sizeof(st2));
    memcpy(statics_vtbl, st, sizeof(st));
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    init();
    if (wcscmp(WindowsGetStringRawBuffer(classid, nullptr), L"Windows.System.User"))
    {
        *factory = nullptr;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    trace("factory");
    *factory = (IActivationFactory *)&statics.vtbl;
    return S_OK;
}

// Windows.Internal.StateRepository.User stand-in for Wine.
//
// On Windows this class is service-activated (ActivationType=1, Server=StateRepository in the
// SOFTWARE hive) and implemented inside Windows.StateRepository.dll, which exports only ServiceMain:
// there is no in-process factory to register, and both Windows.StateRepositoryClient.dll and
// Windows.StateRepositoryBroker.dll answer CLASS_E_CLASSNOTAVAILABLE for it. Without it,
// RoGetActivationFactory returns REGDB_E_CLASSNOTREG and Media Player ends on
// "Exception from HRESULT: 0x80040154" after its whole UI is built.
//
// The interface layouts below are read from the vtables in Windows.StateRepository.dll, named with
// its public PDB (UserFactoryServer at RVA 0x290730, UserServer at 0x28bfe0), and the IIDs come from
// the hive's Classes\Interface map:
//   {84103ccb-2fd7-4d6c-962e-5d8582b4c720} __x_Windows_CInternal_CStateRepository_CIUserStatics
//   {5232f8ea-49c7-4840-bfbb-66e785689e88} __x_Windows_CInternal_CStateRepository_CIUser
//
// The state repository is a database of installed packages per user. Everything here answers for
// the one user the process is running as, which is the only one a Wine prefix has.
#include <windows.h>
#include <appmodel.h>
#include <sddl.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>

static const GUID IID_IUserStatics_sr =
    {0x84103ccb,0x2fd7,0x4d6c,{0x96,0x2e,0x5d,0x85,0x82,0xb4,0xc7,0x20}};
static const GUID IID_IUser_sr =
    {0x5232f8ea,0x49c7,0x4840,{0xbf,0xbb,0x66,0xe7,0x85,0x68,0x9e,0x88}};
static const GUID IID_IAgileObject_sr =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_STATEREPOSITORY_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[staterepository] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* The current process's user SID, in both forms the interface hands out. */
static BYTE token_sid[SECURITY_MAX_SID_SIZE];
static DWORD token_sid_len;
static WCHAR token_sid_text[256];

static void load_current_sid(void)
{
    HANDLE token = nullptr;
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD len = 0;
    LPWSTR text = nullptr;

    if (token_sid_len) return;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &len))
        {
            PSID sid = ((TOKEN_USER *)buffer)->User.Sid;
            token_sid_len = GetLengthSid(sid);
            if (token_sid_len > sizeof(token_sid)) token_sid_len = sizeof(token_sid);
            memcpy(token_sid, sid, token_sid_len);
            if (ConvertSidToStringSidW(sid, &text))
            {
                lstrcpynW(token_sid_text, text, ARRAYSIZE(token_sid_text));
                LocalFree(text);
            }
        }
        CloseHandle(token);
    }
    if (!token_sid_len)  /* no token: S-1-5-18, the account a service would run as */
    {
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        PSID sid = nullptr;
        if (AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sid))
        {
            token_sid_len = GetLengthSid(sid);
            memcpy(token_sid, sid, token_sid_len);
            FreeSid(sid);
        }
        lstrcpynW(token_sid_text, L"S-1-5-18", ARRAYSIZE(token_sid_text));
    }
}

struct user_object
{
    const void **vtbl;
    LONG ref;
};

static HRESULT WINAPI user_QueryInterface(void *iface, REFIID iid, void **out)
{
    user_object *impl = (user_object *)iface;

    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IUser_sr) || IsEqualGUID(iid, IID_IAgileObject_sr))
    {
        InterlockedIncrement(&impl->ref);
        *out = iface;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI user_AddRef(void *iface) { return InterlockedIncrement(&((user_object *)iface)->ref); }
static ULONG WINAPI user_Release(void *iface)
{
    user_object *impl = (user_object *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) free(impl);
    return ref;
}
static HRESULT WINAPI user_GetIids(void *iface, ULONG *count, IID **iids)
{
    *count = 0; *iids = nullptr; return S_OK;
}
static HRESULT WINAPI user_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.Internal.StateRepository.User", 36, name);
}
static HRESULT WINAPI user_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI user_get_UserID(void *iface, INT64 *value)
{
    if (!value) return E_POINTER;
    *value = 1;   /* the single row this prefix has */
    return S_OK;
}
static HRESULT WINAPI user_get_Revision(void *iface, UINT64 *value)
{
    if (!value) return E_POINTER;
    *value = 1;
    return S_OK;
}
static HRESULT WINAPI user_get_UserSid(void *iface, HSTRING *value)
{
    if (!value) return E_POINTER;
    load_current_sid();
    trace("get_UserSid -> %ls", token_sid_text);
    return WindowsCreateString(token_sid_text, lstrlenW(token_sid_text), value);
}
static HRESULT WINAPI user_get_Is_DictionaryNull(void *iface, boolean *value)
{
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}
static HRESULT WINAPI user_Get_Dictionary(void *iface, UINT32 *size, BYTE **value)
{
    if (!size || !value) return E_POINTER;
    *size = 0;
    *value = nullptr;
    return S_OK;
}
static HRESULT WINAPI user_GetUserSidAsSecurityIdentifier(void *iface, UINT32 *size, BYTE **value)
{
    if (!size || !value) return E_POINTER;
    load_current_sid();
    if (!(*value = (BYTE *)CoTaskMemAlloc(token_sid_len))) return E_OUTOFMEMORY;
    memcpy(*value, token_sid, token_sid_len);
    *size = token_sid_len;
    trace("GetUserSidAsSecurityIdentifier -> %u bytes", token_sid_len);
    return S_OK;
}
static HRESULT WINAPI user_SetUserSidAsSecurityIdentifier(void *iface, UINT32 size, BYTE *value)
{
    return S_OK;
}
static const void *user_vtbl[13] =
{
    (void *)user_QueryInterface, (void *)user_AddRef, (void *)user_Release,
    (void *)user_GetIids, (void *)user_GetRuntimeClassName, (void *)user_GetTrustLevel,
    (void *)user_get_UserID, (void *)user_get_Revision, (void *)user_get_UserSid,
    (void *)user_get_Is_DictionaryNull, (void *)user_Get_Dictionary,
    (void *)user_GetUserSidAsSecurityIdentifier, (void *)user_SetUserSidAsSecurityIdentifier,
};

static HRESULT create_user(void **out)
{
    user_object *impl;

    if (!out) return E_POINTER;
    if (!(impl = (user_object *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = user_vtbl;
    impl->ref = 1;
    *out = impl;
    return S_OK;
}

/* IUserStatics, in the order Windows.StateRepository.dll's UserFactoryServer declares it. */
struct statics_object { const void **vtbl; };
static statics_object statics = { nullptr };
static statics_object factory = { nullptr };

static HRESULT WINAPI statics_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IActivationFactory))
    {
        /* A different layout: IInspectable + ActivateInstance, not the statics table. */
        *out = &factory;
        return S_OK;
    }
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IUserStatics_sr) || IsEqualGUID(iid, IID_IAgileObject_sr))
    {
        *out = &statics;
        return S_OK;
    }
    trace("statics: no interface {%08lx-%04x-%04x-...}", iid.Data1, iid.Data2, iid.Data3);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI statics_AddRef(void *iface) { return 2; }
static ULONG WINAPI statics_Release(void *iface) { return 1; }
static HRESULT WINAPI statics_GetIids(void *iface, ULONG *count, IID **iids)
{
    *count = 0; *iids = nullptr; return S_OK;
}
static HRESULT WINAPI statics_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.Internal.StateRepository.User", 36, name);
}
static HRESULT WINAPI statics_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI statics_Exists(void *iface, INT64 id, boolean *value)
{
    if (!value) return E_POINTER;
    *value = (id == 1);
    return S_OK;
}
static HRESULT WINAPI statics_ExistsByUserSid(void *iface, HSTRING sid, boolean *value)
{
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}
static HRESULT WINAPI statics_Get(void *iface, INT64 id, void **value) { return create_user(value); }
static HRESULT WINAPI statics_TryGet(void *iface, INT64 id, void **value) { return create_user(value); }
static HRESULT WINAPI statics_GetByUserSid(void *iface, HSTRING sid, void **value) { return create_user(value); }
static HRESULT WINAPI statics_TryGetByUserSid(void *iface, HSTRING sid, void **value) { return create_user(value); }
static HRESULT WINAPI statics_Find(void *iface, void **value)
{
    /* Returns a collection of users; nothing asks for it here and its element type is unknown. */
    if (value) *value = nullptr;
    return E_NOTIMPL;
}
static HRESULT WINAPI statics_GetCurrent(void *iface, void **value)
{
    trace("GetCurrent");
    return create_user(value);
}
static HRESULT WINAPI statics_GetEveryone(void *iface, void **value) { return create_user(value); }
static HRESULT WINAPI statics_GetDefaultAccount(void *iface, void **value) { return create_user(value); }
static HRESULT WINAPI statics_TryGetDefaultAccount(void *iface, void **value) { return create_user(value); }
static HRESULT WINAPI statics_ExistsByUserSecurityIdentifier(void *iface, UINT32 size, BYTE *sid, boolean *value)
{
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}
static HRESULT WINAPI statics_GetByUserSecurityIdentifier(void *iface, UINT32 size, BYTE *sid, void **value)
{
    return create_user(value);
}
static HRESULT WINAPI statics_TryGetByUserSecurityIdentifier(void *iface, UINT32 size, BYTE *sid, void **value)
{
    return create_user(value);
}
static HRESULT WINAPI statics_TryGetByWindowsSystemUser(void *iface, void *user, void **value)
{
    return create_user(value);
}
static HRESULT WINAPI statics_GetByWindowsSystemUser(void *iface, void *user, void **value)
{
    return create_user(value);
}
static HRESULT WINAPI statics_ActivateInstance(void *iface, IInspectable **instance)
{
    return create_user((void **)instance);
}
static const void *statics_vtbl[22] =
{
    (void *)statics_QueryInterface, (void *)statics_AddRef, (void *)statics_Release,
    (void *)statics_GetIids, (void *)statics_GetRuntimeClassName, (void *)statics_GetTrustLevel,
    (void *)statics_Exists, (void *)statics_ExistsByUserSid,
    (void *)statics_Get, (void *)statics_TryGet,
    (void *)statics_GetByUserSid, (void *)statics_TryGetByUserSid,
    (void *)statics_Find, (void *)statics_GetCurrent, (void *)statics_GetEveryone,
    (void *)statics_GetDefaultAccount, (void *)statics_TryGetDefaultAccount,
    (void *)statics_ExistsByUserSecurityIdentifier, (void *)statics_GetByUserSecurityIdentifier,
    (void *)statics_TryGetByUserSecurityIdentifier,
    (void *)statics_TryGetByWindowsSystemUser, (void *)statics_GetByWindowsSystemUser,
};

/* IActivationFactory is a separate layout: IInspectable + ActivateInstance. A caller that asks the
 * class for IActivationFactory gets this one. */
static const void *factory_vtbl[7] =
{
    (void *)statics_QueryInterface, (void *)statics_AddRef, (void *)statics_Release,
    (void *)statics_GetIids, (void *)statics_GetRuntimeClassName, (void *)statics_GetTrustLevel,
    (void *)statics_ActivateInstance,
};


/*
 * Anything else in the Windows.Internal.StateRepository.* family. These interfaces are large
 * (IApplication alone has forty-odd properties) and guessing at them is how out-parameters get
 * filled with the wrong thing. Instead every slot is its own thunk that names itself, so a run says
 * exactly which method an app called; the signature can then be read off the mangled name in
 * Windows.StateRepository.pdb and the method implemented for real.
 *
 * Until then a slot returns S_OK with its out-parameter zeroed, which for these interfaces means an
 * empty HSTRING, a zero id or a null child object - the same reasoning as the composition fakes:
 * refusing outright tends to be fatal, and writing something of the wrong type is worse than
 * writing nothing.
 */

static HRESULT create_generic(const WCHAR *cls, void **out);

/* A one-element IVectorView<T>. Several of the Find* statics return one, and a null collection is fatal:
 * Windows.Internal.StateRepository.Application slot 55
 * (IApplicationStatics::FindByUserAndPackageFamilyNameAndAppListEntry, named from
 * Windows.StateRepository.pdb) handed back null fail-fasts Media Player forty-odd times. An empty
 * view is the honest answer - this prefix's state repository has no rows. */
struct vectorview_object
{
    const void **vtbl;
    LONG ref;
    WCHAR element[128];   /* runtime class of the single row, empty for an empty view */
};
static HRESULT WINAPI vv_QueryInterface(void *iface, REFIID iid, void **out)
{
    vectorview_object *impl = (vectorview_object *)iface;
    if (!out) return E_POINTER;
    InterlockedIncrement(&impl->ref);
    *out = iface;
    return S_OK;
}
static ULONG WINAPI vv_AddRef(void *iface) { return InterlockedIncrement(&((vectorview_object *)iface)->ref); }
static ULONG WINAPI vv_Release(void *iface)
{
    vectorview_object *impl = (vectorview_object *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) free(impl);
    return ref;
}
static HRESULT WINAPI vv_GetIids(void *iface, ULONG *count, IID **iids) { *count = 0; *iids = nullptr; return S_OK; }
static HRESULT WINAPI vv_GetRuntimeClassName(void *iface, HSTRING *name)
{
    return WindowsCreateString(L"Windows.Foundation.Collections.IVectorView`1<Object>", 51, name);
}
static HRESULT WINAPI vv_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI vv_GetAt(void *iface, UINT32 index, void **item)
{
    vectorview_object *impl = (vectorview_object *)iface;

    if (!item) return E_POINTER;
    *item = nullptr;
    if (!impl->element[0] || index) return E_BOUNDS;
    trace("GetAt(0) -> %ls", impl->element);
    return create_generic(impl->element, item);
}
static HRESULT WINAPI vv_get_Size(void *iface, UINT32 *size)
{
    vectorview_object *impl = (vectorview_object *)iface;

    if (!size) return E_POINTER;
    *size = impl->element[0] ? 1 : 0;
    return S_OK;
}
static HRESULT WINAPI vv_IndexOf(void *iface, void *item, UINT32 *index, boolean *found)
{
    if (index) *index = 0;
    if (found) *found = FALSE;
    return S_OK;
}
static HRESULT WINAPI vv_GetMany(void *iface, UINT32 start, UINT32 capacity, void **items, UINT32 *actual)
{
    vectorview_object *impl = (vectorview_object *)iface;
    UINT32 n = 0;

    if (impl->element[0] && !start && capacity && items && SUCCEEDED(create_generic(impl->element, &items[0])))
        n = 1;
    if (actual) *actual = n;
    return S_OK;
}
/* The collection is handed back for whatever interface the caller asks for - IVectorView,
 * IIterable, IVector - and those tables are different lengths. A short table means a call past its
 * end reads whatever follows in .rdata as a function pointer, which is an access violation inside
 * this DLL. Pad well past the longest of them. */
static HRESULT WINAPI vv_unused(void *iface, void *a, void *b, void *c, void *d)
{
    void *args[4] = { a, b, c, d };
    for (int i = 3; i >= 0; i--)
    {
        if (!args[i] || IsBadWritePtr(args[i], sizeof(void *))) continue;
        *(void **)args[i] = nullptr;
        break;
    }
    return S_OK;
}
#define VV_PAD8 (void *)vv_unused, (void *)vv_unused, (void *)vv_unused, (void *)vv_unused, \
                (void *)vv_unused, (void *)vv_unused, (void *)vv_unused, (void *)vv_unused
static const void *vectorview_vtbl[32] =
{
    (void *)vv_QueryInterface, (void *)vv_AddRef, (void *)vv_Release,
    (void *)vv_GetIids, (void *)vv_GetRuntimeClassName, (void *)vv_GetTrustLevel,
    (void *)vv_GetAt, (void *)vv_get_Size, (void *)vv_IndexOf, (void *)vv_GetMany,
    (void *)vv_unused, (void *)vv_unused,
    VV_PAD8, VV_PAD8, (void *)vv_unused, (void *)vv_unused, (void *)vv_unused, (void *)vv_unused,
};
static HRESULT create_vectorview(const WCHAR *element, void **out)
{
    vectorview_object *impl;
    if (!out) return E_POINTER;
    if (!(impl = (vectorview_object *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = vectorview_vtbl;
    impl->ref = 1;
    if (element) lstrcpynW(impl->element, element, ARRAYSIZE(impl->element));
    *out = impl;
    return S_OK;
}

/* (class, slot) pairs whose out-parameter must be a collection rather than nothing. Slots are read
 * off the statics vtables in Windows.StateRepository.dll with its public PDB. */
static const WCHAR *sr_collection_element(const WCHAR *cls, unsigned slot)
{
    if (!lstrcmpW(cls, L"Windows.Internal.StateRepository.Application") && slot == 55)
        return L"Windows.Internal.StateRepository.Application";
    return nullptr;
}

#define SR_GENERIC_SLOTS 160

struct generic_object
{
    const void **vtbl;
    WCHAR cls[128];
    LONG ref;
    bool instance;   /* a row out of a collection, not the class's statics */
};


static HRESULT WINAPI generic_QueryInterface(void *iface, REFIID iid, void **out)
{
    generic_object *impl = (generic_object *)iface;

    if (!out) return E_POINTER;
    /* Every interface this object is asked for is served by the same table: which one the caller
     * holds decides what a slot means, and the trace prints the slot either way. */
    InterlockedIncrement(&impl->ref);
    *out = iface;
    trace("%ls: QI {%08lx-%04x-%04x-...} -> self", impl->cls, iid.Data1, iid.Data2, iid.Data3);
    return S_OK;
}
static ULONG WINAPI generic_AddRef(void *iface) { return InterlockedIncrement(&((generic_object *)iface)->ref); }
static ULONG WINAPI generic_Release(void *iface)
{
    generic_object *impl = (generic_object *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) free(impl);
    return ref;
}
static HRESULT WINAPI generic_GetIids(void *iface, ULONG *count, IID **iids)
{
    *count = 0; *iids = nullptr; return S_OK;
}
static HRESULT WINAPI generic_GetRuntimeClassName(void *iface, HSTRING *name)
{
    generic_object *impl = (generic_object *)iface;
    return WindowsCreateString(impl->cls, lstrlenW(impl->cls), name);
}
static HRESULT WINAPI generic_GetTrustLevel(void *iface, TrustLevel *level) { *level = BaseTrust; return S_OK; }

/* What to write through a slot's out-parameter, and where. Generated from the server vtables and
 * the public PDB by ./gen-staterepository.py; the out-parameter is the last one, so
 * the arity is what says which argument it is. Getting that wrong is not a small error - writing
 * the eight bytes at the wrong index leaves the real out-parameter holding whatever the caller's
 * stack had, and for an HSTRING that is a fail-fast. */
enum
{
    SR_OUT_NONE, SR_OUT_ZERO1, SR_OUT_ZERO4, SR_OUT_ZERO8,
    SR_OUT_HSTRING, SR_OUT_IFACE, SR_OUT_COLLECTION,
};
#include "staterepository_slots.h"

static const struct sr_slot *sr_find(const WCHAR *cls, bool statics, unsigned slot)
{
    for (size_t i = 0; i < ARRAYSIZE(sr_slots); i++)
    {
        if (sr_slots[i].slot != slot || sr_slots[i].statics != (statics ? 1 : 0)) continue;
        if (!lstrcmpW(sr_slots[i].cls, cls)) return &sr_slots[i];
    }
    return nullptr;
}

static HRESULT generic_slot_call(unsigned slot, void *iface, void *args[6])
{
    generic_object *impl = (generic_object *)iface;
    const struct sr_slot *desc = sr_find(impl->cls, !impl->instance, slot);
    void *out;

    trace("%ls[%s] slot %u %s argc %d", impl->cls, impl->instance ? "row" : "statics", slot,
          desc ? desc->name : "(not in table)", desc ? desc->argc : -1);

    if (!desc || !desc->argc) return S_OK;
    out = args[desc->argc - 1];
    if (!out || IsBadWritePtr(out, sizeof(void *))) return S_OK;

    /* The row has to have an identity: the app reads _ApplicationID straight back into
     * IApplicationStatics::GetByUserAnd_ApplicationId, so zero looks up nothing. One row, id 1. */
    if (impl->instance && desc->kind == SR_OUT_ZERO8 && !strcmp(desc->name, "get__ApplicationID"))
    {
        *(INT64 *)out = 1;
        return S_OK;
    }
    /* An empty display name is not survivable either. run.sh already knows the package's, out of
     * the AppxManifest via stage-app.sh, and MRT hands back anything that is not an ms-resource
     * URI unchanged. */
    if (impl->instance && desc->kind == SR_OUT_HSTRING && !strcmp(desc->name, "get_DisplayNameReference"))
    {
        WCHAR name[128];
        if (!GetEnvironmentVariableW(L"WINE_UWP_WINDOW_TITLE", name, ARRAYSIZE(name))) lstrcpyW(name, L"App");
        trace("  -> %ls", name);
        return WindowsCreateString(name, lstrlenW(name), (HSTRING *)out);
    }

    /* The Package row is the running package, so its full name is the process's. The native
     * AppInfo.Package looks the package up by this name through PackageManager; an empty name
     * found nothing, and Solitaire threw out of the step that follows its bootflow. */
    if (impl->instance && desc->kind == SR_OUT_HSTRING && !strcmp(desc->name, "get_PackageFullName") &&
        !lstrcmpW(impl->cls, L"Windows.Internal.StateRepository.Package"))
    {
        WCHAR name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
        UINT32 len = ARRAYSIZE(name);
        if (GetCurrentPackageFullName(&len, name) == ERROR_SUCCESS)
        {
            trace("  -> %ls", name);
            return WindowsCreateString(name, lstrlenW(name), (HSTRING *)out);
        }
    }

    switch (desc->kind)
    {
    case SR_OUT_ZERO1: *(BYTE *)out = 0; break;
    case SR_OUT_ZERO4: *(UINT32 *)out = 0; break;
    case SR_OUT_ZERO8: *(INT64 *)out = 0; break;
    case SR_OUT_HSTRING: *(HSTRING *)out = nullptr; break;  /* a null HSTRING is the empty string */
    case SR_OUT_IFACE:
        trace("  -> %ls row", desc->element ? desc->element : impl->cls);
        return create_generic(desc->element ? desc->element : impl->cls, (void **)out);
    case SR_OUT_COLLECTION:
        /* One row, not none: the caller is the app looking itself up, and an empty result is a
         * state it treats as fatal. */
        trace("  -> collection of %ls", desc->element ? desc->element : L"(unknown)");
        return create_vectorview(desc->element, (void **)out);
    default: break;
    }
    return S_OK;
}

#define SR_THUNK(n) static HRESULT WINAPI generic_slot_##n(void *iface, void *a, void *b, void *c, \
                                                           void *e, void *f, void *g) \
    { void *args[6] = { a, b, c, e, f, g }; return generic_slot_call(0x##n, iface, args); }
#define SR_ROW(p) SR_THUNK(p##0) SR_THUNK(p##1) SR_THUNK(p##2) SR_THUNK(p##3) \
                  SR_THUNK(p##4) SR_THUNK(p##5) SR_THUNK(p##6) SR_THUNK(p##7) \
                  SR_THUNK(p##8) SR_THUNK(p##9) SR_THUNK(p##a) SR_THUNK(p##b) \
                  SR_THUNK(p##c) SR_THUNK(p##d) SR_THUNK(p##e) SR_THUNK(p##f)
SR_ROW(0) SR_ROW(1) SR_ROW(2) SR_ROW(3) SR_ROW(4) SR_ROW(5) SR_ROW(6) SR_ROW(7) SR_ROW(8) SR_ROW(9)
#define SR_ROWP(p) (void *)generic_slot_##p##0, (void *)generic_slot_##p##1, (void *)generic_slot_##p##2, \
                   (void *)generic_slot_##p##3, (void *)generic_slot_##p##4, (void *)generic_slot_##p##5, \
                   (void *)generic_slot_##p##6, (void *)generic_slot_##p##7, (void *)generic_slot_##p##8, \
                   (void *)generic_slot_##p##9, (void *)generic_slot_##p##a, (void *)generic_slot_##p##b, \
                   (void *)generic_slot_##p##c, (void *)generic_slot_##p##d, (void *)generic_slot_##p##e, \
                   (void *)generic_slot_##p##f
static const void *generic_vtbl[SR_GENERIC_SLOTS] =
{
    SR_ROWP(0), SR_ROWP(1), SR_ROWP(2), SR_ROWP(3), SR_ROWP(4), SR_ROWP(5), SR_ROWP(6), SR_ROWP(7), SR_ROWP(8), SR_ROWP(9)
};

static void generic_vtbl_init(void)
{
    generic_vtbl[0] = (void *)generic_QueryInterface;
    generic_vtbl[1] = (void *)generic_AddRef;
    generic_vtbl[2] = (void *)generic_Release;
    generic_vtbl[3] = (void *)generic_GetIids;
    generic_vtbl[4] = (void *)generic_GetRuntimeClassName;
    generic_vtbl[5] = (void *)generic_GetTrustLevel;
}

static HRESULT create_generic_ex(const WCHAR *cls, bool instance, void **out)
{
    generic_object *impl;

    if (!out) return E_POINTER;
    if (!(impl = (generic_object *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    generic_vtbl_init();
    impl->vtbl = generic_vtbl;
    impl->ref = 1;
    lstrcpynW(impl->cls, cls, ARRAYSIZE(impl->cls));
    impl->instance = instance;
    /* IUnknown and IInspectable are real; the rest of the table is thunks. */
    *out = impl;
    return S_OK;
}

static HRESULT create_generic(const WCHAR *cls, void **out)
{
    return create_generic_ex(cls, true, out);   /* collection rows are instances */
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *str = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!str || wcsncmp(str, L"Windows.Internal.StateRepository.", 33))
    {
        trace("not ours: %ls", str ? str : L"(null)");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    if (lstrcmpW(str, L"Windows.Internal.StateRepository.User"))
    {
        trace("serving %ls with the generic traced table", str);
        return create_generic_ex(str, false, (void **)out);
    }
    statics.vtbl = statics_vtbl;
    factory.vtbl = factory_vtbl;
    trace("serving %ls", str);
    /* Hand back the statics layout: RoGetActivationFactory QIs for the interface the caller wants,
     * and statics_QueryInterface answers IActivationFactory with the factory layout below. */
    *out = (IActivationFactory *)&statics;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

// Windows.Storage.Pickers.FileOpenPicker stand-in for Wine.
//
// Media Player's "Open file(s)" button activates this class. It is registered to the native
// twinui.appcore.dll, whose picker is a shell UI hosted in a broker process that does not exist in
// the prefix, so PickSingleFileAsync never completes and the button does nothing at all.
//
// There is no file-picker UI here to put in its place. What this does instead is return a file
// chosen out-of-band, named by WINE_UWP_PICKER_FILE. That is a test harness, not a picker: it lets
// an app be driven to the point of opening a file without a shell. The picker reports the file
// only if it matches the caller's FileTypeFilter, so an app that asks for audio does not get
// handed something it cannot play.
//
// Interface layout from Windows.Storage.winmd:
//   {2ca8278a-12c5-4c5f-8977-94547793c241} IFileOpenPicker
//       6/7   get/put_ViewMode          8/9   get/put_SettingsIdentifier
//      10/11  get/put_SuggestedStartLocation
//      12/13  get/put_CommitButtonText  14    get_FileTypeFilter
//      15     PickSingleFileAsync       16    PickMultipleFilesAsync
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <eventtoken.h>
#include <commdlg.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>

#define DEF_IID(name, a, b, c, d0,d1,d2,d3,d4,d5,d6,d7) \
    static const GUID name = {a, b, c, {d0,d1,d2,d3,d4,d5,d6,d7}}

DEF_IID(IID_IAgile_,             0x94ea2b94,0xe9cc,0x49e0, 0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90);
DEF_IID(IID_IAsyncInfo_,         0x00000036,0x0000,0x0000, 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEF_IID(IID_IFileOpenPicker_,    0x2ca8278a,0x12c5,0x4c5f, 0x89,0x77,0x94,0x54,0x77,0x93,0xc2,0x41);
DEF_IID(IID_IStorageFileStatics_,0x5984c710,0xdaf2,0x43c8, 0x8b,0xb4,0xa4,0xd3,0xea,0xcf,0xd0,0x3f);
DEF_IID(IID_IStorageItem_,       0x4207a996,0xca2f,0x42f7, 0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30);
/* Parameterised, derived with ./piid.py; both are confirmed against widl's own
 * output and against the IIDs the app asks for at runtime. */
DEF_IID(IID_IFolderPicker_,          0x084f7799,0xf3fb,0x400a, 0x99,0xb1,0x7b,0x4a,0x77,0x2f,0xd6,0x0d);
/* IAsyncOperation<StorageFolder>; StorageFolder's default interface is IStorageFolder {72d1cb78-...}. */
DEF_IID(IID_IAsyncOp_StorageFolder_, 0x6be9e7d7,0xe83a,0x5cbc, 0x80,0x2c,0x17,0x68,0x96,0x0b,0x52,0xc3);
DEF_IID(IID_IAsyncOp_StorageFile_,   0x5e52f8ce,0xaced,0x5a42, 0x95,0xb4,0xf6,0x74,0xdd,0x84,0x88,0x5e);
DEF_IID(IID_IVector_HSTRING_,        0x98b9acc1,0x4b56,0x532e, 0xac,0x73,0x03,0xd5,0x29,0x1c,0xca,0x90);
DEF_IID(IID_IAsyncOp_Files_,         0x03362e33,0xe413,0x5f29, 0x97,0xd0,0x48,0xa4,0x78,0x09,0x35,0xf9);
DEF_IID(IID_IVectorView_StorageFile_,0x80646519,0x5e2a,0x595d, 0xa8,0xcd,0x2a,0x24,0xb4,0x06,0x7f,0x1b);
DEF_IID(IID_IIterable_StorageFile_,  0x9ac00304,0x83ea,0x5688, 0x87,0xb6,0xae,0x38,0xaa,0xb6,0x5d,0x0b);
DEF_IID(IID_IIterator_StorageFile_,  0x43e29f53,0x0298,0x55aa, 0xa6,0xc8,0x4e,0xdd,0x32,0x3d,0x95,0x98);

enum { ASYNC_COMPLETED = 1 };

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_PICKER_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[filepicker] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static HRESULT WINAPI insp_GetIids(void *, ULONG *count, IID **iids)
{ if (count) *count = 0; if (iids) *iids = nullptr; return S_OK; }
static HRESULT WINAPI insp_GetTrustLevel(void *, TrustLevel *level)
{ if (level) *level = BaseTrust; return S_OK; }

/* ---------------------------------------------------------------- the picked file */

/* Make a StorageFile for a path through the public statics: GetFileFromPathAsync completes
 * synchronously in Wine, so its result can be taken straight back out. */
static IUnknown *storage_file_from_path(const WCHAR *path)
{
    static const WCHAR *cls = L"Windows.Storage.StorageFile";
    IUnknown *statics = nullptr, *op = nullptr, *file = nullptr;
    HSTRING name = nullptr, hpath = nullptr;
    HRESULT hr;

    if (FAILED(WindowsCreateString(cls, lstrlenW(cls), &name))) return nullptr;
    hr = RoGetActivationFactory(name, IID_IStorageFileStatics_, (void **)&statics);
    WindowsDeleteString(name);
    if (FAILED(hr)) { trace("StorageFile statics: %#lx", hr); return nullptr; }

    if (SUCCEEDED(WindowsCreateString(path, lstrlenW(path), &hpath)))
    {
        /* IStorageFileStatics slot 6 is GetFileFromPathAsync. */
        typedef HRESULT (WINAPI *from_path)(void *, HSTRING, IUnknown **);
        hr = ((from_path *)(*(void ***)statics))[6](statics, hpath, &op);
        WindowsDeleteString(hpath);
    }
    statics->Release();
    if (FAILED(hr) || !op) { trace("GetFileFromPathAsync(%ls): %#lx", path, hr); return nullptr; }

    /* IAsyncOperation<StorageFile>::GetResults is slot 8. */
    typedef HRESULT (WINAPI *get_results)(IUnknown *, IUnknown **);
    hr = ((get_results *)(*(void ***)op))[8](op, &file);
    op->Release();
    if (FAILED(hr)) { trace("GetResults: %#lx", hr); return nullptr; }

    trace("picked %ls -> %p", path, file);
    return file;
}


/* ---------------------------------------------------------------- the dialogs
 *
 * With nothing staged through the environment, the pickers show Wine's common file dialogs, so an
 * app driven by a person can open and save files. The dialog is modal on the calling (UI) thread,
 * which pumps messages while it is up. WINE_UWP_PICKER_NODIALOG=1 restores the cancelled pick that
 * scripted runs rely on. */
static bool dialogs_enabled()
{
    return !getenv("WINE_UWP_PICKER_NODIALOG");
}

/* "*.stl;*.obj" from a list of ".stl" style extensions; "*" means everything */
static void append_pattern(WCHAR *patterns, size_t size, const WCHAR *ext)
{
    if (!ext || !*ext) return;
    if (patterns[0]) wcsncat(patterns, L";", size - wcslen(patterns) - 1);
    if (!wcscmp(ext, L"*")) { wcsncat(patterns, L"*.*", size - wcslen(patterns) - 1); return; }
    wcsncat(patterns, L"*", size - wcslen(patterns) - 1);
    wcsncat(patterns, ext, size - wcslen(patterns) - 1);
}

/* A double-NUL-terminated OPENFILENAME filter: the given patterns, then all files. */
static void build_filter(WCHAR *filter, size_t size, const WCHAR *patterns)
{
    size_t n = 0;
    auto put = [&](const WCHAR *str) {
        size_t len = wcslen(str) + 1;
        if (n + len + 1 >= size) return;
        memcpy(filter + n, str, len * sizeof(WCHAR));
        n += len;
    };
    if (patterns[0] && wcscmp(patterns, L"*.*"))
    {
        put(patterns);
        put(patterns);
    }
    put(L"All files (*.*)");
    put(L"*.*");
    filter[n] = 0;
}

static IUnknown *storage_folder_from_path(const WCHAR *path)
{
    static const WCHAR *cls = L"Windows.Storage.StorageFolder";
    static const GUID IID_IStorageFolderStatics_ =
        {0x08f327ff, 0x85d5, 0x48b9, {0xae, 0xe9, 0x28, 0x51, 0x1e, 0x33, 0x9f, 0x9f}};
    IUnknown *statics = nullptr, *op = nullptr, *folder = nullptr;
    HSTRING name = nullptr, hpath = nullptr;
    HRESULT hr;

    if (FAILED(WindowsCreateString(cls, lstrlenW(cls), &name))) return nullptr;
    hr = RoGetActivationFactory(name, IID_IStorageFolderStatics_, (void **)&statics);
    WindowsDeleteString(name);
    if (FAILED(hr)) { trace("StorageFolder statics: %#lx", hr); return nullptr; }
    if (SUCCEEDED(WindowsCreateString(path, lstrlenW(path), &hpath)))
    {
        /* IStorageFolderStatics slot 6 is GetFolderFromPathAsync. */
        typedef HRESULT (WINAPI *from_path)(void *, HSTRING, IUnknown **);
        hr = ((from_path *)(*(void ***)statics))[6](statics, hpath, &op);
        WindowsDeleteString(hpath);
    }
    statics->Release();
    if (FAILED(hr) || !op) { trace("GetFolderFromPathAsync(%ls): %#lx", path, hr); return nullptr; }
    typedef HRESULT (WINAPI *get_results)(IUnknown *, IUnknown **);
    hr = ((get_results *)(*(void ***)op))[8](op, &folder);
    op->Release();
    if (FAILED(hr)) { trace("folder GetResults: %#lx", hr); return nullptr; }
    trace("picked folder %ls -> %p", path, folder);
    return folder;
}

/* The dialogs start in Documents, not in the process's current directory (the package). */
static const WCHAR *initial_dir()
{
    static WCHAR dir[MAX_PATH];
    if (!dir[0] && FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, dir))) dir[0] = 0;
    return dir[0] ? dir : nullptr;
}

static bool open_file_dialog(const WCHAR *patterns, WCHAR *path, DWORD size)
{
    WCHAR filter[512];
    OPENFILENAMEW ofn = { sizeof(ofn) };

    build_filter(filter, ARRAYSIZE(filter), patterns);
    path[0] = 0;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = size;
    ofn.lpstrInitialDir = initial_dir();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) { trace("open dialog -> %ls", path); return true; }
    trace("open dialog cancelled (%#lx)", CommDlgExtendedError());
    return false;
}

/* FileTypeChoices -> an OPENFILENAME filter, one entry per choice ("PNG (*.png)"), in the app's
 * order, then all files. Returns FALSE if there are none. first_ext gets the first extension. */
static BOOL choices_filter(IUnknown *map, WCHAR *filter, size_t size, WCHAR *first_ext, size_t ext_size);

static bool save_file_dialog(const WCHAR *suggested, const WCHAR *default_ext, IUnknown *choices,
                             WCHAR *path, DWORD size)
{
    WCHAR filter[1024], patterns[128] = L"", choice_ext[32] = L"";
    const WCHAR *ext = default_ext && *default_ext ? default_ext : suggested ? wcsrchr(suggested, '.') : nullptr;
    OPENFILENAMEW ofn = { sizeof(ofn) };

    if (choices && choices_filter(choices, filter, ARRAYSIZE(filter), choice_ext, ARRAYSIZE(choice_ext)))
    {
        if (!ext || !*ext) ext = choice_ext;
    }
    else
    {
        append_pattern(patterns, ARRAYSIZE(patterns), ext);
        build_filter(filter, ARRAYSIZE(filter), patterns);
    }
    lstrcpynW(path, suggested ? suggested : L"", size);
    if (path[0] && ext && *ext && !(wcslen(path) >= wcslen(ext) && !_wcsicmp(path + wcslen(path) - wcslen(ext), ext)))
        wcsncat(path, ext, size - wcslen(path) - 1);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = size;
    ofn.lpstrDefExt = ext && *ext == '.' ? ext + 1 : nullptr;
    ofn.lpstrInitialDir = initial_dir();
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn)) { trace("save dialog -> %ls", path); return true; }
    trace("save dialog cancelled (%#lx)", CommDlgExtendedError());
    return false;
}

static bool folder_dialog(WCHAR *path)
{
    BROWSEINFOW info = {};
    LPITEMIDLIST pidl;
    bool ok;

    info.hwndOwner = GetActiveWindow();
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!(pidl = SHBrowseForFolderW(&info))) return false;
    ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    trace("folder dialog -> %ls", ok ? path : L"(none)");
    return ok;
}

/* ---------------------------------------------------------------- IAsyncOperation<StorageFile> */

struct async_file
{
    const void **vtbl;
    const void **vtbl_info;
    LONG ref;
    IUnknown *file;      /* null means the user cancelled, which is a valid result */
    bool folder;         /* IAsyncOperation<StorageFolder> rather than <StorageFile> */
};

static const void *async_vtbl[9];
static const void *asyncinfo_vtbl[11];

static async_file *af_from_info(void *i) { return CONTAINING_RECORD(i, async_file, vtbl_info); }

static HRESULT WINAPI af_QueryInterface(void *iface, REFIID iid, void **out)
{
    async_file *impl = (async_file *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) ||
        IsEqualGUID(iid, impl->folder ? IID_IAsyncOp_StorageFolder_ : IID_IAsyncOp_StorageFile_))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IAsyncInfo_)) *out = &impl->vtbl_info;
    else { *out = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI af_AddRef(void *iface) { return InterlockedIncrement(&((async_file *)iface)->ref); }
static ULONG WINAPI af_Release(void *iface)
{
    async_file *impl = (async_file *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->file) impl->file->Release(); free(impl); }
    return ref;
}
static HRESULT WINAPI af_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.IAsyncOperation`1<Windows.Storage.StorageFile>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
/* The file is known before the operation exists, so the handler runs immediately. */
static HRESULT WINAPI af_put_Completed(void *iface, IUnknown *handler)
{
    if (!handler) return S_OK;
    trace("put_Completed: invoking now");
    handler->AddRef();
    typedef HRESULT (WINAPI *invoke)(IUnknown *, void *, INT32);
    ((invoke *)(*(void ***)handler))[3](handler, iface, ASYNC_COMPLETED);
    handler->Release();
    return S_OK;
}
static HRESULT WINAPI af_get_Completed(void *, IUnknown **handler)
{ if (!handler) return E_POINTER; *handler = nullptr; return S_OK; }
static HRESULT WINAPI af_GetResults(void *iface, IUnknown **results)
{
    async_file *impl = (async_file *)iface;
    if (!results) return E_POINTER;
    /* A cancelled pick returns null, and callers handle that; it is not an error. */
    if ((*results = impl->file)) (*results)->AddRef();
    trace("GetResults -> %p", *results);
    return S_OK;
}
static HRESULT WINAPI ai_get_Id(void *, UINT32 *id) { if (id) *id = 1; return S_OK; }
static HRESULT WINAPI ai_get_Status(void *, INT32 *s) { if (s) *s = ASYNC_COMPLETED; return S_OK; }
static HRESULT WINAPI ai_get_ErrorCode(void *, HRESULT *hr) { if (hr) *hr = S_OK; return S_OK; }
static HRESULT WINAPI ai_Cancel(void *) { return S_OK; }
static HRESULT WINAPI ai_Close(void *) { return S_OK; }
static HRESULT WINAPI ai_QueryInterface(void *iface, REFIID iid, void **out)
{ return af_QueryInterface(af_from_info(iface), iid, out); }
static ULONG WINAPI ai_AddRef(void *iface) { return af_AddRef(af_from_info(iface)); }
static ULONG WINAPI ai_Release(void *iface) { return af_Release(af_from_info(iface)); }

/* ---------------------------------------------------------------- FileOpenPicker */

struct picker
{
    const void **vtbl;
    LONG ref;
    bool folder;         /* a FolderPicker: same layout up to slot 15, PickSingleFolderAsync */
    INT32 view_mode;
    INT32 start_location;
    HSTRING settings_id;
    HSTRING commit_text;
    IUnknown *file_type_filter;   /* IVector<HSTRING> from windows.storage */
};

static const void *picker_vtbl[17];

static HRESULT WINAPI picker_QueryInterface(void *iface, REFIID iid, void **out)
{
    picker *impl = (picker *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) ||
        IsEqualGUID(iid, impl->folder ? IID_IFolderPicker_ : IID_IFileOpenPicker_))
    {
        InterlockedIncrement(&impl->ref);
        *out = iface;
        return S_OK;
    }
    trace("QI {%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} -> E_NOINTERFACE",
          iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
          iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI picker_AddRef(void *iface) { return InterlockedIncrement(&((picker *)iface)->ref); }
static ULONG WINAPI picker_Release(void *iface)
{
    picker *impl = (picker *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref)
    {
        if (impl->settings_id) WindowsDeleteString(impl->settings_id);
        if (impl->commit_text) WindowsDeleteString(impl->commit_text);
        if (impl->file_type_filter) impl->file_type_filter->Release();
        free(impl);
    }
    return ref;
}
static HRESULT WINAPI picker_GetRuntimeClassName(void *iface, HSTRING *name)
{
    const WCHAR *s = iface && ((picker *)iface)->folder ? L"Windows.Storage.Pickers.FolderPicker"
                                                        : L"Windows.Storage.Pickers.FileOpenPicker";
    return WindowsCreateString(s, lstrlenW(s), name);
}

static HRESULT WINAPI picker_get_ViewMode(void *iface, INT32 *v)
{ if (!v) return E_POINTER; *v = ((picker *)iface)->view_mode; return S_OK; }
static HRESULT WINAPI picker_put_ViewMode(void *iface, INT32 v)
{ ((picker *)iface)->view_mode = v; return S_OK; }
static HRESULT WINAPI picker_get_StartLocation(void *iface, INT32 *v)
{ if (!v) return E_POINTER; *v = ((picker *)iface)->start_location; return S_OK; }
static HRESULT WINAPI picker_put_StartLocation(void *iface, INT32 v)
{ ((picker *)iface)->start_location = v; return S_OK; }

#define PICKER_STRING(name, field)                                                                 \
    static HRESULT WINAPI picker_get_##name(void *iface, HSTRING *v)                               \
    { if (!v) return E_POINTER; return WindowsDuplicateString(((picker *)iface)->field, v); }       \
    static HRESULT WINAPI picker_put_##name(void *iface, HSTRING v)                                \
    {                                                                                              \
        picker *impl = (picker *)iface;                                                            \
        HSTRING copy = nullptr;                                                                    \
        HRESULT hr = WindowsDuplicateString(v, &copy);                                             \
        if (FAILED(hr)) return hr;                                                                 \
        if (impl->field) WindowsDeleteString(impl->field);                                         \
        impl->field = copy;                                                                        \
        return S_OK;                                                                               \
    }
PICKER_STRING(SettingsIdentifier, settings_id)
PICKER_STRING(CommitButtonText, commit_text)

/* The filter is a live vector the caller appends to, so it has to be a real one; borrow the
 * IVector<HSTRING> that QueryOptions already provides rather than write a second. */
static HRESULT WINAPI picker_get_FileTypeFilter(void *iface, IUnknown **value)
{
    picker *impl = (picker *)iface;
    if (!value) return E_POINTER;
    if (!impl->file_type_filter)
    {
        static const WCHAR *cls = L"Windows.Storage.Search.QueryOptions";
        IUnknown *options = nullptr;
        HSTRING name = nullptr;
        if (SUCCEEDED(WindowsCreateString(cls, lstrlenW(cls), &name)))
        {
            IInspectable *insp = nullptr;
            if (SUCCEEDED(RoActivateInstance(name, &insp)) && insp)
            {
                /* IQueryOptions slot 6 is get_FileTypeFilter. */
                typedef HRESULT (WINAPI *get_filter)(IUnknown *, IUnknown **);
                ((get_filter *)(*(void ***)insp))[6]((IUnknown *)insp, &impl->file_type_filter);
                insp->Release();
            }
            WindowsDeleteString(name);
        }
    }
    if (!impl->file_type_filter) { *value = nullptr; return E_FAIL; }
    impl->file_type_filter->AddRef();
    *value = impl->file_type_filter;
    return S_OK;
}

/* Does the staged file match what the caller asked for? An app that filters for audio should not
 * be handed a text file just because it is what the environment named. */
static bool matches_filter(picker *impl, const WCHAR *path)
{
    IUnknown *filter = nullptr;
    const WCHAR *ext;
    UINT32 count = 0;
    bool ok = false;

    if (FAILED(picker_get_FileTypeFilter(impl, &filter)) || !filter) return true;

    typedef HRESULT (WINAPI *get_size)(IUnknown *, UINT32 *);
    typedef HRESULT (WINAPI *get_at)(IUnknown *, UINT32, HSTRING *);
    /* IVector<HSTRING>: slot 6 GetAt, slot 7 get_Size. */
    if (FAILED(((get_size *)(*(void ***)filter))[7](filter, &count)) || !count) { filter->Release(); return true; }
    if (!(ext = wcsrchr(path, '.'))) { filter->Release(); return false; }

    for (UINT32 i = 0; i < count && !ok; i++)
    {
        HSTRING entry = nullptr;
        if (FAILED(((get_at *)(*(void ***)filter))[6](filter, i, &entry))) continue;
        const WCHAR *s = WindowsGetStringRawBuffer(entry, nullptr);
        ok = s && (!wcscmp(s, L"*") || !_wcsicmp(s, ext));
        WindowsDeleteString(entry);
    }
    filter->Release();
    trace("filter of %u entries %s %ls", count, ok ? "accepts" : "rejects", path);
    return ok;
}

static HRESULT WINAPI picker_get_FileTypeFilter(void *iface, IUnknown **value);
static void picker_patterns(picker *impl, WCHAR *patterns, size_t size)
{
    IUnknown *filter = nullptr;
    UINT32 count = 0;

    patterns[0] = 0;
    if (FAILED(picker_get_FileTypeFilter(impl, &filter)) || !filter) return;
    typedef HRESULT (WINAPI *get_size)(IUnknown *, UINT32 *);
    typedef HRESULT (WINAPI *get_at)(IUnknown *, UINT32, HSTRING *);
    if (SUCCEEDED(((get_size *)(*(void ***)filter))[7](filter, &count)))
        for (UINT32 i = 0; i < count; i++)
        {
            HSTRING entry = nullptr;
            if (FAILED(((get_at *)(*(void ***)filter))[6](filter, i, &entry))) continue;
            append_pattern(patterns, size, WindowsGetStringRawBuffer(entry, nullptr));
            WindowsDeleteString(entry);
        }
    filter->Release();
}

static HRESULT make_async_file(IUnknown *file, IUnknown **out)
{
    async_file *impl;
    if (!(impl = (async_file *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = async_vtbl;
    impl->vtbl_info = asyncinfo_vtbl;
    impl->ref = 1;
    impl->file = file;
    *out = (IUnknown *)impl;
    return S_OK;
}

static HRESULT WINAPI picker_PickSingleFileAsync(void *iface, IUnknown **operation)
{
    picker *impl = (picker *)iface;
    WCHAR path[MAX_PATH];
    IUnknown *file = nullptr;

    if (!operation) return E_POINTER;
    *operation = nullptr;

    /* FolderPicker slot 15 is PickSingleFolderAsync. There is no folder UI; report a cancelled
     * pick, which callers handle (it is what closing the dialog does). */
    if (impl->folder)
    {
        IUnknown *folder = nullptr;
        HRESULT hr;
        if (dialogs_enabled() && folder_dialog(path)) folder = storage_folder_from_path(path);
        else trace("PickSingleFolderAsync: reporting a cancelled pick");
        hr = make_async_file(folder, operation);
        if (SUCCEEDED(hr)) ((async_file *)*operation)->folder = true;
        return hr;
    }

    if (!GetEnvironmentVariableW(L"WINE_UWP_PICKER_FILE", path, ARRAYSIZE(path)))
    {
        WCHAR patterns[256];
        picker_patterns(impl, patterns, ARRAYSIZE(patterns));
        if (!dialogs_enabled() || !open_file_dialog(patterns, path, ARRAYSIZE(path)))
        {
            /* a cancelled pick, which is what a user closing the dialog does */
            trace("no file picked; reporting a cancelled pick");
            return make_async_file(nullptr, operation);
        }
        file = storage_file_from_path(path);
        trace("PickSingleFileAsync -> %ls (%p)", path, file);
        return make_async_file(file, operation);
    }
    if (!matches_filter(impl, path))
        return make_async_file(nullptr, operation);

    file = storage_file_from_path(path);
    trace("PickSingleFileAsync -> %ls (%p)", path, file);
    return make_async_file(file, operation);
}

/* ---------------------------------------------------------------- IVectorView<StorageFile>
 *
 * "Open file(s)" is the plural button, so this is the path Media Player actually takes. The view
 * holds the one staged file; .NET enumerates it through IIterable, so that has to be answered too
 * or the caller can only read the count.
 */
struct file_view
{
    const void **vtbl;            /* IVectorView<StorageFile> */
    const void **vtbl_iterable;   /* IIterable<StorageFile> */
    LONG ref;
    IUnknown *file;
};

struct file_iterator
{
    const void **vtbl;
    LONG ref;
    file_view *view;
    bool done;
};

static const void *fileview_vtbl[10];
static const void *fileiterable_vtbl[7];
static const void *fileiterator_vtbl[10];

static file_view *fv_from_iterable(void *i) { return CONTAINING_RECORD(i, file_view, vtbl_iterable); }

static HRESULT WINAPI fv_QueryInterface(void *iface, REFIID iid, void **out)
{
    file_view *impl = (file_view *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IVectorView_StorageFile_))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IIterable_StorageFile_))
        *out = &impl->vtbl_iterable;
    else { *out = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI fv_AddRef(void *iface) { return InterlockedIncrement(&((file_view *)iface)->ref); }
static ULONG WINAPI fv_Release(void *iface)
{
    file_view *impl = (file_view *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->file) impl->file->Release(); free(impl); }
    return ref;
}
static HRESULT WINAPI fv_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.Collections.IVectorView`1<Windows.Storage.StorageFile>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI fv_GetAt(void *iface, UINT32 index, IUnknown **value)
{
    file_view *impl = (file_view *)iface;
    if (!value) return E_POINTER;
    *value = nullptr;
    if (index || !impl->file) return E_BOUNDS;
    impl->file->AddRef();
    *value = impl->file;
    return S_OK;
}
static HRESULT WINAPI fv_get_Size(void *iface, UINT32 *value)
{
    file_view *impl = (file_view *)iface;
    if (!value) return E_POINTER;
    *value = impl->file ? 1 : 0;
    trace("view get_Size -> %u", *value);
    return S_OK;
}
static HRESULT WINAPI fv_IndexOf(void *iface, IUnknown *item, UINT32 *index, boolean *found)
{
    file_view *impl = (file_view *)iface;
    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = (impl->file && item == impl->file) ? 1 : 0;
    return S_OK;
}
static HRESULT WINAPI fv_GetMany(void *iface, UINT32 start, UINT32 count, IUnknown **items, UINT32 *written)
{
    file_view *impl = (file_view *)iface;
    if (!written) return E_POINTER;
    *written = 0;
    if (!start && count && impl->file) { impl->file->AddRef(); items[0] = impl->file; *written = 1; }
    return S_OK;
}

static HRESULT WINAPI fit_QueryInterface(void *iface, REFIID iid, void **out)
{
    file_iterator *impl = (file_iterator *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IIterator_StorageFile_))
    { InterlockedIncrement(&impl->ref); *out = iface; return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI fit_AddRef(void *iface) { return InterlockedIncrement(&((file_iterator *)iface)->ref); }
static ULONG WINAPI fit_Release(void *iface)
{
    file_iterator *impl = (file_iterator *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { fv_Release(impl->view); free(impl); }
    return ref;
}
static HRESULT WINAPI fit_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.StorageFile>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI fit_get_Current(void *iface, IUnknown **value)
{
    file_iterator *impl = (file_iterator *)iface;
    if (!value) return E_POINTER;
    *value = (impl->done || !impl->view->file) ? nullptr : impl->view->file;
    if (*value) (*value)->AddRef();
    return S_OK;
}
static HRESULT WINAPI fit_get_HasCurrent(void *iface, boolean *value)
{
    file_iterator *impl = (file_iterator *)iface;
    if (!value) return E_POINTER;
    *value = (!impl->done && impl->view->file) ? 1 : 0;
    return S_OK;
}
static HRESULT WINAPI fit_MoveNext(void *iface, boolean *value)
{
    file_iterator *impl = (file_iterator *)iface;
    impl->done = true;
    if (value) *value = 0;
    return S_OK;
}
static HRESULT WINAPI fit_GetMany(void *iface, UINT32 count, IUnknown **items, UINT32 *written)
{
    file_iterator *impl = (file_iterator *)iface;
    if (!written) return E_POINTER;
    *written = 0;
    if (!impl->done && count && impl->view->file)
    { impl->view->file->AddRef(); items[0] = impl->view->file; impl->done = true; *written = 1; }
    return S_OK;
}
static HRESULT WINAPI fv_First(void *iface, void **value)
{
    file_view *impl = fv_from_iterable(iface);
    file_iterator *it;
    if (!value) return E_POINTER;
    if (!(it = (file_iterator *)calloc(1, sizeof(*it)))) return E_OUTOFMEMORY;
    it->vtbl = fileiterator_vtbl;
    it->ref = 1;
    it->view = impl;
    fv_AddRef(impl);
    trace("view First()");
    *value = it;
    return S_OK;
}
static HRESULT WINAPI fvi_QueryInterface(void *iface, REFIID iid, void **out)
{ return fv_QueryInterface(fv_from_iterable(iface), iid, out); }
static ULONG WINAPI fvi_AddRef(void *iface) { return fv_AddRef(fv_from_iterable(iface)); }
static ULONG WINAPI fvi_Release(void *iface) { return fv_Release(fv_from_iterable(iface)); }

/* IAsyncOperation<IVectorView<StorageFile>>, completed like the single-file one. */
struct async_files
{
    const void **vtbl;
    const void **vtbl_info;
    LONG ref;
    file_view *view;
};
static const void *asyncfiles_vtbl[9];
static const void *asyncfilesinfo_vtbl[11];
static async_files *afs_from_info(void *i) { return CONTAINING_RECORD(i, async_files, vtbl_info); }

static HRESULT WINAPI afs_QueryInterface(void *iface, REFIID iid, void **out)
{
    async_files *impl = (async_files *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IAsyncOp_Files_))
        *out = &impl->vtbl;
    else if (IsEqualGUID(iid, IID_IAsyncInfo_)) *out = &impl->vtbl_info;
    else { *out = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&impl->ref);
    return S_OK;
}
static ULONG WINAPI afs_AddRef(void *iface) { return InterlockedIncrement(&((async_files *)iface)->ref); }
static ULONG WINAPI afs_Release(void *iface)
{
    async_files *impl = (async_files *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { if (impl->view) fv_Release(impl->view); free(impl); }
    return ref;
}
static HRESULT WINAPI afs_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s =
        L"Windows.Foundation.IAsyncOperation`1<Windows.Foundation.Collections.IVectorView`1<Windows.Storage.StorageFile>>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
static HRESULT WINAPI afs_put_Completed(void *iface, IUnknown *handler)
{
    if (!handler) return S_OK;
    trace("files put_Completed: invoking now");
    handler->AddRef();
    typedef HRESULT (WINAPI *invoke)(IUnknown *, void *, INT32);
    ((invoke *)(*(void ***)handler))[3](handler, iface, ASYNC_COMPLETED);
    handler->Release();
    return S_OK;
}
static HRESULT WINAPI afs_get_Completed(void *, IUnknown **handler)
{ if (!handler) return E_POINTER; *handler = nullptr; return S_OK; }
static HRESULT WINAPI afs_GetResults(void *iface, void **results)
{
    async_files *impl = (async_files *)iface;
    if (!results) return E_POINTER;
    fv_AddRef(impl->view);
    *results = &impl->view->vtbl;
    trace("files GetResults -> %p", *results);
    return S_OK;
}
static HRESULT WINAPI afsi_QueryInterface(void *iface, REFIID iid, void **out)
{ return afs_QueryInterface(afs_from_info(iface), iid, out); }
static ULONG WINAPI afsi_AddRef(void *iface) { return afs_AddRef(afs_from_info(iface)); }
static ULONG WINAPI afsi_Release(void *iface) { return afs_Release(afs_from_info(iface)); }

static HRESULT WINAPI picker_PickMultipleFilesAsync(void *iface, IUnknown **operation)
{
    picker *impl = (picker *)iface;
    WCHAR path[MAX_PATH];
    file_view *view;
    async_files *async;

    if (!operation) return E_POINTER;
    *operation = nullptr;

    if (!(view = (file_view *)calloc(1, sizeof(*view)))) return E_OUTOFMEMORY;
    view->vtbl = fileview_vtbl;
    view->vtbl_iterable = fileiterable_vtbl;
    view->ref = 1;

    /* An empty list is a cancelled pick, which is what closing the dialog does. One file only:
     * the view holds a single entry. */
    if (GetEnvironmentVariableW(L"WINE_UWP_PICKER_FILE", path, ARRAYSIZE(path)))
    {
        if (matches_filter(impl, path)) view->file = storage_file_from_path(path);
    }
    else
    {
        WCHAR patterns[256];
        picker_patterns(impl, patterns, ARRAYSIZE(patterns));
        if (dialogs_enabled() && open_file_dialog(patterns, path, ARRAYSIZE(path)))
            view->file = storage_file_from_path(path);
        else
            path[0] = 0;
    }
    if (!view->file) trace("PickMultipleFilesAsync: nothing picked; empty selection");

    if (!(async = (async_files *)calloc(1, sizeof(*async)))) { fv_Release(view); return E_OUTOFMEMORY; }
    async->vtbl = asyncfiles_vtbl;
    async->vtbl_info = asyncfilesinfo_vtbl;
    async->ref = 1;
    async->view = view;

    trace("PickMultipleFilesAsync -> %ls (%p)", path, view->file);
    *operation = (IUnknown *)async;
    return S_OK;
}

/* ---------------------------------------------------------------- FileSavePicker
 *
 * Shapr3D constructs one as soon as its dashboard is built (Exporter's constructor), and the
 * native one fails in its constructor for want of the shell broker, so the dashboard never came
 * up after sign-in. There is no save UI here either: PickSaveFileAsync reports a cancelled pick,
 * as the open picker does when no file is staged, unless WINE_UWP_PICKER_SAVE_FILE names a path,
 * which is then created and handed back.
 *
 *   IFileSavePicker {3286ffcb-617f-4cc5-af6a-b3fdf29ad145}:
 *      6/7 SettingsIdentifier  8/9 SuggestedStartLocation  10/11 CommitButtonText
 *      12 FileTypeChoices  13/14 DefaultFileExtension  15/16 SuggestedSaveFile
 *      17/18 SuggestedFileName  19 PickSaveFileAsync
 *
 * FileTypeChoices is an IMap<String, IVector<String>> the caller adds to. It is a PropertySet
 * underneath - the same vtable shape with IInspectable values - behind a wrapper that answers the
 * parameterised IID, {e475ca9d-6afb-5992-993e-53e6ef7a9ecd} (piid.py --signature, checked against
 * the known IVector<String> and IMap<String, Object>).
 */
DEF_IID(IID_IFileSavePicker_,     0x3286ffcb,0x617f,0x4cc5, 0xaf,0x6a,0xb3,0xfd,0xf2,0x9a,0xd1,0x45);
DEF_IID(IID_IMap_String_Vector_,  0xe475ca9d,0x6afb,0x5992, 0x99,0x3e,0x53,0xe6,0xef,0x7a,0x9e,0xcd);
DEF_IID(IID_IMap_String_Object_,  0x1b0d3570,0x0877,0x5ec2, 0x8a,0x2c,0x3b,0x95,0x39,0x50,0x6a,0xca);

struct choices
{
    const void **vtbl;
    LONG ref;
    IUnknown *inner;   /* IMap<HSTRING, IInspectable> of a PropertySet */
};
static const void *choices_vtbl[13];

static HRESULT WINAPI ch_QueryInterface(void *iface, REFIID iid, void **out)
{
    choices *impl = (choices *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IMap_String_Vector_))
    {
        InterlockedIncrement(&impl->ref);
        *out = iface;
        return S_OK;
    }
    trace("FileTypeChoices QI {%08lx-...} -> E_NOINTERFACE", iid.Data1);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI ch_AddRef(void *iface) { return InterlockedIncrement(&((choices *)iface)->ref); }
static ULONG WINAPI ch_Release(void *iface)
{
    choices *impl = (choices *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) { impl->inner->Release(); free(impl); }
    return ref;
}
static HRESULT WINAPI ch_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Foundation.Collections.IMap`2<String, Windows.Foundation.Collections.IVector`1<String>>";
    return WindowsCreateString(s, lstrlenW(s), name);
}
/* Slots 6..12 - Lookup, Size, HasKey, GetView, Insert, Remove, Clear - forward unchanged. */
#define CH_FORWARD(n, name, params, args)                                                         \
    static HRESULT WINAPI ch_##name params                                                        \
    {                                                                                             \
        IUnknown *inner = ((choices *)iface)->inner;                                              \
        typedef HRESULT (WINAPI *fn) params;                                                      \
        return ((fn *)(*(void ***)inner))[n] args;                                                \
    }
CH_FORWARD(6,  Lookup,  (void *iface, HSTRING key, IUnknown **value), (inner, key, value))
CH_FORWARD(7,  get_Size,(void *iface, UINT32 *size), (inner, size))
CH_FORWARD(8,  HasKey,  (void *iface, HSTRING key, boolean *found), (inner, key, found))
CH_FORWARD(9,  GetView, (void *iface, IUnknown **view), (inner, view))
CH_FORWARD(10, Insert,  (void *iface, HSTRING key, IUnknown *value, boolean *replaced), (inner, key, value, replaced))
CH_FORWARD(11, Remove,  (void *iface, HSTRING key), (inner, key))
CH_FORWARD(12, Clear,   (void *iface), (inner))

static HRESULT make_choices(IUnknown **out)
{
    static const WCHAR *cls = L"Windows.Foundation.Collections.PropertySet";
    IInspectable *set = nullptr;
    IUnknown *map = nullptr;
    HSTRING name = nullptr;
    choices *impl;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateString(cls, lstrlenW(cls), &name))) return hr;
    hr = RoActivateInstance(name, &set);
    WindowsDeleteString(name);
    if (FAILED(hr)) return hr;
    hr = set->QueryInterface(IID_IMap_String_Object_, (void **)&map);
    set->Release();
    if (FAILED(hr)) return hr;
    if (!(impl = (choices *)calloc(1, sizeof(*impl)))) { map->Release(); return E_OUTOFMEMORY; }
    impl->vtbl = choices_vtbl;
    impl->ref = 1;
    impl->inner = map;
    *out = (IUnknown *)impl;
    return S_OK;
}

/* IIterable<IKeyValuePair<String, Object>>, which Wine's PropertySet answers */
DEF_IID(IID_IIterable_KVP_String_Object_, 0xfe2f3d47,0x5d47,0x5499, 0x83,0x74,0x43,0x0c,0x7c,0xda,0x02,0x04);
DEF_IID(IID_IVector_String_,              0x98b9acc1,0x4b56,0x532e, 0xac,0x73,0x03,0xd5,0x29,0x1c,0xca,0x90);

static BOOL choices_filter(IUnknown *map, WCHAR *filter, size_t size, WCHAR *first_ext, size_t ext_size)
{
    typedef HRESULT (WINAPI *get_obj)(void *, IUnknown **);
    typedef HRESULT (WINAPI *get_bool)(void *, boolean *);
    typedef HRESULT (WINAPI *get_str)(void *, HSTRING *);
    typedef HRESULT (WINAPI *get_u32)(void *, UINT32 *);
    typedef HRESULT (WINAPI *get_at)(void *, UINT32, HSTRING *);
    IUnknown *iterable = nullptr, *it = nullptr;
    boolean has = FALSE;
    size_t n = 0;
    BOOL any = FALSE;
    auto put = [&](const WCHAR *str) {
        size_t len = wcslen(str);
        if (n + len + 2 >= size) return;
        memcpy(filter + n, str, len * sizeof(WCHAR));
        n += len;
    };
    auto end = [&]() { if (n + 1 < size) filter[n++] = 0; };

    first_ext[0] = 0;
    if (FAILED(map->QueryInterface(IID_IIterable_KVP_String_Object_, (void **)&iterable))) return FALSE;
    if (FAILED(((get_obj *)(*(void ***)iterable))[6](iterable, &it)) || !it) { iterable->Release(); return FALSE; }
    iterable->Release();
    ((get_bool *)(*(void ***)it))[7](it, &has);
    while (has)
    {
        IUnknown *pair = nullptr, *value = nullptr, *vector = nullptr;
        HSTRING key = nullptr;
        UINT32 count = 0, i;
        WCHAR patterns[256] = L"";

        if (SUCCEEDED(((get_obj *)(*(void ***)it))[6](it, &pair)) && pair)
        {
            ((get_str *)(*(void ***)pair))[6](pair, &key);
            if (SUCCEEDED(((get_obj *)(*(void ***)pair))[7](pair, &value)) && value &&
                SUCCEEDED(value->QueryInterface(IID_IVector_String_, (void **)&vector)))
            {
                ((get_u32 *)(*(void ***)vector))[7](vector, &count);
                for (i = 0; i < count; i++)
                {
                    HSTRING ext = nullptr;
                    if (FAILED(((get_at *)(*(void ***)vector))[6](vector, i, &ext))) continue;
                    const WCHAR *e = WindowsGetStringRawBuffer(ext, nullptr);
                    append_pattern(patterns, ARRAYSIZE(patterns), e);
                    if (!first_ext[0] && e && *e == '.') lstrcpynW(first_ext, e, ext_size);
                    WindowsDeleteString(ext);
                }
                vector->Release();
            }
            if (patterns[0])
            {
                const WCHAR *name = WindowsGetStringRawBuffer(key, nullptr);
                put(name && *name ? name : patterns); put(L" ("); put(patterns); put(L")"); end();
                put(patterns); end();
                any = TRUE;
                trace("file type choice %ls: %ls", name, patterns);
            }
            if (value) value->Release();
            WindowsDeleteString(key);
            pair->Release();
        }
        has = FALSE;
        if (FAILED(((get_bool *)(*(void ***)it))[8](it, &has))) break;
    }
    it->Release();
    if (!any) return FALSE;
    put(L"All files (*.*)"); end(); put(L"*.*"); end();
    if (n < size) filter[n] = 0;
    return TRUE;
}

struct save_picker
{
    const void **vtbl;
    LONG ref;
    INT32 start_location;
    HSTRING settings_id, commit_text, default_ext, file_name;
    IUnknown *suggested_file;
    IUnknown *choices;
};
static const void *save_vtbl[20];

static HRESULT WINAPI sp_QueryInterface(void *iface, REFIID iid, void **out)
{
    save_picker *impl = (save_picker *)iface;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IFileSavePicker_))
    {
        InterlockedIncrement(&impl->ref);
        *out = iface;
        return S_OK;
    }
    trace("FileSavePicker QI {%08lx-...} -> E_NOINTERFACE", iid.Data1);
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI sp_AddRef(void *iface) { return InterlockedIncrement(&((save_picker *)iface)->ref); }
static ULONG WINAPI sp_Release(void *iface)
{
    save_picker *impl = (save_picker *)iface;
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref)
    {
        WindowsDeleteString(impl->settings_id);
        WindowsDeleteString(impl->commit_text);
        WindowsDeleteString(impl->default_ext);
        WindowsDeleteString(impl->file_name);
        if (impl->suggested_file) impl->suggested_file->Release();
        if (impl->choices) impl->choices->Release();
        free(impl);
    }
    return ref;
}
static HRESULT WINAPI sp_GetRuntimeClassName(void *, HSTRING *name)
{
    static const WCHAR *s = L"Windows.Storage.Pickers.FileSavePicker";
    return WindowsCreateString(s, lstrlenW(s), name);
}

#define SAVE_STRING(name, field)                                                                   \
    static HRESULT WINAPI sp_get_##name(void *iface, HSTRING *v)                                   \
    { if (!v) return E_POINTER; return WindowsDuplicateString(((save_picker *)iface)->field, v); } \
    static HRESULT WINAPI sp_put_##name(void *iface, HSTRING v)                                    \
    {                                                                                              \
        save_picker *impl = (save_picker *)iface;                                                  \
        HSTRING copy = nullptr;                                                                    \
        HRESULT hr = WindowsDuplicateString(v, &copy);                                             \
        if (FAILED(hr)) return hr;                                                                 \
        WindowsDeleteString(impl->field);                                                          \
        impl->field = copy;                                                                        \
        trace("FileSavePicker " #name " = %ls", WindowsGetStringRawBuffer(v, nullptr));            \
        return S_OK;                                                                               \
    }
SAVE_STRING(SettingsIdentifier, settings_id)
SAVE_STRING(CommitButtonText, commit_text)
SAVE_STRING(DefaultFileExtension, default_ext)
SAVE_STRING(SuggestedFileName, file_name)

static HRESULT WINAPI sp_get_StartLocation(void *iface, INT32 *v)
{ if (!v) return E_POINTER; *v = ((save_picker *)iface)->start_location; return S_OK; }
static HRESULT WINAPI sp_put_StartLocation(void *iface, INT32 v)
{ ((save_picker *)iface)->start_location = v; return S_OK; }

static HRESULT WINAPI sp_get_FileTypeChoices(void *iface, IUnknown **value)
{
    save_picker *impl = (save_picker *)iface;
    HRESULT hr;
    if (!value) return E_POINTER;
    if (!impl->choices && FAILED(hr = make_choices(&impl->choices))) { *value = nullptr; return hr; }
    impl->choices->AddRef();
    *value = impl->choices;
    return S_OK;
}

static HRESULT WINAPI sp_get_SuggestedSaveFile(void *iface, IUnknown **value)
{
    save_picker *impl = (save_picker *)iface;
    if (!value) return E_POINTER;
    if ((*value = impl->suggested_file)) (*value)->AddRef();
    return S_OK;
}
static HRESULT WINAPI sp_put_SuggestedSaveFile(void *iface, IUnknown *value)
{
    save_picker *impl = (save_picker *)iface;
    if (value) value->AddRef();
    if (impl->suggested_file) impl->suggested_file->Release();
    impl->suggested_file = value;
    return S_OK;
}

static HRESULT WINAPI sp_PickSaveFileAsync(void *iface, IUnknown **operation)
{
    save_picker *impl = (save_picker *)iface;
    WCHAR path[MAX_PATH];
    HANDLE file;

    if (!operation) return E_POINTER;
    *operation = nullptr;
    if (!GetEnvironmentVariableW(L"WINE_UWP_PICKER_SAVE_FILE", path, ARRAYSIZE(path)) &&
        !(dialogs_enabled() &&
          save_file_dialog(impl->file_name ? WindowsGetStringRawBuffer(impl->file_name, nullptr) : nullptr,
                           impl->default_ext ? WindowsGetStringRawBuffer(impl->default_ext, nullptr) : nullptr,
                           impl->choices ? ((choices *)impl->choices)->inner : nullptr,
                           path, ARRAYSIZE(path))))
    {
        trace("PickSaveFileAsync (suggested %ls): nothing picked; reporting a cancelled pick",
              impl->file_name ? WindowsGetStringRawBuffer(impl->file_name, nullptr) : L"");
        return make_async_file(nullptr, operation);
    }
    /* A save picker hands back a file that exists, so create it (empty) if it does not. */
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    trace("PickSaveFileAsync -> %ls", path);
    return make_async_file(storage_file_from_path(path), operation);
}

/* ---------------------------------------------------------------- activation */

struct statics_object { const void **vtbl; };
static const void *factory_vtbl[7];
static statics_object factory;

/* Shared by the three factories, so it must hand back the one it was called on. */
static HRESULT WINAPI factory_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgile_) || IsEqualGUID(iid, IID_IActivationFactory))
    { *out = iface; return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG WINAPI factory_AddRef(void *) { return 2; }
static ULONG WINAPI factory_Release(void *) { return 1; }
static HRESULT WINAPI factory_ActivateInstance(void *, IInspectable **instance)
{
    picker *impl;
    if (!instance) return E_POINTER;
    if (!(impl = (picker *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = picker_vtbl;
    impl->ref = 1;
    trace("FileOpenPicker created");
    *instance = (IInspectable *)impl;
    return S_OK;
}

static HRESULT WINAPI save_factory_ActivateInstance(void *, IInspectable **instance)
{
    save_picker *impl;
    if (!instance) return E_POINTER;
    if (!(impl = (save_picker *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = save_vtbl;
    impl->ref = 1;
    trace("FileSavePicker created");
    *instance = (IInspectable *)impl;
    return S_OK;
}
static const void *save_factory_vtbl[7];
static statics_object save_factory;

static HRESULT WINAPI folder_factory_ActivateInstance(void *, IInspectable **instance)
{
    picker *impl;
    if (!instance) return E_POINTER;
    if (!(impl = (picker *)calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->vtbl = picker_vtbl;
    impl->ref = 1;
    impl->folder = true;
    trace("FolderPicker created");
    *instance = (IInspectable *)impl;
    return S_OK;
}
static const void *folder_factory_vtbl[7];
static statics_object folder_factory;

static void init_vtbls(void)
{
    async_vtbl[0] = (void *)af_QueryInterface;
    async_vtbl[1] = (void *)af_AddRef;
    async_vtbl[2] = (void *)af_Release;
    async_vtbl[3] = (void *)insp_GetIids;
    async_vtbl[4] = (void *)af_GetRuntimeClassName;
    async_vtbl[5] = (void *)insp_GetTrustLevel;
    async_vtbl[6] = (void *)af_put_Completed;
    async_vtbl[7] = (void *)af_get_Completed;
    async_vtbl[8] = (void *)af_GetResults;

    asyncinfo_vtbl[0] = (void *)ai_QueryInterface;
    asyncinfo_vtbl[1] = (void *)ai_AddRef;
    asyncinfo_vtbl[2] = (void *)ai_Release;
    asyncinfo_vtbl[3] = (void *)insp_GetIids;
    asyncinfo_vtbl[4] = (void *)af_GetRuntimeClassName;
    asyncinfo_vtbl[5] = (void *)insp_GetTrustLevel;
    asyncinfo_vtbl[6] = (void *)ai_get_Id;
    asyncinfo_vtbl[7] = (void *)ai_get_Status;
    asyncinfo_vtbl[8] = (void *)ai_get_ErrorCode;
    asyncinfo_vtbl[9] = (void *)ai_Cancel;
    asyncinfo_vtbl[10] = (void *)ai_Close;

    picker_vtbl[0]  = (void *)picker_QueryInterface;
    picker_vtbl[1]  = (void *)picker_AddRef;
    picker_vtbl[2]  = (void *)picker_Release;
    picker_vtbl[3]  = (void *)insp_GetIids;
    picker_vtbl[4]  = (void *)picker_GetRuntimeClassName;
    picker_vtbl[5]  = (void *)insp_GetTrustLevel;
    picker_vtbl[6]  = (void *)picker_get_ViewMode;
    picker_vtbl[7]  = (void *)picker_put_ViewMode;
    picker_vtbl[8]  = (void *)picker_get_SettingsIdentifier;
    picker_vtbl[9]  = (void *)picker_put_SettingsIdentifier;
    picker_vtbl[10] = (void *)picker_get_StartLocation;
    picker_vtbl[11] = (void *)picker_put_StartLocation;
    picker_vtbl[12] = (void *)picker_get_CommitButtonText;
    picker_vtbl[13] = (void *)picker_put_CommitButtonText;
    picker_vtbl[14] = (void *)picker_get_FileTypeFilter;
    picker_vtbl[15] = (void *)picker_PickSingleFileAsync;
    picker_vtbl[16] = (void *)picker_PickMultipleFilesAsync;

    fileview_vtbl[0] = (void *)fv_QueryInterface;
    fileview_vtbl[1] = (void *)fv_AddRef;
    fileview_vtbl[2] = (void *)fv_Release;
    fileview_vtbl[3] = (void *)insp_GetIids;
    fileview_vtbl[4] = (void *)fv_GetRuntimeClassName;
    fileview_vtbl[5] = (void *)insp_GetTrustLevel;
    fileview_vtbl[6] = (void *)fv_GetAt;
    fileview_vtbl[7] = (void *)fv_get_Size;
    fileview_vtbl[8] = (void *)fv_IndexOf;
    fileview_vtbl[9] = (void *)fv_GetMany;

    fileiterable_vtbl[0] = (void *)fvi_QueryInterface;
    fileiterable_vtbl[1] = (void *)fvi_AddRef;
    fileiterable_vtbl[2] = (void *)fvi_Release;
    fileiterable_vtbl[3] = (void *)insp_GetIids;
    fileiterable_vtbl[4] = (void *)fv_GetRuntimeClassName;
    fileiterable_vtbl[5] = (void *)insp_GetTrustLevel;
    fileiterable_vtbl[6] = (void *)fv_First;

    fileiterator_vtbl[0] = (void *)fit_QueryInterface;
    fileiterator_vtbl[1] = (void *)fit_AddRef;
    fileiterator_vtbl[2] = (void *)fit_Release;
    fileiterator_vtbl[3] = (void *)insp_GetIids;
    fileiterator_vtbl[4] = (void *)fit_GetRuntimeClassName;
    fileiterator_vtbl[5] = (void *)insp_GetTrustLevel;
    fileiterator_vtbl[6] = (void *)fit_get_Current;
    fileiterator_vtbl[7] = (void *)fit_get_HasCurrent;
    fileiterator_vtbl[8] = (void *)fit_MoveNext;
    fileiterator_vtbl[9] = (void *)fit_GetMany;

    asyncfiles_vtbl[0] = (void *)afs_QueryInterface;
    asyncfiles_vtbl[1] = (void *)afs_AddRef;
    asyncfiles_vtbl[2] = (void *)afs_Release;
    asyncfiles_vtbl[3] = (void *)insp_GetIids;
    asyncfiles_vtbl[4] = (void *)afs_GetRuntimeClassName;
    asyncfiles_vtbl[5] = (void *)insp_GetTrustLevel;
    asyncfiles_vtbl[6] = (void *)afs_put_Completed;
    asyncfiles_vtbl[7] = (void *)afs_get_Completed;
    asyncfiles_vtbl[8] = (void *)afs_GetResults;

    asyncfilesinfo_vtbl[0] = (void *)afsi_QueryInterface;
    asyncfilesinfo_vtbl[1] = (void *)afsi_AddRef;
    asyncfilesinfo_vtbl[2] = (void *)afsi_Release;
    asyncfilesinfo_vtbl[3] = (void *)insp_GetIids;
    asyncfilesinfo_vtbl[4] = (void *)afs_GetRuntimeClassName;
    asyncfilesinfo_vtbl[5] = (void *)insp_GetTrustLevel;
    asyncfilesinfo_vtbl[6] = (void *)ai_get_Id;
    asyncfilesinfo_vtbl[7] = (void *)ai_get_Status;
    asyncfilesinfo_vtbl[8] = (void *)ai_get_ErrorCode;
    asyncfilesinfo_vtbl[9] = (void *)ai_Cancel;
    asyncfilesinfo_vtbl[10] = (void *)ai_Close;

    factory_vtbl[0] = (void *)factory_QueryInterface;
    factory_vtbl[1] = (void *)factory_AddRef;
    factory_vtbl[2] = (void *)factory_Release;
    factory_vtbl[3] = (void *)insp_GetIids;
    factory_vtbl[4] = (void *)picker_GetRuntimeClassName;
    factory_vtbl[5] = (void *)insp_GetTrustLevel;
    factory_vtbl[6] = (void *)factory_ActivateInstance;
    factory.vtbl = factory_vtbl;

    memcpy(save_factory_vtbl, factory_vtbl, sizeof(factory_vtbl));
    save_factory_vtbl[4] = (void *)sp_GetRuntimeClassName;
    save_factory_vtbl[6] = (void *)save_factory_ActivateInstance;
    save_factory.vtbl = save_factory_vtbl;

    memcpy(folder_factory_vtbl, factory_vtbl, sizeof(factory_vtbl));
    folder_factory_vtbl[6] = (void *)folder_factory_ActivateInstance;
    folder_factory.vtbl = folder_factory_vtbl;

    const void *sv[] = {
        (void *)sp_QueryInterface, (void *)sp_AddRef, (void *)sp_Release, (void *)insp_GetIids,
        (void *)sp_GetRuntimeClassName, (void *)insp_GetTrustLevel,
        (void *)sp_get_SettingsIdentifier, (void *)sp_put_SettingsIdentifier,
        (void *)sp_get_StartLocation, (void *)sp_put_StartLocation,
        (void *)sp_get_CommitButtonText, (void *)sp_put_CommitButtonText,
        (void *)sp_get_FileTypeChoices,
        (void *)sp_get_DefaultFileExtension, (void *)sp_put_DefaultFileExtension,
        (void *)sp_get_SuggestedSaveFile, (void *)sp_put_SuggestedSaveFile,
        (void *)sp_get_SuggestedFileName, (void *)sp_put_SuggestedFileName,
        (void *)sp_PickSaveFileAsync,
    };
    memcpy(save_vtbl, sv, sizeof(sv));

    const void *cv[] = {
        (void *)ch_QueryInterface, (void *)ch_AddRef, (void *)ch_Release, (void *)insp_GetIids,
        (void *)ch_GetRuntimeClassName, (void *)insp_GetTrustLevel,
        (void *)ch_Lookup, (void *)ch_get_Size, (void *)ch_HasKey, (void *)ch_GetView,
        (void *)ch_Insert, (void *)ch_Remove, (void *)ch_Clear,
    };
    memcpy(choices_vtbl, cv, sizeof(cv));
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *str = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (str && !lstrcmpW(str, L"Windows.Storage.Pickers.FolderPicker"))
    {
        init_vtbls();
        trace("serving %ls", str);
        *out = (IActivationFactory *)&folder_factory;
        return S_OK;
    }
    if (str && !lstrcmpW(str, L"Windows.Storage.Pickers.FileSavePicker"))
    {
        init_vtbls();
        trace("serving %ls", str);
        *out = (IActivationFactory *)&save_factory;
        return S_OK;
    }
    if (!str || lstrcmpW(str, L"Windows.Storage.Pickers.FileOpenPicker"))
    {
        trace("not ours: %ls", str ? str : L"(null)");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    init_vtbls();
    trace("serving %ls", str);
    *out = (IActivationFactory *)&factory;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

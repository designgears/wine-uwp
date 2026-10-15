/* ------------------------------------------------------------------------------------------- */
/* Installing Store packages: PackageRequest, PackageQueue and the installed Package.              */
/*                                                                                               */
/* Gaming Services installs a game the user owns from the Store's delivery service; so does this, for the     */
/* titles that are ordinary MSIX/APPX packages (not the encrypted MSIXVC that needs Gaming Services' drivers):  */
/*   1. the product's Windows Update category, from the public display catalogue;                              */
/*   2. the package's download address from the Store's delivery service (FE3: SyncUpdates for the category,   */
/*      then GetExtendedUpdateInfo2 for the main package; anonymous, as for any free or owned app);           */
/*   3. wine-uwp's own installer (packaging/install-app.py, run on the host) streams the package from there   */
/*      into a stage directory, with its frameworks from the Windows files, and reports progress;            */
/*   4. the queue reports it as Gaming Services would (ItemAdded, InstallationChanged with progress,           */
/*      ItemRemoved), Storage.EnumerateContent lists it, and Package answers for it: install path, identity,  */
/*      launchable, and Activate starts it through its stage directory's wineuwp-launch.exe.                  */
/*                                                                                               */
/* Where things live comes from the environment run.sh sets: WINE_UWP_GAMES_DATA (the data directory with      */
/* stage/, a Unix path), WINE_UWP_INSTALLER (install-app.py) and WINE_UWP_WINDOWS_TREE (the harvested Windows  */
/* files the frameworks come from).                                                              */

static const GUID IID_IPackageRequest = {0x25198e18,0xd58c,0x5ee2,{0x9c,0x59,0x62,0xf3,0xd4,0x8d,0x84,0x0f}};
static const GUID IID_IPackageRequestSize = {0x929c3222,0x99f2,0x512b,{0x9a,0x55,0xf4,0x91,0x3b,0x1e,0xf8,0x7f}};
static const GUID IID_IPackageInstallationInfo = {0xa07926f8,0xe675,0x51e7,{0x84,0xca,0xce,0x0b,0xd8,0xd3,0xc2,0xf3}};
static const GUID IID_IPackageInstallationSummary = {0xdff0ab9f,0x5b87,0x5d0e,{0x94,0xa4,0x57,0xe2,0x68,0x86,0xa6,0xd9}};
static const GUID IID_IInstallationChangedEventArgs = {0xed49332e,0x122c,0x55ba,{0x9e,0x27,0x17,0x61,0xa9,0xa0,0xc0,0xb7}};
static const GUID IID_IPackageQueueItemEventArgs = {0xfb6c1626,0xe830,0x5f21,{0xa9,0xfb,0xed,0xec,0x84,0x13,0x78,0x07}};
static const GUID IID_IPackageQueueView = {0x7c061aed,0x9cdf,0x54b0,{0xbf,0xef,0x43,0x3f,0xed,0x4c,0x89,0x2c}};
/* IAsyncOperation<Guid>, from piid.py --signature "pinterface({9fc2b0bb-...};g16)" */
static const GUID IID_IAsyncOperation_Guid = {0x6607bc41,0x294b,0x5975,{0x9c,0x3f,0x4b,0x49,0x83,0x6d,0x09,0x16}};
static const GUID IID_IVectorView_HString = {0x2f13c006,0xa03a,0x5f69,{0xb0,0x90,0x75,0xa4,0x3e,0x33,0x42,0x3e}};
static const GUID IID_IIterable_HString = {0xe2fcc7c1,0x3bfc,0x5a0b,{0xb2,0xb0,0x72,0xe7,0x69,0xd1,0xcb,0x7e}};
static const GUID IID_IIterator_HString = {0x8c304ebb,0x6615,0x50a4,{0x88,0x29,0x87,0x9e,0xcd,0x44,0x32,0x36}};

enum { ST_IDLE = 0, ST_PAUSED = 1, ST_STREAMING = 2, ST_COMPLETED = 3, ST_FAILED = 4, ST_CANCELED = 5, ST_WAITING = 6 };

struct InstallMetric { UINT64 streamed, total; };
struct InstallDefragMetric { UINT64 streamed, total; INT32 state; };
struct InstallProgress { InstallMetric package, install, launch, network, optical; InstallDefragMetric defrag; UINT32 data_rate, disc; };
struct InstallFastStart { INT32 state; UINT32 buffer_seconds; UINT64 miss_bytes; UINT32 miss_count, miss_bytes_recent, miss_count_recent; };

struct Install
{
    GUID id;
    std::wstring instance, store_id, family, stage;
    INT32 state = ST_WAITING;
    HRESULT error = S_OK;
    UINT64 total = 0, done = 0;
    bool cancel = false;
};

static SRWLOCK installs_lock = SRWLOCK_INIT;
static std::vector<Install *> installs;     /* the queue: in progress, and finished ones until they are removed */

/* event handlers registered on PackageQueue: InstallationChanged, QueueChanged, ItemAdded, ItemRemoved */
enum { EV_INSTALLATION_CHANGED, EV_QUEUE_CHANGED, EV_ITEM_ADDED, EV_ITEM_REMOVED, EV_COUNT };
static SRWLOCK events_lock = SRWLOCK_INIT;
static std::vector<std::pair<LONGLONG, IUnknown *>> event_handlers[EV_COUNT];
static LONGLONG next_event_token = 100;

static std::string utf8(const std::wstring &w)
{
    std::string s(w.size() * 3 + 1, 0);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}

static std::wstring env_w(const wchar_t *name)
{
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : std::wstring();
}

/* the data directory games are installed into (Unix path), and the same as a Windows path */
static std::wstring games_data_unix(void)
{
    std::wstring d = env_w(L"WINE_UWP_GAMES_DATA");
    if (d.empty() && !(d = env_w(L"UWP_BIN")).empty()) d += L"/xbox-games";
    return d;
}
static std::wstring unix_to_windows(const std::wstring &p)
{
    std::wstring w = L"Z:" + p;
    for (auto &c : w) if (c == L'/') c = L'\\';
    return w;
}

/* A package instance id is a GUID string (the app parses it as one). Gaming Services makes one per installation;
 * this derives it from the package family name, so it is the same for the queued and the installed package. */
static std::wstring instance_for(const std::wstring &family)
{
    UINT64 h1 = 1469598103934665603ULL, h2 = 1099511628211ULL ^ 0x9e3779b97f4a7c15ULL;
    for (wchar_t c : family)
    {
        wchar_t l = towlower(c);
        h1 = (h1 ^ l) * 1099511628211ULL;
        h2 = (h2 ^ (l * 31u)) * 0x100000001b3ULL + 0x632be59bd9b4e019ULL;
    }
    wchar_t buf[40];
    swprintf(buf, 40, L"{%08X-%04X-%04X-%04X-%012llX}", (unsigned)(h1 >> 32), (unsigned)(h1 >> 16) & 0xffff,
             ((unsigned)h1 & 0x0fff) | 0x5000, ((unsigned)(h2 >> 48) & 0x3fff) | 0x8000, (unsigned long long)(h2 & 0xffffffffffffULL));
    return buf;
}

/* C++/WinRT takes weak references to what it is handed (weak_ref<IPackageQueueView> in the GameCatalog), and reads the
 * result of the QueryInterface without checking it: every object here answers IWeakReferenceSource. */
template <class T> static HRESULT weak_source(T *self, void **out)
{
    IUnknown *unk = nullptr;
    if (FAILED(self->QueryInterface(IID_IUnknown, (void **)&unk)) || !unk) { *out = nullptr; return E_NOINTERFACE; }
    *out = static_cast<IUnknown *>(new WeakSource(unk));
    unk->Release();
    return S_OK;
}

/* ---- the installed games: stage directories with a .wine-uwp-xbox marker ---- */

struct InstalledGame { std::wstring instance, store_id, family, full_name, stage_dir /* Windows path */, title, aumid; UINT64 size = 0; };

static bool read_marker(const std::wstring &dir, InstalledGame &g)
{
    HANDLE f = CreateFileW((dir + L"\\.wine-uwp-xbox").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n"))
    {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::wstring v = wide(eq + 1);
        if (!strcmp(line, "store_id")) g.store_id = v;
        else if (!strcmp(line, "family")) g.family = v;
        else if (!strcmp(line, "full_name")) g.full_name = v;
        else if (!strcmp(line, "title")) g.title = v;
        else if (!strcmp(line, "aumid")) g.aumid = v;
        else if (!strcmp(line, "size")) g.size = _wcstoui64(v.c_str(), nullptr, 10);
    }
    g.stage_dir = dir;
    g.instance = instance_for(g.family);
    return !g.family.empty();
}

static std::vector<InstalledGame> installed_games(void)
{
    std::vector<InstalledGame> out;
    std::wstring data = games_data_unix();
    if (data.empty()) return out;
    std::wstring stage = unix_to_windows(data) + L"\\stage";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((stage + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        InstalledGame g;
        if (read_marker(stage + L"\\" + fd.cFileName, g)) out.push_back(g);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static bool find_installed(const std::wstring &key, InstalledGame &g)
{
    for (auto &x : installed_games())
        if (!_wcsicmp(x.instance.c_str(), key.c_str()) || !_wcsicmp(x.store_id.c_str(), key.c_str()) ||
            !_wcsicmp(x.full_name.c_str(), key.c_str()) || !_wcsicmp(x.family.c_str(), key.c_str()))
        { g = x; return true; }
    return false;
}

/* ---- IVectorView<String> ---- */

struct IHStringViewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(HSTRING, UINT32 *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, HSTRING *, UINT32 *) = 0;
};
struct IHStringIterableAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE First(IInspectable **) = 0; };
struct IHStringIteratorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Current(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, HSTRING *, UINT32 *) = 0;
};
struct HStringIterator : IHStringIteratorAbi
{
    LONG ref = 1;
    std::vector<std::wstring> items;
    UINT32 pos = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IIterator_HString)) { *out = static_cast<IHStringIteratorAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.Collections.IIterator`1<String>")
    HRESULT STDMETHODCALLTYPE get_Current(HSTRING *v) override { if (pos >= items.size()) return E_BOUNDS; return hstr(items[pos], v); }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override { if (!v) return E_POINTER; *v = pos < items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override { if (pos < items.size()) pos++; if (v) *v = pos < items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 n, HSTRING *out, UINT32 *got) override
    {
        UINT32 k = 0;
        while (k < n && pos < items.size()) hstr(items[pos++], &out[k++]);
        if (got) *got = k;
        return S_OK;
    }
};
struct HStringView : IHStringViewAbi, IHStringIterableAbi
{
    LONG ref = 1;
    std::vector<std::wstring> items;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IVectorView_HString)) *out = static_cast<IHStringViewAbi *>(this);
        else if (IsEqualGUID(iid, IID_IIterable_HString)) *out = static_cast<IHStringIterableAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.Collections.IVectorView`1<String>")
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, HSTRING *v) override { if (i >= items.size()) return E_BOUNDS; return hstr(items[i], v); }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *v) override { if (!v) return E_POINTER; *v = (UINT32)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(HSTRING s, UINT32 *i, boolean *found) override
    {
        const wchar_t *w = WindowsGetStringRawBuffer(s, nullptr);
        for (UINT32 k = 0; k < items.size(); k++) if (items[k] == w) { *i = k; *found = TRUE; return S_OK; }
        *i = 0; *found = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 n, HSTRING *out, UINT32 *got) override
    {
        UINT32 k = 0;
        while (k < n && start + k < items.size()) { hstr(items[start + k], &out[k]); k++; }
        if (got) *got = k;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE First(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        HStringIterator *it = new HStringIterator;
        it->items = items;
        *out = static_cast<IHStringIteratorAbi *>(it);
        return S_OK;
    }
};

/* ---- PackageInstallationInfo / Summary: snapshots of an install ---- */

static InstallProgress progress_of(const Install &in)
{
    InstallProgress p = {};
    p.package.total = p.install.total = p.network.total = p.launch.total = in.total;
    p.package.streamed = p.install.streamed = p.network.streamed = in.done;
    /* launchable once it is all there */
    p.launch.streamed = in.state == ST_COMPLETED ? in.total : 0;
    return p;
}

struct IInstallationInfoAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_InstallationId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SourceInstanceId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TargetInstanceId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StoreId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BundleStoreId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Type(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WaitReason(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Sources(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FastStart(InstallFastStart *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Launchable(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LaunchRegionsStreaming(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EnhancedStreaming(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Error(HRESULT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DataRate(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StorageSpaceRequired(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PersistentLocalStorageSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PersistentLocalStorageCrdId(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BuildId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlanId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Progress(InstallProgress *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableDate(INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RemediationAction(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExtendedError(HRESULT *) = 0;
};
struct InstallationInfo : IInstallationInfoAbi
{
    LONG ref = 1;
    Install in;
    InstallationInfo(const Install &i) : in(i) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageInstallationInfo)) { *out = static_cast<IInstallationInfoAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageInstallationInfo")
    HRESULT STDMETHODCALLTYPE get_InstallationId(GUID *v) override { if (!v) return E_POINTER; *v = in.id; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SourceInstanceId(HSTRING *v) override { return hstr(in.instance, v); }
    HRESULT STDMETHODCALLTYPE get_TargetInstanceId(HSTRING *v) override { return hstr(in.instance, v); }
    HRESULT STDMETHODCALLTYPE get_StoreId(HSTRING *v) override { return hstr(in.store_id, v); }
    HRESULT STDMETHODCALLTYPE get_BundleStoreId(HSTRING *v) override { return hstr(L"", v); }
    HRESULT STDMETHODCALLTYPE get_Type(INT32 *v) override { if (!v) return E_POINTER; *v = 0; /* Install */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_State(INT32 *v) override { if (!v) return E_POINTER; *v = in.state; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_WaitReason(INT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Sources(UINT32 *v) override { if (!v) return E_POINTER; *v = 2; /* Network */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FastStart(InstallFastStart *v) override { if (!v) return E_POINTER; memset(v, 0, sizeof(*v)); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Launchable(boolean *v) override { if (!v) return E_POINTER; *v = in.state == ST_COMPLETED; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_LaunchRegionsStreaming(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_EnhancedStreaming(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Error(HRESULT *v) override { if (!v) return E_POINTER; *v = in.error; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DataRate(UINT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_StorageSpaceRequired(UINT64 *v) override { if (!v) return E_POINTER; *v = in.total; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_PersistentLocalStorageSize(UINT64 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_PersistentLocalStorageCrdId(UINT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BuildId(GUID *v) override { if (!v) return E_POINTER; *v = GUID{}; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_PlanId(GUID *v) override { if (!v) return E_POINTER; *v = GUID{}; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Progress(InstallProgress *v) override { if (!v) return E_POINTER; *v = progress_of(in); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AvailableDate(INT64 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_RemediationAction(INT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ExtendedError(HRESULT *v) override { if (!v) return E_POINTER; *v = in.error; return S_OK; }
};

struct IInstallationSummaryAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_InstallationId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BundleStoreId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Error(HRESULT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Progress(InstallProgress *) = 0;
};
struct InstallationSummary : IInstallationSummaryAbi
{
    LONG ref = 1;
    Install in;
    InstallationSummary(const Install &i) : in(i) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageInstallationSummary)) { *out = static_cast<IInstallationSummaryAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageInstallationSummary")
    HRESULT STDMETHODCALLTYPE get_InstallationId(GUID *v) override { if (!v) return E_POINTER; *v = in.id; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BundleStoreId(HSTRING *v) override { return hstr(L"", v); }
    HRESULT STDMETHODCALLTYPE get_State(INT32 *v) override { if (!v) return E_POINTER; *v = in.state; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Error(HRESULT *v) override { if (!v) return E_POINTER; *v = in.error; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Progress(InstallProgress *v) override { if (!v) return E_POINTER; *v = progress_of(in); return S_OK; }
};

/* event args */
struct IInstallationChangedArgsAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE get_InstallationInfo(IInspectable **) = 0; };
struct InstallationChangedArgs : IInstallationChangedArgsAbi
{
    LONG ref = 1;
    Install in;
    InstallationChangedArgs(const Install &i) : in(i) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IInstallationChangedEventArgs)) { *out = static_cast<IInstallationChangedArgsAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.InstallationChangedEventArgs")
    HRESULT STDMETHODCALLTYPE get_InstallationInfo(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        *v = static_cast<IInstallationInfoAbi *>(new InstallationInfo(in));
        return S_OK;
    }
};
struct IQueueItemArgsAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE get_PackageInstance(HSTRING *) = 0; };
struct QueueItemArgs : IQueueItemArgsAbi
{
    LONG ref = 1;
    std::wstring instance;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageQueueItemEventArgs)) { *out = static_cast<IQueueItemArgsAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageQueueItemEventArgs")
    HRESULT STDMETHODCALLTYPE get_PackageInstance(HSTRING *v) override { return hstr(instance, v); }
};

static void fire(int ev, IInspectable *sender, IUnknown *args)
{
    std::vector<IUnknown *> handlers;
    AcquireSRWLockShared(&events_lock);
    for (auto &h : event_handlers[ev]) { h.second->AddRef(); handlers.push_back(h.second); }
    ReleaseSRWLockShared(&events_lock);
    for (IUnknown *h : handlers)
    {
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IInspectable *, IUnknown *);
        HRESULT hr = ((invoke_fn)(*(void ***)h)[3])(h, sender, args);
        if (FAILED(hr)) tracef("event %d handler -> %#lx", ev, (unsigned long)hr);
        h->Release();
    }
}

static IInspectable *queue_sender(void);

/* IPackage.InstallationChanged: handlers registered on a package object, by the package's source */
struct PackageHandler { LONGLONG token; std::wstring key; IUnknown *handler; IInspectable *sender; };
static std::vector<PackageHandler> package_handlers;

static void fire_package(const Install &snap)
{
    std::vector<std::pair<IUnknown *, IInspectable *>> targets;
    AcquireSRWLockShared(&events_lock);
    for (auto &h : package_handlers)
        if (!_wcsicmp(h.key.c_str(), snap.instance.c_str()) || !_wcsicmp(h.key.c_str(), snap.store_id.c_str()) ||
            !_wcsicmp(h.key.c_str(), snap.family.c_str()))
        {
            h.handler->AddRef();
            h.sender->AddRef();
            targets.push_back({ h.handler, h.sender });
        }
    ReleaseSRWLockShared(&events_lock);
    for (auto &t : targets)
    {
        InstallationChangedArgs *c = new InstallationChangedArgs(snap);
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IInspectable *, IUnknown *);
        HRESULT hr = ((invoke_fn)(*(void ***)t.first)[3])(t.first, t.second, static_cast<IInstallationChangedArgsAbi *>(c));
        if (FAILED(hr)) tracef("package InstallationChanged handler -> %#lx", (unsigned long)hr);
        c->Release();
        t.first->Release();
        t.second->Release();
    }
}

static void report(Install *in, bool added_or_removed = false, bool removed = false)
{
    Install snap;
    AcquireSRWLockShared(&installs_lock);
    snap = *in;
    ReleaseSRWLockShared(&installs_lock);
    if (added_or_removed)
    {
        QueueItemArgs *a = new QueueItemArgs;
        a->instance = snap.instance;
        fire(removed ? EV_ITEM_REMOVED : EV_ITEM_ADDED, queue_sender(), static_cast<IQueueItemArgsAbi *>(a));
        a->Release();
        fire(EV_QUEUE_CHANGED, queue_sender(), nullptr);
    }
    InstallationChangedArgs *c = new InstallationChangedArgs(snap);
    fire(EV_INSTALLATION_CHANGED, queue_sender(), static_cast<IInstallationChangedArgsAbi *>(c));
    c->Release();
    fire_package(snap);
}

/* ---- the Store's delivery service (FE3) ---- */

static std::string http_post_soap(const wchar_t *path, const std::string &body, DWORD *status_out)
{
    std::string out;
    DWORD status = 0, size = sizeof(status);
    HINTERNET s = WinHttpOpen(L"Windows-Update-Agent/10.0.10011.16384 Client-Protocol/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), c = nullptr, r = nullptr;
    if (!s) s = WinHttpOpen(L"Windows-Update-Agent/10.0.10011.16384 Client-Protocol/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s && (c = WinHttpConnect(s, L"fe3.delivery.mp.microsoft.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) &&
        (r = WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)))
    {
        WinHttpSetTimeouts(r, 20000, 20000, 60000, 60000);
        if (WinHttpSendRequest(r, L"Content-Type: application/soap+xml; charset=utf-8\r\n", (DWORD)-1, (void *)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, nullptr))
        {
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
            for (;;)
            {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                size_t old = out.size();
                out.resize(old + avail);
                if (!WinHttpReadData(r, &out[old], avail, &got)) { out.resize(old); break; }
                out.resize(old + got);
            }
        }
        else tracef("FE3 POST %ls: error %lu", path, GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (status_out) *status_out = status;
    return out;
}

static std::string xml_unescape(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != '&') { o += s[i]; continue; }
        static const struct { const char *e; char c; } ents[] = { {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'} };
        bool done = false;
        for (auto &e : ents)
            if (!s.compare(i, strlen(e.e), e.e)) { o += e.c; i += strlen(e.e) - 1; done = true; break; }
        if (!done) o += '&';
    }
    return o;
}

static std::string soap_time(int offset_minutes)
{
    SYSTEMTIME st;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = { { ft.dwLowDateTime, ft.dwHighDateTime } };
    u.QuadPart += (LONGLONG)offset_minutes * 60 * 10000000;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static std::string soap_envelope(const char *action, const char *to, const std::string &body)
{
    GUID g;
    CoCreateGuid(&g);
    char id[64];
    snprintf(id, sizeof(id), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string("<s:Envelope xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header><a:Action s:mustUnderstand=\"1\">http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService/") + action +
        "</a:Action><a:MessageID>urn:uuid:" + id + "</a:MessageID><a:To s:mustUnderstand=\"1\">" + to + "</a:To>"
        "<o:Security s:mustUnderstand=\"1\" xmlns:o=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><Created>" + soap_time(0) +
        "</Created><Expires>" + soap_time(5) + "</Expires></Timestamp>"
        "<wuws:WindowsUpdateTicketsToken wsu:id=\"ClientMSA\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" "
        "xmlns:wuws=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\"><TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"></TicketType>"
        "</wuws:WindowsUpdateTicketsToken></o:Security></s:Header><s:Body>" + body + "</s:Body></s:Envelope>";
}

static const char FE3_NS[] = "xmlns=\"http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService\"";
static const char FE3_ATTRS[] = "BranchReadinessLevel=CB;CurrentBranch=ge_release;OEMModel=Virtual Machine;FlightRing=Retail;AttrDataVer=21;"
    "SystemManufacturer=Microsoft Corporation;InstallLanguage=en-US;OSUILocale=en-US;InstallationType=Client;FlightingBranchName=external;"
    "OSSkuId=48;FlightContent=Branch;App=WU;AppVer=10.0.26100.1;OSArchitecture=AMD64;IsFlightingEnabled=0;IsDeviceRetailDemo=0;"
    "TelemetryLevel=3;OSVersion=10.0.26100.1;DeviceFamily=Windows.Desktop;";

struct Fe3Update { std::string moniker, update_id, revision; };

/* the packages of a Store category: SyncUpdates until the leaf updates (the packages) arrive. The Windows Update
 * protocol answers with the infrastructure ("non-leaf") updates first; reporting those as installed in the next
 * request is what makes it go on to the packages. */
static HRESULT fe3_packages(const std::string &category, std::vector<Fe3Update> &out)
{
    static const wchar_t PATH[] = L"/ClientWebService/client.asmx";
    static const char TO[] = "https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx";
    DWORD status = 0;
    std::string r = http_post_soap(PATH, soap_envelope("GetCookie", TO, std::string("<GetCookie ") + FE3_NS +
        "><oldCookie></oldCookie><lastChange>2015-10-21T17:01:07.1472913Z</lastChange><currentTime>" + soap_time(0) +
        "</currentTime><protocolVersion>1.40</protocolVersion></GetCookie>"), &status);
    size_t a = r.find("<EncryptedData>"), b = r.find("</EncryptedData>");
    if (status != 200 || a == std::string::npos || b == std::string::npos)
    {
        tracef("FE3 GetCookie: HTTP %lu", status);
        return HRESULT_FROM_WIN32(ERROR_WINHTTP_CANNOT_CONNECT);
    }
    std::string cookie = r.substr(a + 15, b - a - 15);
    std::vector<std::string> installed, other;
    for (int pass = 0; pass < 8; pass++)
    {
        std::string ins, oth;
        for (auto &i : installed) ins += "<int>" + i + "</int>";
        for (auto &i : other) oth += "<int>" + i + "</int>";
        std::string body = std::string("<SyncUpdates ") + FE3_NS + "><cookie><Expiration>2045-03-11T02:02:48Z</Expiration><EncryptedData>" + cookie +
            "</EncryptedData></cookie><parameters><ExpressQuery>false</ExpressQuery><InstalledNonLeafUpdateIDs>" + ins +
            "</InstalledNonLeafUpdateIDs><OtherCachedUpdateIDs>" + oth + "</OtherCachedUpdateIDs><SkipSoftwareSync>false</SkipSoftwareSync>"
            "<NeedTwoGroupOutOfScopeUpdates>true</NeedTwoGroupOutOfScopeUpdates><FilterAppCategoryIds><CategoryIdentifier><Id>" + category +
            "</Id></CategoryIdentifier></FilterAppCategoryIds><TreatAppCategoryIdsAsInstalled>true</TreatAppCategoryIdsAsInstalled>"
            "<AlsoPerformRegularSync>false</AlsoPerformRegularSync><ComputerSpec/><ExtendedUpdateInfoParameters><XmlUpdateFragmentTypes>"
            "<XmlUpdateFragmentType>Extended</XmlUpdateFragmentType></XmlUpdateFragmentTypes><Locales><string>en-US</string><string>en</string>"
            "</Locales></ExtendedUpdateInfoParameters><ClientPreferredLanguages><string>en-US</string></ClientPreferredLanguages>"
            "<ProductsParameters><SyncCurrentVersionOnly>false</SyncCurrentVersionOnly><DeviceAttributes>" + FE3_ATTRS +
            "</DeviceAttributes><CallerAttributes>Interactive=1;IsSeeker=0;</CallerAttributes><Products/></ProductsParameters></parameters></SyncUpdates>";
        r = xml_unescape(http_post_soap(PATH, soap_envelope("SyncUpdates", TO, body), &status));
        if (status != 200) { tracef("FE3 SyncUpdates: HTTP %lu", status); return E_FAIL; }
        int leaves = 0, nonleaves = 0;
        for (size_t p = 0; (p = r.find("<UpdateInfo><ID>", p)) != std::string::npos;)
        {
            size_t end = r.find("</UpdateInfo>", p);
            if (end == std::string::npos) break;
            std::string u = r.substr(p, end - p);
            p = end;
            std::string id = u.substr(16, u.find("</ID>") - 16);
            bool leaf = u.find("<IsLeaf>true</IsLeaf>") != std::string::npos;
            if (!leaf) { nonleaves++; installed.push_back(id); continue; }
            leaves++;
            other.push_back(id);
            size_t m = u.find("PackageMoniker=\""), ui = u.find("<UpdateIdentity UpdateID=\"");
            if (m == std::string::npos || ui == std::string::npos || u.find("SecuredFragment") == std::string::npos) continue;
            Fe3Update f;
            f.moniker = u.substr(m + 16, u.find('"', m + 16) - m - 16);
            f.update_id = u.substr(ui + 26, u.find('"', ui + 26) - ui - 26);
            size_t rv = u.find("RevisionNumber=\"", ui);
            f.revision = rv == std::string::npos ? "1" : u.substr(rv + 16, u.find('"', rv + 16) - rv - 16);
            out.push_back(f);
        }
        tracef("FE3 SyncUpdates pass %d: %d infrastructure updates, %d packages", pass, nonleaves, leaves);
        if (leaves || !nonleaves) break;
    }
    return out.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : S_OK;
}

/* the first bytes of a download (a ranged GET): a package is a zip ("PK"), its block map a cabinet ("MSCF") */
static std::string http_head_bytes(const std::string &url)
{
    std::wstring w(url.begin(), url.end()), host(256, 0), path(4096, 0);
    std::string out;
    URL_COMPONENTS uc = {sizeof(uc)};
    uc.lpszHostName = &host[0]; uc.dwHostNameLength = (DWORD)host.size();
    uc.lpszUrlPath = &path[0]; uc.dwUrlPathLength = (DWORD)path.size();
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return out;
    host.resize(uc.dwHostNameLength);
    path.resize(uc.dwUrlPathLength);
    HINTERNET s = WinHttpOpen(L"Microsoft-Delivery-Optimization/10.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0), c = nullptr, r = nullptr;
    if (s && (c = WinHttpConnect(s, host.c_str(), uc.nPort, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
    {
        WinHttpSetTimeouts(r, 20000, 20000, 20000, 20000);
        char buf[4];
        DWORD got = 0;
        if (WinHttpSendRequest(r, L"Range: bytes=0-3\r\n", (DWORD)-1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
            WinHttpReadData(r, buf, sizeof(buf), &got))
            out.assign(buf, got);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return out;
}

static std::string fe3_file_url(const Fe3Update &u)
{
    static const char TO[] = "https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx/secured";
    DWORD status = 0;
    std::string body = std::string("<GetExtendedUpdateInfo2 ") + FE3_NS + "><updateIDs><UpdateIdentity><UpdateID>" + u.update_id +
        "</UpdateID><RevisionNumber>" + u.revision + "</RevisionNumber></UpdateIdentity></updateIDs><infoTypes>"
        "<XmlUpdateFragmentType>FileUrl</XmlUpdateFragmentType><XmlUpdateFragmentType>FileDecryption</XmlUpdateFragmentType></infoTypes>"
        "<deviceAttributes>" + FE3_ATTRS + "</deviceAttributes></GetExtendedUpdateInfo2>";
    std::string r = xml_unescape(http_post_soap(L"/ClientWebService/client.asmx/secured", soap_envelope("GetExtendedUpdateInfo2", TO, body), &status));
    /* the package's own file: the one that is not a block map */
    for (size_t p = 0; (p = r.find("<Url>", p)) != std::string::npos; p += 5)
    {
        std::string url = r.substr(p + 5, r.find("</Url>", p) - p - 5);
        if (url.find("filestreamingservice") == std::string::npos) continue;
        /* both URLs look alike; only the first bytes tell the block map (a cabinet) from the package */
        std::string magic = http_head_bytes(url);
        tracef("FE3 file starts %02x %02x", magic.size() > 0 ? (BYTE)magic[0] : 0, magic.size() > 1 ? (BYTE)magic[1] : 0);
        if (!magic.compare(0, 4, "MSCF")) continue;
        return url;
    }
    tracef("FE3 GetExtendedUpdateInfo2: HTTP %lu, no file URL", status);
    return "";
}

/* "Name_Version_Arch_ResourceId_PublisherId": the package for this family with the newest version, a bundle or x64 */
static bool pick_package(const std::vector<Fe3Update> &all, const std::wstring &family, Fe3Update &best)
{
    std::string fam = utf8(family);
    size_t us = fam.rfind('_');
    if (us == std::string::npos) return false;
    std::string name = fam.substr(0, us), pub = fam.substr(us + 1);
    UINT64 best_version = 0;
    bool found = false;
    for (auto &u : all)
    {
        if (u.moniker.compare(0, name.size() + 1, name + "_")) continue;
        if (u.moniker.size() < pub.size() || u.moniker.compare(u.moniker.size() - pub.size(), pub.size(), pub)) continue;
        std::string rest = u.moniker.substr(name.size() + 1);
        std::string ver = rest.substr(0, rest.find('_'));
        std::string arch = rest.substr(ver.size() + 1, rest.find('_', ver.size() + 1) - ver.size() - 1);
        if (arch != "neutral" && arch != "x64") continue;
        unsigned v[4] = {};
        sscanf(ver.c_str(), "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
        UINT64 version = ((UINT64)v[0] << 48) | ((UINT64)v[1] << 32) | ((UINT64)v[2] << 16) | v[3];
        if (!found || version > best_version) { best = u; best_version = version; found = true; }
    }
    return found;
}

/* ---- the install worker ---- */

static HRESULT run_installer(Install *in, const std::string &url, const std::wstring &stage_name, std::wstring &log_out)
{
    std::wstring data = games_data_unix(), script = env_w(L"WINE_UWP_INSTALLER"), tree = env_w(L"WINE_UWP_WINDOWS_TREE");
    if (data.empty() || script.empty() || tree.empty())
    {
        tracef("install: WINE_UWP_GAMES_DATA, WINE_UWP_INSTALLER or WINE_UWP_WINDOWS_TREE not set");
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    CreateDirectoryW((unix_to_windows(data)).c_str(), nullptr);
    std::wstring log_unix = data + L"/install-" + stage_name + L".log";
    log_out = unix_to_windows(log_unix);
    DeleteFileW(log_out.c_str());
    /* the host's Python runs wine-uwp's installer; Wine starts a Unix program given by its path */
    /* the exit status goes to the log too: a Unix child's process handle is not something to rely on */
    std::wstring cmd = L"\"Z:\\bin\\sh\" -c \"python3 '" + script + L"' '" + wide(url) + L"' '" + data + L"' --windows '" + tree +
                       L"' --stage '" + stage_name + L"' > '" + log_unix + L"' 2>&1; echo EXIT $? >> '" + log_unix + L"'\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        tracef("install: cannot start the installer (%lu)", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(pi.hThread);
    tracef("install: installer started for %ls", stage_name.c_str());
    std::string seen;
    bool installed = false, exited = false;
    int exit_code = -1, grace = 0;
    for (;;)
    {
        DWORD wait = WaitForSingleObject(pi.hProcess, 500);
        HANDLE f = CreateFileW(log_out.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            std::string all;
            char buf[8192];
            DWORD got;
            while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) all.append(buf, got);
            CloseHandle(f);
            if (all.size() > seen.size())
            {
                std::string fresh = all.substr(seen.size());
                seen = all;
                for (size_t p = 0; p < fresh.size();)
                {
                    size_t e = fresh.find('\n', p);
                    if (e == std::string::npos) e = fresh.size();
                    std::string line = fresh.substr(p, e - p);
                    p = e + 1;
                    unsigned pct, total;
                    if (sscanf(line.c_str(), "PROGRESS %u %u", &pct, &total) == 2 && total)
                    {
                        AcquireSRWLockExclusive(&installs_lock);
                        in->done = in->total * pct / total;
                        in->state = ST_STREAMING;
                        ReleaseSRWLockExclusive(&installs_lock);
                        report(in);
                    }
                    else if (!line.compare(0, 10, "INSTALLED ")) installed = true;
                    else if (!line.compare(0, 5, "EXIT ")) { exit_code = atoi(line.c_str() + 5); exited = true; }
                    else if (!line.compare(0, 6, "STAGE ")) tracef("install: %s", line.c_str() + 6);
                }
            }
        }
        if (exited) break;
        if (wait == WAIT_OBJECT_0)
        {
            /* the shell is gone; its last words may still be on their way to the log */
            if (++grace > 10) break;
            Sleep(500);
        }
        if (in->cancel) { TerminateProcess(pi.hProcess, 1); break; }
    }
    CloseHandle(pi.hProcess);
    tracef("install: installer exited %d%s", exit_code, installed ? ", installed" : "");
    if (!installed)
    {
        /* the installer's own last line says why */
        size_t x = seen.rfind("EXIT");
        size_t e = x == std::string::npos || !x ? std::string::npos : seen.find_last_not_of("\r\n", x - 1);
        size_t b = seen.rfind('\n', e == std::string::npos ? 0 : e);
        tracef("install: %s", seen.substr(b == std::string::npos ? 0 : b + 1, e == std::string::npos ? 0 : e - b).c_str());
    }
    return installed && !exit_code ? S_OK : E_FAIL;
}

/* the stage directory's identity.env: the package full name and AUMID install-app.py found */
static void read_identity(const std::wstring &dir, std::wstring &full, std::wstring &aumid, std::wstring &title)
{
    HANDLE f = CreateFileW((dir + L"\\identity.env").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[4096];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n"))
    {
        if (!strncmp(line, "export ", 7)) line += 7;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string v = eq + 1;
        if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"')) v = v.substr(1, v.size() - 2);
        if (!strcmp(line, "WINE_UWP_PACKAGE_FULL_NAME")) full = wide(v);
        else if (!strcmp(line, "WINE_UWP_AUMID")) aumid = wide(v);
        else if (!strcmp(line, "WINE_UWP_WINDOW_TITLE")) title = wide(v);
    }
}

static DWORD WINAPI install_thread(void *param)
{
    Install *in = (Install *)param;
    HRESULT hr;
    ProductDetails d = query_product(in->store_id);
    std::vector<Fe3Update> updates;
    Fe3Update pkg;
    std::string url;
    std::wstring stage_name, log;

    AcquireSRWLockExclusive(&installs_lock);
    in->total = d.size;
    in->state = ST_STREAMING;
    ReleaseSRWLockExclusive(&installs_lock);
    report(in);
    if (!d.uwp)
    {
        tracef("install %ls: an MSIXVC title; it needs Gaming Services' encrypted package support", in->store_id.c_str());
        hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        goto done;
    }
    if (d.wu_category.empty()) { tracef("install %ls: no Store category in the catalogue", in->store_id.c_str()); hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto done; }
    if (FAILED(hr = fe3_packages(utf8(d.wu_category), updates))) goto done;
    if (!pick_package(updates, d.family, pkg)) { tracef("install %ls: no package for %ls", in->store_id.c_str(), d.family.c_str()); hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto done; }
    tracef("install %ls: package %s", in->store_id.c_str(), pkg.moniker.c_str());
    if ((url = fe3_file_url(pkg)).empty()) { hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto done; }
    tracef("install %ls: download address found", in->store_id.c_str());
    stage_name = d.family.substr(0, d.family.find(L'_'));
    stage_name = stage_name.substr(stage_name.rfind(L'.') + 1);
    for (auto &c : stage_name) c = towlower(c);
    if (FAILED(hr = run_installer(in, url, stage_name, log))) goto done;
    {
        std::wstring dir = unix_to_windows(games_data_unix()) + L"\\stage\\" + stage_name, full, aumid, title;
        read_identity(dir, full, aumid, title);
        std::string marker = "store_id=" + utf8(in->store_id) + "\nfamily=" + utf8(d.family) + "\nfull_name=" + utf8(full) + "\naumid=" +
                             utf8(aumid) + "\ntitle=" + utf8(title) + "\nsize=" + std::to_string(d.size) + "\n";
        HANDLE f = CreateFileW((dir + L"\\.wine-uwp-xbox").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, marker.data(), (DWORD)marker.size(), &written, nullptr); CloseHandle(f); }
        AcquireSRWLockExclusive(&installs_lock);
        in->stage = dir;
        ReleaseSRWLockExclusive(&installs_lock);
    }
done:
    AcquireSRWLockExclusive(&installs_lock);
    in->state = SUCCEEDED(hr) ? ST_COMPLETED : ST_FAILED;
    in->error = hr;
    if (SUCCEEDED(hr)) in->done = in->total;
    ReleaseSRWLockExclusive(&installs_lock);
    tracef("install %ls -> %s (%#lx)", in->store_id.c_str(), SUCCEEDED(hr) ? "completed" : "failed", (unsigned long)hr);
    report(in);
    if (SUCCEEDED(hr))
    {
        /* finished items leave the queue */
        Sleep(1000);
        report(in, true, true);
        AcquireSRWLockExclusive(&installs_lock);
        for (size_t i = 0; i < installs.size(); i++) if (installs[i] == in) { installs.erase(installs.begin() + i); break; }
        ReleaseSRWLockExclusive(&installs_lock);
        /* Storage content changed: the app re-reads its library */
    }
    return 0;
}

/* ---- PackageRequest ---- */

struct IPackageRequestAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Destination(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Destination(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationName(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Alias(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Alias(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Metadata(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Metadata(IInspectable *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Options(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Options(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Type(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Type(INT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Sources(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Specifiers(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConfigureFeatures(IInspectable *) = 0;
    virtual HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **) = 0;
};
struct IPackageRequestSizeAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_IsUpgrade(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DownloadSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FreeSpaceRequired(UINT64 *) = 0;
};
struct PackageRequestSize : IPackageRequestSizeAbi
{
    LONG ref = 1;
    UINT64 size;
    PackageRequestSize(UINT64 s) : size(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageRequestSize)) { *out = static_cast<IPackageRequestSizeAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageRequestSize")
    HRESULT STDMETHODCALLTYPE get_IsUpgrade(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DownloadSize(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FreeSpaceRequired(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
};
struct PackageRequest : IPackageRequestAbi
{
    LONG ref = 1;
    std::wstring source, store_id, sku, avail, destination, destination_name, alias;
    INT32 source_type = 1, type = 0;
    UINT32 options = 0;
    IInspectable *metadata = nullptr;
    ~PackageRequest() { if (metadata) metadata->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageRequest)) { *out = static_cast<IPackageRequestAbi *>(this); AddRef(); return S_OK; }
        log_guid("request QueryInterface refused", kind_of(L"Microsoft.GamePlatform.Services.PackageRequest"), iid);
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageRequest")
    HRESULT STDMETHODCALLTYPE get_Destination(HSTRING *v) override { return hstr(destination, v); }
    HRESULT STDMETHODCALLTYPE put_Destination(HSTRING v) override { destination = WindowsGetStringRawBuffer(v, nullptr); tracef("PackageRequest.Destination = %ls", destination.c_str()); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DestinationName(HSTRING *v) override { return hstr(destination_name, v); }
    HRESULT STDMETHODCALLTYPE put_DestinationName(HSTRING v) override { destination_name = WindowsGetStringRawBuffer(v, nullptr); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Alias(HSTRING *v) override { return hstr(alias, v); }
    HRESULT STDMETHODCALLTYPE put_Alias(HSTRING v) override { alias = WindowsGetStringRawBuffer(v, nullptr); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Metadata(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if (!metadata)
        {
            HSTRING cls;
            WindowsCreateString(L"Windows.Foundation.Collections.PropertySet", 42, &cls);
            RoActivateInstance(cls, &metadata);
            WindowsDeleteString(cls);
        }
        if ((*v = metadata)) metadata->AddRef();
        return metadata ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE put_Metadata(IInspectable *v) override { if (v) v->AddRef(); if (metadata) metadata->Release(); metadata = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Options(UINT32 *v) override { if (!v) return E_POINTER; *v = options; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Options(UINT32 v) override { options = v; tracef("PackageRequest.Options = %#x", v); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Type(INT32 *v) override { if (!v) return E_POINTER; *v = type; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Type(INT32 v) override { type = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Sources(IInspectable **v) override { tracef("PackageRequest.Sources: not implemented"); if (v) *v = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Specifiers(IInspectable **v) override { tracef("PackageRequest.Specifiers: not implemented"); if (v) *v = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConfigureFeatures(IInspectable *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        *v = static_cast<IPackageRequestSizeAbi *>(new PackageRequestSize(query_product(store_id).size));
        return S_OK;
    }
};

static HRESULT WINAPI rq_CreateForSource(Factory *, HSTRING source, INT32 type, IInspectable **out)
{
    if (!out) return E_POINTER;
    PackageRequest *r = new PackageRequest;
    r->source = WindowsGetStringRawBuffer(source, nullptr);
    r->source_type = type;
    if (type == 1 /* StoreId */) r->store_id = r->source;
    else
    {
        InstalledGame g;
        if (find_installed(r->source, g)) r->store_id = g.store_id;
    }
    tracef("PackageRequest.CreateForSource(%ls, type %d)", r->source.c_str(), type);
    *out = static_cast<IPackageRequestAbi *>(r);
    return S_OK;
}
static HRESULT WINAPI rq_CreateForSourceDevice(Factory *f, HSTRING source, INT32 type, GUID target, IInspectable **out)
{
    return rq_CreateForSource(f, source, type, out);
}
static HRESULT WINAPI rq_CreateForStoreId(Factory *, HSTRING store, HSTRING sku, HSTRING avail, IInspectable **out)
{
    if (!out) return E_POINTER;
    PackageRequest *r = new PackageRequest;
    r->source = r->store_id = WindowsGetStringRawBuffer(store, nullptr);
    r->sku = WindowsGetStringRawBuffer(sku, nullptr);
    r->avail = WindowsGetStringRawBuffer(avail, nullptr);
    tracef("PackageRequest.CreateForStoreId(%ls, %ls, %ls)", r->store_id.c_str(), r->sku.c_str(), r->avail.c_str());
    *out = static_cast<IPackageRequestAbi *>(r);
    return S_OK;
}

/* ---- PackageQueue ---- */

struct IAsyncOpGuidAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(GUID *) = 0;
};
/* already complete when handed out: SubmitRequestAsync answers as soon as the item is queued */
struct AsyncGuid : IAsyncOpGuidAbi, IAsyncInfoAbi
{
    LONG ref = 1;
    GUID value;
    HRESULT error;
    IUnknown *handler = nullptr;
    AsyncGuid(const GUID &v, HRESULT e) : value(v), error(e) {}
    ~AsyncGuid() { if (handler) handler->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IAsyncOperation_Guid)) *out = static_cast<IAsyncOpGuidAbi *>(this);
        else if (IsEqualGUID(iid, IID_IAsyncInfo_)) *out = static_cast<IAsyncInfoAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.IAsyncOperation`1<Guid>")
    HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *h) override
    {
        if (handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
        if (!h) return S_OK;
        (handler = h)->AddRef();
        typedef HRESULT (STDMETHODCALLTYPE *invoke_fn)(IUnknown *, IAsyncOpGuidAbi *, INT32);
        ((invoke_fn)(*(void ***)h)[3])(h, static_cast<IAsyncOpGuidAbi *>(this), FAILED(error) ? 3 : 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **h) override { if (!h) return E_POINTER; if ((*h = handler)) handler->AddRef(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetResults(GUID *v) override { if (!v) return E_POINTER; *v = value; return error; }
    HRESULT STDMETHODCALLTYPE get_Id(UINT32 *v) override { if (!v) return E_POINTER; *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Status(INT32 *v) override { if (!v) return E_POINTER; *v = FAILED(error) ? 3 : 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *v) override { if (!v) return E_POINTER; *v = error; return S_OK; }
    HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};

static HRESULT WINAPI pq_SubmitRequestAsync(Factory *, IInspectable *request, INT32 priority, HSTRING cv, IInspectable **out)
{
    if (!out || !request) return E_POINTER;
    IPackageRequestAbi *req = nullptr;
    if (FAILED(request->QueryInterface(IID_IPackageRequest, (void **)&req))) return E_INVALIDARG;
    PackageRequest *r = static_cast<PackageRequest *>(req);
    Install *in = new Install;
    CoCreateGuid(&in->id);
    in->store_id = r->store_id;
    req->Release();
    ProductDetails d = query_product(in->store_id);
    in->family = d.family;
    in->instance = instance_for(d.family.empty() ? in->store_id : d.family);
    in->total = d.size;
    tracef("PackageQueue.SubmitRequestAsync(%ls, priority %d) -> installation %08lx", in->store_id.c_str(), priority, (unsigned long)in->id.Data1);
    AcquireSRWLockExclusive(&installs_lock);
    installs.push_back(in);
    ReleaseSRWLockExclusive(&installs_lock);
    *out = static_cast<IAsyncOpGuidAbi *>(new AsyncGuid(in->id, S_OK));
    report(in, true, false);
    HANDLE t = CreateThread(nullptr, 0, install_thread, in, 0, nullptr);
    if (t) CloseHandle(t);
    return S_OK;
}

static Install *find_install(const std::wstring *instance, const GUID *id)
{
    for (Install *in : installs)
        if ((instance && !_wcsicmp(in->instance.c_str(), instance->c_str())) || (id && IsEqualGUID(in->id, *id))) return in;
    return nullptr;
}

static HRESULT WINAPI pq_Enumerate(Factory *, IInspectable **out)
{
    if (!out) return E_POINTER;
    HStringView *v = new HStringView;
    AcquireSRWLockShared(&installs_lock);
    for (Install *in : installs) v->items.push_back(in->instance);
    ReleaseSRWLockShared(&installs_lock);
    tracef("PackageQueue.Enumerate -> %zu", v->items.size());
    *out = static_cast<IHStringViewAbi *>(v);
    return S_OK;
}
static HRESULT WINAPI pq_EnumerateId(Factory *, GUID id, IInspectable **out)
{
    if (!out) return E_POINTER;
    HStringView *v = new HStringView;
    AcquireSRWLockShared(&installs_lock);
    if (Install *in = find_install(nullptr, &id)) v->items.push_back(in->instance);
    ReleaseSRWLockShared(&installs_lock);
    *out = static_cast<IHStringViewAbi *>(v);
    return S_OK;
}
static HRESULT WINAPI pq_QueryInstallationInfo(Factory *, HSTRING instance, IInspectable **out)
{
    if (!out) return E_POINTER;
    std::wstring key = WindowsGetStringRawBuffer(instance, nullptr);
    AcquireSRWLockShared(&installs_lock);
    Install *in = find_install(&key, nullptr);
    Install snap;
    if (in) snap = *in;
    ReleaseSRWLockShared(&installs_lock);
    if (!in)
    {
        /* an installed game: its installation is complete */
        InstalledGame g;
        if (!find_installed(key, g)) { *out = nullptr; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
        snap.instance = g.instance; snap.store_id = g.store_id; snap.family = g.family; snap.stage = g.stage_dir;
        snap.state = ST_COMPLETED; snap.total = snap.done = g.size;
        memset(&snap.id, 0, sizeof(snap.id));
    }
    *out = static_cast<IInstallationInfoAbi *>(new InstallationInfo(snap));
    return S_OK;
}
static HRESULT WINAPI pq_QueryInstallationSummary(Factory *, GUID id, IInspectable **out)
{
    if (!out) return E_POINTER;
    AcquireSRWLockShared(&installs_lock);
    Install *in = find_install(nullptr, &id);
    Install snap;
    if (in) snap = *in;
    ReleaseSRWLockShared(&installs_lock);
    if (!in) { *out = nullptr; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    *out = static_cast<IInstallationSummaryAbi *>(new InstallationSummary(snap));
    return S_OK;
}
static HRESULT cancel_install(Install *in)
{
    if (!in) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    in->cancel = true;
    return S_OK;
}
static HRESULT WINAPI pq_CancelInstance(Factory *, HSTRING instance, boolean nondestructive, boolean *out)
{
    std::wstring key = WindowsGetStringRawBuffer(instance, nullptr);
    AcquireSRWLockShared(&installs_lock);
    HRESULT hr = cancel_install(find_install(&key, nullptr));
    ReleaseSRWLockShared(&installs_lock);
    if (out) *out = SUCCEEDED(hr);
    return S_OK;
}
static HRESULT WINAPI pq_CancelId(Factory *, GUID id, boolean nondestructive, boolean *out)
{
    AcquireSRWLockShared(&installs_lock);
    HRESULT hr = cancel_install(find_install(nullptr, &id));
    ReleaseSRWLockShared(&installs_lock);
    if (out) *out = SUCCEEDED(hr);
    return S_OK;
}
static HRESULT WINAPI pq_ok_instance(Factory *, HSTRING) { return S_OK; }
static HRESULT WINAPI pq_ok_id(Factory *, GUID) { return S_OK; }
static HRESULT WINAPI pq_ok(Factory *) { return S_OK; }
static HRESULT WINAPI pq_CancelAll(Factory *, boolean, boolean *out)
{
    AcquireSRWLockShared(&installs_lock);
    for (Install *in : installs) in->cancel = true;
    ReleaseSRWLockShared(&installs_lock);
    if (out) *out = TRUE;
    return S_OK;
}

static HRESULT add_event(int ev, IUnknown *handler, LONGLONG *token)
{
    if (!handler || !token) return E_POINTER;
    handler->AddRef();
    AcquireSRWLockExclusive(&events_lock);
    *token = ++next_event_token;
    event_handlers[ev].push_back({ *token, handler });
    ReleaseSRWLockExclusive(&events_lock);
    tracef("PackageQueue event %d: registered (token %lld)", ev, *token);
    return S_OK;
}
static HRESULT remove_event(int ev, LONGLONG token)
{
    IUnknown *gone = nullptr;
    AcquireSRWLockExclusive(&events_lock);
    for (size_t i = 0; i < event_handlers[ev].size(); i++)
        if (event_handlers[ev][i].first == token) { gone = event_handlers[ev][i].second; event_handlers[ev].erase(event_handlers[ev].begin() + i); break; }
    ReleaseSRWLockExclusive(&events_lock);
    if (gone) gone->Release();
    return S_OK;
}
static HRESULT WINAPI pq_add_InstallationChanged(Factory *, IUnknown *h, LONGLONG *t) { return add_event(EV_INSTALLATION_CHANGED, h, t); }
static HRESULT WINAPI pq_remove_InstallationChanged(Factory *, LONGLONG t) { return remove_event(EV_INSTALLATION_CHANGED, t); }
static HRESULT WINAPI pq_add_QueueChanged(Factory *, IUnknown *h, LONGLONG *t) { return add_event(EV_QUEUE_CHANGED, h, t); }
static HRESULT WINAPI pq_remove_QueueChanged(Factory *, LONGLONG t) { return remove_event(EV_QUEUE_CHANGED, t); }
static HRESULT WINAPI pq_add_ItemAdded(Factory *, IUnknown *h, LONGLONG *t) { return add_event(EV_ITEM_ADDED, h, t); }
static HRESULT WINAPI pq_remove_ItemAdded(Factory *, LONGLONG t) { return remove_event(EV_ITEM_ADDED, t); }
static HRESULT WINAPI pq_add_ItemRemoved(Factory *, IUnknown *h, LONGLONG *t) { return add_event(EV_ITEM_REMOVED, h, t); }
static HRESULT WINAPI pq_remove_ItemRemoved(Factory *, LONGLONG t) { return remove_event(EV_ITEM_REMOVED, t); }

/* PackageQueueView: one installation (or the installation of one package) */
struct IQueueViewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE Pause() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resume() = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel(boolean, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Enumerate(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryInstallationSummary(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_InstallationChanged(IUnknown *, LONGLONG *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_InstallationChanged(LONGLONG) = 0;
};
struct QueueView : IQueueViewAbi
{
    LONG ref = 1;
    std::wstring instance;
    GUID id = {};
    bool by_id = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageQueueView)) { *out = static_cast<IQueueViewAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageQueueView")
    Install *find() { return by_id ? find_install(nullptr, &id) : find_install(&instance, nullptr); }
    HRESULT STDMETHODCALLTYPE Pause() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Resume() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Cancel(boolean, boolean *out) override
    {
        AcquireSRWLockShared(&installs_lock);
        HRESULT hr = cancel_install(find());
        ReleaseSRWLockShared(&installs_lock);
        if (out) *out = SUCCEEDED(hr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Enumerate(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        HStringView *v = new HStringView;
        AcquireSRWLockShared(&installs_lock);
        if (Install *in = find()) v->items.push_back(in->instance);
        ReleaseSRWLockShared(&installs_lock);
        *out = static_cast<IHStringViewAbi *>(v);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInstallationSummary(IInspectable **out) override
    {
        if (!out) return E_POINTER;
        AcquireSRWLockShared(&installs_lock);
        Install *in = find();
        Install snap;
        if (in) snap = *in;
        ReleaseSRWLockShared(&installs_lock);
        if (!in) { *out = nullptr; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
        *out = static_cast<IInstallationSummaryAbi *>(new InstallationSummary(snap));
        return S_OK;
    }
    /* the view's changes are the queue's: one list of handlers */
    HRESULT STDMETHODCALLTYPE add_InstallationChanged(IUnknown *h, LONGLONG *t) override { return add_event(EV_INSTALLATION_CHANGED, h, t); }
    HRESULT STDMETHODCALLTYPE remove_InstallationChanged(LONGLONG t) override { return remove_event(EV_INSTALLATION_CHANGED, t); }
};
static HRESULT WINAPI pq_CreateViewInstance(Factory *, HSTRING instance, IInspectable **out)
{
    if (!out) return E_POINTER;
    QueueView *v = new QueueView;
    v->instance = WindowsGetStringRawBuffer(instance, nullptr);
    *out = static_cast<IQueueViewAbi *>(v);
    return S_OK;
}
static HRESULT WINAPI pq_CreateViewId(Factory *, GUID id, IInspectable **out)
{
    if (!out) return E_POINTER;
    QueueView *v = new QueueView;
    v->id = id;
    v->by_id = true;
    *out = static_cast<IQueueViewAbi *>(v);
    return S_OK;
}

/* ---- the installed Package ---- */

/* an installed game, or one in the queue (its identity is known before it is installed) */
static bool is_package_instance(const GUID *id)
{
    WCHAR str[64];
    StringFromGUID2(*id, str, ARRAYSIZE(str));
    InstalledGame g;
    if (find_installed(str, g)) return true;
    bool found = false;
    AcquireSRWLockShared(&installs_lock);
    for (Install *in : installs) if (!_wcsicmp(in->instance.c_str(), str)) found = true;
    ReleaseSRWLockShared(&installs_lock);
    return found;
}
static bool package_game(Factory *f, InstalledGame &g) { return find_installed(f->source, g); }
static bool queued_game(Factory *f, InstalledGame &g)
{
    std::wstring key = f->source;
    bool found = false;
    AcquireSRWLockShared(&installs_lock);
    for (Install *in : installs)
        if (!_wcsicmp(in->instance.c_str(), key.c_str()) || !_wcsicmp(in->store_id.c_str(), key.c_str()))
        {
            g.instance = in->instance; g.family = in->family; g.store_id = in->store_id; g.full_name = in->family;
            g.size = in->total;
            found = true;
            break;
        }
    ReleaseSRWLockShared(&installs_lock);
    return found;
}

static HRESULT WINAPI pi_QueryInstallPath(Factory *f, HSTRING *out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 6, out, nullptr, __builtin_return_address(0));
    tracef("Package(%ls).QueryInstallPath -> %ls", f->source, g.stage_dir.c_str());
    return hstr(g.stage_dir, out);
}
static HRESULT WINAPI pi_QueryInstanceId(Factory *f, HSTRING *out)
{
    InstalledGame g;
    if (!package_game(f, g) && !queued_game(f, g)) return generic_slot(f, 7, out, nullptr, __builtin_return_address(0));
    return hstr(g.instance, out);
}
static HRESULT WINAPI pi_QueryIdentity(Factory *f, INT32 type, HSTRING *out)
{
    InstalledGame g;
    if (!package_game(f, g) && !queued_game(f, g)) return generic_slot(f, 8, (void *)(INT_PTR)type, out, __builtin_return_address(0));
    tracef("Package(%ls).QueryIdentity(%d)", f->source, type);
    return hstr(type == 0 ? g.family : type == 1 ? g.full_name : g.store_id, out);
}
static HRESULT WINAPI pi_QuerySize(Factory *f, UINT64 *out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 10, out, nullptr, __builtin_return_address(0));
    if (!out) return E_POINTER;
    *out = g.size;
    return S_OK;
}
static HRESULT WINAPI pi_QueryStorageCategory(Factory *f, INT32 *out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 11, out, nullptr, __builtin_return_address(0));
    if (!out) return E_POINTER;
    *out = 0; /* App: a Store app, installed with the Windows apps */
    return S_OK;
}
static HRESULT WINAPI pi_IsLaunchable(Factory *f, boolean *out)
{
    InstalledGame g;
    if (!package_game(f, g))
    {
        /* still in the queue: there is a package, it just cannot be started yet */
        if (!queued_game(f, g)) return generic_slot(f, 25, out, nullptr, __builtin_return_address(0));
        if (!out) return E_POINTER;
        *out = FALSE;
        return S_OK;
    }
    if (!out) return E_POINTER;
    *out = TRUE;
    return S_OK;
}
static HRESULT WINAPI pi_IsFlatStreamed(Factory *f, boolean *out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 24, out, nullptr, __builtin_return_address(0));
    if (!out) return E_POINTER;
    *out = FALSE;
    return S_OK;
}
/* A game gets the environment a launcher would give it, not ours: everything that run.sh exported for the
 * app that starts it (its package identity, DLL overrides, framework path, test switches) is left out, and the
 * game's wineuwp-launch.exe sets its own from identity.env and launch.env. */
static std::wstring clean_environment(void)
{
    static const wchar_t *drop[] = { L"WINE_UWP_", L"UWP_", L"WINEDLLOVERRIDES=", L"WINEPATH=", L"WINE_D3D_CONFIG=",
                                     L"WINE_SKIP_NATIVE_THREAD_CALLS=" };
    std::wstring block;
    WCHAR *env = GetEnvironmentStringsW();
    for (WCHAR *p = env; p && *p; p += wcslen(p) + 1)
    {
        bool skip = false;
        for (auto d : drop) if (!_wcsnicmp(p, d, wcslen(d))) skip = true;
        if (skip) continue;
        block += p;
        block += L'\0';
    }
    if (env) FreeEnvironmentStringsW(env);
    block += L'\0';
    return block;
}

static HRESULT launch_game(Factory *f, const wchar_t *what)
{
    InstalledGame g;
    if (!package_game(f, g)) return GP_E_NO_SERVICE;
    std::wstring exe = g.stage_dir + L"\\wineuwp-launch.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    tracef("Package(%ls).%ls: starting %ls", f->source, what, exe.c_str());
    std::wstring env = clean_environment();
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, &env[0], g.stage_dir.c_str(), &si, &pi))
    {
        tracef("  CreateProcess failed %lu", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return S_OK;
}
static HRESULT WINAPI pi_Activate(Factory *f, HSTRING exe, HSTRING params, HSTRING caller, HSTRING cv, UINT32 timeout)
{
    return launch_game(f, L"Activate");
}
static HRESULT WINAPI pi_ActivateAumid(Factory *f, HSTRING exe, HSTRING params, HSTRING caller, HSTRING cv)
{
    return launch_game(f, L"ActivateAumid");
}
static HRESULT WINAPI pi_GetRunningProcessId(Factory *f, HSTRING exe, UINT32 *out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 27, exe, out, __builtin_return_address(0));
    if (!out) return E_POINTER;
    *out = 0;
    return S_OK;
}

static HRESULT WINAPI pi_add_InstallationChanged(Factory *f, IUnknown *handler, LONGLONG *token)
{
    if (!handler || !token) return E_POINTER;
    handler->AddRef();
    f_AddRef(f);
    AcquireSRWLockExclusive(&events_lock);
    *token = ++next_event_token;
    package_handlers.push_back({ *token, f->source, handler, (IInspectable *)f });
    ReleaseSRWLockExclusive(&events_lock);
    tracef("Package(%ls).add_InstallationChanged (token %lld)", f->source, *token);
    return S_OK;
}
static HRESULT WINAPI pi_remove_InstallationChanged(Factory *f, LONGLONG token)
{
    PackageHandler gone = {};
    AcquireSRWLockExclusive(&events_lock);
    for (size_t i = 0; i < package_handlers.size(); i++)
        if (package_handlers[i].token == token) { gone = package_handlers[i]; package_handlers.erase(package_handlers.begin() + i); break; }
    ReleaseSRWLockExclusive(&events_lock);
    if (gone.handler) { gone.handler->Release(); gone.sender->Release(); }
    return S_OK;
}

/* PackageDetails and PackageHeader of an installed game */
static const GUID IID_IPackageDetails = {0x97751bca,0x2add,0x5e51,{0xb3,0x7d,0xf9,0x5c,0xc7,0x47,0xcd,0xb1}};
static const GUID IID_IPackageHeader = {0xc6e63704,0x7787,0x59af,{0xad,0x43,0xa3,0x74,0x78,0x69,0xc6,0x53}};
struct IPackageDetailsAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_InstanceId(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PackageFullName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DefaultDisplayName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Square44x44LogoPath(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Square150x150LogoPath(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SplashScreenPath(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TitleId(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Registered(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Registerable(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NotFullyStreamed(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsUwp(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UsesFeatures(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsMoving(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InstallDate(INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UpdateDate(INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LaunchDate(INT64 *) = 0;
};
static INT64 file_time_of(const std::wstring &path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((INT64)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}
struct PackageDetails : IPackageDetailsAbi
{
    LONG ref = 1;
    InstalledGame g;
    PackageDetails(const InstalledGame &x) : g(x) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageDetails)) { *out = static_cast<IPackageDetailsAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageDetails")
    HRESULT STDMETHODCALLTYPE get_InstanceId(HSTRING *v) override { return hstr(g.instance, v); }
    HRESULT STDMETHODCALLTYPE get_PackageFullName(HSTRING *v) override { return hstr(g.full_name, v); }
    HRESULT STDMETHODCALLTYPE get_DefaultDisplayName(HSTRING *v) override { return hstr(g.title, v); }
    HRESULT STDMETHODCALLTYPE get_Square44x44LogoPath(HSTRING *v) override { return hstr(g.stage_dir + L"\\.wine-uwp-icon.png", v); }
    HRESULT STDMETHODCALLTYPE get_Square150x150LogoPath(HSTRING *v) override { return hstr(g.stage_dir + L"\\.wine-uwp-icon.png", v); }
    HRESULT STDMETHODCALLTYPE get_SplashScreenPath(HSTRING *v) override { return hstr(L"", v); }
    HRESULT STDMETHODCALLTYPE get_TitleId(UINT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Registered(boolean *v) override { if (!v) return E_POINTER; *v = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Registerable(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; /* already registered: TRUE shows the "found on your PC, add it" Install button */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_NotFullyStreamed(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsUwp(boolean *v) override { if (!v) return E_POINTER; *v = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_UsesFeatures(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsMoving(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_InstallDate(INT64 *v) override { if (!v) return E_POINTER; *v = file_time_of(g.stage_dir + L"\\.wine-uwp-xbox"); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_UpdateDate(INT64 *v) override { if (!v) return E_POINTER; *v = file_time_of(g.stage_dir + L"\\.wine-uwp-xbox"); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_LaunchDate(INT64 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
};
struct PackageVersionStruct { UINT64 value; UINT16 major, minor, build, revision; };
struct IPackageHeaderAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_ContentId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ProductId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BuildId(GUID *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BlobSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Version(PackageVersionStruct *) = 0;
};
struct PackageHeader : IPackageHeaderAbi
{
    LONG ref = 1;
    InstalledGame g;
    GUID content_id;
    PackageHeader(const InstalledGame &x) : g(x) { content_id = query_product(g.store_id).content_id; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageHeader)) { *out = static_cast<IPackageHeaderAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageHeader")
    HRESULT STDMETHODCALLTYPE get_ContentId(GUID *v) override { if (!v) return E_POINTER; *v = content_id; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ProductId(GUID *v) override { if (!v) return E_POINTER; *v = GUID{}; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BuildId(GUID *v) override { if (!v) return E_POINTER; *v = GUID{}; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BlobSize(UINT64 *v) override { if (!v) return E_POINTER; *v = g.size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Version(PackageVersionStruct *v) override
    {
        if (!v) return E_POINTER;
        unsigned a = 0, b = 0, c = 0, d = 0;
        size_t us = g.full_name.find(L'_');
        if (us != std::wstring::npos) swscanf(g.full_name.c_str() + us + 1, L"%u.%u.%u.%u", &a, &b, &c, &d);
        v->major = (UINT16)a; v->minor = (UINT16)b; v->build = (UINT16)c; v->revision = (UINT16)d;
        v->value = ((UINT64)a << 48) | ((UINT64)b << 32) | ((UINT64)c << 16) | d;
        return S_OK;
    }
};
static HRESULT WINAPI pi_QueryDetails(Factory *f, IInspectable **out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 13, out, nullptr, __builtin_return_address(0));
    if (!out) return E_POINTER;
    *out = static_cast<IPackageDetailsAbi *>(new PackageDetails(g));
    return S_OK;
}
static HRESULT WINAPI pi_QueryHeader(Factory *f, IInspectable **out)
{
    InstalledGame g;
    if (!package_game(f, g)) return generic_slot(f, 12, out, nullptr, __builtin_return_address(0));
    if (!out) return E_POINTER;
    *out = static_cast<IPackageHeaderAbi *>(new PackageHeader(g));
    return S_OK;
}

static IInspectable *queue_sender(void)
{
    int k = kind_of(L"Microsoft.GamePlatform.Services.PackageQueue");
    return (IInspectable *)&factories[k];
}

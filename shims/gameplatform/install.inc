/* ------------------------------------------------------------------------------------------- */
/* Installing Store packages: PackageRequest, PackageQueue and the installed Package.              */
/*                                                                                               */
/* Gaming Services installs a game the user owns from the Store's delivery service; so does this, for the     */
/* titles that are ordinary MSIX/APPX packages (not the encrypted MSIXVC that needs Gaming Services' drivers):  */
/*   1. the product's Windows Update category, from the public display catalogue;                              */
/*   2. the package's download address from the Store's delivery service (FE3: SyncUpdates for the category,   */
/*      then GetExtendedUpdateInfo2 for the main package; anonymous, as for any free or owned app);           */
/*   3. wine-uwp's own installer (Wine's appxinstall.dll, called on the install's thread) streams the package */
/*      from there into a stage directory, with its frameworks from the Windows files, and reports progress;  */
/*   4. the queue reports it as Gaming Services would (ItemAdded, InstallationChanged with progress,           */
/*      ItemRemoved), Storage.EnumerateContent lists it, and Package answers for it: install path, identity,  */
/*      launchable, and Activate starts it through its stage directory's wineuwp-launch.exe.                  */
/*                                                                                               */
/* Where things live comes from the environment run.sh sets, all Unix paths: WINE_UWP_GAMES_DATA (the data    */
/* directory with stage/), WINE_UWP_WINDOWS_TREE (the harvested Windows files the frameworks come from),       */
/* WINE_UWP_ROOT (launch-defaults.env) and WINE_UWP_D3D_RUNTIME; wineuwp-launch.exe is Wine's, from system32.    */

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
    UINT32 rate = 0;                    /* bytes a second, over the last few seconds */
    bool cancel = false;
    bool features_chosen = false;       /* the app called ConfigureFeatures; otherwise the default recipe */
    std::vector<std::string> features;
    bool paused = false;
    bool running = false;               /* the installer has started */
    volatile LONG pause_flag = 0, cancel_flag = 0;  /* what the installer polls: paused, cancel */
    bool uwp = true;
};

static std::wstring stage_name_for_family(const std::wstring &family)
{
    std::wstring name = family.substr(0, family.find(L'_'));
    name = name.substr(name.rfind(L'.') + 1);
    for (auto &c : name) c = towlower(c);
    return name;
}

static SRWLOCK installs_lock = SRWLOCK_INIT;
static std::vector<Install *> installs;     /* the queue: in progress, and finished ones until they are removed */

/* event handlers registered on PackageQueue: InstallationChanged, QueueChanged, ItemAdded, ItemRemoved */
enum { EV_INSTALLATION_CHANGED, EV_QUEUE_CHANGED, EV_ITEM_ADDED, EV_ITEM_REMOVED, EV_COUNT };
static SRWLOCK events_lock = SRWLOCK_INIT;
static std::vector<std::pair<LONGLONG, IUnknown *>> event_handlers[EV_COUNT];
static LONGLONG next_event_token = 100;

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

struct InstalledGame
{
    std::wstring instance, store_id, family, full_name, stage_dir /* Windows path */, title, aumid;
    std::vector<std::string> features;
    UINT64 size = 0;
    bool uwp = true, features_known = false;
};

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
        else if (!strcmp(line, "uwp")) g.uwp = v != L"false" && v != L"0";
        else if (!strcmp(line, "features"))
        {
            g.features_known = true;
            std::string list = utf8(v);
            size_t start = 0;
            while (start < list.size())
            {
                size_t end = list.find(';', start);
                g.features.push_back(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
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
    p.data_rate = in.state == ST_STREAMING ? in.rate : 0;
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
    HRESULT STDMETHODCALLTYPE get_DataRate(UINT32 *v) override { if (!v) return E_POINTER; *v = in.state == ST_STREAMING ? in.rate : 0; return S_OK; }
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

/* ---- the install worker ---- */

/* gdkpackage.inc: a GDK package's install size and installer arguments for a choice of features */
static UINT64 gdk_request_size(const std::wstring &store_id, const std::vector<std::string> *features, UINT64 fallback);
struct PackageRequest;
static void request_features_from_specifiers(PackageRequest *r, std::vector<std::string> &features, bool &chosen);
static bool gdk_installer_choice(const std::wstring &store_id, const std::vector<std::string> *features,
                                 std::wstring &tags, std::wstring &languages);

/* An encrypted package's content keys, from the Store licence (shims/xboxauth/windevice.inc), as the installer
 * takes them: "keyid:key;..." in hex. They go to it in its environment, never into a file or a log. */
static HRESULT encrypted_package_keys(const GUID &content_id, std::string &keys)
{
    typedef HRESULT (WINAPI *content_keys_fn)(const char *, const char *, char *, UINT32);
    HMODULE auth = LoadLibraryW(L"wineuwp_xboxauth.dll");
    content_keys_fn fn = auth ? (content_keys_fn)GetProcAddress(auth, "XboxAuthContentKeys") : nullptr;
    char id[40], buf[4096];
    if (!fn) { tracef("install: no wineuwp_xboxauth.dll with XboxAuthContentKeys"); return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
    sprintf(id, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", content_id.Data1, content_id.Data2, content_id.Data3,
            content_id.Data4[0], content_id.Data4[1], content_id.Data4[2], content_id.Data4[3], content_id.Data4[4],
            content_id.Data4[5], content_id.Data4[6], content_id.Data4[7]);
    HRESULT hr = fn(id, "US", buf, sizeof(buf));
    tracef("install: content keys for %s -> %#lx", id, (unsigned long)hr);
    if (SUCCEEDED(hr)) keys = buf;
    SecureZeroMemory(buf, sizeof(buf));
    return hr;
}

/* A GDK title's package (.msixvc) address: packagespc.xboxlive.com's GetBasePackage for its content id, signed and
 * authorized like any Xbox Live call (http://update.xboxlive.com). The first CDN root plus the file's relative URL. */
static HRESULT msixvc_url(const GUID &content_id, std::string &url)
{
    typedef HRESULT (WINAPI *get_fn)(const char *, const char *, const char *, unsigned char **, UINT32 *, DWORD *);
    HMODULE xa = LoadLibraryW(L"wineuwp_xboxauth.dll");
    get_fn get = xa ? (get_fn)GetProcAddress(xa, "XboxAuthHttpGet") : nullptr;
    char req[128];
    unsigned char *body = nullptr;
    UINT32 len = 0;
    DWORD status = 0;
    if (!get) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    snprintf(req, sizeof(req), "https://packagespc.xboxlive.com/GetBasePackage/%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             content_id.Data1, content_id.Data2, content_id.Data3, content_id.Data4[0], content_id.Data4[1], content_id.Data4[2],
             content_id.Data4[3], content_id.Data4[4], content_id.Data4[5], content_id.Data4[6], content_id.Data4[7]);
    HRESULT hr = get(req, "Accept: application/json", "3", &body, &len, &status);
    if (FAILED(hr) || status != 200)
    {
        tracef("install: GetBasePackage -> %#lx, HTTP %lu", (unsigned long)hr, status);
        if (body) CoTaskMemFree(body);
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    Json j = parse_json(std::string((char *)body, len));
    CoTaskMemFree(body);
    for (auto &f : j["PackageFiles"].a)
    {
        std::string name = f["FileName"].str();
        if (name.size() < 7 || _stricmp(name.c_str() + name.size() - 7, ".msixvc")) continue;
        url = f["CdnRootPaths"].at(0).str() + f["RelativeUrl"].str();
        tracef("install: GDK package %s, %.0f bytes", name.c_str(), f["FileSize"].n);
        return url.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : S_OK;
    }
    tracef("install: GetBasePackage lists no .msixvc (found %d)", j["PackageFound"].b);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

struct InstallerProgress
{
    Install *in;
    ULONGLONG rate_t0, report_t0;
    UINT64 rate_b0;
    bool installed;
    std::string last;               /* the installer's last message: why, when it fails */
};

static void CALLBACK installer_event(void *context, enum appx_event event, UINT64 completed, UINT64 total, const WCHAR *text)
{
    InstallerProgress *p = (InstallerProgress *)context;
    Install *in = p->in;

    switch (event)
    {
    case APPX_EVENT_STAGE: tracef("install: %ls", text); break;
    case APPX_EVENT_MESSAGE: tracef("install: %ls", text); p->last = utf8(text); break;
    case APPX_EVENT_INSTALLED: p->installed = true; break;
    case APPX_EVENT_PROGRESS:
    {
        ULONGLONG now = GetTickCount64();
        if (!total) break;
        AcquireSRWLockExclusive(&installs_lock);
        UINT64 fraction_done = (UINT64)((long double)in->total * std::min(completed, total) / total);
        in->done = std::min(in->total, fraction_done);
        if (in->cancel) { ReleaseSRWLockExclusive(&installs_lock); break; }
        /* do not average a pause interval into the next speed sample */
        if (in->paused) { p->rate_t0 = 0; ReleaseSRWLockExclusive(&installs_lock); break; }
        /* Byte-precise progress avoids permille quantization on very large packages. */
        if (!p->rate_t0) { p->rate_t0 = now; p->rate_b0 = in->done; }
        else if (now - p->rate_t0 >= 2000)
        {
            UINT64 delta = in->done >= p->rate_b0 ? in->done - p->rate_b0 : 0;
            in->rate = (UINT32)std::min(delta * 1000 / (now - p->rate_t0), (UINT64)0xffffffff);
            p->rate_t0 = now; p->rate_b0 = in->done;
        }
        in->state = ST_STREAMING;
        ReleaseSRWLockExclusive(&installs_lock);
        /* Keep XAML event traffic bounded while still refreshing speed smoothly. */
        if (!p->report_t0 || now - p->report_t0 >= 500 || completed >= total)
        {
            p->report_t0 = now;
            report(in);
        }
        break;
    }
    }
}

/* a Unix path from the environment as a Windows one; empty if unset */
static std::wstring env_path(const wchar_t *name, const wchar_t *suffix = L"")
{
    std::wstring v = env_w(name);
    return v.empty() ? v : unix_to_windows(v + suffix);
}

static HRESULT run_installer(Install *in, const std::string &url, const std::wstring &stage_name,
                             const std::string &keys = std::string(), const std::wstring &tags = std::wstring(),
                             const std::wstring &languages = std::wstring(), bool gdk_choice = false)
{
    typedef HRESULT (CDECL *install_fn)(const struct appx_install_params *);
    static install_fn appx_install;
    AcquireSRWLockShared(&installs_lock);
    bool canceled_before_download = in->cancel;
    ReleaseSRWLockShared(&installs_lock);
    if (canceled_before_download) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::wstring data = games_data_unix(), tree = env_path(L"WINE_UWP_WINDOWS_TREE");
    if (data.empty() || tree.empty())
    {
        tracef("install: WINE_UWP_GAMES_DATA or WINE_UWP_WINDOWS_TREE not set");
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    if (!appx_install)
    {
        HMODULE lib = LoadLibraryW(L"appxinstall.dll");
        if (!lib || !(appx_install = (install_fn)GetProcAddress(lib, "appx_install")))
        {
            tracef("install: no appxinstall.dll (%lu)", GetLastError());
            return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }
    }
    data = unix_to_windows(data);
    CreateDirectoryW(data.c_str(), nullptr);
    std::wstring package = wide(url), defaults = env_path(L"WINE_UWP_ROOT", L"/launch-defaults.env");
    std::wstring d3d = env_path(L"WINE_UWP_D3D_RUNTIME");
    if (d3d.empty()) d3d = env_path(L"UWP_BIN", L"/ext/d3d-runtime/x86_64-windows");

    InstallerProgress progress = { in };
    struct appx_install_params params = {};
    params.package = package.c_str();
    params.data_dir = data.c_str();
    params.windows_tree = tree.c_str();
    params.stage = stage_name.c_str();
    if (gdk_choice)
    {
        params.tags = tags.c_str();
        params.languages = languages.c_str();
    }
    /* an encrypted package's content keys go to the installer in memory, never into a file or a log */
    params.content_keys = keys.empty() ? nullptr : keys.c_str();
    params.launch_defaults = defaults.empty() ? nullptr : defaults.c_str();
    params.d3d_runtime = d3d.empty() ? nullptr : d3d.c_str();
    params.pause = &in->pause_flag;
    params.cancel = &in->cancel_flag;
    params.callback = installer_event;
    params.context = &progress;
    AcquireSRWLockExclusive(&installs_lock);
    in->running = true;
    InterlockedExchange(&in->pause_flag, in->paused);
    ReleaseSRWLockExclusive(&installs_lock);
    tracef("install: installer started for %ls", stage_name.c_str());
    HRESULT hr = appx_install(&params);
    tracef("install: installer -> %#lx%s", (unsigned long)hr, progress.installed ? ", installed" : "");
    if (FAILED(hr) && !progress.last.empty()) tracef("install: %s", progress.last.c_str());
    return SUCCEEDED(hr) && progress.installed ? S_OK : FAILED(hr) ? hr : E_FAIL;
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
    std::wstring stage_name;

    AcquireSRWLockExclusive(&installs_lock);
    /* SubmitRequestAsync already set the selected GDK feature size. The catalog's d.size is the
     * full package and must not replace it when the worker starts. */
    if (d.uwp) in->total = d.size;
    /* Pause can be requested as soon as SubmitRequestAsync publishes the queued item, before
     * this worker gets scheduled. Do not overwrite that state while resolving the delivery URL. */
    bool canceled_before_start = in->cancel;
    if (!canceled_before_start) in->state = in->paused ? ST_PAUSED : ST_STREAMING;
    ReleaseSRWLockExclusive(&installs_lock);
    if (canceled_before_start) { hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); goto done; }
    report(in);
    if (!d.uwp)
    {
        /* a GDK title: its .msixvc from the Xbox CDN, decrypted with the licence's content key */
        std::string keys;
        stage_name = stage_name_for_family(d.family);
        if (FAILED(hr = msixvc_url(d.content_id, url))) goto done;
        if (FAILED(hr = encrypted_package_keys(d.content_id, keys))) goto done;
        std::wstring tags, languages;
        bool choice = gdk_installer_choice(in->store_id, in->features_chosen ? &in->features : nullptr, tags, languages);
        hr = run_installer(in, url, stage_name, keys, tags, languages, choice);
        SecureZeroMemory(&keys[0], keys.size());
        if (FAILED(hr)) goto done;
        goto installed;
    }
    if (d.wu_category.empty()) { tracef("install %ls: no Store category in the catalogue", in->store_id.c_str()); hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto done; }
    if (FAILED(hr = fe3_packages(utf8(d.wu_category), updates))) goto done;
    if (!pick_package(updates, d.family, pkg)) { tracef("install %ls: no package for %ls", in->store_id.c_str(), d.family.c_str()); hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto done; }
    tracef("install %ls: package %s", in->store_id.c_str(), pkg.moniker.c_str());
    if ((url = fe3_file_url(pkg)).empty()) { hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); goto done; }
    tracef("install %ls: download address found", in->store_id.c_str());
    stage_name = stage_name_for_family(d.family);
    if (d.encrypted)
    {
        std::string keys;
        if (FAILED(hr = encrypted_package_keys(d.content_id, keys))) goto done;
        hr = run_installer(in, url, stage_name, keys);
        SecureZeroMemory(&keys[0], keys.size());
        if (FAILED(hr)) goto done;
    }
    else if (FAILED(hr = run_installer(in, url, stage_name))) goto done;
installed:
    {
        std::wstring dir = unix_to_windows(games_data_unix()) + L"\\stage\\" + stage_name, full, aumid, title;
        read_identity(dir, full, aumid, title);
        std::string marker = "store_id=" + utf8(in->store_id) + "\nfamily=" + utf8(d.family) + "\nfull_name=" + utf8(full) + "\naumid=" +
                             utf8(aumid) + "\ntitle=" + utf8(title) + "\nsize=" + std::to_string(d.size) + "\nuwp=" + (d.uwp ? "true" : "false") + "\n";
        if (in->features_chosen)
        {
            marker += "features=";
            for (size_t i = 0; i < in->features.size(); i++) marker += (i ? ";" : "") + in->features[i];
            marker += "\n";
        }
        HANDLE f = CreateFileW((dir + L"\\.wine-uwp-xbox").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, marker.data(), (DWORD)marker.size(), &written, nullptr); CloseHandle(f); }
        AcquireSRWLockExclusive(&installs_lock);
        in->stage = dir;
        ReleaseSRWLockExclusive(&installs_lock);
    }
done:
    AcquireSRWLockExclusive(&installs_lock);
    bool canceled = in->cancel;
    in->state = canceled ? ST_CANCELED : (SUCCEEDED(hr) ? ST_COMPLETED : ST_FAILED);
    in->error = canceled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
    if (SUCCEEDED(hr) && !canceled) in->done = in->total;
    ReleaseSRWLockExclusive(&installs_lock);
    tracef("install %ls -> %s (%#lx)", in->store_id.c_str(), canceled ? "canceled" : (SUCCEEDED(hr) ? "completed" : "failed"), (unsigned long)in->error);
    report(in);
    if (SUCCEEDED(hr) || canceled)
    {
        /* Completed and canceled items leave the queue after their terminal state is observable. */
        if (!canceled) Sleep(1000);
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
    bool upgrade;
    PackageRequestSize(UINT64 s, bool u = false) : size(s), upgrade(u) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageRequestSize)) { *out = static_cast<IPackageRequestSizeAbi *>(this); AddRef(); return S_OK; }
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageRequestSize")
    HRESULT STDMETHODCALLTYPE get_IsUpgrade(boolean *v) override { if (!v) return E_POINTER; *v = upgrade; return S_OK; }
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
    ~PackageRequest() { if (metadata) metadata->Release(); if (specs) specs->Release(); }
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
    HRESULT STDMETHODCALLTYPE get_Specifiers(IInspectable **v) override;   /* gdkpackage.inc */
    IUnknown *specs = nullptr;   /* a SpecVector, made on first use */
    /* the features to install, as an IIterable<String> of feature ids */
    HRESULT STDMETHODCALLTYPE ConfigureFeatures(IInspectable *iterable) override
    {
        struct IStrIterableAbi : IInspectable { virtual HRESULT STDMETHODCALLTYPE First(IInspectable **) = 0; };
        struct IStrIteratorAbi : IInspectable
        {
            virtual HRESULT STDMETHODCALLTYPE get_Current(HSTRING *) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *) = 0;
            virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *) = 0;
        };
        static const GUID IID_IIterable_HString_ = {0xe2fcc7c1,0x3bfc,0x5a0b,{0xb2,0xb0,0x72,0xe7,0x69,0xd1,0xcb,0x7e}};
        IStrIterableAbi *it = nullptr;
        IInspectable *first = nullptr;
        features.clear();
        features_chosen = true;
        if (!iterable || FAILED(iterable->QueryInterface(IID_IIterable_HString_, (void **)&it)) || !it) { tracef("PackageRequest.ConfigureFeatures: no IIterable<String>"); return S_OK; }
        if (SUCCEEDED(it->First(&first)) && first)
        {
            IStrIteratorAbi *iter = (IStrIteratorAbi *)first;
            boolean has = FALSE;
            for (iter->get_HasCurrent(&has); has; iter->MoveNext(&has))
            {
                HSTRING h = nullptr;
                if (SUCCEEDED(iter->get_Current(&h)) && h) { features.push_back(utf8(WindowsGetStringRawBuffer(h, nullptr))); WindowsDeleteString(h); }
            }
            first->Release();
        }
        it->Release();
        std::string list;
        for (auto &f : features) list += (list.empty() ? "" : ";") + f;
        tracef("PackageRequest.ConfigureFeatures(%s)", list.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **v) override;
    std::vector<std::string> features;
    bool features_chosen = false;
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
    in->features = r->features;
    in->features_chosen = r->features_chosen;
    request_features_from_specifiers(r, in->features, in->features_chosen);
    req->Release();
    ProductDetails d = query_product(in->store_id);
    in->family = d.family;
    in->uwp = d.uwp;
    in->stage = stage_name_for_family(d.family);
    in->instance = instance_for(d.family.empty() ? in->store_id : d.family);
    in->total = d.size;
    if (!d.uwp) in->total = gdk_request_size(in->store_id, in->features_chosen ? &in->features : nullptr, d.size);
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
    if (in->state == ST_COMPLETED || in->state == ST_FAILED || in->state == ST_CANCELED)
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    tracef("install %ls: cancel requested", in->store_id.c_str());
    in->cancel = true;
    in->paused = false;
    InterlockedExchange(&in->cancel_flag, 1);
    InterlockedExchange(&in->pause_flag, 0);
    in->rate = 0;
    in->state = ST_CANCELED;
    in->error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return S_OK;
}
static HRESULT WINAPI pq_CancelInstance(Factory *, HSTRING instance, boolean nondestructive, boolean *out)
{
    std::wstring key = WindowsGetStringRawBuffer(instance, nullptr);
    AcquireSRWLockExclusive(&installs_lock);
    HRESULT hr = cancel_install(find_install(&key, nullptr));
    Install *in = find_install(&key, nullptr);
    ReleaseSRWLockExclusive(&installs_lock);
    if (SUCCEEDED(hr) && in) report(in);
    if (out) *out = SUCCEEDED(hr);
    return S_OK;
}
static HRESULT WINAPI pq_CancelId(Factory *, GUID id, boolean nondestructive, boolean *out)
{
    AcquireSRWLockExclusive(&installs_lock);
    HRESULT hr = cancel_install(find_install(nullptr, &id));
    Install *in = find_install(nullptr, &id);
    ReleaseSRWLockExclusive(&installs_lock);
    if (SUCCEEDED(hr) && in) report(in);
    if (out) *out = SUCCEEDED(hr);
    return S_OK;
}
/* Pause and Resume: the installer waits between chunks while its pause flag is set */
static void set_paused(Install *in, bool pause)
{
    bool changed;
    INT32 old_state, new_state;
    AcquireSRWLockExclusive(&installs_lock);
    old_state = in->state;
    changed = in->paused != pause && (in->state == ST_STREAMING || in->state == ST_PAUSED || in->state == ST_WAITING);
    if (changed)
    {
        in->paused = pause;
        in->rate = 0;
        in->state = pause ? ST_PAUSED : (in->running ? ST_STREAMING : ST_WAITING);
        InterlockedExchange(&in->pause_flag, pause);
    }
    new_state = in->state;
    ReleaseSRWLockExclusive(&installs_lock);
    if (!changed) return;
    tracef("install %ls: %s (state %d -> %d)", in->store_id.c_str(), pause ? "paused" : "resumed", old_state, new_state);
    report(in);
}
static void set_paused_where(const std::wstring *instance, const GUID *id, bool pause)
{
    std::vector<Install *> hits;
    AcquireSRWLockShared(&installs_lock);
    for (Install *in : installs)
        if ((!instance && !id) || (instance && (!_wcsicmp(in->instance.c_str(), instance->c_str()) || !_wcsicmp(in->store_id.c_str(), instance->c_str()))) ||
            (id && IsEqualGUID(in->id, *id)))
            hits.push_back(in);
    ReleaseSRWLockShared(&installs_lock);
    for (Install *in : hits) set_paused(in, pause);
}
static HRESULT WINAPI pq_PauseInstance(Factory *, HSTRING instance)
{ std::wstring s = WindowsGetStringRawBuffer(instance, nullptr); tracef("PackageQueue.PauseInstance(%ls)", s.c_str()); set_paused_where(&s, nullptr, true); return S_OK; }
static HRESULT WINAPI pq_ResumeInstance(Factory *, HSTRING instance)
{ std::wstring s = WindowsGetStringRawBuffer(instance, nullptr); tracef("PackageQueue.ResumeInstance(%ls)", s.c_str()); set_paused_where(&s, nullptr, false); return S_OK; }
static HRESULT WINAPI pq_PauseId(Factory *, GUID id) { tracef("PackageQueue.PauseId"); set_paused_where(nullptr, &id, true); return S_OK; }
static HRESULT WINAPI pq_ResumeId(Factory *, GUID id) { tracef("PackageQueue.ResumeId"); set_paused_where(nullptr, &id, false); return S_OK; }
static HRESULT WINAPI pq_PauseAll(Factory *) { tracef("PackageQueue.PauseAll"); set_paused_where(nullptr, nullptr, true); return S_OK; }
static HRESULT WINAPI pq_ResumeAll(Factory *) { tracef("PackageQueue.ResumeAll"); set_paused_where(nullptr, nullptr, false); return S_OK; }
static HRESULT WINAPI pq_CancelAll(Factory *, boolean, boolean *out)
{
    std::vector<Install *> canceled;
    AcquireSRWLockExclusive(&installs_lock);
    for (Install *in : installs) if (SUCCEEDED(cancel_install(in))) canceled.push_back(in);
    ReleaseSRWLockExclusive(&installs_lock);
    for (Install *in : canceled) report(in);
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
    HRESULT STDMETHODCALLTYPE Pause() override { tracef("PackageQueueView.Pause(%ls)", instance.c_str()); if (by_id) set_paused_where(nullptr, &id, true); else set_paused_where(&instance, nullptr, true); return S_OK; }
    HRESULT STDMETHODCALLTYPE Resume() override { tracef("PackageQueueView.Resume(%ls)", instance.c_str()); if (by_id) set_paused_where(nullptr, &id, false); else set_paused_where(&instance, nullptr, false); return S_OK; }
    HRESULT STDMETHODCALLTYPE Cancel(boolean, boolean *out) override
    {
        AcquireSRWLockExclusive(&installs_lock);
        HRESULT hr = cancel_install(find());
        Install *in = find();
        ReleaseSRWLockExclusive(&installs_lock);
        if (SUCCEEDED(hr) && in) report(in);
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
        tracef("PackageQueueView.Enumerate(%ls) -> %zu", instance.c_str(), v->items.size());
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
            /* Pending payloads are unpacked beside the final directory. QueryDetails needs a real
             * stage path so its logo properties point into the visible .new tree during download. */
            if (!in->stage.empty() && in->stage.find(L'\\') == std::wstring::npos)
                g.stage_dir = unix_to_windows(games_data_unix()) + L"\\stage\\" + in->stage + L".new";
            g.uwp = in->uwp;
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
static std::wstring package_logo_path(const InstalledGame &g)
{
    /* Keep the actual codec in the extension. Candy Crush's package logo is JPEG; the legacy
     * .png copy is retained for wine-uwp's desktop launcher, but XAML's image loader uses the
     * extension to choose its decoder. */
    const WCHAR *extensions[] = { L".jpg", L".jpeg", L".png" };
    for (const auto *extension : extensions)
    {
        std::wstring path = g.stage_dir + L"\\.wine-uwp-icon" + extension;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return g.stage_dir + L"\\.wine-uwp-icon.png";
}
struct PackageDetails : IPackageDetailsAbi
{
    LONG ref = 1;
    InstalledGame g;
    bool pending;
    PackageDetails(const InstalledGame &x, bool p = false) : g(x), pending(p) {}
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
    HRESULT STDMETHODCALLTYPE get_Square44x44LogoPath(HSTRING *v) override
    {
        std::wstring path = package_logo_path(g);
        tracef("PackageDetails(%ls).Square44x44LogoPath -> %ls", g.title.c_str(), path.c_str());
        return hstr(path, v);
    }
    HRESULT STDMETHODCALLTYPE get_Square150x150LogoPath(HSTRING *v) override
    {
        std::wstring path = package_logo_path(g);
        tracef("PackageDetails(%ls).Square150x150LogoPath -> %ls", g.title.c_str(), path.c_str());
        return hstr(path, v);
    }
    HRESULT STDMETHODCALLTYPE get_SplashScreenPath(HSTRING *v) override { return hstr(L"", v); }
    HRESULT STDMETHODCALLTYPE get_TitleId(UINT32 *v) override { if (!v) return E_POINTER; *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Registered(boolean *v) override { if (!v) return E_POINTER; *v = !pending; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Registerable(boolean *v) override { if (!v) return E_POINTER; *v = FALSE; /* installed packages are already registered; pending ones do not have all their files yet */ return S_OK; }
    HRESULT STDMETHODCALLTYPE get_NotFullyStreamed(boolean *v) override { if (!v) return E_POINTER; *v = pending; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsUwp(boolean *v) override
    {
        if (!v) return E_POINTER;
        *v = g.uwp;
        tracef("PackageDetails(%ls).IsUwp -> %d", g.title.c_str(), (int)g.uwp);
        return S_OK;
    }
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
    /* Paused installs take the client's package-metadata path that Streaming bypasses. The queued
     * package exists before it is registered; returning GP_E_NO_SERVICE here drops its queue row. */
    bool pending = !package_game(f, g);
    if (pending && !queued_game(f, g)) return generic_slot(f, 13, out, nullptr, __builtin_return_address(0));
    if (!out) return E_POINTER;
    tracef("Package(%ls).QueryDetails -> %s", f->source, pending ? "pending" : "installed");
    *out = static_cast<IPackageDetailsAbi *>(new PackageDetails(g, pending));
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

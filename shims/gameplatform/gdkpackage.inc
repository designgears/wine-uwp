/* ------------------------------------------------------------------------------------------- */
/* A GDK package (.msixvc) read where it lies on the CDN: its install features and recipes,    */
/* and which of its regions a choice of features and languages needs.                          */
/*                                                                                               */
/* Gaming Services answers the install dialog before anything is downloaded, so it reads the  */
/* package header remotely. What it needs is all near the start of the file:                   */
/*   - the XVD header (page 0) gives the layout: header, embedded XVD, mutable data, hash tree, */
/*     user data, XVC info, each a whole number of 4096-byte pages;                             */
/*   - the XVC info holds the region table (0x80 bytes a region from 0xDA8), then 12 bytes per  */
/*     update segment, then the region specifiers: a region id, then a key and a value in      */
/*     UTF-16 (64 and 128 characters). Key "Tags" names the features a region belongs to,      */
/*     "a#b" meaning a region needed only when both are chosen (Halo Infinite's game#hd1 is    */
/*     the base game's high-resolution textures); key "Languages" lists its languages;         */
/*   - the user data holds named files: Metadata.json with "InstallFeatures" and               */
/*     "InstallRecipes", Resources.pri with the features' display names (ms-resource:), and    */
/*     their images.                                                                            */
/* A region with no Tags specifier is always installed, and so is one whose languages are none */
/* of the user's only if it has no Languages specifier.                                         */
/* ------------------------------------------------------------------------------------------- */

static const UINT32 XVD_PAGE = 4096;

struct GdkRegion { UINT32 id; UINT16 key; UINT64 offset, length; std::vector<std::string> tags, languages; };
struct GdkFeature { std::string id, name, image, tags; bool hidden = false; };
struct GdkRecipe { std::string id, included, excluded; std::vector<std::string> store_ids; };
struct GdkPackage
{
    bool ok = false;
    std::string url, family;
    std::vector<GdkRegion> regions;
    std::vector<GdkFeature> features;
    std::vector<GdkRecipe> recipes;
};

static std::string http_range(const std::string &url, UINT64 offset, UINT64 length)
{
    std::wstring w(url.begin(), url.end()), host(256, 0), path(4096, 0), extra(4096, 0);
    std::string out;
    URL_COMPONENTS uc = {sizeof(uc)};
    uc.lpszHostName = &host[0]; uc.dwHostNameLength = (DWORD)host.size();
    uc.lpszUrlPath = &path[0]; uc.dwUrlPathLength = (DWORD)path.size();
    uc.lpszExtraInfo = &extra[0]; uc.dwExtraInfoLength = (DWORD)extra.size();
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return out;
    host.resize(uc.dwHostNameLength);
    path.resize(uc.dwUrlPathLength);
    path += extra.substr(0, uc.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"Microsoft-Delivery-Optimization/10.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0), c = nullptr, r = nullptr;
    wchar_t range[96];
    swprintf(range, 96, L"Range: bytes=%llu-%llu\r\n", (unsigned long long)offset, (unsigned long long)(offset + length - 1));
    if (s && (c = WinHttpConnect(s, host.c_str(), uc.nPort, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
    {
        WinHttpSetTimeouts(r, 20000, 20000, 30000, 30000);
        if (WinHttpSendRequest(r, range, (DWORD)-1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
        {
            char buf[65536];
            DWORD got = 0;
            out.reserve((size_t)length);
            while (out.size() < length && WinHttpReadData(r, buf, sizeof(buf), &got) && got) out.append(buf, got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (out.size() != length) out.clear();
    return out;
}

static UINT32 le32(const std::string &s, size_t o) { return (UINT8)s[o] | (UINT8)s[o + 1] << 8 | (UINT8)s[o + 2] << 16 | (UINT32)(UINT8)s[o + 3] << 24; }
static UINT16 le16(const std::string &s, size_t o) { return (UINT16)((UINT8)s[o] | (UINT8)s[o + 1] << 8); }
static UINT64 le64(const std::string &s, size_t o) { return le32(s, o) | (UINT64)le32(s, o + 4) << 32; }

static std::string utf16_field(const std::string &s, size_t o, size_t chars)
{
    std::wstring w;
    for (size_t i = 0; i < chars && o + 2 * i + 1 < s.size(); i++)
    {
        wchar_t c = le16(s, o + 2 * i);
        if (!c) break;
        w += c;
    }
    return utf8(w);
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    for (size_t a = 0, b; a <= s.size(); a = b + 1)
    {
        b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
    }
    return out;
}

/* ---- Resources.pri: one string resource, for the user's languages ---- */

struct Pri
{
    std::string d;
    std::vector<std::pair<std::string, size_t>> sections;   /* name, content offset */
    size_t find(const char *name, int nth = 0) const
    {
        for (auto &s : sections) if (s.first == name && !nth--) return s.second;
        return std::string::npos;
    }
};

/* the languages of a qualifier set, upper-cased; empty for the neutral one */
static std::vector<std::string> pri_qualifier_set_languages(const Pri &pri, size_t dec, unsigned qset)
{
    const std::string &d = pri.d;
    unsigned nd = le16(d, dec), nq = le16(d, dec + 2), nqs = le16(d, dec + 4), ndec = le16(d, dec + 6), nidx = le16(d, dec + 8);
    size_t qsets = dec + 12 + 4 * ndec, quals = qsets + 4 * nqs, distinct = quals + 8 * nq, index = distinct + 12 * nd,
           data = index + 2 * nidx;
    std::vector<std::string> out;
    if (qset >= nqs) return out;
    unsigned first = le16(d, qsets + 4 * qset), n = le16(d, qsets + 4 * qset + 2);
    for (unsigned k = 0; k < n; k++)
    {
        unsigned q = le16(d, index + 2 * (first + k));
        unsigned dq = le16(d, quals + 8 * q);
        if (le16(d, distinct + 12 * dq + 2) != 0) continue;   /* not a Language qualifier */
        std::string v = utf16_field(d, data + 2 * le32(d, distinct + 12 * dq + 8), 64);
        for (auto &c : v) c = (char)toupper((unsigned char)c);
        out.push_back(v);
    }
    return out;
}

/* ms-resource:name, ms-resource:///resources/name or ms-resource://package/resources/name */
static std::string pri_lookup(const std::string &blob, const std::string &ref, const std::vector<std::string> &langs)
{
    Pri pri;
    pri.d = blob;
    const std::string &d = pri.d;
    if (d.size() < 32 || d.compare(0, 7, "mrm_pri")) return "";
    size_t toc = le32(d, 16), start = le32(d, 20);
    unsigned nsec = le16(d, 24);
    for (unsigned i = 0; i < nsec && toc + 32 * (i + 1) <= d.size(); i++)
    {
        std::string name = d.substr(toc + 32 * i, 16);
        name = name.substr(0, name.find_last_not_of(std::string("\0 ", 2)) + 1);
        pri.sections.push_back({ name, start + le32(d, toc + 32 * i + 24) + 32 });
    }
    std::string path = ref.substr(strlen("ms-resource:"));
    if (!path.compare(0, 2, "//"))
    {
        size_t s = path.find('/', 2);
        path = s == std::string::npos ? "" : path.substr(s + 1);
    }
    while (!path.empty() && path[0] == '/') path.erase(0, 1);
    if (path.find('/') == std::string::npos) path = "resources/" + path;
    for (auto &c : path) c = (char)tolower((unsigned char)c);

    /* the hierarchical schema: the item with that full name */
    size_t hs = pri.find("[mrm_hschemaex]"), dec = pri.find("[mrm_decn_info]"), map = pri.find("[mrm_res_map2_]");
    if (hs == std::string::npos || dec == std::string::npos || map == std::string::npos) return "";
    size_t p = hs + 8 + 16 + 20 + 2 * le16(d, hs + 2) + 2 * le16(d, hs + 4);
    unsigned total = le32(d, p + 6), nscopes = le32(d, p + 10), nitems = le32(d, p + 14), ulen = le32(d, p + 18);
    size_t ents = p + 30, udata = ents + 12 * total + 8 * nscopes + 2 * nitems, adata = udata + 2 * ulen;
    std::function<std::string(unsigned)> full = [&](unsigned i) -> std::string {
        size_t e = ents + 12 * i;
        unsigned parent = le16(d, e), len = (UINT8)d[e + 6], flags = (UINT8)d[e + 7], off = le16(d, e + 8);
        std::string n = flags & 0x20 ? d.substr(adata + off, len) : utf16_field(d, udata + 2 * off, len);
        if (!i || parent == i) return n;
        std::string up = full(parent);
        return up.empty() ? n : up + "/" + n;
    };
    int item = -1;
    for (unsigned i = 0; i < total && item < 0; i++)
    {
        size_t e = ents + 12 * i;
        if ((UINT8)d[e + 7] & 0x10) continue;   /* a scope */
        std::string n = full(i);
        for (auto &c : n) c = (char)tolower((unsigned char)c);
        if (n == path) item = le16(d, e + 10);
    }
    if (item < 0) return "";

    /* the resource map: the item's candidates, and the decision whose qualifier sets they follow */
    unsigned envlen = le16(d, map), hslen = le16(d, map + 6), vtsize = le16(d, map + 10), i2g = le16(d, map + 12),
             ngroups = le16(d, map + 14);
    unsigned niteminfo = le32(d, map + 16), largelen = le32(d, map + 28);
    p = map + 32 + envlen + hslen;
    size_t vtypes = p;
    p += 8 * vtsize;
    size_t i2gtab = p;
    p += 4 * i2g;
    size_t groups = p;
    p += 4 * ngroups;
    size_t infos = p;
    p += 4 * niteminfo + largelen;
    size_t cands = p;
    int info = -1;
    for (unsigned g = 0; g < i2g && info < 0; g++)
    {
        unsigned first = le16(d, i2gtab + 4 * g), grp = le16(d, i2gtab + 4 * g + 2);
        if (grp >= ngroups) continue;
        unsigned size = le16(d, groups + 4 * grp), firstinfo = le16(d, groups + 4 * grp + 2);
        if ((unsigned)item >= first && (unsigned)item < first + size) info = firstinfo + item - first;
    }
    if (info < 0 || (unsigned)info >= niteminfo) return "";
    unsigned decision = le16(d, infos + 4 * info), firstcand = le16(d, infos + 4 * info + 2);
    unsigned ndec = le16(d, dec + 6), nidx = le16(d, dec + 8), nd = le16(d, dec), nq = le16(d, dec + 2), nqs = le16(d, dec + 4);
    if (decision >= ndec) return "";
    unsigned qfirst = le16(d, dec + 12 + 4 * decision), qn = le16(d, dec + 12 + 4 * decision + 2);
    size_t index = dec + 12 + 4 * ndec + 4 * nqs + 8 * nq + 12 * nd;
    (void)nidx;

    /* best candidate: the user's language exactly, then its base language, then the neutral one */
    int best = -1, best_score = -1;
    for (unsigned k = 0; k < qn; k++)
    {
        unsigned qs = le16(d, index + 2 * (qfirst + k));
        std::vector<std::string> cl = pri_qualifier_set_languages(pri, dec, qs);
        int score = cl.empty() ? 1 : 0;
        for (size_t li = 0; li < langs.size(); li++)
        {
            std::string want = langs[li], base = want.substr(0, want.find('-'));
            for (auto &l : cl)
            {
                int s = 0;
                if (l == want) s = 100;
                else if (l == base || l.substr(0, l.find('-')) == base) s = 50;
                if (s) score = std::max(score, s - (int)li);
            }
        }
        if (score > best_score) { best_score = score; best = k; }
    }
    if (best < 0) return "";
    size_t c = cands;
    for (unsigned k = 0; k < firstcand + best; k++)
    {
        if ((UINT8)d[c] != 1) return "";   /* only data-item candidates are understood */
        c += 8;
    }
    if ((UINT8)d[c] != 1) return "";
    unsigned vt = (UINT8)d[c + 1], di = le16(d, c + 4), ds = le16(d, c + 6);
    unsigned type = vt < vtsize ? le32(d, vtypes + 8 * vt + 4) : 4;
    if (ds >= pri.sections.size()) return "";
    size_t o = pri.sections[ds].second;
    unsigned nstr = le16(d, o + 4), nblob = le16(d, o + 6);
    if (di >= nstr) return "";
    size_t base = o + 12 + 4 * nstr + 8 * nblob;
    unsigned so = le16(d, o + 12 + 4 * di), sl = le16(d, o + 12 + 4 * di + 2);
    if (base + so + sl > d.size()) return "";
    std::string v = d.substr(base + so, sl);
    if (type == 0) v = utf16_field(v, 0, v.size() / 2);          /* String: UTF-16 */
    while (!v.empty() && !v.back()) v.pop_back();               /* AsciiString, Utf8String */
    return v;
}

static std::vector<std::string> user_languages(void)
{
    std::vector<std::string> out;
    ULONG count = 0, len = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &len) && len)
    {
        std::wstring buf(len, 0);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, &buf[0], &len))
            for (const wchar_t *p = buf.c_str(); *p; p += wcslen(p) + 1)
            {
                std::string s = utf8(p);
                for (auto &c : s) c = (char)toupper((unsigned char)c);
                out.push_back(s);
            }
    }
    if (out.empty()) out.push_back("EN-US");
    return out;
}

/* ---- the package, read once per product ---- */

static HRESULT msixvc_url(const GUID &content_id, std::string &url);

static bool gdk_read(const std::string &url, GdkPackage &pkg)
{
    std::string h = http_range(url, 0, XVD_PAGE);
    if (h.size() < XVD_PAGE || h.compare(0x200, 8, "msft-xvd")) { tracef("gdk: no XVD header at %s", url.c_str()); return false; }
    UINT64 drive = le64(h, 0x218);
    UINT32 emb = le32(h, 0x288), user = le32(h, 0x28c), xvc = le32(h, 0x290), dyn = le32(h, 0x294);
    auto pages = [](UINT64 n) { return (n + XVD_PAGE - 1) / XVD_PAGE; };
    UINT64 n = pages(user) + pages(xvc) + pages(dyn) + pages(drive), level = std::max<UINT64>(1, (n + 169) / 170), tree = level;
    for (int i = 0; i < 3; i++) { level = level == 1 ? 0 : (level + 169) / 170; tree += level; }
    UINT64 user_at = 3 + pages(emb) + (UINT8)h[0x470] + tree, xvc_at = user_at + pages(user);

    std::string info = http_range(url, xvc_at * XVD_PAGE, pages(xvc) * XVD_PAGE);
    if (info.size() < 0xDA8 || le32(info, 0xD10) < 1) { tracef("gdk: no region table"); return false; }
    UINT32 nregions = le32(info, 0xD14), nupdates = le32(info, 0xD3C), nspecs = le32(info, 0xD50);
    size_t specs = 0xDA8 + 0x80 * (size_t)nregions + 12 * (size_t)nupdates;
    if (specs + 392 * (size_t)nspecs > info.size()) { tracef("gdk: region specifiers past the XVC info"); return false; }
    for (UINT32 i = 0; i < nregions; i++)
    {
        size_t r = 0xDA8 + 0x80 * (size_t)i;
        GdkRegion g = { le32(info, r), le16(info, r + 4), le64(info, r + 0x50), le64(info, r + 0x58) };
        pkg.regions.push_back(g);
    }
    for (UINT32 i = 0; i < nspecs; i++)
    {
        size_t s = specs + 392 * (size_t)i;
        UINT32 id = le32(info, s);
        std::string key = utf16_field(info, s + 8, 64), value = utf16_field(info, s + 136, 128);
        for (auto &r : pkg.regions)
        {
            if (r.id != id) continue;
            if (key == "Tags") r.tags.push_back(value);
            else if (key == "Languages") for (auto &l : split(value, ';')) r.languages.push_back(l);
        }
    }

    /* the user data's files */
    std::string ud = http_range(url, user_at * XVD_PAGE, user);
    if (ud.size() < 16) return false;
    size_t table = le32(ud, 0);
    std::map<std::string, std::string> files;
    if (table + 528 <= ud.size())
    {
        pkg.family = utf16_field(ud, table + 4, 260);
        UINT32 count = le32(ud, table + 524);
        for (UINT32 i = 0; i < count && table + 528 * (i + 2) <= ud.size(); i++)
        {
            size_t e = table + 528 * (i + 1);
            std::string name = utf16_field(ud, e, 260);
            UINT32 size = le32(ud, e + 520), off = le32(ud, e + 524);
            if (16 + (size_t)off + size <= ud.size()) files[name] = ud.substr(16 + off, size);
        }
    }

    /* Metadata.json: the features and recipes; names from Resources.pri, images kept beside our other state */
    Json meta = parse_json(files["Metadata.json"]);
    std::vector<std::string> langs = user_languages();
    std::wstring image_dir;
    {
        WCHAR base[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (len && len < MAX_PATH)
        {
            image_dir = std::wstring(base) + L"\\wineuwp";
            CreateDirectoryW(image_dir.c_str(), nullptr);
            image_dir += L"\\gdk";
            CreateDirectoryW(image_dir.c_str(), nullptr);
            image_dir += L"\\" + wide(pkg.family);
            CreateDirectoryW(image_dir.c_str(), nullptr);
        }
    }
    for (auto &f : meta["InstallFeatures"].a)
    {
        GdkFeature g;
        g.id = f["Id"].str();
        g.name = f["DisplayName"].str();
        g.tags = f["Tags"].str();
        g.hidden = f["Hidden"].b;
        if (!g.name.compare(0, 12, "ms-resource:"))
        {
            std::string n = pri_lookup(files["Resources.pri"], g.name, langs);
            if (!n.empty()) g.name = n;
        }
        std::string image = f["Image"].str();
        if (!image.empty() && files.count(image) && !image_dir.empty() && image.find_first_of("\\/:") == std::string::npos)
        {
            std::wstring path = image_dir + L"\\" + wide(image);
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD written;
            if (file != INVALID_HANDLE_VALUE)
            {
                WriteFile(file, files[image].data(), (DWORD)files[image].size(), &written, nullptr);
                CloseHandle(file);
                g.image = utf8(path);
            }
        }
        pkg.features.push_back(g);
    }
    for (auto &r : meta["InstallRecipes"].a)
    {
        GdkRecipe g;
        g.id = r["Id"].str();
        g.included = r["IncludedFeatures"].str();
        g.excluded = r["ExcludedFeatures"].str();
        for (auto &s : r["StoreIds"].a) g.store_ids.push_back(s.str());
        pkg.recipes.push_back(g);
    }
    pkg.url = url;
    pkg.ok = true;
    tracef("gdk: %s: %zu regions, %u specifiers, %zu features, %zu recipes", pkg.family.c_str(), pkg.regions.size(), nspecs,
           pkg.features.size(), pkg.recipes.size());
    return true;
}

static std::shared_ptr<GdkPackage> gdk_package(const std::wstring &store_id)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::map<std::wstring, std::shared_ptr<GdkPackage>> cache;
    AcquireSRWLockShared(&lock);
    auto it = cache.find(store_id);
    std::shared_ptr<GdkPackage> found = it == cache.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&lock);
    if (found) return found;

    auto pkg = std::make_shared<GdkPackage>();
    ProductDetails d = query_product(store_id);
    std::string url;
    if (d.found && !d.uwp && SUCCEEDED(msixvc_url(d.content_id, url))) gdk_read(url, *pkg);
    AcquireSRWLockExclusive(&lock);
    cache[store_id] = pkg;
    ReleaseSRWLockExclusive(&lock);
    return pkg;
}

/* ---- entitlement: owned outright, or in the subscription's catalogue ---- */

static bool entitled_to(const std::string &store_id)
{
    typedef HRESULT (WINAPI *owns_fn)(const char *);
    HMODULE xa = LoadLibraryW(L"wineuwp_xboxauth.dll");
    owns_fn owns = xa ? (owns_fn)GetProcAddress(xa, "XboxAuthOwnsProduct") : nullptr;
    if (owns && owns(store_id.c_str()) == S_OK) return true;
    /* a length-prefixed string in the protobuf: the product is part of the subscription */
    std::string needle = std::string(1, (char)store_id.size()) + store_id;
    AcquireSRWLockShared(&content_access_lock);
    bool found = content_access_blob.find(needle) != std::string::npos;
    ReleaseSRWLockShared(&content_access_lock);
    return found;
}

/* ---- the choice: features -> tags -> regions ---- */

/* the features of the default recipe (and every hidden one), or of `chosen` when the app configured some */
static std::set<std::string> gdk_selected_features(const GdkPackage &pkg, const std::vector<std::string> *chosen)
{
    std::set<std::string> sel;
    for (auto &f : pkg.features) if (f.hidden) sel.insert(f.id);
    if (chosen) for (auto &c : *chosen) sel.insert(c);
    else
        for (auto &r : pkg.recipes)
            if (r.id == "default") for (auto &f : split(r.included, ';')) sel.insert(f);
    return sel;
}

static std::set<std::string> gdk_tags(const GdkPackage &pkg, const std::set<std::string> &features)
{
    std::set<std::string> tags;
    for (auto &f : pkg.features)
        if (features.count(f.id)) for (auto &t : split(f.tags, ';')) tags.insert(t);
    return tags;
}

static bool gdk_region_wanted(const GdkRegion &r, const std::set<std::string> &tags, const std::vector<std::string> &langs)
{
    if (!r.tags.empty())
    {
        bool any = false;
        for (auto &spec : r.tags)
        {
            bool all = true;
            for (auto &t : split(spec, '#')) all &= tags.count(t) != 0;
            any |= all;
        }
        if (!any) return false;
    }
    if (!r.languages.empty())
    {
        bool match = false;
        for (auto &l : r.languages)
        {
            std::string u = l;
            for (auto &c : u) c = (char)toupper((unsigned char)c);
            for (auto &want : langs)
                match |= u == want || u == want.substr(0, want.find('-')) || u.substr(0, u.find('-')) == want.substr(0, want.find('-'));
        }
        if (!match) return false;
    }
    return true;
}

static UINT64 gdk_install_size(const GdkPackage &pkg, const std::set<std::string> &features)
{
    std::set<std::string> tags = gdk_tags(pkg, features);
    std::vector<std::string> langs = user_languages();
    UINT64 size = 0;
    for (auto &r : pkg.regions)
        if ((r.id < 0x40000000 || r.id == 0x40000003) && gdk_region_wanted(r, tags, langs)) size += r.length;
    return size;
}

/* ---- PackageFeature, PackageRecipe, and a view of them ---- */

static const GUID IID_IPackageFeature = {0xa7a6472c,0x7eaf,0x58e9,{0xb2,0x1e,0x85,0xa3,0x07,0xa2,0x98,0x9e}};
static const GUID IID_IPackageRecipe = {0xafd64aa0,0x060f,0x51e8,{0x93,0xab,0x9b,0xcb,0x20,0x04,0x8c,0x86}};
static const GUID IID_IVectorView_PackageFeature = {0xe8024431,0xb448,0x5d50,{0xb8,0xc3,0x71,0x1d,0x18,0xe7,0xd8,0x66}};
static const GUID IID_IIterable_PackageFeature = {0x4d32f49a,0xd606,0x51c9,{0xbf,0x36,0x2d,0x76,0xbb,0x63,0xb3,0xaf}};
static const GUID IID_IIterator_PackageFeature = {0xebd85e37,0x7bd3,0x51fd,{0x8f,0xb0,0xcd,0xb4,0x73,0x36,0xf9,0x4d}};
static const GUID IID_IVectorView_PackageRecipe = {0x35e6aa5d,0x24eb,0x5f80,{0xa6,0xb1,0x75,0xf6,0x02,0xf3,0xfe,0xc8}};
static const GUID IID_IIterable_PackageRecipe = {0x6856e35e,0xdfaf,0x5d2a,{0xbb,0x25,0x94,0xe4,0x34,0x76,0x25,0xa1}};
static const GUID IID_IIterator_PackageRecipe = {0x10a66354,0xdf59,0x5c40,{0xb6,0x90,0xe7,0x45,0x02,0xf7,0xd1,0xc5}};

struct IPackageFeatureAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Image(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Tags(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Entitled(boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Selected(boolean *) = 0;
};
struct PackageFeatureObj : IPackageFeatureAbi
{
    LONG ref = 1;
    GdkFeature f;
    bool entitled, selected;
    PackageFeatureObj(const GdkFeature &g, bool e, bool s) : f(g), entitled(e), selected(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageFeature)) { *out = static_cast<IPackageFeatureAbi *>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageFeature")
    HRESULT STDMETHODCALLTYPE get_Id(HSTRING *v) override { return hstr(wide(f.id), v); }
    HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *v) override { return hstr(wide(f.name), v); }
    HRESULT STDMETHODCALLTYPE get_Image(HSTRING *v) override { return hstr(wide(f.image), v); }
    HRESULT STDMETHODCALLTYPE get_Tags(HSTRING *v) override { return hstr(wide(f.tags), v); }
    HRESULT STDMETHODCALLTYPE get_Entitled(boolean *v) override { if (!v) return E_POINTER; *v = entitled; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Selected(boolean *v) override { if (!v) return E_POINTER; *v = selected; return S_OK; }
};

struct IPackageRecipeAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IncludedFeatures(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExcludedFeatures(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StoreIds(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Selected(boolean *) = 0;
};
struct PackageRecipeObj : IPackageRecipeAbi
{
    LONG ref = 1;
    GdkRecipe r;
    bool selected;
    PackageRecipeObj(const GdkRecipe &g, bool s) : r(g), selected(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageRecipe)) { *out = static_cast<IPackageRecipeAbi *>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageRecipe")
    HRESULT STDMETHODCALLTYPE get_Id(HSTRING *v) override { return hstr(wide(r.id), v); }
    HRESULT STDMETHODCALLTYPE get_IncludedFeatures(HSTRING *v) override { return hstr(wide(r.included), v); }
    HRESULT STDMETHODCALLTYPE get_ExcludedFeatures(HSTRING *v) override { return hstr(wide(r.excluded), v); }
    HRESULT STDMETHODCALLTYPE get_StoreIds(HSTRING *v) override
    {
        std::string s;
        for (auto &id : r.store_ids) s += (s.empty() ? "" : ";") + id;
        return hstr(wide(s), v);
    }
    HRESULT STDMETHODCALLTYPE get_Selected(boolean *v) override { if (!v) return E_POINTER; *v = selected; return S_OK; }
};

/* IVectorView<T> of interfaces, with IIterable<T> and its iterators */
struct ObjView;
struct ObjIterator : IEmptyIteratorAbi
{
    LONG ref = 1;
    ObjView *view;
    UINT32 pos = 0;
    ObjIterator(ObjView *v);
    ~ObjIterator();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override;
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.Collections.IIterator`1")
    HRESULT STDMETHODCALLTYPE get_Current(IInspectable **v) override;
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *v) override;
    HRESULT STDMETHODCALLTYPE MoveNext(boolean *v) override;
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 n, IInspectable **out, UINT32 *got) override;
};
struct ObjView : IEmptyViewAbi, IGuidIterableAbi
{
    LONG ref = 1;
    GUID view_iid, iterable_iid, iterator_iid;
    std::wstring name;
    std::vector<IInspectable *> items;
    ObjView(REFIID v, REFIID it, REFIID itr, const wchar_t *n) : view_iid(v), iterable_iid(it), iterator_iid(itr), name(n) {}
    ~ObjView() { for (auto *i : items) i->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, view_iid)) *out = static_cast<IEmptyViewAbi *>(this);
        else if (IsEqualGUID(iid, iterable_iid)) *out = static_cast<IGuidIterableAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(name)
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if (i >= items.size()) { *v = nullptr; return E_BOUNDS; }
        (*v = items[i])->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *v) override { if (!v) return E_POINTER; *v = (UINT32)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *x, UINT32 *idx, boolean *found) override
    {
        if (!idx || !found) return E_POINTER;
        *idx = 0; *found = FALSE;
        for (UINT32 i = 0; i < items.size(); i++) if (items[i] == x) { *idx = i; *found = TRUE; break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 n, IInspectable **out, UINT32 *got) override
    {
        if (!got) return E_POINTER;
        *got = 0;
        for (UINT32 i = start; i < items.size() && *got < n; i++) { (out[*got] = items[i])->AddRef(); ++*got; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE First(IInspectable **v) override { if (!v) return E_POINTER; *v = static_cast<IEmptyIteratorAbi *>(new ObjIterator(this)); return S_OK; }
};
ObjIterator::ObjIterator(ObjView *v) : view(v) { view->AddRef(); }
ObjIterator::~ObjIterator() { view->Release(); }
HRESULT STDMETHODCALLTYPE ObjIterator::QueryInterface(REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (base_iid(iid) || IsEqualGUID(iid, view->iterator_iid)) { *out = static_cast<IEmptyIteratorAbi *>(this); AddRef(); return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE ObjIterator::get_Current(IInspectable **v) { return view->GetAt(pos, v); }
HRESULT STDMETHODCALLTYPE ObjIterator::get_HasCurrent(boolean *v) { if (!v) return E_POINTER; *v = pos < view->items.size(); return S_OK; }
HRESULT STDMETHODCALLTYPE ObjIterator::MoveNext(boolean *v) { if (pos < view->items.size()) pos++; return get_HasCurrent(v); }
HRESULT STDMETHODCALLTYPE ObjIterator::GetMany(UINT32 n, IInspectable **out, UINT32 *got)
{
    HRESULT hr = view->GetMany(pos, n, out, got);
    if (SUCCEEDED(hr)) pos += *got;
    return hr;
}

/* the product a PackageInstance stands for: its own source when that is a Store id, or the install's */
static std::wstring instance_store_id(Factory *f)
{
    InstalledGame g;
    if (package_game(f, g) || queued_game(f, g)) return g.store_id;
    return f->source;
}

/* PackageInstance.QueryFeatures: the package's install features. The install dialog of a game that has them asks
 * before it offers Install; "not implemented" closed it (error 50253) and Install did nothing. */
static HRESULT WINAPI pi_QueryFeatures(Factory *f, IInspectable **out)
{
    if (!out) return E_POINTER;
    std::wstring store_id = instance_store_id(f);
    auto pkg = gdk_package(store_id);
    ObjView *view = new ObjView(IID_IVectorView_PackageFeature, IID_IIterable_PackageFeature, IID_IIterator_PackageFeature,
                                L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageFeature>");
    std::set<std::string> sel = gdk_selected_features(*pkg, nullptr);
    for (auto &feat : pkg->features)
    {
        if (feat.hidden) continue;
        /* entitled unless a recipe that brings it in names products this account has none of */
        bool entitled = true;
        for (auto &r : pkg->recipes)
        {
            if (r.store_ids.empty()) continue;
            auto inc = split(r.included, ';');
            if (std::find(inc.begin(), inc.end(), feat.id) == inc.end()) continue;
            entitled = false;
            for (auto &id : r.store_ids) entitled |= entitled_to(id);
        }
        view->items.push_back(static_cast<IPackageFeatureAbi *>(new PackageFeatureObj(feat, entitled, sel.count(feat.id) != 0)));
        tracef("PackageInstance.QueryFeatures(%ls): %s \"%s\", tags %s, %s, %s", store_id.c_str(), feat.id.c_str(), feat.name.c_str(),
               feat.tags.c_str(), entitled ? "entitled" : "not entitled", sel.count(feat.id) ? "selected" : "not selected");
    }
    *out = static_cast<IEmptyViewAbi *>(view);
    return S_OK;
}

static HRESULT WINAPI pi_QueryRecipes(Factory *f, IInspectable **out)
{
    if (!out) return E_POINTER;
    auto pkg = gdk_package(instance_store_id(f));
    ObjView *view = new ObjView(IID_IVectorView_PackageRecipe, IID_IIterable_PackageRecipe, IID_IIterator_PackageRecipe,
                                L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageRecipe>");
    for (auto &r : pkg->recipes)
        view->items.push_back(static_cast<IPackageRecipeAbi *>(new PackageRecipeObj(r, r.id == "default")));
    tracef("PackageInstance.QueryRecipes -> %zu", view->items.size());
    *out = static_cast<IEmptyViewAbi *>(view);
    return S_OK;
}

/* ---- what a request installs ---- */

static UINT64 gdk_request_size(const std::wstring &store_id, const std::vector<std::string> *features, UINT64 fallback)
{
    auto pkg = gdk_package(store_id);
    if (!pkg->ok || pkg->features.empty()) return fallback;
    return gdk_install_size(*pkg, gdk_selected_features(*pkg, features));
}

/* " --tags 'a;b' --languages 'EN-US;EN'" for install-app.py; nothing for a package without install features */
static std::wstring gdk_installer_args(const std::wstring &store_id, const std::vector<std::string> *features)
{
    auto pkg = gdk_package(store_id);
    if (!pkg->ok || pkg->features.empty()) return L"";
    std::string tags, langs;
    for (auto &t : gdk_tags(*pkg, gdk_selected_features(*pkg, features))) tags += (tags.empty() ? "" : ";") + t;
    for (auto &l : user_languages()) langs += (langs.empty() ? "" : ";") + l;
    for (auto c : tags + langs) if (c == '\'' || c == '"' || c == '\\') return L"";   /* nothing that could leave the quotes */
    tracef("install: features -> tags %s, languages %s", tags.c_str(), langs.c_str());
    return L" --tags '" + wide(tags) + L"' --languages '" + wide(langs) + L"'";
}

HRESULT STDMETHODCALLTYPE PackageRequest::QuerySize(IInspectable **v)
{
    if (!v) return E_POINTER;
    ProductDetails d = query_product(store_id);
    UINT64 size = d.uwp ? d.size : gdk_request_size(store_id, features_chosen ? &features : nullptr, d.size);
    tracef("PackageRequest.QuerySize(%ls) -> %llu", store_id.c_str(), (unsigned long long)size);
    *v = static_cast<IPackageRequestSizeAbi *>(new PackageRequestSize(size));
    return S_OK;
}

/* ---- PackageSpecifier vectors, PackageFeatureSize, PackagePreview ---- */

static const GUID IID_IPackageSpecifier = {0xb9f1224d,0x53d2,0x5c45,{0x8a,0x00,0x2b,0xdf,0x75,0xea,0xd8,0xf7}};
static const GUID IID_IVector_PackageSpecifier = {0x194d8e83,0x8d1f,0x5ad2,{0x97,0xc6,0xcf,0xb8,0xcc,0xfd,0x9c,0x9c}};
static const GUID IID_IVectorView_PackageSpecifier = {0x3b289596,0x3c6e,0x5aa2,{0xa4,0x12,0xdc,0xfa,0x01,0x3e,0x5c,0x5e}};
static const GUID IID_IIterable_PackageSpecifier = {0x915a098a,0x14e9,0x5ae3,{0x9a,0xa3,0x88,0x39,0x2a,0x15,0x23,0xa1}};
static const GUID IID_IIterator_PackageSpecifier = {0xc9f7da29,0x7588,0x52fd,{0xa1,0xec,0x50,0x73,0x6b,0xb9,0x33,0xa7}};
static const GUID IID_IPackageFeatureSize = {0x49efa3f5,0x5fac,0x5707,{0x9a,0xe5,0x9f,0xd8,0x13,0x7e,0xf4,0xbf}};
static const GUID IID_IVectorView_PackageFeatureSize = {0x1e81dbf1,0x9d0c,0x5821,{0xb0,0x5f,0x32,0xae,0xf7,0xf6,0xd4,0xad}};
static const GUID IID_IIterable_PackageFeatureSize = {0x08dba954,0x6261,0x5882,{0xb6,0xf7,0xf1,0xff,0xd4,0x6e,0xf3,0xe6}};
static const GUID IID_IIterator_PackageFeatureSize = {0xb0a92423,0xbd15,0x5a71,{0x93,0x05,0xeb,0xdc,0xc2,0x2d,0x7b,0xfa}};
static const GUID IID_IPackagePreview = {0xf99e1a4a,0x5416,0x5636,{0xb6,0x2f,0x98,0x17,0x3e,0x97,0xa8,0x1d}};
static const GUID IID_IPackagePreviewSize = {0xdfbca13d,0xe4ac,0x5eca,{0xa7,0xec,0x5b,0xe8,0x92,0x60,0x8c,0x8c}};

enum { SPEC_CONTENT_TYPE, SPEC_DEVICE, SPEC_LANGUAGE, SPEC_TAG, SPEC_RECIPE, SPEC_FEATURE };

struct IPackageSpecifierAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Type(INT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Type(INT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Value(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Value(HSTRING) = 0;
};

/* IVector<PackageSpecifier>: what the app adds to a request or a preview (features, languages, recipes...) */
struct ISpecVectorAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetView(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *, UINT32 *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAt(UINT32, IInspectable *) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAt(UINT32, IInspectable *) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAt(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE Append(IInspectable *) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAtEnd() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clear() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IInspectable **, UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReplaceAll(UINT32, IInspectable **) = 0;
};
struct SpecVector : ISpecVectorAbi, IGuidIterableAbi
{
    LONG ref = 1;
    std::vector<IInspectable *> items;
    ~SpecVector() { for (auto *i : items) i->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IVector_PackageSpecifier)) *out = static_cast<ISpecVectorAbi *>(this);
        else if (IsEqualGUID(iid, IID_IIterable_PackageSpecifier)) *out = static_cast<IGuidIterableAbi *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IINSPECTABLE_BOILERPLATE(L"Windows.Foundation.Collections.IVector`1<Microsoft.GamePlatform.Services.PackageSpecifier>")
    ObjView *snapshot()
    {
        ObjView *v = new ObjView(IID_IVectorView_PackageSpecifier, IID_IIterable_PackageSpecifier, IID_IIterator_PackageSpecifier,
                                 L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageSpecifier>");
        for (auto *i : items) { i->AddRef(); v->items.push_back(i); }
        return v;
    }
    static void log_item(const char *what, IInspectable *item)
    {
        IPackageSpecifierAbi *spec = nullptr;
        INT32 type = -1;
        HSTRING value = nullptr;
        if (item && SUCCEEDED(item->QueryInterface(IID_IPackageSpecifier, (void **)&spec)) && spec)
        {
            spec->get_Type(&type);
            spec->get_Value(&value);
            spec->Release();
        }
        tracef("PackageSpecifier %s: type %d, value %ls", what, type, value ? WindowsGetStringRawBuffer(value, nullptr) : L"?");
        if (value) WindowsDeleteString(value);
    }
    HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, IInspectable **v) override
    {
        if (!v) return E_POINTER;
        if (i >= items.size()) { *v = nullptr; return E_BOUNDS; }
        (*v = items[i])->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Size(UINT32 *v) override { if (!v) return E_POINTER; *v = (UINT32)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetView(IInspectable **v) override { if (!v) return E_POINTER; *v = static_cast<IEmptyViewAbi *>(snapshot()); return S_OK; }
    HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *x, UINT32 *idx, boolean *found) override
    {
        if (!idx || !found) return E_POINTER;
        *idx = 0; *found = FALSE;
        for (UINT32 i = 0; i < items.size(); i++) if (items[i] == x) { *idx = i; *found = TRUE; break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAt(UINT32 i, IInspectable *x) override
    {
        if (i >= items.size() || !x) return E_BOUNDS;
        log_item("set", x);
        x->AddRef(); items[i]->Release(); items[i] = x;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InsertAt(UINT32 i, IInspectable *x) override
    {
        if (i > items.size() || !x) return E_BOUNDS;
        log_item("inserted", x);
        x->AddRef(); items.insert(items.begin() + i, x);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RemoveAt(UINT32 i) override
    {
        if (i >= items.size()) return E_BOUNDS;
        items[i]->Release(); items.erase(items.begin() + i);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Append(IInspectable *x) override { return InsertAt((UINT32)items.size(), x); }
    HRESULT STDMETHODCALLTYPE RemoveAtEnd() override { return items.empty() ? E_BOUNDS : RemoveAt((UINT32)items.size() - 1); }
    HRESULT STDMETHODCALLTYPE Clear() override { for (auto *i : items) i->Release(); items.clear(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 n, IInspectable **out, UINT32 *got) override
    {
        if (!got) return E_POINTER;
        *got = 0;
        for (UINT32 i = start; i < items.size() && *got < n; i++) { (out[*got] = items[i])->AddRef(); ++*got; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReplaceAll(UINT32 n, IInspectable **x) override
    {
        Clear();
        for (UINT32 i = 0; i < n; i++) if (x[i]) Append(x[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE First(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        ObjView *view = snapshot();
        *v = static_cast<IEmptyIteratorAbi *>(new ObjIterator(view));
        view->Release();
        return S_OK;
    }
    /* the values of one type */
    std::vector<std::string> values(INT32 want)
    {
        std::vector<std::string> out;
        for (auto *i : items)
        {
            IPackageSpecifierAbi *spec = nullptr;
            INT32 type = -1;
            HSTRING value = nullptr;
            if (FAILED(i->QueryInterface(IID_IPackageSpecifier, (void **)&spec)) || !spec) continue;
            spec->get_Type(&type);
            if (type == want && SUCCEEDED(spec->get_Value(&value)) && value)
                out.push_back(utf8(WindowsGetStringRawBuffer(value, nullptr)));
            if (value) WindowsDeleteString(value);
            spec->Release();
        }
        return out;
    }
};

/* The features a request or preview stands for: Feature specifiers, else the features of its Recipe specifiers,
 * else what ConfigureFeatures named, else the default recipe. */
static std::vector<std::string> *choose_features(const GdkPackage &pkg, SpecVector *specs, bool configured,
                                                 const std::vector<std::string> &configured_list, std::vector<std::string> &storage)
{
    if (specs)
    {
        storage = specs->values(SPEC_FEATURE);
        for (auto &recipe : specs->values(SPEC_RECIPE))
            for (auto &r : pkg.recipes)
                if (r.id == recipe) for (auto &f : split(r.included, ';')) storage.push_back(f);
        if (!storage.empty()) return &storage;
    }
    if (configured) { storage = configured_list; return &storage; }
    return nullptr;
}

struct IPackageFeatureSizeAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DownloadSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Shared(boolean *) = 0;
};
struct PackageFeatureSizeObj : IPackageFeatureSizeAbi
{
    LONG ref = 1;
    std::string id;
    UINT64 size;
    bool shared;
    PackageFeatureSizeObj(const std::string &i, UINT64 s, bool sh) : id(i), size(s), shared(sh) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackageFeatureSize)) { *out = static_cast<IPackageFeatureSizeAbi *>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackageFeatureSize")
    HRESULT STDMETHODCALLTYPE get_Id(HSTRING *v) override { return hstr(wide(id), v); }
    HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DownloadSize(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Shared(boolean *v) override { if (!v) return E_POINTER; *v = shared; return S_OK; }
};

/* What a feature adds on its own: the regions the choice with it needs that the choice without it does not. A
 * region another feature also needs (game#hd1 for HD1 next to Game) is shared. */
static UINT64 gdk_feature_size(const GdkPackage &pkg, const std::string &feature, bool *shared)
{
    std::set<std::string> with = gdk_selected_features(pkg, nullptr), without;
    with.insert(feature);
    without = with;
    without.erase(feature);
    std::set<std::string> tw = gdk_tags(pkg, with), two = gdk_tags(pkg, without);
    std::vector<std::string> langs = user_languages();
    UINT64 size = 0;
    *shared = false;
    for (auto &r : pkg.regions)
    {
        if (r.id >= 0x40000000 || r.tags.empty()) continue;
        if (gdk_region_wanted(r, tw, langs) && !gdk_region_wanted(r, two, langs))
        {
            size += r.length;
            for (auto &spec : r.tags) *shared |= spec.find('#') != std::string::npos;
        }
    }
    return size;
}

struct IPackagePreviewSizeAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DownloadSize(UINT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StorageSpaceChange(INT64 *) = 0;
};
struct PackagePreviewSizeObj : IPackagePreviewSizeAbi
{
    LONG ref = 1;
    UINT64 size;
    PackagePreviewSizeObj(UINT64 s) : size(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackagePreviewSize)) { *out = static_cast<IPackagePreviewSizeAbi *>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackagePreviewSize")
    HRESULT STDMETHODCALLTYPE get_TotalSize(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DownloadSize(UINT64 *v) override { if (!v) return E_POINTER; *v = size; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_StorageSpaceChange(INT64 *v) override { if (!v) return E_POINTER; *v = (INT64)size; return S_OK; }
};

struct IPackagePreviewAbi : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Specifiers(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryFeatureSizes(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConfigureFeatures(IInspectable *) = 0;
};
/* PackagePreview: a package before it is installed - its size for a choice of features, and each feature's */
struct PackagePreviewObj : IPackagePreviewAbi
{
    LONG ref = 1;
    std::wstring store_id;
    SpecVector *specs = new SpecVector;
    std::vector<std::string> features;
    bool configured = false;
    ~PackagePreviewObj() { specs->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (base_iid(iid) || IsEqualGUID(iid, IID_IPackagePreview)) { *out = static_cast<IPackagePreviewAbi *>(this); AddRef(); return S_OK; }
        log_guid("PackagePreview: QueryInterface refused", kind_of(L"Microsoft.GamePlatform.Services.PackagePreview"), iid);
        if (IsEqualGUID(iid, IID_IWeakReferenceSource_)) return weak_source(this, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IINSPECTABLE_BOILERPLATE(L"Microsoft.GamePlatform.Services.PackagePreview")
    HRESULT STDMETHODCALLTYPE get_Specifiers(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        specs->AddRef();
        *v = static_cast<ISpecVectorAbi *>(specs);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QuerySize(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        auto pkg = gdk_package(store_id);
        std::vector<std::string> storage;
        ProductDetails d = query_product(store_id);
        UINT64 size = pkg->ok && !pkg->features.empty()
            ? gdk_install_size(*pkg, gdk_selected_features(*pkg, choose_features(*pkg, specs, configured, features, storage)))
            : d.size;
        tracef("PackagePreview(%ls).QuerySize -> %llu", store_id.c_str(), (unsigned long long)size);
        *v = static_cast<IPackagePreviewSizeAbi *>(new PackagePreviewSizeObj(size));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryFeatureSizes(IInspectable **v) override
    {
        if (!v) return E_POINTER;
        auto pkg = gdk_package(store_id);
        ObjView *view = new ObjView(IID_IVectorView_PackageFeatureSize, IID_IIterable_PackageFeatureSize, IID_IIterator_PackageFeatureSize,
                                    L"Windows.Foundation.Collections.IVectorView`1<Microsoft.GamePlatform.Services.PackageFeatureSize>");
        for (auto &f : pkg->features)
        {
            if (f.hidden) continue;
            bool shared = false;
            UINT64 size = gdk_feature_size(*pkg, f.id, &shared);
            view->items.push_back(static_cast<IPackageFeatureSizeAbi *>(new PackageFeatureSizeObj(f.id, size, shared)));
            tracef("PackagePreview(%ls).QueryFeatureSizes: %s %llu%s", store_id.c_str(), f.id.c_str(), (unsigned long long)size,
                   shared ? " (shared)" : "");
        }
        *v = static_cast<IEmptyViewAbi *>(view);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConfigureFeatures(IInspectable *iterable) override
    {
        PackageRequest tmp;   /* the request's reader of an IIterable<String> */
        tmp.ConfigureFeatures(iterable);
        features = tmp.features;
        configured = true;
        tracef("PackagePreview(%ls).ConfigureFeatures: %zu features", store_id.c_str(), features.size());
        return S_OK;
    }
};

/* PackagePreview.Create(source, sourceType) */
static HRESULT WINAPI pv_Create(Factory *, HSTRING source, INT32 type, IInspectable **out)
{
    if (!out) return E_POINTER;
    PackagePreviewObj *p = new PackagePreviewObj;
    p->store_id = WindowsGetStringRawBuffer(source, nullptr);
    if (type != 1)
    {
        InstalledGame g;
        if (find_installed(p->store_id, g)) p->store_id = g.store_id;
    }
    tracef("PackagePreview.Create(%ls, type %d)", p->store_id.c_str(), type);
    *out = static_cast<IPackagePreviewAbi *>(p);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE PackageRequest::get_Specifiers(IInspectable **v)
{
    if (!v) return E_POINTER;
    if (!specs) specs = static_cast<ISpecVectorAbi *>(new SpecVector);
    specs->AddRef();
    *v = static_cast<ISpecVectorAbi *>(static_cast<SpecVector *>(static_cast<ISpecVectorAbi *>(specs)));
    return S_OK;
}

/* Feature and Recipe specifiers on the request win over ConfigureFeatures */
static void request_features_from_specifiers(PackageRequest *r, std::vector<std::string> &features, bool &chosen)
{
    if (!r->specs) return;
    SpecVector *specs = static_cast<SpecVector *>(static_cast<ISpecVectorAbi *>(r->specs));
    auto pkg = gdk_package(r->store_id);
    std::vector<std::string> storage;
    if (choose_features(*pkg, specs, false, features, storage))
    {
        features = storage;
        chosen = true;
    }
}

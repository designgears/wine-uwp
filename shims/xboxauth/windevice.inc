/* ------------------------------------------------------------------------------------------- */
/* A Windows device identity, and content licences with it                                     */
/*                                                                                               */
/* Encrypted Store packages (EAppx, MSIXVC) are opened with content keys from                   */
/* licensing.mp.microsoft.com, which hands them out only to a Windows *device*: its answer packs */
/* each key (AES key wrap, RFC 3394) under a key carried in that device's licence. Windows gets */
/* the device licence once, from login.live.com/ppsecure/deviceaddcredential.srf, and a device  */
/* ticket for the licensing service from RST2.srf (WS-Trust):                                   */
/*   1. deviceaddcredential.srf: a random member name and password plus hardware components    */
/*      answer with an SPLicenseBlock; its ClepSignState block holds the device's RSA key.     */
/*   2. RST2.srf, signed with that RSA key (XML-DSig, exclusive c14n, rsa-sha256): the device   */
/*      STS token (an opaque EncryptedData) and its proof key (ClepHmacState: the HMAC secret). */
/*   3. RST2.srf again with that token, HMAC-signed with SP800-108 derived keys, for             */
/*      www.microsoft.com (mbi_ssl): the compact device ticket for the Authorization header.   */
/*   4. licensing.mp.microsoft.com/v7.0/licenses/content with the device ticket and the user's  */
/*      ticket for the same service (our MSA sign-in's refresh token gets that one); the        */
/*      answer's SPLicenseBlock has the packed keys, and the device licence's EncryptedDeviceKey */
/*      block the key-encryption key.                                                           */
/* The protocol was learnt from Xodus (github.com/xodus-project, GPL-3.0); this is our own code. */
/*                                                                                               */
/* Hardware components: Windows sends the real SMBIOS system entry, disk serials and so on.     */
/* We send a synthetic SMBIOS type 1 entry (vendor "Wine", a random serial and UUID made once   */
/* per prefix), so no identifier of this machine leaves it.                                    */
/* ------------------------------------------------------------------------------------------- */

static const char MSA_UA[] = "User-Agent: MSAWindows/55 (OS 10.0.26100.0.0 ge_release; IDK 10.0.26100.5074 ge_release; Cfg 16.000.29325.00; Test 0)";

static const char NS_S[] = "http://www.w3.org/2003/05/soap-envelope";
static const char NS_PS[] = "http://schemas.microsoft.com/Passport/SoapServices/PPCRL";
static const char NS_WSSE[] = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
static const char NS_WSU[] = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
static const char NS_WSP[] = "http://schemas.xmlsoap.org/ws/2004/09/policy";
static const char NS_WSA[] = "http://www.w3.org/2005/08/addressing";
static const char NS_WSSC[] = "http://schemas.xmlsoap.org/ws/2005/02/sc";
static const char NS_WST[] = "http://schemas.xmlsoap.org/ws/2005/02/trust";
static const char NS_DSIG[] = "http://www.w3.org/2000/09/xmldsig#";
static const char EXC_C14N[] = "http://www.w3.org/2001/10/xml-exc-c14n#";

/* -- small crypto helpers (BCrypt) -- */

static bool random_bytes(void *p, size_t n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

static std::string hmac_sha256(const std::string &key, const std::string &data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char out[32];
    std::string r;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return r;
    if (BCRYPT_SUCCESS(BCryptHash(alg, (PUCHAR)key.data(), (ULONG)key.size(), (PUCHAR)data.data(), (ULONG)data.size(), out, 32)))
        r.assign((char *)out, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return r;
}

/* AES in ECB or CBC (iv non-empty), no padding; decrypt or encrypt. */
static bool aes_raw(const std::string &key, const std::string &iv, bool decrypt, std::string &data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = false;
    const wchar_t *mode = iv.empty() ? BCRYPT_CHAIN_MODE_ECB : BCRYPT_CHAIN_MODE_CBC;
    std::string ivc = iv;
    ULONG done = 0;
    if (data.size() % 16) return false;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) return false;
    if (BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)(wcslen(mode) + 1) * 2, 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0)))
    {
        std::string out(data.size(), '\0');
        NTSTATUS st = decrypt
            ? BCryptDecrypt(k, (PUCHAR)data.data(), (ULONG)data.size(), nullptr, ivc.empty() ? nullptr : (PUCHAR)&ivc[0],
                            (ULONG)ivc.size(), (PUCHAR)&out[0], (ULONG)out.size(), &done, 0)
            : BCryptEncrypt(k, (PUCHAR)data.data(), (ULONG)data.size(), nullptr, ivc.empty() ? nullptr : (PUCHAR)&ivc[0],
                            (ULONG)ivc.size(), (PUCHAR)&out[0], (ULONG)out.size(), &done, 0);
        if (BCRYPT_SUCCESS(st) && done == data.size()) { data = out; ok = true; }
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* RFC 3394 key unwrap with the default IV. */
static bool aes_key_unwrap(const std::string &kek, const std::string &wrapped, std::string &key)
{
    size_t n = wrapped.size() / 8 - 1;
    if (wrapped.size() % 8 || n < 2) return false;
    unsigned char a[8];
    std::vector<std::string> r(n);
    memcpy(a, wrapped.data(), 8);
    for (size_t i = 0; i < n; i++) r[i] = wrapped.substr(8 + 8 * i, 8);
    for (int j = 5; j >= 0; j--)
        for (size_t i = n; i >= 1; i--)
        {
            unsigned long long t = (unsigned long long)n * j + i;
            for (int b = 0; b < 8; b++) a[7 - b] ^= (unsigned char)(t >> (8 * b));
            std::string block((char *)a, 8);
            block += r[i - 1];
            if (!aes_raw(kek, "", true, block)) return false;
            memcpy(a, block.data(), 8);
            r[i - 1] = block.substr(8);
        }
    for (int b = 0; b < 8; b++) if (a[b] != 0xa6) return false;
    key.clear();
    for (auto &x : r) key += x;
    return true;
}

/* SP800-108 counter mode, HMAC-SHA256, one 32-byte block: Windows' WS-SecureConversation key. */
static std::string sp800108_key(const std::string &secret, const std::string &nonce)
{
    static const char label[] = "WS-SecureConversationWS-SecureConversation";
    std::string m("\0\0\0\1", 4);
    m += label;
    m += '\0';
    m += nonce;
    m += std::string("\0\0\1\0", 4);   /* 256 bits */
    return hmac_sha256(secret, m);
}

/* -- the SPLicenseBlock: a header, then type/length/value blocks -- */

struct SpLicense
{
    std::string sign_state;      /* 0x12d ClepSignState */
    std::string device_key;      /* 0x01 EncryptedDeviceKey */
    std::vector<std::pair<std::string, std::string>> packed_keys;  /* 0xca: (key id, 40 wrapped bytes) */
    std::string package_full_name;
    bool ok = false;
};

static unsigned get_u16(const std::string &s, size_t o) { return (unsigned char)s[o] | (unsigned char)s[o + 1] << 8; }
static unsigned get_u32(const std::string &s, size_t o) { return get_u16(s, o) | get_u16(s, o + 2) << 16; }

static SpLicense parse_splicense(const std::string &blob)
{
    SpLicense l;
    size_t pos = 8;
    if (blob.size() < 8) return l;
    while (pos + 8 <= blob.size())
    {
        unsigned id = get_u32(blob, pos), size = get_u32(blob, pos + 4);
        pos += 8;
        if (pos + size > blob.size()) return l;
        std::string v = blob.substr(pos, size);
        pos += size;
        if (id == 0x12d) l.sign_state = v;
        else if (id == 0x01) l.device_key = v;
        else if (id == 0xce)
        {
            std::wstring w((const wchar_t *)v.data(), v.size() / 2);
            l.package_full_name = narrow(w.c_str(), w.size());
        }
        else if (id == 0xca)
        {
            size_t p = 0;
            while (p + 4 <= v.size())
            {
                unsigned idlen = get_u16(v, p), keylen = get_u16(v, p + 2);
                if (idlen < 16 || p + 4 + idlen + keylen > v.size()) break;
                l.packed_keys.push_back({ v.substr(p + 4, 16), v.substr(p + 4 + idlen, keylen) });
                p += 4 + idlen + keylen;
            }
        }
    }
    l.ok = pos == blob.size();
    return l;
}

/* ClepSignState, ClepHmacState and EncryptedDeviceKey share one 4096-byte shape: AES-128-CBC, zero IV, under a key
 * read out of an expanded key schedule stored next to the data (4 words, each two schedule words and constants). */
static std::string clep_key(const std::string &state, size_t schedule)
{
    unsigned k[4];
    auto ks = [&](int i) { return get_u32(state, schedule + 4 * i); };
    k[0] = ks(46) ^ ks(56) ^ 0xE20DF371 ^ 0xCCB22FE6;
    k[1] = ks(36) ^ ks(47) ^ 0xDF080E39;
    k[2] = ks(40) ^ ks(51) ^ 0x6D09B2F5 ^ 0x2AE17AB9;
    k[3] = ks(30) ^ ks(41) ^ 0x37288CEC;
    return std::string((char *)k, 16);
}

static bool clep_decrypt(const std::string &state, size_t version_at, size_t data_at, size_t data_len, size_t schedule,
                         std::string &out)
{
    if (state.size() != 4096 || get_u32(state, version_at) != 4) return false;
    out = state.substr(data_at, data_len);
    return aes_raw(clep_key(state, schedule), std::string(16, '\0'), true, out);
}

static bool clep_sign_key(const std::string &state, std::string &rsa_blob)
{ return clep_decrypt(state, 0, 4, 544, 548, rsa_blob); }

static bool clep_hmac_secret(const std::string &state, std::string &secret)
{
    std::string plain;
    if (!clep_decrypt(state, 0, 4, 48, 52, plain)) return false;
    secret = plain.substr(12, 32);
    return true;
}

static bool device_key_from(const std::string &state, std::string &key)
{
    if (state.size() != 4096 || get_u32(state, 2) != 4) return false;
    key = state.substr(518, 16);
    if (!aes_raw(clep_key(state, 6), std::string(16, '\0'), true, key)) return false;
    return key == clep_key(state, 6);   /* the block carries its own key: a check that it decoded */
}

/* -- the hardware challenge (components 8196/8197): a fixed, keyless Feistel obfuscation over 8-byte blocks -- */

namespace clep {
static const unsigned M_HI = 0x24181621, M_LO = 0x41393243, M1 = 0x2418, M2 = 0x1621, M3 = 0x4139, M4 = 0x3243;
static unsigned rotr(unsigned x, int n) { return x >> n | x << (32 - n); }
static unsigned rotl(unsigned x, int n) { return x << n | x >> (32 - n); }
static unsigned f(int r, unsigned x)
{
    switch (r)
    {
    case 1: return M4 * rotr(x ^ M_HI, 22) - rotr(x, 8);
    case 2: return M4 * (rotr(x, 15) ^ M1);
    case 3: return (x >> 9) + M2 * rotl(x ^ M3, 3);
    case 4: return rotr(x, 28) ^ M3 * rotr(x ^ M_HI, 9);
    case 5: return rotr(x, 12) + M4 * rotr(x - M_HI, 14);
    case 6: return rotr(x, 11) ^ M1 * rotl(x ^ M2, 2);
    case 7: return x - M_LO - M2;
    default: return M3 * rotl(x ^ M1, 2) - rotr(x, 18);
    }
}
static unsigned seed(unsigned x) { return M4 * rotr(x - M_HI, 18) - rotr(x, 9); }
static unsigned whiten(unsigned x) { return M3 * rotr(x + M_HI, 10) - rotr(x, 29); }
static unsigned initial_state(void)
{
    unsigned a = 0, b = ~(M3 * rotr(M_HI, 10));
    for (int r = 1; r <= 8; r++) { unsigned t = a ^ f(r, b); a = b; b = t; }
    return b;
}
static void obfuscate(unsigned char buf[2048])
{
    unsigned iv = buf[4] | buf[5] << 8 | buf[6] << 16 | (unsigned)buf[7] << 24;
    unsigned lo = initial_state() ^ iv, hi = 0, plo = iv, phi = 0;
    memcpy(buf + 4, &lo, 4);
    for (int blk = 1; blk < 256; blk++)
    {
        unsigned char *p = buf + 8 * blk;
        unsigned blo, bhi;
        memcpy(&blo, p, 4); memcpy(&bhi, p + 4, 4);
        unsigned a = lo ^ blo, b = hi ^ bhi ^ seed(a);
        for (int r = 8; r >= 1; r--) { unsigned t = b ^ f(r, a); b = a; a = t; }   /* (a,b) -> (b ^ f(a), a) */
        unsigned nlo = plo ^ a ^ whiten(b), nhi = b ^ phi;
        lo = nlo; hi = nhi; plo = blo; phi = bhi;
        memcpy(p, &nlo, 4); memcpy(p + 4, &nhi, 4);
    }
}
}

/* -- the device: credentials, a synthetic SMBIOS entry, the licence and its tokens -- */

struct WinDevice
{
    std::string member, password, puid, license;       /* license: the SPLicenseBlock, raw */
    std::string smbios;                                /* our synthetic SMBIOS type 1 entry, raw */
    std::string sts_token, sts_secret;                 /* the device STS token (EncryptedData XML) and its proof key */
    ULONGLONG sts_expires = 0;
};
static WinDevice windev;
static CRITICAL_SECTION windev_cs;

static void windev_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        InitializeCriticalSection(&windev_cs);
        return TRUE;
    }, nullptr, nullptr);
}

static std::string random_alnum(size_t n)
{
    static const char t[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[64];
    std::string s;
    random_bytes(r, n);
    for (size_t i = 0; i < n; i++) s += t[r[i] % 62];
    return s;
}

/* An SMBIOS 3.x type 1 (system information) structure with its strings. */
static std::string make_smbios(void)
{
    unsigned char uuid[16], serial[8];
    char hex[17];
    random_bytes(uuid, 16);
    random_bytes(serial, 8);
    for (int i = 0; i < 8; i++) sprintf(hex + 2 * i, "%02X", serial[i]);
    std::string s;
    s += (char)1; s += (char)0x1b; s += std::string("\0\1", 2);   /* type 1, length 27, handle 0x0100 */
    s += (char)1; s += (char)2; s += (char)3; s += (char)4;          /* manufacturer, product, version, serial */
    s.append((char *)uuid, 16);
    s += (char)6; s += (char)5; s += (char)6;                          /* wake-up: power switch; SKU; family */
    s += "Wine"; s += '\0'; s += "wine-uwp"; s += '\0'; s += "1.0"; s += '\0'; s += hex; s += '\0';
    s += "wine-uwp"; s += '\0'; s += "Wine"; s += '\0'; s += '\0';
    return s;
}

static std::string smbios_string(const std::string &e, int index)
{
    size_t p = (unsigned char)e[1];
    for (int i = 1; p < e.size(); i++)
    {
        size_t z = e.find('\0', p);
        if (z == std::string::npos || z == p) break;
        if (i == index) return e.substr(p, z - p);
        p = z + 1;
    }
    return "";
}

static void windev_load(void)
{
    auto kv = store_read();
    windev.member = kv["windev_member"];
    windev.password = kv["windev_password"];
    windev.puid = kv["windev_puid"];
    windev.license = unb64(kv["windev_license"]);
    windev.smbios = unb64(kv["windev_smbios"]);
    windev.sts_token = unb64(kv["windev_sts_token"]);
    windev.sts_secret = unb64(kv["windev_sts_secret"]);
    windev.sts_expires = _strtoui64(kv["windev_sts_expires"].c_str(), nullptr, 10);
}

static void windev_save(void)
{
    store_update({ { "windev_member", windev.member }, { "windev_password", windev.password }, { "windev_puid", windev.puid },
                   { "windev_license", b64((const unsigned char *)windev.license.data(), windev.license.size(), false) },
                   { "windev_smbios", b64((const unsigned char *)windev.smbios.data(), windev.smbios.size(), false) },
                   { "windev_sts_token", b64((const unsigned char *)windev.sts_token.data(), windev.sts_token.size(), false) },
                   { "windev_sts_secret", b64((const unsigned char *)windev.sts_secret.data(), windev.sts_secret.size(), false) },
                   { "windev_sts_expires", std::to_string(windev.sts_expires) } });
}

/* -- minimal XML reading: the first element with a local name, at or after a position -- */

struct XmlElem { size_t start = std::string::npos, inner = 0, inner_end = 0, end = 0; };

static XmlElem xml_find(const std::string &x, const std::string &local, size_t from = 0)
{
    XmlElem e;
    for (size_t p = x.find('<', from); p != std::string::npos; p = x.find('<', p + 1))
    {
        if (x[p + 1] == '/' || x[p + 1] == '?' || x[p + 1] == '!') continue;
        size_t q = p + 1;
        while (q < x.size() && !strchr(" \t\r\n/>", x[q])) q++;
        std::string name = x.substr(p + 1, q - p - 1);
        size_t colon = name.find(':');
        if ((colon == std::string::npos ? name : name.substr(colon + 1)) != local) continue;
        size_t gt = x.find('>', q);
        if (gt == std::string::npos) return e;
        e.start = p;
        if (x[gt - 1] == '/') { e.inner = e.inner_end = gt; e.end = gt + 1; return e; }
        e.inner = gt + 1;
        /* the matching end tag: nesting of the same name is counted */
        int depth = 1;
        size_t s = e.inner;
        while (depth)
        {
            size_t o = x.find("<" + name, s), c = x.find("</" + name + ">", s);
            if (c == std::string::npos) { e.start = std::string::npos; return e; }
            if (o != std::string::npos && o < c && strchr(" \t\r\n/>", x[o + 1 + name.size()]))
            {
                size_t og = x.find('>', o);
                if (x[og - 1] != '/') depth++;
                s = og + 1;
                continue;
            }
            depth--;
            if (!depth) { e.inner_end = c; e.end = c + name.size() + 3; }
            s = c + name.size() + 3;
        }
        return e;
    }
    return e;
}

static std::string xml_text(const std::string &x, const std::string &local, size_t from = 0)
{
    XmlElem e = xml_find(x, local, from);
    return e.start == std::string::npos ? "" : x.substr(e.inner, e.inner_end - e.inner);
}

static std::string xml_attr(const std::string &x, const XmlElem &e, const std::string &attr)
{
    if (e.start == std::string::npos) return "";
    std::string tag = x.substr(e.start, x.find('>', e.start) - e.start);
    for (size_t p = tag.find(attr + "=\""); p != std::string::npos; p = tag.find(attr + "=\"", p + 1))
    {
        if (p && tag[p - 1] != ' ' && tag[p - 1] != ':') continue;
        size_t v = p + attr.size() + 2;
        return tag.substr(v, tag.find('"', v) - v);
    }
    return "";
}

static std::string xml_unescape(std::string s)
{
    static const std::pair<const char *, char> ents[] = { { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
                                                          { "&amp;", '&' } };
    for (auto &en : ents)
        for (size_t p = s.find(en.first); p != std::string::npos; p = s.find(en.first, p + 1))
            s.replace(p, strlen(en.first), 1, en.second);
    return s;
}

static std::string xml_escape(const std::string &s)
{
    std::string o;
    for (char c : s)
        o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    return o;
}

/* -- deviceaddcredential.srf -- */

static std::string component(int name, const std::string &value)
{
    return "<Component name=\"" + std::to_string(name) + "\">" + b64((const unsigned char *)value.data(), value.size(), false) +
           "</Component>";
}

static std::string component_error(int name)
{
    return "<Component name=\"" + std::to_string(name) + "\" error=\"-2147024894\"></Component>";
}

static HRESULT windev_provision(void)
{
    unsigned char v2[2048] = {}, v4[2048] = {};
    windev.member = "02" + random_alnum(14);
    windev.password = random_alnum(20);
    if (windev.smbios.empty()) windev.smbios = make_smbios();

    /* ClepV2 / ClepV4: version, the SMBIOS entry (256 bytes), the disk serial (64, ours empty), a few flags */
    v2[0] = 2;
    memcpy(v2 + 4, windev.smbios.data(), min(windev.smbios.size(), (size_t)256));
    v2[328] = 1;                                                   /* always set */
    v4[0] = 4;
    memcpy(v4 + 4, windev.smbios.data(), min(windev.smbios.size(), (size_t)256));
    v4[1231] = 1;                                                  /* no debugger */
    clep::obfuscate(v2);
    clep::obfuscate(v4);

    std::string body = "<DeviceAddRequest><ClientInfo name=\"IDCRL\" version=\"1.0\"><BinaryVersion>55</BinaryVersion></ClientInfo>"
        "<Authentication><Membername>" + windev.member + "</Membername><Password>" + windev.password +
        "</Password></Authentication><DeviceInfo Id=\"DeviceInfo\">" +
        component(4113, std::string(1, '\0')) +
        component(8196, std::string((char *)v2, sizeof(v2))) +
        component(8197, std::string((char *)v4, sizeof(v4))) +
        component(4100, smbios_string(windev.smbios, 3)) +
        component(4101, smbios_string(windev.smbios, 4)) +
        component(4102, windev.smbios.substr(8, 16)) +
        component(4145, std::string("\1\0\0\0", 4)) +
        component_error(4160) + component_error(4161) +
        "</DeviceInfo></DeviceAddRequest>";
    std::string resp;
    DWORD status = http_request(L"POST", "https://login.live.com/ppsecure/deviceaddcredential.srf",
                                { MSA_UA, "Content-Type: application/soap+xml" }, body, resp);
    XmlElem root = xml_find(resp, "DeviceAddResponse");
    std::string ok = xml_attr(resp, root, "Success");
    std::string block = xml_text(resp, "SPLicenseBlock");
    trace("windev: deviceaddcredential HTTP %lu, Success \"%s\", licence %zu chars", status, ok.c_str(), block.size());
    if (status != 200 || block.empty())
    {
        trace("windev: deviceaddcredential answered: %.400s", resp.c_str());
        return XA_E_NETWORK;
    }
    windev.license = unb64(block);
    windev.puid = xml_text(resp, "puid");
    windev.sts_token.clear();
    windev.sts_secret.clear();
    windev.sts_expires = 0;
    SpLicense l = parse_splicense(windev.license);
    trace("windev: device licence %zu bytes, parsed %d, sign state %zu, device key %zu", windev.license.size(), l.ok,
          l.sign_state.size(), l.device_key.size());
    windev_save();
    return l.sign_state.size() == 4096 ? S_OK : E_FAIL;
}

/* -- RST2.srf -- */

static std::string iso_time(ULONGLONG t)
{
    time_t tt = (time_t)t;
    struct tm g;
    char b[32];
    gmtime_s(&g, &tt);
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

/* Every element that is signed is written here exactly as exclusive c14n renders it on its own: no whitespace,
 * start and end tags, its namespace declarations on it, attributes in order. So its bytes are its digest input. */
static std::string rst_auth_info(const std::string &hosting_app, bool sso_restricted, bool key_version)
{
    return std::string("<ps:AuthInfo xmlns:ps=\"") + NS_PS + "\" Id=\"PPAuthInfo\">" +
        (sso_restricted ? "<ps:SSOFlags>SsoRestr</ps:SSOFlags>" : "") +
        "<ps:HostingApp>" + hosting_app + "</ps:HostingApp><ps:BinaryVersion>55</ps:BinaryVersion><ps:UIVersion>1</ps:UIVersion>"
        "<ps:InlineUX>TokenBroker</ps:InlineUX><ps:IsAdmin>1</ps:IsAdmin><ps:Cookies></ps:Cookies>"
        "<ps:RequestParams>AQAAAAIAAABsYwQAAAAxMDMz</ps:RequestParams>"
        "<ps:WindowsClientString>b4d/QB7Zy5pjUAY9ByQ1echTyTITx6ZCErOEztuIVtw=</ps:WindowsClientString>" +
        (key_version ? "<ps:LicenseSignatureKeyVersion>2</ps:LicenseSignatureKeyVersion>" : "") +
        "<ps:ClientCapabilities>1</ps:ClientCapabilities></ps:AuthInfo>";
}

static std::string rst_timestamp(ULONGLONG now)
{
    return std::string("<wsu:Timestamp xmlns:wsu=\"") + NS_WSU + "\" wsu:Id=\"Timestamp\"><wsu:Created>" + iso_time(now) +
        "</wsu:Created><wsu:Expires>" + iso_time(now + 300) + "</wsu:Expires></wsu:Timestamp>";
}

static std::string rst_request(const std::string &scope, const std::string &policy)
{
    return std::string("<wst:RequestSecurityToken xmlns:wst=\"") + NS_WST + "\" Id=\"RST0\">"
        "<wst:RequestType>http://schemas.xmlsoap.org/ws/2005/02/trust/Issue</wst:RequestType>"
        "<wsp:AppliesTo xmlns:wsp=\"" + NS_WSP + "\"><wsa:EndpointReference xmlns:wsa=\"" + NS_WSA + "\"><wsa:Address>" +
        xml_escape(scope) + "</wsa:Address></wsa:EndpointReference></wsp:AppliesTo>" +
        (policy.empty() ? "" : "<wsp:PolicyReference xmlns:wsp=\"" + std::string(NS_WSP) + "\" URI=\"" + policy +
                               "\"></wsp:PolicyReference>") +
        "</wst:RequestSecurityToken>";
}

static std::string digest_b64(const std::string &s)
{
    unsigned char h[32];
    sha256(s, h);
    return b64(h, 32, false);
}

static std::string signed_info(const char *method, const std::string &rst, const std::string &ts, const std::string &ai)
{
    std::string si = std::string("<SignedInfo xmlns=\"") + NS_DSIG + "\"><CanonicalizationMethod Algorithm=\"" + EXC_C14N +
        "\"></CanonicalizationMethod><SignatureMethod Algorithm=\"" + method + "\"></SignatureMethod>";
    const std::pair<const char *, const std::string *> refs[] = { { "#RST0", &rst }, { "#Timestamp", &ts }, { "#PPAuthInfo", &ai } };
    for (auto &r : refs)
        si += std::string("<Reference URI=\"") + r.first + "\"><Transforms><Transform Algorithm=\"" + EXC_C14N +
              "\"></Transform></Transforms><DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\"></DigestMethod>"
              "<DigestValue>" + digest_b64(*r.second) + "</DigestValue></Reference>";
    return si + "</SignedInfo>";
}

static std::string rst_envelope(const std::string &auth_info, const std::string &security, const std::string &rst)
{
    return std::string("<s:Envelope xmlns:s=\"") + NS_S + "\" xmlns:ps=\"" + NS_PS + "\" xmlns:wsse=\"" + NS_WSSE +
        "\" xmlns:wsu=\"" + NS_WSU + "\" xmlns:wsp=\"" + NS_WSP + "\" xmlns:wsa=\"" + NS_WSA + "\" xmlns:wssc=\"" + NS_WSSC +
        "\" xmlns:wst=\"" + NS_WST + "\"><s:Header>"
        "<wsa:Action s:mustUnderstand=\"1\">http://schemas.xmlsoap.org/ws/2005/02/trust/RST/Issue</wsa:Action>"
        "<wsa:To s:mustUnderstand=\"1\">https://login.live.com:443/RST2.srf</wsa:To>"
        "<wsa:MessageID>" + std::to_string(now_s()) + "</wsa:MessageID>" + auth_info +
        "<wsse:Security>" + security + "</wsse:Security></s:Header><s:Body>" + rst + "</s:Body></s:Envelope>";
}

static DWORD rst2_post(const std::string &xml, std::string &resp)
{
    return http_request(L"POST", "https://login.live.com/RST2.srf", { MSA_UA, "Content-Type: application/soap+xml" }, xml, resp);
}

static void rst2_log_fault(const char *what, DWORD status, const std::string &resp)
{
    std::string code = xml_text(resp, "Value"), reason = xml_text(resp, "Text"), req = xml_text(resp, "reqstatus"),
                err = xml_text(resp, "error");
    trace("windev: %s: HTTP %lu, fault %s \"%s\", reqstatus %s, error %s", what, status, code.c_str(), reason.c_str(),
          req.c_str(), err.c_str());
}

static bool rsa_sign_sha256(const std::string &blob, const std::string &data, std::string &sig)
{
    if (blob.size() < 24) return false;
    unsigned magic = get_u32(blob, 0), e = get_u32(blob, 8), n = get_u32(blob, 12), p1 = get_u32(blob, 16), p2 = get_u32(blob, 20);
    size_t len = 24 + e + n + p1 + p2;
    const wchar_t *type = BCRYPT_RSAPRIVATE_BLOB;
    if (magic == BCRYPT_RSAFULLPRIVATE_MAGIC) { len += p1 + p2 + p1 + n; type = BCRYPT_RSAFULLPRIVATE_BLOB; }
    else if (magic != BCRYPT_RSAPRIVATE_MAGIC) return false;
    if (len > blob.size()) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    unsigned char h[32];
    sha256(data, h);
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0))) return false;
    if (BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, type, &key, (PUCHAR)blob.data(), (ULONG)len, 0)))
    {
        BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
        ULONG size = 0;
        sig.assign(n, '\0');
        ok = BCRYPT_SUCCESS(BCryptSignHash(key, &pad, h, 32, (PUCHAR)&sig[0], (ULONG)sig.size(), &size, BCRYPT_PAD_PKCS1)) &&
             size == n;
        BCryptDestroyKey(key);
    }
    else trace("windev: the device key does not import (magic %#x, %zu bytes)", magic, len);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* The device STS token: RST2 signed with the device's RSA key, for http://Passport.NET/tb. */
static HRESULT windev_authenticate(void)
{
    SpLicense l = parse_splicense(windev.license);
    std::string rsa, sig;
    if (!clep_sign_key(l.sign_state, rsa)) { trace("windev: no usable sign state in the device licence"); return E_FAIL; }
    ULONGLONG now = now_s();
    std::string ai = rst_auth_info("{DF60E2DF-88AD-4526-AE21-83D130EF0F68}", false, true), ts = rst_timestamp(now),
                rst = rst_request("http://Passport.NET/tb", "");
    std::string si = signed_info("http://www.w3.org/2001/04/xmldsig-more#rsa-sha256", rst, ts, ai);
    bool signed_ok = rsa_sign_sha256(rsa, si, sig);
    SecureZeroMemory(&rsa[0], rsa.size());
    if (!signed_ok) { trace("windev: signing with the device key failed"); return E_FAIL; }
    std::string security = "<wsse:UsernameToken wsu:Id=\"devicesoftware\"><wsse:Username>" + windev.member +
        "</wsse:Username></wsse:UsernameToken>" + ts + "<Signature xmlns=\"" + NS_DSIG + "\">" + si.substr(si.find('>') + 1).insert(0, "<SignedInfo>") +
        "<SignatureValue>" + b64((const unsigned char *)sig.data(), sig.size(), false) + "</SignatureValue></Signature>";
    std::string resp;
    DWORD status = rst2_post(rst_envelope(ai, security, rst), resp);
    XmlElem rstr = xml_find(resp, "RequestSecurityTokenResponse");
    XmlElem enc = rstr.start == std::string::npos ? XmlElem() : xml_find(resp, "EncryptedData", rstr.inner);
    std::string secret = xml_text(resp, "BinarySecret", rstr.start == std::string::npos ? 0 : rstr.inner);
    if (status != 200 || enc.start == std::string::npos || secret.empty())
    {
        rst2_log_fault("device authentication", status, resp);
        return XA_E_NETWORK;
    }
    windev.sts_token = resp.substr(enc.start, enc.end - enc.start);
    windev.sts_secret = unb64(secret);
    windev.sts_expires = parse_iso(xml_text(resp, "Expires", rstr.inner));
    trace("windev: device STS token %zu chars, proof key %zu bytes, expires in %lld s", windev.sts_token.size(),
          windev.sts_secret.size(), (long long)(windev.sts_expires - now));
    windev_save();
    return windev.sts_secret.size() == 4096 ? S_OK : E_FAIL;
}

/* An RST2 request with the device STS token, HMAC-signed: a token for another scope. */
static HRESULT windev_exchange(const std::string &hosting_app, const std::string &scope, const std::string &policy,
                               std::string &token)
{
    std::string secret, nonce(32, '\0');
    if (!clep_hmac_secret(windev.sts_secret, secret)) { trace("windev: no usable proof key"); return E_FAIL; }
    random_bytes(&nonce[0], nonce.size());
    std::string key = sp800108_key(secret, nonce);
    ULONGLONG now = now_s();
    std::string ai = rst_auth_info(hosting_app, true, true), ts = rst_timestamp(now), rst = rst_request(scope, policy);
    std::string si = signed_info("http://www.w3.org/2001/04/xmldsig-more#hmac-sha256", rst, ts, ai);
    std::string mac = hmac_sha256(key, si);
    std::string security = windev.sts_token +
        "<wssc:DerivedKeyToken wsu:Id=\"SignKey\" Algorithm=\"urn:liveid:SP800108_CTR_HMAC_SHA256_DOUBLEDERIVED\">"
        "<wsse:RequestedTokenReference><wsse:KeyIdentifier ValueType=\"http://docs.oasis-open.org/wss/2004/XX/oasis-2004XX-wss-saml-token-profile-1.0#SAMLAssertionID\"></wsse:KeyIdentifier>"
        "<wsse:Reference URI=\"\"></wsse:Reference></wsse:RequestedTokenReference><wssc:Nonce>" +
        b64((const unsigned char *)nonce.data(), nonce.size(), false) + "</wssc:Nonce></wssc:DerivedKeyToken>" + ts +
        "<Signature xmlns=\"" + NS_DSIG + "\">" + si.substr(si.find('>') + 1).insert(0, "<SignedInfo>") + "<SignatureValue>" +
        b64((const unsigned char *)mac.data(), mac.size(), false) + "</SignatureValue><KeyInfo><wsse:SecurityTokenReference>"
        "<wsse:Reference URI=\"#SignKey\"></wsse:Reference></wsse:SecurityTokenReference></KeyInfo></Signature>";
    std::string resp;
    DWORD status = rst2_post(rst_envelope(ai, security, rst), resp);

    /* the body comes back encrypted (AES-256-CBC, the IV first) under a key derived from a nonce the server picked */
    XmlElem body = xml_find(resp, "Body");
    XmlElem enc = body.start == std::string::npos ? XmlElem() : xml_find(resp, "EncryptedData", body.inner);
    if (status != 200 || enc.start == std::string::npos)
    {
        rst2_log_fault(("exchange for " + scope).c_str(), status, resp);
        return XA_E_NETWORK;
    }
    XmlElem ref = xml_find(resp, "Reference", enc.inner);
    std::string id = xml_attr(resp, ref, "URI");
    std::string server_nonce;
    for (XmlElem d = xml_find(resp, "DerivedKeyToken"); d.start != std::string::npos; d = xml_find(resp, "DerivedKeyToken", d.end))
        if ("#" + xml_attr(resp, d, "Id") == id) server_nonce = unb64(xml_text(resp, "Nonce", d.inner));
    std::string cipher = unb64(xml_text(resp, "CipherValue", enc.inner));
    if (server_nonce.empty() || cipher.size() < 32 || cipher.size() % 16)
    {
        trace("windev: exchange answer not understood (key %s, %zu cipher bytes)", id.c_str(), cipher.size());
        return E_FAIL;
    }
    std::string plain = cipher.substr(16);
    if (!aes_raw(sp800108_key(secret, server_nonce), cipher.substr(0, 16), true, plain) || plain.empty() ||
        (unsigned char)plain.back() > 16)
    {
        trace("windev: the exchange answer does not decrypt");
        return E_FAIL;
    }
    plain.resize(plain.size() - (unsigned char)plain.back());
    std::string bst = xml_text(plain, "BinarySecurityToken");
    if (bst.empty())
    {
        rst2_log_fault(("exchange for " + scope + " (decrypted)").c_str(), status, plain);
        return E_FAIL;
    }
    token = xml_unescape(bst);
    trace("windev: %s ticket, %zu chars", scope.c_str(), token.size());
    return S_OK;
}

/* Provision the device once, and keep its STS token fresh. */
static HRESULT windev_ready(void)
{
    HRESULT hr = S_OK;
    windev_init();
    EnterCriticalSection(&windev_cs);
    if (windev.license.empty()) windev_load();
    if (windev.license.empty() && FAILED(hr = windev_provision())) goto done;
    if (windev.sts_token.empty() || windev.sts_expires < now_s() + 600)
    {
        hr = windev_authenticate();
        if (FAILED(hr) && hr != XA_E_NETWORK)
        {
            /* a licence the service no longer knows: start over with a new device */
            trace("windev: re-provisioning");
            windev.license.clear();
            if (SUCCEEDED(hr = windev_provision())) hr = windev_authenticate();
        }
    }
done:
    LeaveCriticalSection(&windev_cs);
    return hr;
}

/* -- licensing.mp.microsoft.com: the content keys of a package this account is entitled to -- */

struct ContentKey { std::string id, key; };   /* id: the 16-byte key id (GUID bytes); key: 32 bytes */

static HRESULT content_keys(const std::string &content_id, const std::string &market, std::vector<ContentKey> &keys,
                            std::string *package_full_name)
{
    std::string device_ticket, user_ticket, resp;
    HRESULT hr;
    if (FAILED(hr = windev_ready())) return hr;
    EnterCriticalSection(&windev_cs);
    hr = windev_exchange("{d6d5a677-0872-4ab0-9442-bb792fce85c5}", "www.microsoft.com", "mbi_ssl", device_ticket);
    LeaveCriticalSection(&windev_cs);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&account.signin_cs);
    hr = msa_refresh("service::www.microsoft.com::MBI_SSL", &user_ticket);
    LeaveCriticalSection(&account.signin_cs);
    if (FAILED(hr)) return hr;
    std::string puid;
    EnterCriticalSection(&account.cs);
    puid = account.puid;
    LeaveCriticalSection(&account.cs);
    if (puid.empty()) puid = store_read()["puid"];

    static const char challenge[] = "<?xml version=\"1.0\" encoding=\"utf-8\" ?><ClientChallenge xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns=\"http://schemas.microsoft.com/onestore/security/mkms/LicReq/v1\" Version=\"2\"><LicenseProtocolVersion>5</LicenseProtocolVersion><SigningKeyVersion>1</SigningKeyVersion><ClientVersion>2</ClientVersion></ClientChallenge>";
    std::string body = "{\"clientChallenge\":\"" + b64((const unsigned char *)challenge, sizeof(challenge) - 1, false) +
        "\",\"concurrencyMode\":\"Rude\",\"contentId\":\"" + json_escape(content_id) +
        "\",\"deviceContext\":{\"hardwareManufacturer\":\"Public\",\"hardwareType\":\"Public\",\"mobileOperator\":\"Public\"},"
        "\"licenseVersion\":4,\"market\":\"" + json_escape(market) + "\",\"needKey\":true,\"keyOnly\":true,"
        "\"users\":{\"S-1-5-21-0000000000-0000000000-0000000000-1001\":[{\"identityType\":\"Msa\",\"identityValue\":\"" +
        json_escape(user_ticket) + "\",\"localTicketReference\":\"" + json_escape(puid) + "\"}]}}";
    DWORD status = http_request(L"POST", "https://licensing.mp.microsoft.com/v7.0/licenses/content",
                                { "Content-Type: application/json", "from: XboxLicenseManager", "Authorization: " + device_ticket,
                                  "User-Agent: XboxLm-PC/Microsoft.GamingServices_32.107.4002.0_x64__8wekyb3d8bbwe" },
                                body, resp);
    Json j = parse_json(resp);
    const Json &value = j["license"]["keys"][0]["value"];
    if (status != 200 || value.str().empty())
    {
        trace("windev: licence for %s: HTTP %lu: %.500s", content_id.c_str(), status, resp.c_str());
        return status == 200 || status == 403 || status == 400 ? XA_E_MSA : XA_E_NETWORK;
    }
    std::string xml = unb64(value.str());
    SpLicense game = parse_splicense(unb64(xml_text(xml, "SPLicenseBlock")));
    std::string kek;
    EnterCriticalSection(&windev_cs);
    bool have_kek = device_key_from(parse_splicense(windev.license).device_key, kek);
    LeaveCriticalSection(&windev_cs);
    if (!have_kek) { trace("windev: no device key in the device licence"); return E_FAIL; }
    keys.clear();
    for (auto &pk : game.packed_keys)
    {
        ContentKey k;
        k.id = pk.first;
        if (!aes_key_unwrap(kek, pk.second, k.key)) { trace("windev: a content key does not unwrap"); return E_FAIL; }
        keys.push_back(k);
    }
    SecureZeroMemory(&kek[0], kek.size());
    if (package_full_name) *package_full_name = game.package_full_name;
    trace("windev: licence for %s: %zu content keys, package %s", content_id.c_str(), keys.size(), game.package_full_name.c_str());
    return keys.empty() ? E_FAIL : S_OK;
}

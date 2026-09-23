// Windows.Security.Cryptography.Certificates.Certificate, backed by crypt32.
//
// Windows.Web.Http wraps every server certificate of every HTTPS response in one of these
// (CertificateHelper::ParseCertificateInformation -> ICertificateFactory::CreateCertificate), at
// INTERNET_STATUS_SENDING_REQUEST. The class lives in certenroll.dll, which this Windows image does
// not have, so RoGetActivationFactory failed and the client silently closed the WinINet handle:
//
//     trace:wininet:HTTPREQ_QueryOption INTERNET_OPTION_SERVER_CERT_CHAIN_CONTEXT
//     err:combase:RoGetActivationFactory Failed to find library for L"...Certificates.Certificate"
//     trace:wininet:InternetCloseHandle 0000000000000009
//
// - every HTTPS request made through HttpClient died there, with no error originated anywhere.
// That is what stopped Shapr3D's sign-in code exchange.
//
// The certificate is real: the DER is parsed by crypt32 and subject, issuer, validity, serial,
// hashes and blob are answered from it. Chain building and EKUs are not implemented and are traced,
// so a caller that wants them shows up in the log.
//
//   ICertificateFactory {17b4221c-4baf-44a2-9608-04fb62b16942}: 6 CreateCertificate(IBuffer)
//   ICertificate        {333f740c-04d8-43b3-b278-8c5fcc9be5a0}:
//      6,7 BuildChainAsync  8 SerialNumber  9 GetHashValue  10 GetHashValue(alg)
//      11 GetCertificateBlob  12 Subject  13 Issuer  14 HasPrivateKey  15 IsStronglyProtected
//      16 ValidFrom  17 ValidTo  18 EnhancedKeyUsages  19,20 FriendlyName
#include <windows.h>
#include <wincrypt.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <cstdio>
#include <cstdarg>

static const GUID IID_ICertificateFactory_ = {0x17b4221c,0x4baf,0x44a2,{0x96,0x08,0x04,0xfb,0x62,0xb1,0x69,0x42}};
static const GUID IID_ICertificate_        = {0x333f740c,0x04d8,0x43b3,{0xb2,0x78,0x8c,0x5f,0xcc,0x9b,0xe5,0xa0}};
static const GUID IID_IAgileObject_        = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_IBufferByteAccess_   = {0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_ICryptographicBufferStatics_ = {0x320b7e22,0x3cb0,0x4cdf,{0x86,0x63,0x1d,0x28,0x91,0x00,0x65,0xeb}};

static void trace(const char *fmt, ...)
{
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("WINE_UWP_TRACE_CERT", nullptr, 0) > 0;
    if (!on) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[certificate] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static void notimpl(const char *what)
{
    fprintf(stderr, "[certificate] %s: not implemented\n", what);
}


/* IBuffer: slot 6 get_Capacity, 7 get_Length. IBufferByteAccess: slot 3 Buffer. */
static HRESULT buffer_bytes(IUnknown *buffer, BYTE **data, UINT32 *len)
{
    IUnknown *access;
    void **vt = *(void ***)buffer;
    HRESULT hr;

    if (FAILED(hr = ((HRESULT (WINAPI *)(void *, UINT32 *))vt[7])(buffer, len))) return hr;
    if (FAILED(hr = buffer->QueryInterface(IID_IBufferByteAccess_, (void **)&access))) return hr;
    hr = ((HRESULT (WINAPI *)(void *, BYTE **))(*(void ***)access)[3])(access, data);
    access->Release();
    return hr;
}

/* A new IBuffer holding a copy of the bytes, from CryptographicBuffer (native CryptoWinRT). */
static HRESULT make_buffer(const BYTE *data, UINT32 len, IUnknown **out)
{
    static const WCHAR cls[] = L"Windows.Security.Cryptography.CryptographicBuffer";
    HSTRING_HEADER hdr;
    HSTRING name;
    IUnknown *statics;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateStringReference(cls, ARRAYSIZE(cls) - 1, &hdr, &name))) return hr;
    if (FAILED(hr = RoGetActivationFactory(name, IID_ICryptographicBufferStatics_, (void **)&statics))) return hr;
    /* ICryptographicBufferStatics slot 9: CreateFromByteArray(UINT32, BYTE *, IBuffer **) */
    hr = ((HRESULT (WINAPI *)(void *, UINT32, const BYTE *, IUnknown **))(*(void ***)statics)[9])
            (statics, len, data, out);
    statics->Release();
    return hr;
}

static HRESULT byte_array(const BYTE *data, DWORD len, UINT32 *out_len, BYTE **out)
{
    if (!out_len || !out) return E_POINTER;
    if (!(*out = (BYTE *)CoTaskMemAlloc(len ? len : 1))) return E_OUTOFMEMORY;
    memcpy(*out, data, len);
    *out_len = len;
    return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * Certificate
 * ------------------------------------------------------------------------------------------- */
struct Certificate
{
    const void **vtbl;
    LONG ref;
    PCCERT_CONTEXT ctx;
    HSTRING friendly_name;
};

static HRESULT WINAPI cert_QueryInterface(Certificate *c, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_ICertificate_))
    {
        *out = c;
        InterlockedIncrement(&c->ref);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

static ULONG WINAPI cert_AddRef(Certificate *c) { return InterlockedIncrement(&c->ref); }

static ULONG WINAPI cert_Release(Certificate *c)
{
    ULONG ref = InterlockedDecrement(&c->ref);
    if (!ref)
    {
        CertFreeCertificateContext(c->ctx);
        WindowsDeleteString(c->friendly_name);
        delete c;
    }
    return ref;
}

static HRESULT WINAPI cert_GetIids(Certificate *, ULONG *count, IID **iids)
{
    if (!(*iids = (IID *)CoTaskMemAlloc(sizeof(IID)))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_ICertificate_;
    *count = 1;
    return S_OK;
}

static HRESULT WINAPI cert_GetRuntimeClassName(Certificate *, HSTRING *name)
{
    static const WCHAR cls[] = L"Windows.Security.Cryptography.Certificates.Certificate";
    return WindowsCreateString(cls, ARRAYSIZE(cls) - 1, name);
}

static HRESULT WINAPI cert_GetTrustLevel(Certificate *, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI cert_BuildChainAsync(Certificate *, void *, void **out)
{
    notimpl("BuildChainAsync");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}

static HRESULT WINAPI cert_BuildChainWithParametersAsync(Certificate *, void *, void *, void **out)
{
    notimpl("BuildChainAsync(parameters)");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}

/* CERT_INFO keeps the serial little-endian; the WinRT property is the certificate's byte order. */
static HRESULT WINAPI cert_get_SerialNumber(Certificate *c, UINT32 *len, BYTE **value)
{
    const CRYPT_INTEGER_BLOB *serial = &c->ctx->pCertInfo->SerialNumber;
    HRESULT hr = byte_array(serial->pbData, serial->cbData, len, value);

    if (SUCCEEDED(hr))
        for (DWORD i = 0; i < serial->cbData / 2; i++)
        {
            BYTE t = (*value)[i];
            (*value)[i] = (*value)[serial->cbData - 1 - i];
            (*value)[serial->cbData - 1 - i] = t;
        }
    return hr;
}

static HRESULT hash_value(Certificate *c, DWORD prop, UINT32 *len, BYTE **value)
{
    BYTE hash[64];
    DWORD size = sizeof(hash);

    if (!CertGetCertificateContextProperty(c->ctx, prop, hash, &size))
        return HRESULT_FROM_WIN32(GetLastError());
    return byte_array(hash, size, len, value);
}

static HRESULT WINAPI cert_GetHashValue(Certificate *c, UINT32 *len, BYTE **value)
{
    return hash_value(c, CERT_SHA1_HASH_PROP_ID, len, value);
}

static HRESULT WINAPI cert_GetHashValueWithAlgorithm(Certificate *c, HSTRING alg, UINT32 *len, BYTE **value)
{
    const WCHAR *name = WindowsGetStringRawBuffer(alg, nullptr);

    if (!_wcsicmp(name, L"SHA1")) return hash_value(c, CERT_SHA1_HASH_PROP_ID, len, value);
    if (!_wcsicmp(name, L"SHA256")) return hash_value(c, CERT_SHA256_HASH_PROP_ID, len, value);
    if (!_wcsicmp(name, L"MD5")) return hash_value(c, CERT_MD5_HASH_PROP_ID, len, value);
    fprintf(stderr, "[certificate] GetHashValue(%ls): unsupported algorithm\n", name);
    return E_INVALIDARG;
}

static HRESULT WINAPI cert_GetCertificateBlob(Certificate *c, IUnknown **out)
{
    return make_buffer(c->ctx->pbCertEncoded, c->ctx->cbCertEncoded, out);
}

static HRESULT cert_name(Certificate *c, DWORD flags, HSTRING *out)
{
    WCHAR buf[512];
    DWORD n = CertGetNameStringW(c->ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buf, ARRAYSIZE(buf));
    return WindowsCreateString(buf, n ? n - 1 : 0, out);
}

static HRESULT WINAPI cert_get_Subject(Certificate *c, HSTRING *out) { return cert_name(c, 0, out); }
static HRESULT WINAPI cert_get_Issuer(Certificate *c, HSTRING *out) { return cert_name(c, CERT_NAME_ISSUER_FLAG, out); }

/* Built from a bare DER blob, so there is never a key behind it. */
static HRESULT WINAPI cert_get_HasPrivateKey(Certificate *, boolean *out) { *out = FALSE; return S_OK; }
static HRESULT WINAPI cert_get_IsStronglyProtected(Certificate *, boolean *out) { *out = FALSE; return S_OK; }

/* Windows.Foundation.DateTime is a FILETIME in 100 ns units. */
static HRESULT WINAPI cert_get_ValidFrom(Certificate *c, INT64 *out)
{
    *out = ((INT64)c->ctx->pCertInfo->NotBefore.dwHighDateTime << 32) | c->ctx->pCertInfo->NotBefore.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI cert_get_ValidTo(Certificate *c, INT64 *out)
{
    *out = ((INT64)c->ctx->pCertInfo->NotAfter.dwHighDateTime << 32) | c->ctx->pCertInfo->NotAfter.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI cert_get_EnhancedKeyUsages(Certificate *, void **out)
{
    notimpl("EnhancedKeyUsages");
    *out = nullptr;
    return E_NOTIMPL;
}

static HRESULT WINAPI cert_put_FriendlyName(Certificate *c, HSTRING value)
{
    HSTRING copy;
    HRESULT hr = WindowsDuplicateString(value, &copy);
    if (SUCCEEDED(hr)) { WindowsDeleteString(c->friendly_name); c->friendly_name = copy; }
    return hr;
}

static HRESULT WINAPI cert_get_FriendlyName(Certificate *c, HSTRING *out)
{
    return WindowsDuplicateString(c->friendly_name, out);
}

static const void *cert_vtbl[] =
{
    (void *)cert_QueryInterface, (void *)cert_AddRef, (void *)cert_Release,
    (void *)cert_GetIids, (void *)cert_GetRuntimeClassName, (void *)cert_GetTrustLevel,
    (void *)cert_BuildChainAsync, (void *)cert_BuildChainWithParametersAsync,
    (void *)cert_get_SerialNumber, (void *)cert_GetHashValue, (void *)cert_GetHashValueWithAlgorithm,
    (void *)cert_GetCertificateBlob, (void *)cert_get_Subject, (void *)cert_get_Issuer,
    (void *)cert_get_HasPrivateKey, (void *)cert_get_IsStronglyProtected,
    (void *)cert_get_ValidFrom, (void *)cert_get_ValidTo, (void *)cert_get_EnhancedKeyUsages,
    (void *)cert_put_FriendlyName, (void *)cert_get_FriendlyName,
};

/* ---------------------------------------------------------------------------------------------
 * The activation factory: IActivationFactory + ICertificateFactory, a static singleton.
 * ------------------------------------------------------------------------------------------- */
struct Factory
{
    const void **activation_vtbl;
    const void **factory_vtbl;
};
static Factory factory_obj;

static HRESULT factory_qi(REFIID iid, void **out)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IInspectable) ||
        IsEqualGUID(iid, IID_IAgileObject_) || IsEqualGUID(iid, IID_IActivationFactory))
        *out = &factory_obj.activation_vtbl;
    else if (IsEqualGUID(iid, IID_ICertificateFactory_))
        *out = &factory_obj.factory_vtbl;
    else
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT WINAPI f_QueryInterface(void *, REFIID iid, void **out) { return factory_qi(iid, out); }
static ULONG WINAPI f_AddRef(void *) { return 2; }
static ULONG WINAPI f_Release(void *) { return 1; }
static HRESULT WINAPI f_GetIids(void *, ULONG *count, IID **iids)
{
    if (!(*iids = (IID *)CoTaskMemAlloc(sizeof(IID)))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_ICertificateFactory_;
    *count = 1;
    return S_OK;
}
static HRESULT WINAPI f_GetRuntimeClassName(void *, HSTRING *name) { *name = nullptr; return E_NOTIMPL; }
static HRESULT WINAPI f_GetTrustLevel(void *, TrustLevel *level) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI f_ActivateInstance(void *, IInspectable **out)
{
    *out = nullptr;
    return E_NOTIMPL;  /* Certificate has no default constructor */
}

static HRESULT WINAPI f_CreateCertificate(void *, IUnknown *buffer, void **out)
{
    Certificate *c;
    PCCERT_CONTEXT ctx;
    BYTE *data;
    UINT32 len;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!buffer) return E_INVALIDARG;
    if (FAILED(hr = buffer_bytes(buffer, &data, &len))) return hr;
    if (!(ctx = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data, len)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        trace("CreateCertificate: %u bytes do not parse, hr %#lx", len, hr);
        return hr;
    }
    c = new Certificate{cert_vtbl, 1, ctx, nullptr};
    if (GetEnvironmentVariableA("WINE_UWP_TRACE_CERT", nullptr, 0))
    {
        WCHAR subject[256];
        CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, ARRAYSIZE(subject));
        trace("CreateCertificate: %u bytes, %ls -> %p", len, subject, c);
    }
    *out = c;
    return S_OK;
}

static const void *activation_vtbl[] =
{
    (void *)f_QueryInterface, (void *)f_AddRef, (void *)f_Release,
    (void *)f_GetIids, (void *)f_GetRuntimeClassName, (void *)f_GetTrustLevel,
    (void *)f_ActivateInstance,
};

static const void *certificate_factory_vtbl[] =
{
    (void *)f_QueryInterface, (void *)f_AddRef, (void *)f_Release,
    (void *)f_GetIids, (void *)f_GetRuntimeClassName, (void *)f_GetTrustLevel,
    (void *)f_CreateCertificate,
};

extern "C" __declspec(dllexport)
HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
    const WCHAR *cls = name ? WindowsGetStringRawBuffer(name, nullptr) : nullptr;

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!cls || wcscmp(cls, L"Windows.Security.Cryptography.Certificates.Certificate")) return REGDB_E_CLASSNOTREG;
    factory_obj.activation_vtbl = activation_vtbl;
    factory_obj.factory_vtbl = certificate_factory_vtbl;
    *out = (IActivationFactory *)&factory_obj.activation_vtbl;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

// Is the HTTP path a packaged app actually uses usable in this prefix?
//
// Solitaire holds on its splash screen waiting for a PlayFab login that never completes, and `ss`
// shows the process never opens a single TCP socket - while Wine's own NetworkListManager reports
// 0x660 (IPV4_INTERNET | IPV6_INTERNET | ...), i.e. fully online. So the request never leaves the
// process, and the question is which layer swallows it.
//
// The app's HTTP goes through libHttpClient (its .rdata carries
// .../libHttpClient/Source/HTTP/httpcall.cpp), whose UWP backend is **IXMLHTTPRequest2** - the
// RTTI shows IXMLHTTPRequest2Callback implemented via WRL. That interface comes from msxml6's
// CLSID_FreeThreadedXMLHTTP60, which native msxml6.dll provides but which is **not registered in
// this prefix**.
//
// This probe isolates that one layer: create the object, fire a GET, and report every step. Run it
// inside the same prefix as the app.
//
//   probe-http.exe [url]     default: https://www.microsoft.com/
//
// It reports the CoCreateInstance HRESULT separately from Open/Send, because "not registered" and
// "registered but never completes" are different problems with different fixes.
#include <windows.h>
#include <objbase.h>
#include <msxml6.h>
#include <initguid.h>
#include <cstdio>
#include <cstring>

/* The mingw import libraries here do not export these, so define them locally. Values are from
 * msxml6.h / the Windows SDK. */
static const GUID CLSID_FreeThreadedXMLHTTP60_ =
    {0x88d96a09,0xf192,0x11d4,{0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};
static const GUID IID_IXMLHTTPRequest2_ =
    {0xe5d37dc0,0x552a,0x4d52,{0x9c,0xc0,0xa1,0x4d,0x54,0x6f,0xbd,0x04}};
static const GUID IID_IXMLHTTPRequest2Callback_ =
    {0xa44a9299,0xe321,0x40de,{0x88,0x66,0x34,0x1b,0x41,0x66,0x91,0x62}};

#define REPORT(hr, what) \
    printf("  %-34s hr=%#010lx %s\n", what, (unsigned long)(hr), SUCCEEDED(hr) ? "ok" : "FAILED"); \
    fflush(stdout)

/* The callback is how IXMLHTTPRequest2 reports completion; a request that never calls it is the
 * symptom we are looking for. */
class Callback : public IXMLHTTPRequest2Callback
{
    LONG ref = 1;
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ULONG status = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IXMLHTTPRequest2Callback_))
        { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&ref); }

    HRESULT STDMETHODCALLTYPE OnRedirect(IXMLHTTPRequest2 *, const WCHAR *url) override
    { printf("  redirect -> %ls\n", url); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnHeadersAvailable(IXMLHTTPRequest2 *, DWORD code, const WCHAR *phrase) override
    { status = code; printf("  headers: HTTP %lu %ls\n", code, phrase ? phrase : L""); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(IXMLHTTPRequest2 *, ISequentialStream *) override
    { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnResponseReceived(IXMLHTTPRequest2 *, ISequentialStream *) override
    { result = S_OK; SetEvent(done); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnError(IXMLHTTPRequest2 *, HRESULT hr) override
    { result = hr; printf("  OnError hr=%#010lx\n", (unsigned long)hr); SetEvent(done); return S_OK; }
};

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : "https://www.microsoft.com/";
    WCHAR wurl[1024];
    MultiByteToWideChar(CP_ACP, 0, url, -1, wurl, ARRAYSIZE(wurl));

    /* argv[2]: apartment, "mta" (default) or "sta" - Solitaire's COM call comes from a UWP
     * apartment, and argv[3]: CLSCTX, because the app asks with 0x15
     * (INPROC_SERVER|LOCAL_SERVER|REMOTE_SERVER) rather than a bare INPROC_SERVER. Both are
     * candidate explanations for the class resolving standalone but not inside the app. */
    const bool sta = argc > 2 && !strcmp(argv[2], "sta");
    const DWORD clsctx = argc > 3 ? (DWORD)strtoul(argv[3], nullptr, 0) : CLSCTX_INPROC_SERVER;

    printf("probe-http: %s  apartment=%s clsctx=%#lx\n", url, sta ? "sta" : "mta",
           (unsigned long)clsctx);
    HRESULT hr = CoInitializeEx(nullptr, sta ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED);
    REPORT(hr, "CoInitializeEx");

    IXMLHTTPRequest2 *req = nullptr;
    hr = CoCreateInstance(CLSID_FreeThreadedXMLHTTP60_, nullptr, clsctx,
                          IID_IXMLHTTPRequest2_, (void **)&req);
    REPORT(hr, "CoCreateInstance(XMLHTTP60)");
    if (FAILED(hr))
    {
        printf("  -> this is the layer that is missing: libHttpClient's UWP backend cannot make a\n"
               "     single request without it, which is consistent with the app opening no socket.\n");
        return 1;
    }

    Callback cb;
    hr = req->Open(L"GET", wurl, &cb, nullptr, nullptr, nullptr, nullptr);
    REPORT(hr, "Open");
    if (SUCCEEDED(hr))
    {
        hr = req->Send(nullptr, 0);
        REPORT(hr, "Send");
        if (SUCCEEDED(hr))
        {
            DWORD w = WaitForSingleObject(cb.done, 20000);
            if (w == WAIT_TIMEOUT)
                printf("  -> Send returned but the callback NEVER fired (20 s). Registered but\n"
                       "     non-functional: exactly the shape of the app's hang.\n");
            else
                printf("  callback fired: result=%#010lx status=%lu\n",
                       (unsigned long)cb.result, cb.status);
        }
    }
    req->Release();
    CoUninitialize();
    return 0;
}

// dmprocessxmlfiltered.dll: DMProcessConfigXMLFiltered, the one function the Microsoft Store imports.
//
// WinStore.App.dll imports it statically to read device-management (MDM) policies through OMA-DM SyncML, such as
// whether only the private store is allowed. Windows' DLL drags in the whole enrolment stack (dmcmnutils, dsreg,
// Azure attestation, TPM provisioning), none of which can work in a prefix, and without the DLL the Store does not
// load at all (c0000135). A machine with no management enrolment has no such policies, so every Get is answered
// with SyncML status 404 (not found), which is what Windows reports for a policy that was never set.
//
// HRESULT DMProcessConfigXMLFiltered(LPCWSTR pszXmlIn, const WCHAR **rgszAllowedCspNode, DWORD dwNumAllowedCspNodes,
//                                    BSTR *pbstrXmlOut)   (public, mdmregistration / dmprocessxmlfiltered.h)
#include <windows.h>
#include <oleauto.h>
#include <cstdio>
#include <string>

static std::wstring between(const std::wstring &s, size_t from, const wchar_t *open, const wchar_t *close, size_t *end)
{
    size_t a = s.find(open, from);
    if (a == std::wstring::npos) return {};
    a += wcslen(open);
    size_t b = s.find(close, a);
    if (b == std::wstring::npos) return {};
    if (end) *end = b;
    return s.substr(a, b - a);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DMProcessConfigXMLFiltered(LPCWSTR in, const WCHAR **allowed,
                                                                             DWORD allowed_count, BSTR *out)
{
    (void)allowed; (void)allowed_count;
    if (!in || !out) return E_INVALIDARG;
    *out = nullptr;
    std::wstring xml(in);
    fprintf(stderr, "[dmprocessxml] request: %ls\n", in);

    std::wstring msgid = between(xml, 0, L"<MsgID>", L"</MsgID>", nullptr);
    if (msgid.empty()) msgid = L"1";
    std::wstring body;
    size_t pos = 0, end = 0;
    for (;;)
    {
        size_t get = xml.find(L"<Get>", pos);
        if (get == std::wstring::npos) break;
        std::wstring cmd = between(xml, get, L"<CmdID>", L"</CmdID>", &end);
        body += L"<Status><CmdID>" + std::to_wstring(body.size() + 1) + L"</CmdID><MsgRef>" + msgid +
                L"</MsgRef><CmdRef>" + cmd + L"</CmdRef><Cmd>Get</Cmd><Data>404</Data></Status>";
        pos = get + 5;
    }
    std::wstring reply = L"<SyncML xmlns=\"SYNCML:SYNCML1.2\"><SyncBody>" + body + L"<Final/></SyncBody></SyncML>";
    fprintf(stderr, "[dmprocessxml] reply: %ls\n", reply.c_str());
    if (!(*out = SysAllocString(reply.c_str()))) return E_OUTOFMEMORY;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DMProcessConfigXML(LPCWSTR in, BSTR *out)
{
    return DMProcessConfigXMLFiltered(in, nullptr, 0, out);
}

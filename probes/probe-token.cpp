// probe-token: dump NtQueryInformationToken(TokenSecurityAttributes) in the kernel layout, and the
// user-mode package identity APIs. Verifies what a hosted UWP app will see.
#include <windows.h>
#include <winternl.h>
#include <appmodel.h>
#include <cstdio>
struct ATTR_V1 { UNICODE_STRING Name; USHORT ValueType; USHORT Reserved; ULONG Flags; ULONG ValueCount; void* Values; };
struct ATTR_INFO { USHORT Version; USHORT Reserved; ULONG AttributeCount; ATTR_V1* pAttributeV1; };
typedef NTSTATUS (WINAPI *pNtQIT)(HANDLE, int, void*, ULONG, ULONG*);
int wmain() {
    auto NtQIT = (pNtQIT)GetProcAddress(GetModuleHandleW(L"ntdll"), "NtQueryInformationToken");
    ULONG len = 0; NTSTATUS st = NtQIT(GetCurrentProcessToken(), 39 /*TokenSecurityAttributes*/, nullptr, 0, &len);
    wprintf(L"size query -> 0x%08lx, needed %lu\n", st, len);
    BYTE* buf = (BYTE*)malloc(len ? len : 1);
    st = NtQIT(GetCurrentProcessToken(), 39, buf, len, &len);
    wprintf(L"query -> 0x%08lx\n", st);
    if (st == 0) {
        auto* hdr = (ATTR_INFO*)buf;
        wprintf(L"version %u count %lu\n", hdr->Version, hdr->AttributeCount);
        for (ULONG i = 0; i < hdr->AttributeCount; i++) {
            ATTR_V1& a = hdr->pAttributeV1[i];
            wprintf(L"  [%lu] %.*s type %u flags %#lx values %lu\n", i, a.Name.Length/2, a.Name.Buffer, a.ValueType, a.Flags, a.ValueCount);
            for (ULONG v = 0; v < a.ValueCount; v++) {
                if (a.ValueType == 3) { auto* s = (UNICODE_STRING*)a.Values; wprintf(L"      \"%.*s\" (len %u, max %u, nul=%d)\n", s[v].Length/2, s[v].Buffer, s[v].Length, s[v].MaximumLength, s[v].Buffer[s[v].Length/2]==0); }
                else if (a.ValueType == 2) wprintf(L"      %llu\n", ((ULONG64*)a.Values)[v]);
            }
        }
    }
    WCHAR name[512]; UINT32 n = 512;
    wprintf(L"GetCurrentPackageFullName -> %ld %ls\n", GetCurrentPackageFullName(&n, name), name);
    n = 512; wprintf(L"GetCurrentPackageFamilyName -> %ld %ls\n", GetCurrentPackageFamilyName(&n, name), name);
    n = 512; wprintf(L"GetCurrentApplicationUserModelId -> %ld %ls\n", GetCurrentApplicationUserModelId(&n, name), name);
    n = 512; wprintf(L"GetCurrentPackagePath -> %ld %ls\n", GetCurrentPackagePath(&n, name), name);
    UINT32 sz = 0, cnt = 0; LONG r = GetCurrentPackageInfo(PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &sz, nullptr, &cnt);
    wprintf(L"GetCurrentPackageInfo size -> %ld need %u\n", r, sz);
    BYTE* pb = (BYTE*)malloc(sz); r = GetCurrentPackageInfo(PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &sz, pb, &cnt);
    wprintf(L"GetCurrentPackageInfo -> %ld count %u\n", r, cnt);
    for (UINT32 i = 0; i < cnt; i++) { auto* pi = (PACKAGE_INFO*)pb + i; wprintf(L"  flags %#x full %ls family %ls path %ls id.name %ls pub %ls\n", pi->flags, pi->packageFullName, pi->packageFamilyName, pi->path, pi->packageId.name, pi->packageId.publisherId); }
    return 0;
}

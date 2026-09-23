// DPAPI-NG round trip: NCryptProtectSecret -> NCryptUnprotectSecret, the calls
// DataProtectionProvider makes. Prints PASS/FAIL per step.
#include <windows.h>
#include <cstdio>
#include <cstring>

typedef LONG (WINAPI *create_fn)(const wchar_t *, DWORD, void **);
typedef LONG (WINAPI *close_fn)(void *);
typedef LONG (WINAPI *protect_fn)(void *, DWORD, const BYTE *, ULONG, const void *, HWND, BYTE **, ULONG *);
typedef LONG (WINAPI *unprotect_fn)(void **, DWORD, const BYTE *, ULONG, const void *, HWND, BYTE **, ULONG *);

int main()
{
    HMODULE m = LoadLibraryW(L"ncrypt.dll");
    auto create = (create_fn)GetProcAddress(m, "NCryptCreateProtectionDescriptor");
    auto close = (close_fn)GetProcAddress(m, "NCryptCloseProtectionDescriptor");
    auto protect = (protect_fn)GetProcAddress(m, "NCryptProtectSecret");
    auto unprotect = (unprotect_fn)GetProcAddress(m, "NCryptUnprotectSecret");
    const char secret[] = "{\"token\":\"abc\"}";
    void *desc = nullptr, *desc2 = nullptr;
    BYTE *blob = nullptr, *plain = nullptr;
    ULONG blob_size = 0, plain_size = 0;
    LONG st;

    st = create(L"LOCAL=user", 0, &desc);
    printf("create %s %#lx\n", st ? "FAIL" : "PASS", st);
    st = protect(desc, 0, (const BYTE *)secret, sizeof(secret), nullptr, nullptr, &blob, &blob_size);
    printf("protect %s %#lx size %lu\n", st ? "FAIL" : "PASS", st, blob_size);
    st = unprotect(&desc2, 0, blob, blob_size, nullptr, nullptr, &plain, &plain_size);
    printf("unprotect %s %#lx size %lu\n", st ? "FAIL" : "PASS", st, plain_size);
    printf("roundtrip %s\n", plain_size == sizeof(secret) && !memcmp(plain, secret, plain_size) ? "PASS" : "FAIL");
    blob[blob_size - 1] ^= 1;
    st = unprotect(nullptr, 0, blob, blob_size, nullptr, nullptr, &plain, &plain_size);
    printf("tampered rejected %s %#lx\n", st ? "PASS" : "FAIL", st);
    close(desc); if (desc2) close(desc2);
    return 0;
}

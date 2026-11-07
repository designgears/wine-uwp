#include <windows.h>
#include <appmodel.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef LONG (WINAPI *info_fn)(UINT32, UINT32 *, BYTE *, UINT32 *);
typedef LONG (WINAPI *info2_fn)(UINT32, UINT32, UINT32 *, BYTE *, UINT32 *);
#define CHECK(x) do { if (!(x)) { printf("PACKAGE_INFO_FAIL line=%d result=%ld size=%u count=%u\n", __LINE__, result, size, count); return 1; } } while (0)
int main(void)
{
    HMODULE base = GetModuleHandleW(L"kernelbase.dll");
    info_fn info = (info_fn)GetProcAddress(base, "GetCurrentPackageInfo");
    info2_fn info2 = (info2_fn)GetProcAddress(base, "GetCurrentPackageInfo2");
    UINT32 size=0, count=0, required;
    LONG result=0;
    BYTE *buffer;
    CHECK(info && info2);
    SetEnvironmentVariableW(L"WINE_UWP_PACKAGE_FULL_NAME", L"WineUwp.ApiProbe_1.0.0.0_x64__123456789abcd");
    SetEnvironmentVariableW(L"WINE_UWP_PACKAGE_FAMILY_NAME", L"WineUwp.ApiProbe_123456789abcd");
    SetEnvironmentVariableW(L"WINE_UWP_PACKAGE_PATH", L"C:\\package-info-probe");
    result = info(PACKAGE_FILTER_HEAD, &size, NULL, NULL);
    CHECK(result == ERROR_INSUFFICIENT_BUFFER && size > sizeof(PACKAGE_INFO));
    required=size;
    result = info2(PACKAGE_FILTER_HEAD, 0, &size, NULL, &count);
    CHECK(result == ERROR_INSUFFICIENT_BUFFER && count == 1 && size == required);
    buffer=malloc(required); CHECK(buffer);
    result = info(PACKAGE_FILTER_HEAD, &size, buffer, NULL);
    CHECK(result == ERROR_SUCCESS);
    CHECK(!wcscmp(((PACKAGE_INFO *)buffer)->packageFullName, L"WineUwp.ApiProbe_1.0.0.0_x64__123456789abcd"));
    size=required; count=0;
    result=info2(PACKAGE_FILTER_HEAD, 0, &size, buffer, &count);
    CHECK(result == ERROR_SUCCESS && count == 1);
    result=info(PACKAGE_FILTER_HEAD, NULL, NULL, NULL);
    CHECK(result == ERROR_INVALID_PARAMETER);
    free(buffer);
    printf("PACKAGE_INFO_PASS optional_count=1 package_identity=1 both_apis=1\n");
    return 0;
}

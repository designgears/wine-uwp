#define INITGUID
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <wchar.h>
int main(void)
{
    WCHAR *normal=NULL, *unredirected=NULL;
    HRESULT hr;
    SetEnvironmentVariableW(L"WINE_UWP_PACKAGE_FULL_NAME", L"WineUwp.ApiProbe_1.0.0.0_x64__123456789abcd");
    SetEnvironmentVariableW(L"WINE_UWP_PACKAGE_FAMILY_NAME", L"WineUwp.ApiProbe_123456789abcd");
    hr=SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, NULL, &normal);
    if (FAILED(hr)) { printf("KNOWN_FOLDER_BASELINE_FAIL hr=%08lx\n", hr); return 1; }
    hr=SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY | KF_FLAG_NO_APPCONTAINER_REDIRECTION, NULL, &unredirected);
    if (FAILED(hr) || !unredirected || wcscmp(normal, unredirected))
    { printf("KNOWN_FOLDER_FLAG_FAIL hr=%08lx\n", hr); return 1; }
    printf("KNOWN_FOLDER_FLAG_PASS same_normal_appdata=1\n");
    CoTaskMemFree(normal); CoTaskMemFree(unredirected);
    return 0;
}

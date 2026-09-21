// probe-load: LoadLibrary each argument and report what happened. Run under Wine with
// WINEDEBUG=+loaddll to see which dependency failed to resolve.
#include <windows.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fwprintf(stderr, L"usage: probe-load <dll>...\n"); return 2; }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        SetLastError(0);
        /* Time the load: a DLL whose DllMain is expensive (msftedit builds its HTML and MathML
         * tag tables on PROCESS_ATTACH) holds the loader lock for the whole process meanwhile. */
        DWORD start = GetTickCount();
        HMODULE h = LoadLibraryExW(argv[i], nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        if (!h) h = LoadLibraryW(argv[i]);
        DWORD err = GetLastError(), ms = GetTickCount() - start;
        if (h) { wprintf(L"OK   %ls -> %p (%lu ms)\n", argv[i], (void*)h, ms); }
        else   { wprintf(L"FAIL %ls (error %lu / 0x%08lx, %lu ms)\n", argv[i], err, err, ms); ++failures; }
    }
    return failures ? 1 : 0;
}

// probe-peek <exe name> <hex addr> <qwords>: dump another process's memory, following nothing.
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (argc < 4) { printf("usage: probe-peek <exe> <addr> <qwords>...\n"); return 1; }
    if (Process32FirstW(snap, &pe))
        do if (!_wcsicmp(pe.szExeFile, argv[1])) pid = pe.th32ProcessID; while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    HANDLE proc = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    for (int a = 2; a + 1 < argc; a += 2)
    {
        ULONG64 addr = wcstoull(argv[a], NULL, 16), n = wcstoull(argv[a + 1], NULL, 10);
        for (ULONG64 i = 0; i < n; i++)
        {
            ULONG64 v; SIZE_T got;
            if (!ReadProcessMemory(proc, (void *)(addr + i * 8), &v, 8, &got)) { printf("%016llx: <%lu>\n", addr + i * 8, GetLastError()); break; }
            printf("%016llx: %016llx\n", addr + i * 8, v);
        }
    }
    return 0;
}

// probe-threads <exe name> [tid...]: list another process's threads by Wine tid and description, and say
// whether each named tid still exists. Used to identify the holder of a lock named only as "blocked by 03cc".
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <winternl.h>

typedef NTSTATUS (WINAPI *qit_fn)(HANDLE, THREADINFOCLASS, void *, ULONG, ULONG *);
struct tbi { NTSTATUS ExitStatus; void *TebBaseAddress; CLIENT_ID ClientId; ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority; };

/* the thread's stack range, read from its TEB (NT_TIB StackBase at +8, StackLimit at +0x10) */
static void print_stack_range(HANDLE proc, DWORD tid)
{
    static qit_fn qit;
    struct tbi info = {};
    ULONG64 tib[3];
    SIZE_T got;
    if (!qit) qit = (qit_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return;
    if (!qit(h, (THREADINFOCLASS)0, &info, sizeof(info), NULL) &&
        ReadProcessMemory(proc, info.TebBaseAddress, tib, sizeof(tib), &got))
        printf("  teb %p stack %016llx-%016llx\n", info.TebBaseAddress, tib[2], tib[1]);
    CloseHandle(h);
}

int wmain(int argc, wchar_t **argv)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (argc < 2) { printf("usage: probe-threads <exe> [tid...]\n"); return 1; }
    if (Process32FirstW(snap, &pe))
        do if (!_wcsicmp(pe.szExeFile, argv[1])) pid = pe.th32ProcessID; while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    printf("pid %04lx\n", pid);
    HANDLE msnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32W me = { sizeof(me) };
    if (msnap != INVALID_HANDLE_VALUE && Module32FirstW(msnap, &me))
        do printf("M %p %lx %ls\n", me.modBaseAddr, me.modBaseSize, me.szModule); while (Module32NextW(msnap, &me));
    if (msnap != INVALID_HANDLE_VALUE) CloseHandle(msnap);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(snap, &te))
        do
        {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            WCHAR *desc = NULL;
            if (h) GetThreadDescription(h, &desc);
            printf("tid %04lx %ls\n", te.th32ThreadID, desc ? desc : L"");
            {
                HANDLE proc = OpenProcess(PROCESS_VM_READ, FALSE, pid);
                if (proc) { print_stack_range(proc, te.th32ThreadID); CloseHandle(proc); }
            }
            if (argc > 2 && !wcscmp(argv[2], L"all"))
            {
                HANDLE s = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                for (int k = 0; s && k < 3; k++)
                {
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    SuspendThread(s);
                    if (GetThreadContext(s, &ctx)) printf("  rip %016llx rsp %016llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx\n", ctx.Rip, ctx.Rsp, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.R8);
                    ResumeThread(s);
                    Sleep(20);
                }
                if (s) CloseHandle(s);
            }
            if (desc) LocalFree(desc);
            if (h) CloseHandle(h);
        } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    for (int i = (argc > 2 && !wcscmp(argv[2], L"all")) ? 3 : 2; i < argc; i++)
    {
        DWORD tid = wcstoul(argv[i], NULL, 16), code = 0;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!h) { printf("tid %04lx: OpenThread failed %lu\n", tid, GetLastError()); continue; }
        GetExitCodeThread(h, &code);
        printf("tid %04lx: exists, exit code %lx, owner pid %04lx\n", tid, code, GetProcessIdOfThread(h));
        CloseHandle(h);
        // two samples a second apart: a thread whose rip moves is running, one whose rip stays is blocked
        for (int sample = 0; sample < 2; sample++)
        {
            if (!(h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid))) break;
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            SuspendThread(h);
            BOOL ok = GetThreadContext(h, &ctx);
            ResumeThread(h);
            CloseHandle(h);
            if (!ok) { printf("  GetThreadContext failed %lu\n", GetLastError()); break; }
            printf("  rip %016llx rsp %016llx rax %llx rcx %llx rdx %llx rbx %llx rsi %llx rdi %llx rbp %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n", ctx.Rip, ctx.Rsp, ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);
            HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
            ULONG64 stack[512]; SIZE_T got = 0;
            if (proc && ReadProcessMemory(proc, (void *)ctx.Rsp, stack, sizeof(stack), &got))
                for (SIZE_T k = 0; k < got / 8; k++) printf("  stk %016llx\n", stack[k]);
            else printf("  ReadProcessMemory failed %lu\n", GetLastError());
            if (proc) CloseHandle(proc);
            Sleep(1000);
        }
    }
    return 0;
}

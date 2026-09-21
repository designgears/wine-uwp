// probe-wait: does a multi-object wait on an ALPC connection port wake when a client connects?
#include <windows.h>
#include <winternl.h>
#include <cstdio>
typedef struct { ULONG Flags; ULONG SecurityQos[3]; SIZE_T MaxMessageLength, MemoryBandwidth, MaxPoolUsage, MaxSectionSize, MaxViewSize, MaxTotalSectionSize; ULONG DupObjectTypes, Reserved; } PORT_ATTRS;
typedef struct { USHORT DataLength, TotalLength, Type, DataInfoOffset; ULONGLONG cp, ct; ULONG MessageId, pad; ULONGLONG cvs; } APMSG;
typedef NTSTATUS (WINAPI *pCreate)(HANDLE*, OBJECT_ATTRIBUTES*, PORT_ATTRS*);
typedef NTSTATUS (WINAPI *pConnectEx)(HANDLE*, OBJECT_ATTRIBUTES*, OBJECT_ATTRIBUTES*, PORT_ATTRS*, ULONG, void*, APMSG*, SIZE_T*, void*, void*, LARGE_INTEGER*);
static HANDLE conn, dummy;
static DWORD WINAPI waiter_single(void*) { DWORD r = WaitForSingleObject(conn, 5000); printf("%s: single wait -> %lu\n", r == WAIT_OBJECT_0 ? "ok" : "FAIL", r); return 0; }
static DWORD WINAPI waiter_multi(void*) { HANDLE h[2] = { dummy, conn }; DWORD r = WaitForMultipleObjects(2, h, FALSE, 5000); printf("%s: multi wait -> %lu\n", r == WAIT_OBJECT_0 + 1 ? "ok" : "FAIL", r); return 0; }
int wmain() {
    HMODULE nt = GetModuleHandleW(L"ntdll");
    auto Create = (pCreate)GetProcAddress(nt, "NtAlpcCreatePort"); auto ConnectEx = (pConnectEx)GetProcAddress(nt, "NtAlpcConnectPortEx");
    UNICODE_STRING name; RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\WineUwpWaitPort");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    PORT_ATTRS pa = {}; pa.Flags = 0x70000; pa.MaxMessageLength = 0x100;
    Create(&conn, &oa, &pa); dummy = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE t1 = CreateThread(NULL, 0, waiter_single, NULL, 0, NULL); HANDLE t2 = CreateThread(NULL, 0, waiter_multi, NULL, 0, NULL); Sleep(300);
    APMSG m = {}; m.TotalLength = sizeof m; SIZE_T size = sizeof m; LARGE_INTEGER to; to.QuadPart = -10000000; HANDLE port = NULL;
    PORT_ATTRS ca = {}; ca.Flags = 0x400018; ca.MaxMessageLength = 0x100;
    ConnectEx(&port, &oa, NULL, &ca, 0x20000, NULL, &m, &size, NULL, NULL, &to);
    WaitForSingleObject(t1, 6000); WaitForSingleObject(t2, 6000);
    return 0;
}

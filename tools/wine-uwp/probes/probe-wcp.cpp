// probe-wcp: wait completion packet + I/O completion port on an ALPC connection port, the way
// CoreMessaging's registrar waits for connections. Server: create port, associate packet, block in
// NtRemoveIoCompletion. Client (main thread): connect. Expect the completion to arrive.
#include <windows.h>
#include <winternl.h>
#include <cstdio>
typedef struct { ULONG Flags; ULONG SecurityQos[3]; SIZE_T MaxMessageLength, MemoryBandwidth, MaxPoolUsage, MaxSectionSize, MaxViewSize, MaxTotalSectionSize; ULONG DupObjectTypes, Reserved; } PORT_ATTRS;
typedef struct { USHORT DataLength, TotalLength, Type, DataInfoOffset; ULONGLONG cp, ct; ULONG MessageId, pad; ULONGLONG cvs; } APMSG;
typedef NTSTATUS (WINAPI *pCreate)(HANDLE*, OBJECT_ATTRIBUTES*, PORT_ATTRS*);
typedef NTSTATUS (WINAPI *pConnectEx)(HANDLE*, OBJECT_ATTRIBUTES*, OBJECT_ATTRIBUTES*, PORT_ATTRS*, ULONG, void*, APMSG*, SIZE_T*, void*, void*, LARGE_INTEGER*);
typedef NTSTATUS (WINAPI *pCreateIocp)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*, ULONG);
typedef NTSTATUS (WINAPI *pRemove)(HANDLE, ULONG_PTR*, ULONG_PTR*, IO_STATUS_BLOCK*, LARGE_INTEGER*);
typedef NTSTATUS (WINAPI *pCreateWcp)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*);
typedef NTSTATUS (WINAPI *pAssoc)(HANDLE, HANDLE, HANDLE, void*, void*, NTSTATUS, ULONG_PTR, BOOLEAN*);
static HANDLE conn, iocp;
static DWORD WINAPI server_thread(void*) {
    HMODULE nt = GetModuleHandleW(L"ntdll");
    auto CreateIocp = (pCreateIocp)GetProcAddress(nt, "NtCreateIoCompletion"); auto Remove = (pRemove)GetProcAddress(nt, "NtRemoveIoCompletion");
    auto CreateWcp = (pCreateWcp)GetProcAddress(nt, "NtCreateWaitCompletionPacket"); auto Assoc = (pAssoc)GetProcAddress(nt, "NtAssociateWaitCompletionPacket");
    NTSTATUS st = CreateIocp(&iocp, IO_COMPLETION_ALL_ACCESS, NULL, 0); printf("iocp -> 0x%08lx\n", st);
    HANDLE wcp; st = CreateWcp(&wcp, 1, NULL); printf("packet -> 0x%08lx\n", st);
    BOOLEAN already = 0; st = Assoc(wcp, iocp, conn, (void*)0x40000402, NULL, 0, 0, &already); printf("associate -> 0x%08lx already=%d\n", st, already);
    ULONG_PTR key = 0, val = 0; IO_STATUS_BLOCK iosb = {}; LARGE_INTEGER to; to.QuadPart = -100000000;
    st = Remove(iocp, &key, &val, &iosb, &to);
    printf("%s: NtRemoveIoCompletion -> 0x%08lx key=%p\n", (st == 0 && key == 0x40000402) ? "ok" : "FAIL", st, (void*)key);
    return 0;
}
int wmain() {
    HMODULE nt = GetModuleHandleW(L"ntdll");
    auto Create = (pCreate)GetProcAddress(nt, "NtAlpcCreatePort"); auto ConnectEx = (pConnectEx)GetProcAddress(nt, "NtAlpcConnectPortEx");
    UNICODE_STRING name; RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\WineUwpWcpPort");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    PORT_ATTRS pa = {}; pa.Flags = 0x70000; pa.MaxMessageLength = 0x100;
    NTSTATUS st = Create(&conn, &oa, &pa); printf("create -> 0x%08lx\n", st);
    HANDLE th = CreateThread(NULL, 0, server_thread, NULL, 0, NULL); Sleep(500);
    APMSG m = {}; m.TotalLength = sizeof m; SIZE_T size = sizeof m; LARGE_INTEGER to; to.QuadPart = -20000000; HANDLE port = NULL;
    PORT_ATTRS ca = {}; ca.Flags = 0x400018; ca.MaxMessageLength = 0x100;
    st = ConnectEx(&port, &oa, NULL, &ca, 0x20000, NULL, &m, &size, NULL, NULL, &to);
    printf("client connect (expected timeout since nobody accepts) -> 0x%08lx\n", st);
    WaitForSingleObject(th, 5000);
    return 0;
}

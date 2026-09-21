// probe-alpc: exercise Wine's ALPC end to end in one process, the way CoreMessaging uses it.
// Server thread: create named connection port, receive connection request, accept with a port context,
// then answer one synchronous request and read one datagram (with a transferred event handle).
// Client: connect with a connection message, send sync request + wait for reply, send datagram + handle.
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstring>
#define ALPC_MSGFLG_REPLY_MESSAGE 0x10000
#define ALPC_MSGFLG_SYNC_REQUEST  0x20000
#define ALPC_MESSAGE_CONTEXT_ATTRIBUTE 0x20000000
#define ALPC_MESSAGE_HANDLE_ATTRIBUTE  0x10000000
typedef struct _PORT_MESSAGE64 { USHORT DataLength; USHORT TotalLength; USHORT Type; USHORT DataInfoOffset; ULONGLONG ClientProcess, ClientThread; ULONG MessageId; ULONG pad; ULONGLONG ClientViewSize; } APMSG;
typedef struct { ULONG Flags; ULONG SecurityQos[3]; SIZE_T MaxMessageLength, MemoryBandwidth, MaxPoolUsage, MaxSectionSize, MaxViewSize, MaxTotalSectionSize; ULONG DupObjectTypes, Reserved; } PORT_ATTRS;
typedef struct { ULONG AllocatedAttributes, ValidAttributes; } MSG_ATTRS;
typedef struct { void* PortContext; void* MessageContext; ULONG Sequence, MessageId, CallbackId; } CTX_ATTR;
typedef struct { ULONG Flags; HANDLE Handle; ULONG ObjectType; ACCESS_MASK DesiredAccess; } HANDLE_ATTR;
typedef NTSTATUS (WINAPI *pCreate)(HANDLE*, OBJECT_ATTRIBUTES*, PORT_ATTRS*);
typedef NTSTATUS (WINAPI *pConnectEx)(HANDLE*, OBJECT_ATTRIBUTES*, OBJECT_ATTRIBUTES*, PORT_ATTRS*, ULONG, void*, APMSG*, SIZE_T*, MSG_ATTRS*, MSG_ATTRS*, LARGE_INTEGER*);
typedef NTSTATUS (WINAPI *pAccept)(HANDLE*, HANDLE, ULONG, OBJECT_ATTRIBUTES*, PORT_ATTRS*, void*, APMSG*, MSG_ATTRS*, BOOLEAN);
typedef NTSTATUS (WINAPI *pSWR)(HANDLE, ULONG, APMSG*, MSG_ATTRS*, APMSG*, SIZE_T*, MSG_ATTRS*, LARGE_INTEGER*);
typedef NTSTATUS (WINAPI *pInitAttr)(ULONG, MSG_ATTRS*, SIZE_T, SIZE_T*);
typedef void* (WINAPI *pGetAttr)(MSG_ATTRS*, ULONG);
static pCreate Create; static pConnectEx ConnectEx; static pAccept Accept; static pSWR SWR; static pInitAttr InitAttr; static pGetAttr GetAttr;
struct Buf { APMSG hdr; char data[256]; };
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)
static MSG_ATTRS* make_attrs(char* buf, size_t size) { SIZE_T need; InitAttr(ALPC_MESSAGE_CONTEXT_ATTRIBUTE | ALPC_MESSAGE_HANDLE_ATTRIBUTE, (MSG_ATTRS*)buf, size, &need); return (MSG_ATTRS*)buf; }
static DWORD WINAPI server_thread(void*) {
    UNICODE_STRING name; RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\WineUwpProbePort");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    PORT_ATTRS pa = {}; pa.Flags = 0x70000; pa.MaxMessageLength = 0x100;
    HANDLE conn; NTSTATUS st = Create(&conn, &oa, &pa);
    CHECK(st == 0, "server: NtAlpcCreatePort -> 0x%08lx", st);
    Buf b; SIZE_T size = sizeof(b); char abuf[128]; MSG_ATTRS* ra = make_attrs(abuf, sizeof abuf);
    st = SWR(conn, 0, NULL, NULL, &b.hdr, &size, ra, NULL);
    CHECK(st == 0 && b.hdr.Type == 10, "server: got connection request (status 0x%08lx type %u data \"%.*s\")", st, b.hdr.Type, b.hdr.DataLength, b.data);
    HANDLE comm; APMSG reply = b.hdr; strcpy(b.data, "welcome"); b.hdr.DataLength = 8;
    st = Accept(&comm, conn, 0, NULL, &pa, (void*)0x1234, &b.hdr, NULL, TRUE);
    CHECK(st == 0 && comm, "server: NtAlpcAcceptConnectPort -> 0x%08lx", st);
    size = sizeof(b); st = SWR(comm, 0, NULL, NULL, &b.hdr, &size, ra, NULL);
    CTX_ATTR* ctx = (CTX_ATTR*)GetAttr(ra, ALPC_MESSAGE_CONTEXT_ATTRIBUTE);
    CHECK(st == 0 && b.hdr.Type == 1 && !strcmp(b.data, "ping"), "server: got request (status 0x%08lx type %u data \"%s\")", st, b.hdr.Type, b.data);
    CHECK((ra->ValidAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE) && ctx->PortContext == (void*)0x1234, "server: context attribute PortContext=%p", ctx ? ctx->PortContext : NULL);
    strcpy(b.data, "pong"); b.hdr.DataLength = 5; b.hdr.TotalLength = sizeof(APMSG) + 5;
    st = SWR(comm, ALPC_MSGFLG_REPLY_MESSAGE, &b.hdr, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == 0, "server: reply sent -> 0x%08lx", st);
    size = sizeof(b); st = SWR(comm, 0, NULL, NULL, &b.hdr, &size, ra, NULL);
    HANDLE_ATTR* ha = (HANDLE_ATTR*)GetAttr(ra, ALPC_MESSAGE_HANDLE_ATTRIBUTE);
    CHECK(st == 0 && b.hdr.Type == 3 && !strcmp(b.data, "gram"), "server: got datagram (status 0x%08lx type %u data \"%s\")", st, b.hdr.Type, b.data);
    CHECK((ra->ValidAttributes & ALPC_MESSAGE_HANDLE_ATTRIBUTE) && ha->Handle, "server: received handle %p", ha ? ha->Handle : NULL);
    if (ha && ha->Handle) { SetEvent(ha->Handle); }
    LARGE_INTEGER to; to.QuadPart = -5000000; size = sizeof(b);
    st = SWR(comm, 0, NULL, NULL, &b.hdr, &size, ra, &to);
    CHECK(st == 0 && b.hdr.Type == 5, "server: peer disconnect delivered as PORT_CLOSED (status 0x%08lx type %u)", st, b.hdr.Type);
    return 0;
}
int wmain() {
    HMODULE nt = GetModuleHandleW(L"ntdll");
    Create = (pCreate)GetProcAddress(nt, "NtAlpcCreatePort"); ConnectEx = (pConnectEx)GetProcAddress(nt, "NtAlpcConnectPortEx");
    Accept = (pAccept)GetProcAddress(nt, "NtAlpcAcceptConnectPort"); SWR = (pSWR)GetProcAddress(nt, "NtAlpcSendWaitReceivePort");
    InitAttr = (pInitAttr)GetProcAddress(nt, "AlpcInitializeMessageAttribute"); GetAttr = (pGetAttr)GetProcAddress(nt, "AlpcGetMessageAttribute");
    typedef NTSTATUS (WINAPI *pDisc)(HANDLE, ULONG); pDisc Disc = (pDisc)GetProcAddress(nt, "NtAlpcDisconnectPort");
    HANDLE th = CreateThread(NULL, 0, server_thread, NULL, 0, NULL); Sleep(300);
    UNICODE_STRING name; RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\WineUwpProbePort");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    PORT_ATTRS pa = {}; pa.Flags = 0x400018; pa.MaxMessageLength = 0x100;
    Buf b = {}; strcpy(b.data, "hello"); b.hdr.DataLength = 6; b.hdr.TotalLength = sizeof(APMSG) + 6; SIZE_T size = sizeof(b);
    LARGE_INTEGER to; to.QuadPart = -50000000; HANDLE port = NULL;
    NTSTATUS st = ConnectEx(&port, &oa, NULL, &pa, ALPC_MSGFLG_SYNC_REQUEST, NULL, &b.hdr, &size, NULL, NULL, &to);
    CHECK(st == 0 && port, "client: NtAlpcConnectPortEx -> 0x%08lx (reply \"%s\")", st, b.data);
    char abuf[128]; MSG_ATTRS* ra = make_attrs(abuf, sizeof abuf);
    Buf req = {}; strcpy(req.data, "ping"); req.hdr.DataLength = 5; req.hdr.TotalLength = sizeof(APMSG) + 5;
    Buf rep = {}; size = sizeof(rep);
    st = SWR(port, ALPC_MSGFLG_SYNC_REQUEST, &req.hdr, NULL, &rep.hdr, &size, ra, &to);
    CHECK(st == 0 && rep.hdr.Type == 2 && !strcmp(rep.data, "pong"), "client: sync request/reply (status 0x%08lx type %u data \"%s\")", st, rep.hdr.Type, rep.data);
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    char sbuf[128]; MSG_ATTRS* sa = make_attrs(sbuf, sizeof sbuf); HANDLE_ATTR* ha = (HANDLE_ATTR*)GetAttr(sa, ALPC_MESSAGE_HANDLE_ATTRIBUTE);
    ha->Handle = ev; ha->DesiredAccess = EVENT_ALL_ACCESS; sa->ValidAttributes = ALPC_MESSAGE_HANDLE_ATTRIBUTE;
    Buf dg = {}; strcpy(dg.data, "gram"); dg.hdr.DataLength = 5; dg.hdr.TotalLength = sizeof(APMSG) + 5;
    st = SWR(port, 0, &dg.hdr, sa, NULL, NULL, NULL, NULL);
    CHECK(st == 0, "client: datagram with handle -> 0x%08lx", st);
    CHECK(WaitForSingleObject(ev, 3000) == WAIT_OBJECT_0, "client: server signaled the transferred event");
    Disc(port, 0);
    CHECK(WaitForSingleObject(th, 8000) == WAIT_OBJECT_0, "server thread finished");
    printf("%s (%d failures)\n", fails ? "PROBE FAILED" : "PROBE PASSED", fails);
    return fails ? 1 : 0;
}

#!/usr/bin/env python3
"""Execute wineuwp's shutdown bridge C code with synthetic PE data and Win32 mocks.

These ASan/UBSan checks cover caller validation, import installation and API routing.
They do not establish Wine ABI correctness, real termination, or save persistence.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'wine/dlls/wineuwp/main.c').read_text()
a = source.index('static BYTE *shutdown_module;')
b = source.index('static DWORD view_wait_tls', a)
body = source[a:b]
prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#define WINAPI
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define _stricmp strcasecmp
#define __builtin_return_address(n) test_caller
#define EPT_NT_NOT_REGISTERED ((int32_t)0xc0020036)
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((int32_t)0xc0000001)
#define STATUS_INVALID_PARAMETER ((int32_t)0xc000000d)
#define IMAGE_SCN_MEM_EXECUTE 0x20000000u
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION 3
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define IMAGE_SNAP_BY_ORDINAL(x) ((x)&(1ull<<63))
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
#define PAGE_READWRITE 4
#define SYM_APP_TEARDOWN "teardown"
typedef unsigned char BYTE;
typedef uint32_t DWORD, ULONG;
typedef int32_t LONG, NTSTATUS;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef int BOOL;
typedef void *HMODULE;
typedef struct { BYTE unused[60]; DWORD e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { uint16_t Machine, NumberOfSections; BYTE unused[12]; uint16_t SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { BYTE unused[56]; DWORD SizeOfImage; BYTE rest[48]; DWORD NumberOfRvaAndSizes; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER OptionalHeader; } IMAGE_NT_HEADERS;
typedef struct { BYTE Name[8]; union { DWORD VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData; BYTE unused[12]; DWORD Characteristics; } IMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((const IMAGE_SECTION_HEADER *)((const BYTE *)&(nt)->OptionalHeader+(nt)->FileHeader.SizeOfOptionalHeader))
typedef struct { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION;
typedef struct { union { DWORD AllAttributes; } Attributes; DWORD DllNameRVA, ModuleHandleRVA, ImportAddressTableRVA, ImportNameTableRVA, BoundImportAddressTableRVA, UnloadInformationTableRVA, TimeDateStamp; } IMAGE_DELAYLOAD_DESCRIPTOR;
typedef struct { union { ULONG_PTR AddressOfData; } u1; } IMAGE_THUNK_DATA;
typedef struct { void *BaseAddress, *AllocationBase; SIZE_T RegionSize; } MEMORY_BASIC_INFORMATION;
static _Alignas(16) BYTE image[16384];
static void *test_caller, *native_output, *last_connection, *last_callback;
static DWORD test_tid, last_reason;
static int connect_calls, terminate_calls, disconnect_calls, process_terminations;
static NTSTATUS connect_status, terminate_status, disconnect_status;
static int missing_symbol, missing_export, missing_module, protect_fail, protect_calls, writable, module_refs, short_region;
static LONG InterlockedCompareExchange(LONG *p,LONG x,LONG expected) { LONG old=*p; if(old==expected)*p=x;return old; }
static DWORD GetCurrentThreadId(void) { return test_tid; }
static void *GetCurrentProcess(void) { return (void *)-1; }
static BOOL TerminateProcess(void *process,DWORD code) { assert(process==(void *)-1 && code==0);++process_terminations;return FALSE; }
static DWORD GetLastError(void) { return 5; }
static void *uwp_symbol(const char *dll,const char *name) { assert(!strcmp(name,SYM_APP_TEARDOWN));return missing_symbol?NULL:image+0x1000; }
static HMODULE LoadLibraryExW(const wchar_t *name,void *file,DWORD flags) {
    assert(!wcscmp(name,L"rmclient.dll") && !file && flags==LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(missing_module)return NULL;++module_refs;return (void *)0xfeed;
}
static void FreeLibrary(HMODULE mod) { assert(mod==(void *)0xfeed && module_refs==1);--module_refs; }
static NTSTATUS mock_connect(void *callback,void **connection) {
    ++connect_calls;last_callback=callback;if(connection)*connection=native_output;return connect_status;
}
static NTSTATUS mock_terminate(void *connection,ULONG reason) {
    ++terminate_calls;last_connection=connection;last_reason=reason;return terminate_status;
}
static NTSTATUS mock_disconnect(void *connection) {
    ++disconnect_calls;last_connection=connection;return disconnect_status;
}
static void *GetProcAddress(HMODULE mod,const char *name) {
    assert(mod==(void *)0xfeed);
    if(!strcmp(name,"HamConnectForExtendedExecution"))return missing_export==1?NULL:mock_connect;
    if(!strcmp(name,"HamTerminateSelfEx"))return missing_export==2?NULL:mock_terminate;
    assert(!strcmp(name,"HamDisconnectForExtendedExecution"));return missing_export==3?NULL:mock_disconnect;
}
static SIZE_T VirtualQuery(void *ptr,MEMORY_BASIC_INFORMATION *info,SIZE_T size) {
    info->BaseAddress=image;info->AllocationBase=image;info->RegionSize=short_region?0x3408:sizeof(image);return size;
}
static BOOL VirtualProtect(void *ptr,SIZE_T size,DWORD mode,DWORD *old) {
    assert((BYTE *)ptr>=image && (BYTE *)ptr+size<=image+sizeof(image));
    if(++protect_calls==protect_fail)return FALSE;
    *old=writable?PAGE_READWRITE:2;writable=(mode==PAGE_READWRITE);return TRUE;
}
'''
suffix = r'''
static IMAGE_NT_HEADERS *nt;
static RUNTIME_FUNCTION *table;
static IMAGE_DELAYLOAD_DESCRIPTOR *desc;
static IMAGE_THUNK_DATA *imports;
static void **iat;
static void setup(void) {
    memset(image,0,sizeof(image));
    ((IMAGE_DOS_HEADER *)image)->e_lfanew=64;nt=(void *)(image+64);
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt->FileHeader.NumberOfSections=1;
    nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage=sizeof(image);nt->OptionalHeader.NumberOfRvaAndSizes=16;
    IMAGE_SECTION_HEADER *section=(void *)IMAGE_FIRST_SECTION(nt);
    section->VirtualAddress=0x1000;section->Misc.VirtualSize=0x1000;section->Characteristics=IMAGE_SCN_MEM_EXECUTE;
    nt->OptionalHeader.DataDirectory[3]=(IMAGE_DATA_DIRECTORY){0x2100,4*sizeof(RUNTIME_FUNCTION)};
    table=(void *)(image+0x2100);
    table[0]=(RUNTIME_FUNCTION){0x1000,0x1100,0x2500};image[0x2500]=1;
    table[1]=(RUNTIME_FUNCTION){0x1200,0x1240,0x2520};image[0x2520]=1;
    table[2]=(RUNTIME_FUNCTION){0x1300,0x1340,0x2540};image[0x2540]=0x21;
    memcpy(image+0x2544,table,sizeof(*table));
    table[3]=(RUNTIME_FUNCTION){0x1400,0x1440,0x2581};memcpy(image+0x2580,table,sizeof(*table));
    nt->OptionalHeader.DataDirectory[13]=(IMAGE_DATA_DIRECTORY){0x3000,2*sizeof(*desc)};
    desc=(void *)(image+0x3000);desc->Attributes.AllAttributes=1;desc->DllNameRVA=0x3200;
    desc->ImportNameTableRVA=0x3300;desc->ImportAddressTableRVA=0x3400;
    strcpy((char *)image+0x3200,"RMCLIENT.dll");
    imports=(void *)(image+0x3300);iat=(void *)(image+0x3400);
    const char *names[]={"HamConnectForExtendedExecution","HamTerminateSelfEx","HamDisconnectForExtendedExecution"};
    for(int i=0;i<3;i++) { imports[i].u1.AddressOfData=0x3500+i*64;strcpy((char *)image+0x3502+i*64,names[i]);iat[i]=image+0x1600+i*16; }
    shutdown_module=image;shutdown_image_size=sizeof(image);shutdown_teardown_rva=0x1000;
    native_view_started=1;shutdown_owner=0;
    test_caller=image+0x1050;test_tid=19;
    native_ham_connect=mock_connect;native_ham_terminate=mock_terminate;native_ham_disconnect=mock_disconnect;
    native_output=NULL;connect_status=EPT_NT_NOT_REGISTERED;terminate_status=STATUS_UNSUCCESSFUL;disconnect_status=STATUS_UNSUCCESSFUL;
    connect_calls=terminate_calls=disconnect_calls=process_terminations=0;
    missing_symbol=missing_export=missing_module=protect_fail=protect_calls=writable=module_refs=short_region=0;
}
static void test_unwind(void) {
    DWORD root;
    setup();assert(shutdown_caller(image+0x1050));assert(shutdown_caller(image+0x1320));assert(shutdown_caller(image+0x1420));
    assert(!shutdown_caller(image+0x1100));assert(!shutdown_caller(image+0x1210));assert(!shutdown_caller(image+0x1350));
    assert(!shutdown_caller((void *)1));assert(!shutdown_caller(image+sizeof(image)));
    setup();table[1]=table[0];assert(!shutdown_caller(image+0x1050));
    setup();nt->OptionalHeader.DataDirectory[3].Size--;assert(!shutdown_caller(image+0x1050));
    setup();nt->OptionalHeader.DataDirectory[3].VirtualAddress=sizeof(image)-8;assert(!shutdown_caller(image+0x1050));
    setup();nt->OptionalHeader.DataDirectory[3].VirtualAddress++;assert(!shutdown_caller(image+0x1050));
    setup();table[0].EndAddress=sizeof(image)+1;assert(!shutdown_caller(image+0x1050));
    setup();table[0].UnwindData=sizeof(image)-2;assert(!shutdown_caller(image+0x1050));
    setup();table[0].UnwindData=0x3ffc;image[0x3ffc]=1;image[0x3ffe]=2;assert(!shutdown_caller(image+0x1050));
    setup();image[0x2500]=3;assert(!shutdown_caller(image+0x1050));
    setup();image[0x2500]=0x29;assert(!shutdown_caller(image+0x1050));
    setup();image[0x2500]=9;DWORD handler=0x1500;memcpy(image+0x2504,&handler,4);assert(shutdown_caller(image+0x1050));
    handler=0x3000;memcpy(image+0x2504,&handler,4);assert(!shutdown_caller(image+0x1050));
    setup();memcpy(image+0x2544,table+2,sizeof(*table));assert(!shutdown_caller(image+0x1320));
    setup();memcpy(image+0x2580,table+3,sizeof(*table));assert(!shutdown_caller(image+0x1420));
    setup();table[3].UnwindData=sizeof(image)-7;assert(!shutdown_caller(image+0x1420));
    setup();table[2].BeginAddress=0x3000;table[2].EndAddress=0x3040;assert(!shutdown_unwind_root(table[2],&root));
    setup();
    memcpy(image+0x2500,"\x01\x03\x04\x00\x03\x34\x01\x00\x02\x12\x01\x50",12);
    memcpy(image+0x2540,"\x21\x01\x02\x00\x01\x74\x00\x00",8);
    memcpy(image+0x2548,table,sizeof(*table));
    assert(shutdown_caller(image+0x1320)); /* Additional save, unchanged frame/allocation. */
    puts("unwind: contiguous, split, chained, out-of-range, invalid and cyclic records PASS");
}
static void test_unwind_rejections(void) {
    const char *cases[]={"undefined opcode", "chained frame register mismatch", "frame offset without register",
        "unsupported version", "volatile register push", "truncated save operand", "ascending code offsets",
        "code beyond prolog", "allocation after push", "chained push", "chained allocation", "framed primary",
        "volatile register save", "duplicate allocation", "unsupported allocation form"};
    for(unsigned kind=0;kind<ARRAY_SIZE(cases);kind++) {
        setup();
        BYTE *info=image+0x2500;
        /* Supported frameless prolog: save RBX, allocate 16 bytes, push RBP. */
        memcpy(info,"\x01\x03\x04\x00\x03\x34\x01\x00\x02\x12\x01\x50",12);
        assert(shutdown_caller(test_caller));
        void *token=NULL;
        assert(host_ham_connect(NULL,&token)==0 && token==&shutdown_connection);
        switch(kind) {
        case 0: info[5]=15;break;
        case 1: image[0x2543]=5;test_caller=image+0x1320;break;
        case 2: info[3]=0x10;break;
        case 3: info[0]=2;break;
        case 4: info[11]=0;break;
        case 5: info[2]=1;break;
        case 6: info[4]=1;break;
        case 7: info[1]=2;break;
        case 8: info[5]=0x50;info[7]=0x12;break;
        case 9: case 10:
            info=image+0x2540;memcpy(info,"\x21\x01\x01\x00\x01\x50\x00\x00",8);
            if(kind==10)info[5]=0x12;
            memcpy(info+8,table,sizeof(*table));test_caller=image+0x1320;break;
        case 11: info[3]=5;break;
        case 12: info[5]=0x44;break;
        case 13: info[11]=0x12;break;
        case 14: info[5]=1;break;
        }
        assert(!shutdown_caller(test_caller));
        /* Invalid metadata must also reject termination with an already-issued token. */
        assert(host_ham_terminate(token,3)==STATUS_INVALID_PARAMETER && !process_terminations);
        shutdown_owner=0;token=NULL;
        assert(host_ham_connect(NULL,&token)==EPT_NT_NOT_REGISTERED && !token && !shutdown_owner);
        if(kind==0)assert(!prepare_shutdown_host(image));
        printf("unwind rejection: %s PASS\n",cases[kind]);
    }
}
static void test_routing(void) {
    void *out, *token;
    setup();out=NULL;assert(host_ham_connect((void *)0xabc,&out)==0);token=out;
    assert(token==&shutdown_connection && shutdown_owner==19 && connect_calls==1 && last_callback==(void *)0xabc);
    out=NULL;assert(host_ham_connect(NULL,&out)==EPT_NT_NOT_REGISTERED && !out && shutdown_owner==19);
    assert(host_ham_terminate(token,2)==STATUS_INVALID_PARAMETER && !process_terminations);
    test_tid=20;assert(host_ham_terminate(token,3)==STATUS_INVALID_PARAMETER);
    assert(host_ham_disconnect(token)==STATUS_INVALID_PARAMETER && shutdown_owner==19);
    test_tid=19;test_caller=image+0x1210;assert(host_ham_terminate(token,3)==STATUS_INVALID_PARAMETER);
    assert(host_ham_disconnect(token)==STATUS_INVALID_PARAMETER && shutdown_owner==19);
    test_caller=image+0x1320;assert(host_ham_terminate(token,3)==STATUS_UNSUCCESSFUL && process_terminations==1 && !terminate_calls);
    assert(host_ham_disconnect(token)==0 && !shutdown_owner && !disconnect_calls);
    assert(host_ham_disconnect(token)==STATUS_INVALID_PARAMETER);
    assert(host_ham_terminate(token,3)==STATUS_INVALID_PARAMETER && process_terminations==1);
    test_tid=21;out=NULL;assert(host_ham_connect(NULL,&out)==0 && out==token && shutdown_owner==21);
    assert(host_ham_disconnect(token)==0);
    const NTSTATUS statuses[]={0,1,STATUS_UNSUCCESSFUL,STATUS_INVALID_PARAMETER, EPT_NT_NOT_REGISTERED};
    for(unsigned i=0;i<ARRAY_SIZE(statuses);i++) {
        setup();connect_status=statuses[i];native_output=(void *)0x1234;out=NULL;
        assert(host_ham_connect((void *)0xabc,&out)==statuses[i] && out==native_output && !shutdown_owner);
        terminate_status=statuses[i];assert(host_ham_terminate(out,7)==statuses[i]);
        assert(last_connection==out && last_reason==7 && terminate_calls==1 && !process_terminations);
        disconnect_status=statuses[i];assert(host_ham_disconnect(out)==statuses[i] && last_connection==out && disconnect_calls==1);
    }
    setup();native_view_started=0;out=NULL;assert(host_ham_connect(NULL,&out)==EPT_NT_NOT_REGISTERED && !out && !shutdown_owner);
    setup();test_caller=image+0x1210;out=NULL;assert(host_ham_connect(NULL,&out)==EPT_NT_NOT_REGISTERED && !out && !shutdown_owner);
    setup();table[0].UnwindData=0;out=NULL;assert(host_ham_connect(NULL,&out)==EPT_NT_NOT_REGISTERED && !out && !shutdown_owner);
    setup();assert(host_ham_connect(NULL,NULL)==EPT_NT_NOT_REGISTERED && !shutdown_owner);
    setup();connect_status=STATUS_UNSUCCESSFUL;out=NULL;assert(host_ham_connect(NULL,&out)==STATUS_UNSUCCESSFUL && !out && !shutdown_owner);
    puts("routing: native results, wrong caller/context/token/thread/reason, reuse and cleanup PASS");
}
static void test_install(void) {
    BYTE before[sizeof(image)];
    for(int kind=0;kind<19;kind++) {
        setup();
        switch(kind) {
        case 1: missing_symbol=1;break;
        case 2: missing_module=1;break;
        case 3: missing_export=2;break;
        case 4: protect_fail=1;break;
        case 5: protect_fail=2;break;
        case 6: short_region=1;break;
        case 7: imports[1].u1.AddressOfData=0;break;
        case 8: imports[1].u1.AddressOfData=imports[0].u1.AddressOfData;break;
        case 9: imports[1].u1.AddressOfData=sizeof(image)-1;break;
        case 10: desc->ImportAddressTableRVA=sizeof(image);break;
        case 11: desc->Attributes.AllAttributes=0;break;
        case 12: desc->DllNameRVA=sizeof(image)-1;image[sizeof(image)-1]='x';break;
        case 13: nt->FileHeader.Machine=0x14c;break;
        case 14: table[0].BeginAddress++;break;
        case 15: iat[1]=(void *)0xdead;break;
        case 16: desc->ImportNameTableRVA++;break;
        case 17: nt->OptionalHeader.DataDirectory[13].VirtualAddress++;break;
        case 18: iat[0]=mock_connect;iat[1]=mock_terminate;iat[2]=mock_disconnect;break;
        }
        memcpy(before,image,sizeof(image));
        BOOL ok=prepare_shutdown_host(image);
        if(kind==0 || kind==18) {
            assert(ok && module_refs==1 && !writable);
            assert(iat[0]==host_ham_connect && iat[1]==host_ham_terminate && iat[2]==host_ham_disconnect);
            memcpy(before+0x3400,iat,3*sizeof(void *));assert(!memcmp(before,image,sizeof(image)));
            assert(native_ham_connect==mock_connect && native_ham_terminate==mock_terminate && native_ham_disconnect==mock_disconnect);
        } else {
            assert(!ok && !module_refs && !writable && !shutdown_module);
            assert(!memcmp(before,image,sizeof(image)));
        }
    }
    puts("installation: 19 cases, named delay imports, malformed data, native/thunk targets, protection rollback PASS");
}
int main(void) { test_unwind();test_unwind_rejections();test_routing();test_install();return 0; }
'''
with tempfile.TemporaryDirectory(prefix='native-shutdown-test-') as directory:
    path = Path(directory)
    (path / 'probe.c').write_text(prefix + body + suffix)
    subprocess.run(['clang', '-std=c11', '-g', '-O1', '-fsanitize=address,undefined',
                    str(path / 'probe.c'), '-o', str(path / 'probe')], check=True)
    subprocess.run([str(path / 'probe')], check=True)

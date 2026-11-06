#!/usr/bin/env python3
"""Check the native-view call redirect with the actual C installer and mocked Win32 calls.

Synthetic PE data covers rejection and rollback before a factory can escape. These
host checks do not execute native code or establish TLS/SEH, Wine ABI, or app behavior.
Run: python3 tests/test_native_view_readiness.py
"""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'wine/dlls/wineuwp/main.c').read_text()
a=s.index('HRESULT WINAPI __wine_uwp_prepare_native_views( void )')
b=s.index('\n}',a)+2
body=s[a:b]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define WINAPI
#define ERR(...) ((void)0)
#define TRACE(...) ((void)0)
#define TRUE 1
#define FALSE 0
#define E_NOINTERFACE -1
#define E_NOTIMPL -2
#define E_UNEXPECTED -3
#define E_OUTOFMEMORY -4
#define S_OK 0
#define TLS_OUT_OF_INDEXES 0xffffffffu
#define IMAGE_SCN_MEM_EXECUTE 0x20000000u
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define PAGE_EXECUTE_READWRITE 0x40
#define HRESULT_FROM_WIN32(x) (-(int)(x))
#define min(a,b) ((a)<(b)?(a):(b))
typedef void *HMODULE;
typedef int HRESULT, BOOL;
typedef unsigned char BYTE;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef struct { BYTE unused[60]; DWORD e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { uint16_t Machine, NumberOfSections; BYTE unused[12]; uint16_t SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { BYTE unused[56]; DWORD SizeOfImage; BYTE rest[180]; } IMAGE_OPTIONAL_HEADER;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER OptionalHeader; } IMAGE_NT_HEADERS;
typedef struct { BYTE Name[8]; union { DWORD VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData; BYTE unused[12]; DWORD Characteristics; } IMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((const IMAGE_SECTION_HEADER *)((const BYTE *)&(nt)->OptionalHeader+(nt)->FileHeader.SizeOfOptionalHeader))
#define SYM_VIEW_WAIT_READY "wait"
#define SYM_VIEW_SIGNAL_READY "signal"
static BYTE image[8192], before[8192];
static DWORD view_wait_tls=TLS_OUT_OF_INDEXES;
static void (WINAPI *native_wait_for_view_ready)(void *);
static void (WINAPI *native_signal_view_ready)(void *);
static void *wait_for_native_view_ready;
static int kind, tls_live, protect_calls, flush_calls, writable;
static int prepare_shutdown_host(HMODULE module) { return 1; }
static HMODULE GetModuleHandleW(const wchar_t *name) { return kind==12?NULL:image; }
static void *uwp_symbol(const char *dll,const char *name) {
    if (!strcmp(name,"wait")) return kind==1?NULL: image+(kind==6?512:0x1100);
    return kind==2?NULL:image+(kind==5?512:0x1200);
}
static DWORD TlsAlloc(void) { if(kind==11)return TLS_OUT_OF_INDEXES; ++tls_live; return 3; }
static void TlsFree(DWORD i) { assert(i==3 && tls_live==1); --tls_live; }
static DWORD GetLastError(void) { return 5; }
static void *GetCurrentProcess(void) { return (void *)-1; }
static BOOL VirtualProtect(void *addr, size_t size,DWORD protect,DWORD *old) {
    ++protect_calls;
    if ((kind==8 && protect_calls==1)||(kind==10 && protect_calls==2)) return 0;
    *old=writable?PAGE_EXECUTE_READWRITE:0x20; writable=(protect==PAGE_EXECUTE_READWRITE); return 1;
}
static BOOL FlushInstructionCache(void *process,void *addr,size_t size) {
    assert(writable); return !(kind==9 && ++flush_calls==1);
}
'''
suffix=r'''
int main(int argc,char **argv) {
    kind=atoi(argv[1]);
    IMAGE_DOS_HEADER *dos=(void *)image; dos->e_lfanew=64;
    IMAGE_NT_HEADERS *nt=(void *)(image+64);
    nt->FileHeader.Machine=kind==7?0x14c:IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections=1;
    nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage=sizeof(image);
    IMAGE_SECTION_HEADER *sec=(void *)IMAGE_FIRST_SECTION(nt);
    sec->VirtualAddress=0x1000;sec->Misc.VirtualSize=0x1000;sec->Characteristics=IMAGE_SCN_MEM_EXECUTE;
    BYTE *p=image+0x1030;
    memcpy(p,"\x48\x8b\xcf\xe8",4);
    LONG displacement=(image+(kind==6?512:0x1100))-(p+8);
    memcpy(p+4,&displacement,4);
    if(kind==3)p[2]=0xce;
    if(kind==4){memcpy(p+16,p,8);displacement-=16;memcpy(p+20,&displacement,4);}
    wait_for_native_view_ready=kind==13?(void *)((uintptr_t)image+0x100000000ull):image+0x1300;
    memcpy(before,image,sizeof(image));
    HRESULT hr=__wine_uwp_prepare_native_views();
    if(kind==0) {
        assert(hr==S_OK && tls_live==1 && view_wait_tls==3 && !writable);
        memcpy(&displacement,p+4,4);
        assert(p+8+displacement==(BYTE *)wait_for_native_view_ready);
        memcpy(before+0x1034,p+4,4);
        assert(!memcmp(before,image,sizeof(image)));
        assert(native_wait_for_view_ready==(void *)(image+0x1100));
        assert(native_signal_view_ready==(void *)(image+0x1200));
    } else {
        assert(hr<0 && !tls_live && view_wait_tls==TLS_OUT_OF_INDEXES && !writable);
        assert(!memcmp(before,image,sizeof(image)));
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='view-readiness-test-') as d:
    p=Path(d);(p/'probe.c').write_text(prefix+body+suffix)
    subprocess.run(['clang','-std=c11','-g','-O1','-fsanitize=address,undefined',str(p/'probe.c'),'-o',str(p/'probe')],check=True)
    cases = ['install', 'missing wait', 'missing signal', 'wrong register', 'ambiguous calls',
             'noncode signal', 'noncode wait', 'wrong architecture', 'write protection failure',
             'cache flush failure', 'restore protection failure', 'TLS exhaustion', 'missing module',
             'rel32 out of range']
    for i, label in enumerate(cases):
        r=subprocess.run([str(p/'probe'),str(i)],capture_output=True,text=True)
        print(label + ': ' + ('PASS' if r.returncode==0 else 'FAIL'))
        if r.stderr: print(r.stderr[:1500])
        if r.returncode:raise SystemExit(1)

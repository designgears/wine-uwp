#!/usr/bin/env python3
"""Exercise the production CoreWindow adapter with mock COM/Win32 calls.

Checks routing, reference balance and failed installation under ASan/UBSan.
The Windows probe covers the real DXVK vtable and D3D12 presentation separately.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'wine/dlls/wineuwp/dxgi.c').read_text()
source = source[source.index('static const GUID core_window_interop_iid'):]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define WINAPI
#define CALLBACK
#define TRUE 1
#define S_OK 0
#define E_NOTIMPL ((int32_t)0x80004001)
#define E_FAIL ((int32_t)0x80004005)
#define DXGI_ERROR_INVALID_CALL ((int32_t)0x887a0001)
#define FAILED(hr) ((hr)<0)
#define HRESULT_FROM_WIN32(e) ((int32_t)(0x80070000u|(e)))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define INIT_ONCE_STATIC_INIT 0
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define GET_MODULE_HANDLE_EX_FLAG_PIN 4
#define PAGE_READWRITE 4
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
typedef uint32_t DWORD;
typedef int32_t HRESULT;
typedef int BOOL, INIT_ONCE;
typedef wchar_t WCHAR;
typedef void *HMODULE, *HWND;
typedef struct {uint32_t a; uint16_t b,c; uint8_t d[8];} GUID;
typedef const GUID *REFIID;
typedef struct IUnknown IUnknown;
typedef struct {
    HRESULT (*query)(IUnknown *,REFIID,void **);
    unsigned (*addref)(IUnknown *);
    unsigned (*release)(IUnknown *);
    HRESULT (*get_hwnd)(IUnknown *, HWND *);
} unknown_vtbl;
struct IUnknown {const unknown_vtbl *lpVtbl;};
typedef struct {int unused;} DXGI_SWAP_CHAIN_DESC1;
typedef struct {int unused;} IDXGIOutput;
typedef struct {int unused;} IDXGISwapChain1;
typedef struct IDXGIFactory2 IDXGIFactory2;
typedef struct {
    HRESULT (*CreateSwapChainForCoreWindow)(IDXGIFactory2 *,IUnknown *,IUnknown *,const DXGI_SWAP_CHAIN_DESC1 *,IDXGIOutput *,IDXGISwapChain1 **);
    HRESULT (*CreateSwapChainForHwnd)(IDXGIFactory2 *,IUnknown *,HWND,const DXGI_SWAP_CHAIN_DESC1 *,const void *,IDXGIOutput *,IDXGISwapChain1 **);
    unsigned (*Release)(IDXGIFactory2 *);
} factory_vtbl;
struct IDXGIFactory2 {const factory_vtbl *lpVtbl;};
#define IUnknown_QueryInterface(o,i,p) ((o)->lpVtbl->query(o,i,p))
#define IUnknown_Release(o) ((o)->lpVtbl->release(o))
#define IDXGIFactory2_Release(o) ((o)->lpVtbl->Release(o))
#define IDXGIFactory2_CreateSwapChainForHwnd(o,...) ((o)->lpVtbl->CreateSwapChainForHwnd(o,__VA_ARGS__))
static GUID IID_IDXGIFactory2;
static HRESULT stub_core(IDXGIFactory2 *f,IUnknown *d,IUnknown *w,const DXGI_SWAP_CHAIN_DESC1 *s,IDXGIOutput *o,IDXGISwapChain1 **r) {return E_NOTIMPL;}
static HRESULT route_hwnd(IDXGIFactory2 *,IUnknown *,HWND,const DXGI_SWAP_CHAIN_DESC1 *,const void *,IDXGIOutput *,IDXGISwapChain1 **);
static unsigned release_factory(IDXGIFactory2 *);
static factory_vtbl table={stub_core,route_hwnd,release_factory};
static IDXGIFactory2 bootstrap={&table}, actual={&table};
static int failure, loads, releases, factory_refs, pinned, protection=2, protect_calls, calls;
static const wchar_t *setting=L"1";
static int module_id;
static HRESULT create_factory(REFIID iid,void **out) {
    assert(iid==&IID_IDXGIFactory2);
    if(failure==3)return E_FAIL;
    *out=&bootstrap; ++factory_refs; return S_OK;
}
static unsigned release_factory(IDXGIFactory2 *f) {assert(f==&bootstrap);return --factory_refs;}
static HMODULE LoadLibraryW(const WCHAR *name) {assert(!wcscmp(name,L"dxgi.dll"));++loads;return failure==1?NULL:&module_id;}
static void *GetProcAddress(HMODULE m,const char *name) {assert(m==&module_id && !strcmp(name,"CreateDXGIFactory1"));return failure==2?NULL:create_factory;}
static void FreeLibrary(HMODULE m) {assert(m==&module_id);++releases;}
static DWORD GetLastError(void) {return 5;}
static BOOL GetModuleHandleExW(DWORD flags,const WCHAR *address,HMODULE *out) {
    if(flags&GET_MODULE_HANDLE_EX_FLAG_PIN) {if(failure==5)return 0;pinned=1;}
    else if(failure==4)return 0;
    *out=&module_id;return 1;
}
static BOOL VirtualProtect(void *p,size_t n,DWORD requested,DWORD *previous) {
    assert(p==&table.CreateSwapChainForCoreWindow && n==sizeof(void *));
    ++protect_calls;
    if((failure==6 && protect_calls==1)||(failure==7 && protect_calls==2))return 0;
    *previous=protection;protection=requested;return 1;
}
static void *InterlockedExchangePointer(void **slot,void *p) {
    assert(protection==PAGE_READWRITE);void *old=*slot;*slot=p;return old;
}
static DWORD GetEnvironmentVariableW(const WCHAR *name,WCHAR *out,DWORD n) {
    assert(!wcscmp(name,L"WINE_UWP_DXVK_COREWINDOW") && n==2);
    size_t len=wcslen(setting);
    if(len<n)wcscpy(out,setting);
    return len<n?len:len+1;
}
static BOOL InitOnceExecuteOnce(INIT_ONCE *once,BOOL (*cb)(INIT_ONCE *,void *,void **),void *arg,void **ctx) {
    if(!*once){assert(cb(once,arg,ctx));*once=1;}return 1;
}
'''
suffix = r'''
static IUnknown core;
static IUnknown device;
static DXGI_SWAP_CHAIN_DESC1 description;
static IDXGIOutput output;
static IDXGISwapChain1 result;
static int core_refs=1, query_fail, handle_fail;
static HWND window=(void *)0x1234;
static HRESULT native_result=S_OK;
static HRESULT query(IUnknown *c,REFIID iid,void **out) {
    assert(c==&core && !memcmp(iid,&core_window_interop_iid,sizeof(GUID)));
    *out=NULL;if(query_fail)return E_NOTIMPL;
    ++core_refs;*out=&core;return S_OK;
}
static unsigned release(IUnknown *c) {assert(c==&core);return --core_refs;}
static HRESULT get_hwnd(IUnknown *c,HWND *out) {assert(c==&core);if(handle_fail)return E_FAIL;*out=window;return S_OK;}
static const unknown_vtbl core_table={query,NULL,release,get_hwnd};
static HRESULT route_hwnd(IDXGIFactory2 *f,IUnknown *d,HWND w,const DXGI_SWAP_CHAIN_DESC1 *s,const void *fullscreen,IDXGIOutput *o,IDXGISwapChain1 **r) {
    assert(f==&actual && d==&device && w==window && s==&description && !fullscreen && o==&output);
    ++calls;if(FAILED(native_result))return native_result;*r=&result;return S_OK;
}
int main(int argc,char **argv) {
    failure=atoi(argv[1]);core.lpVtbl=&core_table;
    if(failure>=8) setting=failure==8?L"":failure==9?L"0":L"true";
    assert(table.CreateSwapChainForCoreWindow(&actual,&device,&core,&description,&output,NULL)==E_NOTIMPL);
    HRESULT hr=__wine_uwp_prepare_dxgi();
    if(failure>=8) {assert(hr==S_OK && !loads && table.CreateSwapChainForCoreWindow==stub_core);return 0;}
    if(failure) {
        assert(FAILED(hr) && table.CreateSwapChainForCoreWindow==stub_core && !factory_refs && protection==2);
        assert(releases==(failure==1?0:1));return 0;
    }
    assert(hr==S_OK && pinned && !releases && !factory_refs && protection==2);
    assert(__wine_uwp_prepare_dxgi()==S_OK && loads==1 && protect_calls==2);
    IDXGISwapChain1 *swap=NULL;
#define CALL(d,w,s,r) table.CreateSwapChainForCoreWindow(&actual,d,w,s,&output,r)
    assert(CALL(&device,&core,&description,&swap)==S_OK && swap==&result && calls==1 && core_refs==1);
    swap=(void *)1;assert(CALL(NULL,&core,&description,&swap)==DXGI_ERROR_INVALID_CALL && !swap);
    swap=(void *)1;assert(CALL(&device,NULL,&description,&swap)==DXGI_ERROR_INVALID_CALL && !swap);
    swap=(void *)1;assert(CALL(&device,&core,NULL,&swap)==DXGI_ERROR_INVALID_CALL && !swap);
    assert(CALL(&device,&core,&description,NULL)==DXGI_ERROR_INVALID_CALL);
    query_fail=1;swap=(void *)1;assert(CALL(&device,&core,&description,&swap)==DXGI_ERROR_INVALID_CALL && !swap && core_refs==1);query_fail=0;
    handle_fail=1;assert(CALL(&device,&core,&description,&swap)==E_FAIL && !swap && core_refs==1);handle_fail=0;
    window=NULL;assert(CALL(&device,&core,&description,&swap)==DXGI_ERROR_INVALID_CALL && !swap && core_refs==1);window=(void *)0x1234;
    native_result=E_FAIL;assert(CALL(&device,&core,&description,&swap)==E_FAIL && !swap && core_refs==1 && calls==2);
    puts("routing, errors, reference balance and idempotent installation: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dxgi-corewindow-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(prefix + source + suffix)
    subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                    str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    for case in range(11):
        subprocess.run([str(directory / 'test'), str(case)], check=True)
print('7 failed installations and 3 disabled settings: PASS')

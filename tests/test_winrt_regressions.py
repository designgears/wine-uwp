#!/usr/bin/env python3
"""Host-only checks of extracted WinRT C code. Requires clang and AddressSanitizer.

Run: python3 tests/test_winrt_regressions.py [--ref REVISION]
Mocks replace COM and Wine scheduling. These checks do not establish Wine ABI,
DLL loading, filesystem behavior, or application compatibility.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ref', help='Read sources from a Git revision with the current file paths.')
args = parser.parse_args()


def source(path):
    if args.ref:
        return subprocess.check_output(['git', '--no-pager', 'show', f'{args.ref}:{path}'], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def function(text, name):
    match = re.search(r'^(?:static[^;{}]*?\b|(?:int|void) )' + re.escape(name) + r'\([^;{}]*?\)\s*\{', text, re.M)
    if not match:
        raise ValueError(f'Missing function: {name}')
    end = text.index('\n}', match.end()) + 2
    return text[match.start():end]


def structure(text, name):
    start = text.index('struct ' + name + '\n{')
    return text[start:text.index('\n};', start) + 3]


COMMON = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define WINAPI
#define TRACE(...) ((void)0)
#define FIXME(...) ((void)0)
#define S_OK 0
#define E_POINTER (-1)
#define E_OUTOFMEMORY (-2)
#define E_NOTIMPL (-3)
#define E_FAIL (-4)
#define STATUS_PENDING 259
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
typedef int HRESULT, BOOL, INT32, LONG;
typedef unsigned UINT32, UINT16;
#define CONTAINING_RECORD(a,t,f) ((t *)((char *)(a)-offsetof(t,f)))
'''

STORAGE = COMMON + r'''
typedef struct { int unused; } IUnknown, IStorageFileQueryResultInternal, IStorageQueryResultBase;
typedef void IVector_HSTRING, IVectorView_StorageFile, IStorageItem, IStorageFile, IStorageFolder, PROPVARIANT;
typedef void IAsyncOperation_IVectorView_StorageFile, IAsyncOperation_UINT32;
typedef wchar_t WCHAR;
typedef pthread_mutex_t SRWLOCK;
#define AcquireSRWLockShared pthread_mutex_lock
#define ReleaseSRWLockShared pthread_mutex_unlock
#define AcquireSRWLockExclusive pthread_mutex_lock
#define ReleaseSRWLockExclusive pthread_mutex_unlock
static int live_options, fail_schedule, block_getter, entered, resume_getter, observed;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
typedef struct { atomic_int refs; int payload; } IQueryOptionsInternal;
typedef IQueryOptionsInternal IQueryOptions;
static IQueryOptionsInternal *options_new(int payload) {
    IQueryOptionsInternal *p = malloc(sizeof(*p));
    atomic_init(&p->refs, 1); p->payload = payload; ++live_options; return p;
}
static void IQueryOptionsInternal_AddRef(IQueryOptionsInternal *p) { ++p->refs; }
static void IQueryOptionsInternal_Release(IQueryOptionsInternal *p) {
    if (--p->refs == 0) { --live_options; free(p); }
}
static HRESULT IQueryOptionsInternal_get_FileTypeFilter(IQueryOptionsInternal *p, IVector_HSTRING **out) {
    pthread_mutex_lock(&gate);
    entered = 1; pthread_cond_broadcast(&changed);
    while (block_getter && !resume_getter) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    observed = p->payload; *out = NULL; return S_OK;
}
static HRESULT IQueryOptionsInternal_get_FolderDepth(IQueryOptionsInternal *p, INT32 *out) {
    assert(p->payload == observed); *out = 0; return S_OK;
}
#define IVector_HSTRING_Release(p) ((void)0)
/* PRODUCTION_STRUCTS */
#define impl_from_IStorageFileQueryResultInternal(p) CONTAINING_RECORD(p,struct file_query_result,IStorageFileQueryResultInternal_iface)
#define impl_from_IStorageQueryResultBase_file(p) CONTAINING_RECORD(p,struct file_query_result,IStorageQueryResultBase_iface)
#define file_query_range_from_IUnknown(p) CONTAINING_RECORD(p,struct file_query_range,IUnknown_iface)
/* PRODUCTION_DESTROY */
static struct file_query_range *file_query_range_alloc(void) {
    struct file_query_range *p = calloc(1, sizeof(*p)); if (p) p->ref = 1; return p;
}
static void IUnknown_Release(IUnknown *p) {
    struct file_query_range *r = file_query_range_from_IUnknown(p);
    if (!--r->ref) file_query_range_destroy(r);
}
/* PRODUCTION_SNAPSHOT */
/* PRODUCTION_CALLBACK_PREFIX */
static HRESULT file_query_count(IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL async) {
    return query_result_files(invoker, param, result, async);
}
static IUnknown *pending_param, *pending_invoker;
static HRESULT (*pending_callback)(IUnknown *, IUnknown *, PROPVARIANT *, BOOL);
static HRESULT schedule(IUnknown *invoker, IUnknown *param,
    HRESULT (*callback)(IUnknown *, IUnknown *, PROPVARIANT *, BOOL), void **out) {
    if (fail_schedule) return E_FAIL;
    assert(callback(invoker, param, NULL, 0) == STATUS_PENDING);
    if (param) ++file_query_range_from_IUnknown(param)->ref;
    pending_param = param; pending_invoker = invoker; pending_callback = callback;
    *out = (void *)1; return S_OK;
}
#define async_operation_files_create schedule
#define async_operation_uint32_create schedule
/* PRODUCTION_METHODS */
static void *worker(void *unused) {
    assert(pending_callback(pending_invoker, pending_param, NULL, 1) == S_OK);
    return NULL;
}
int main(int argc, char **argv) {
    struct file_query_result query = {0};
    IQueryOptions *current = NULL;
    IQueryOptionsInternal *replacement = options_new(7);
    void *operation = NULL;
    pthread_t thread;
    int kind = atoi(argv[1]);
    block_getter = atoi(argv[2]);
    query.options = options_new(42);
    /* INIT_LOCK */
    fail_schedule = argc > 3;
    HRESULT hr;
    if (kind == 0) hr = file_query_GetFilesAsync(&query.IStorageFileQueryResultInternal_iface, 2, 3, &operation);
    else if (kind == 1) hr = file_query_GetFilesAsyncDefaultStartAndCount(&query.IStorageFileQueryResultInternal_iface, &operation);
    else hr = file_query_base_GetItemCountAsync(&query.IStorageQueryResultBase_iface, &operation);
    if (fail_schedule) { assert(hr == E_FAIL && !operation && query.options->refs == 1); }
    else {
        assert(hr == S_OK && operation);
        if (block_getter) {
            pthread_create(&thread, NULL, worker, NULL);
            pthread_mutex_lock(&gate);
            while (!entered) pthread_cond_wait(&changed, &gate);
            pthread_mutex_unlock(&gate);
        }
        assert(file_query_base_ApplyNewQueryOptions(&query.IStorageQueryResultBase_iface, replacement) == S_OK);
        assert(file_query_base_GetCurrentQueryOptions(&query.IStorageQueryResultBase_iface, &current) == S_OK);
        assert(current == replacement); IQueryOptionsInternal_Release(current);
        if (block_getter) {
            pthread_mutex_lock(&gate); resume_getter = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
            pthread_join(thread, NULL);
        } else worker(NULL);
        assert(observed == 42); /* The operation retains the options selected when it was requested. */
        if (pending_param) IUnknown_Release(pending_param);
        assert(live_options == 1);
        assert(file_query_base_ApplyNewQueryOptions(&query.IStorageQueryResultBase_iface, replacement) == S_OK);
        assert(replacement->refs == 2); /* Reapplying the same options preserves ownership. */
    }
    IQueryOptionsInternal_Release(query.options); IQueryOptionsInternal_Release(replacement);
    assert(!live_options);
    puts("storage ownership PASS"); return 0;
}
'''

PUSH = COMMON + r'''
typedef void IPushNotificationChannelManagerStatics, IAsyncOperation_PushNotificationChannel;
typedef void IPushNotificationChannel, IAsyncInfo;
typedef int AsyncStatus;
static int failures, mode; static void *module;
#define Started 0
#define Error 3
#define RO_INIT_MULTITHREADED 1
#define RoInitialize(x) 0
#define RoUninitialize() ((void)0)
#define LoadLibraryA(x) NULL
#define GetLastError() 0ul
static const int IID_IPushNotificationChannelManagerStatics = 0, IID_IAsyncInfo = 0;
#define WPN_E_PLATFORM_UNAVAILABLE (-9)
#define IAsyncInfo_get_Status(i,s) (*(s)=Error,S_OK)
#define IAsyncInfo_get_ErrorCode(i,e) (*(e)=WPN_E_PLATFORM_UNAVAILABLE,S_OK)
#define IAsyncInfo_Release(i) ((void)0)
#define IAsyncOperation_PushNotificationChannel_GetResults(o,c) (*(c)=NULL,WPN_E_PLATFORM_UNAVAILABLE)
#define IAsyncOperation_PushNotificationChannel_Release(o) ((void)0)
#define IPushNotificationChannelManagerStatics_Release(m) ((void)0)
#define IAsyncOperation_PushNotificationChannel_QueryInterface(o,i,p) (*(p)=mode==3?NULL:(void *)1,S_OK)
#define Sleep(x) ((void)0)
#define CHECK(x) do { if (!(x)) ++failures; } while (0)
static HRESULT get_factory(const wchar_t *name,const void *iid,void **out) {
    *out = mode==1?NULL:(void *)1; return S_OK;
}
static HRESULT IPushNotificationChannelManagerStatics_CreatePushNotificationChannelForApplicationAsync(void *manager, void **out) {
    *out = mode==2?NULL:(void *)1; return S_OK;
}
/* PRODUCTION_MAIN */
int main(int argc,char **argv) {
    mode = atoi(argv[1]);
    int result = probe_main(1, argv);
    assert((result != 0) == (mode != 0));
    return 0;
}
'''

storage_source = source('wine/dlls/windows.storage/storagefolder.c')
callback = function(storage_source, 'query_result_files')
# Exercise the actual ownership-sensitive prefix. Directory enumeration remains a Linux check.
callback = callback[:callback.index('    if (FAILED(hr = list_query_directory(')] + '    return S_OK;\n}'
methods = ['file_query_GetFilesAsync', 'file_query_GetFilesAsyncDefaultStartAndCount',
           'file_query_base_GetItemCountAsync', 'file_query_base_GetCurrentQueryOptions',
           'file_query_base_ApplyNewQueryOptions']
new_snapshot = 'file_query_get_options(' in storage_source
storage = STORAGE.replace('/* PRODUCTION_STRUCTS */', '\n'.join(structure(storage_source, n) for n in ['file_query_result', 'file_query_range']))
storage = storage.replace('/* PRODUCTION_DESTROY */', function(storage_source, 'file_query_range_destroy'))
storage = storage.replace('/* PRODUCTION_SNAPSHOT */', '\n'.join(function(storage_source, n) for n in ['file_query_get_options', 'file_query_range_create']) if new_snapshot else '')
storage = storage.replace('/* PRODUCTION_CALLBACK_PREFIX */', callback)
storage = storage.replace('/* PRODUCTION_METHODS */', '\n'.join(function(storage_source, n) for n in methods))
storage = storage.replace('/* INIT_LOCK */', 'pthread_mutex_init(&query.options_lock, NULL);' if new_snapshot else '')
push = PUSH.replace('/* PRODUCTION_MAIN */', function(source('probes/probe-push-notifications.c'), 'main').replace('int main(', 'int probe_main(', 1))

manager = source('wine/dlls/windows.gaming.input/manager.c')
hardware = COMMON + (ROOT / 'tests/controller_factories.c').read_text()
has_registration = 'struct hardware_factory_registration\n{' in manager
hardware = hardware.replace('/* PRODUCTION_STRUCTS */', structure(manager, 'controller') + (
    '\n' + structure(manager, 'hardware_factory_registration') + '\nstatic struct list hardware_factories = LIST_INIT(hardware_factories);'
    if has_registration else ''))
hardware = hardware.replace('/* PRODUCTION_REFS */', '\n'.join(function(manager, n) for n in [
    'controller_QueryInterface', 'controller_AddRef', 'controller_Release']))
hardware = hardware.replace('/* PRODUCTION_HELPERS */', '\n'.join(function(manager, n) for n in [
    'controller_snapshot', 'controller_get_public_iface', 'controller_matches_factory',
    'custom_controller_removed', 'hardware_factory_add_controller'] if n + '(' in manager))
hardware = hardware.replace('/* PRODUCTION_METHODS */', '\n'.join(function(manager, n) for n in [
    'statics_RegisterCustomFactoryForHardwareId', 'statics2_TryGetFactoryControllerFromGameController',
    'controller_create', 'manager_on_provider_created', 'manager_on_provider_removed']))
hardware = hardware.replace('/* RELEASE_REGISTRATIONS */', '''
    struct hardware_factory_registration *entry, *next;
    LIST_FOR_EACH_ENTRY_SAFE(entry,next,&hardware_factories,struct hardware_factory_registration,entry) {
        list_remove(&entry->entry); RELEASE_FACTORY; free(entry);
    }
'''.replace('RELEASE_FACTORY', 'IAgileReference_Release(entry->reference)' if 'IAgileReference *reference' in manager else 'ICustomGameControllerFactory_Release(entry->factory)') if has_registration else '')

pickers = source('wine/dlls/windows.storage.pickers/main.c')
folder_picker = COMMON + (ROOT / 'tests/folder_picker.c').read_text()
folder_picker = folder_picker.replace('/* PRODUCTION_METHODS */', '\n'.join(function(pickers, n) for n in [
    'dialogs_enabled', 'folder_picker_PickSingleFolderAsync']))

failures = 0
with tempfile.TemporaryDirectory(prefix='winrt-host-tests-') as directory:
    directory = Path(directory)
    for name, code, cases in [
        ('storage', storage, [(str(k), str(b)) for k in range(3) for b in range(2)] + [(str(k), '0', 'fail') for k in range(3)]),
        ('push', push, [(str(k),) for k in range(4)]),
        ('hardware', hardware, [(str(k),) for k in range(13)]),
        ('folder_picker', folder_picker, [(str(k),) for k in range(16)]),
    ]:
        path = directory / (name + '.c'); path.write_text(code)
        binary = directory / name
        build = subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-g', '-O1', '-pthread',
                                '-fsanitize=address,undefined', str(path), '-o', str(binary)], capture_output=True, text=True)
        if build.returncode:
            print(f'{name}: compile FAILED\n{build.stderr}'); failures += 1; continue
        for case in cases:
            run = subprocess.run([str(binary), *case], capture_output=True, text=True, timeout=15)
            print(f'{name} {" ".join(case)}: {"FAIL" if run.returncode else "PASS"}')
            if run.returncode:
                print((run.stdout + run.stderr)[:2200]); failures += 1
print(f'{failures} failed cases')
raise SystemExit(bool(failures))

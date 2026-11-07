/* Folder override API checks. Requires Wine's generated WinRT headers.
 * Default mode needs no interaction and creates only temporary fixtures.
 * An incorrect dialog fallback can block; use the external timeout below.
 * --dialog-cancel unsets both overrides: manually cancel the normal dialog.
 * Run in an isolated prefix with an external timeout. */
#define COBJMACROS
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Storage
#define WIDL_using_Windows_Storage_Pickers
#include <wchar.h>
#include <windows.h>
#include <initguid.h>
#include "storage-probe.h"
#include "windows.storage.pickers.h"

/* Escape UTF-16 units so a C-locale printf cannot hide the failing path. */
static void print_path(const WCHAR *path)
{
    for (; *path; ++path)
        if (*path>=32 && *path<127) putchar(*path);
        else printf("\\u%04x",(unsigned int)*path);
}
static void win32_failure(const char *call,const WCHAR *path)
{
    DWORD error=GetLastError();
    printf("FIXTURE_FAIL call=%s error=%lu path=",call,(unsigned long)error);
    print_path(path); putchar('\n');
}

static int check_pick(IFolderPicker *picker,const WCHAR *value,HRESULT expected,
                      const WCHAR *expected_path,const char *label)
{
    IAsyncOperation_StorageFolder *operation=NULL;
    IStorageFolder *folder=NULL;
    IStorageItem *item=NULL;
    HSTRING path=NULL;
    HRESULT hr;
    int good=0;

    if (!SetEnvironmentVariableW(L"WINE_UWP_PICKER_FOLDER",value) &&
        (value || GetLastError()!=ERROR_ENVVAR_NOT_FOUND)) return 0;
    hr=IFolderPicker_PickSingleFolderAsync(picker,&operation);
    if (FAILED(expected))
    {
        good=FAILED(hr) && !operation && (expected==E_FAIL || hr==expected);
        goto done;
    }
    if (FAILED(hr) || !operation) goto done;
    if (FAILED(hr=await_op((object *)operation))) goto done;
    if (FAILED(hr=IAsyncOperation_StorageFolder_GetResults(operation,&folder))) goto done;
    if (!expected_path) { good=!folder; goto done; }
    if (!folder) goto done;
    if (FAILED(hr=IStorageFolder_QueryInterface(folder,&IID_IStorageItem,(void **)&item))) goto done;
    if (FAILED(hr=IStorageItem_get_Path(item,&path))) goto done;
    good=!wcscmp(WindowsGetStringRawBuffer(path,NULL),expected_path);
 done:
    printf("FOLDER_PICKER_%s case=%s hr=%08lx folder=%d\n",good?"PASS":"FAIL",label,(unsigned long)hr,folder!=NULL);
    WindowsDeleteString(path);
    if (item) IStorageItem_Release(item);
    if (folder) IStorageFolder_Release(folder);
    if (operation) IAsyncOperation_StorageFolder_Release(operation);
    return good;
}

int main(int argc,char **argv)
{
    IInspectable *instance=NULL;
    IFolderPicker *picker=NULL;
    IVector_HSTRING *filter=NULL;
    WCHAR temp[MAX_PATH],root[MAX_PATH]=L"",selected[MAX_PATH]=L"",file[MAX_PATH]=L"",missing[MAX_PATH],oversized[MAX_PATH+1];
    HSTRING name=NULL,all=NULL;
    HANDLE handle;
    HRESULT hr;
    DWORD length;
    int good=0,created_root=0,created_selected=0,created_file=0,written;
    DWORD attributes;
    BOOL interactive=argc==2 && !strcmp(argv[1],"--dialog-cancel");

    if (argc>1 && !interactive) { puts("usage: probe-folder-picker.exe [--dialog-cancel]"); return 2; }
    if (FAILED(hr=RoInitialize(RO_INIT_MULTITHREADED))) return 1;
#define WIN32_FAIL(call,path) win32_failure(call,path)
#define TRY(call) do { hr=(call); if (FAILED(hr)) { printf("FAIL %s hr=%08lx\n",#call,(unsigned long)hr); goto done; } } while (0)
    TRY(WindowsCreateString(RuntimeClass_Windows_Storage_Pickers_FolderPicker,
        lstrlenW(RuntimeClass_Windows_Storage_Pickers_FolderPicker),&name));
    TRY(RoActivateInstance(name,&instance));
    TRY(IInspectable_QueryInterface(instance,&IID_IFolderPicker,(void **)&picker));
    TRY(IFolderPicker_get_FileTypeFilter(picker,&filter));
    TRY(WindowsCreateString(L"*",1,&all));
    TRY(IVector_HSTRING_Append(filter,all));
    if (interactive)
    {
        if (!SetEnvironmentVariableW(L"WINE_UWP_PICKER_NODIALOG",NULL) &&
            GetLastError()!=ERROR_ENVVAR_NOT_FOUND) goto done;
        puts("Cancel the folder dialog. Do not select a folder."); fflush(stdout);
        good=check_pick(picker,NULL,S_OK,NULL,"unset-dialog-cancel");
        goto done;
    }
    if (!SetEnvironmentVariableW(L"WINE_UWP_PICKER_NODIALOG",L"1")) goto done;
    length=GetTempPathW(MAX_PATH,temp);
    if (!length) { WIN32_FAIL("GetTempPathW",L""); goto done; }
    if (length>MAX_PATH-90)
    {
        printf("FIXTURE_FAIL call=GetTempPathW length=%lu capacity=%u\n",(unsigned long)length,MAX_PATH-90);
        goto done;
    }
    attributes=GetFileAttributesW(temp);
    if (attributes==INVALID_FILE_ATTRIBUTES) { WIN32_FAIL("GetFileAttributesW",temp); goto done; }
    if (!(attributes&FILE_ATTRIBUTE_DIRECTORY))
    {
        printf("FIXTURE_FAIL call=GetFileAttributesW path=%ls error=%u\n",temp,ERROR_DIRECTORY);
        goto done;
    }
    written=swprintf(root,MAX_PATH,L"%lsfolder-picker-%lu-%lu",temp,GetCurrentProcessId(),GetTickCount());
    if (written<0 || written>=MAX_PATH) { puts("FIXTURE_FAIL format=root"); goto done; }
    printf("FIXTURE root="); print_path(root); putchar('\n');
    if (!CreateDirectoryW(root,NULL)) { WIN32_FAIL("CreateDirectoryW",root); goto done; }
    created_root=1;
    written=swprintf(selected,MAX_PATH,L"%ls\\folder \x03a9",root);
    if (written<0 || written>=MAX_PATH) { puts("FIXTURE_FAIL format=selected"); goto done; }
    if (!CreateDirectoryW(selected,NULL)) { WIN32_FAIL("CreateDirectoryW",selected); goto done; }
    created_selected=1;
    written=swprintf(file,MAX_PATH,L"%ls\\file.bin",root);
    if (written<0 || written>=MAX_PATH) { puts("FIXTURE_FAIL format=file"); goto done; }
    handle=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (handle==INVALID_HANDLE_VALUE) { WIN32_FAIL("CreateFileW",file); goto done; }
    CloseHandle(handle); created_file=1;
    written=swprintf(missing,MAX_PATH,L"%ls\\missing",root);
    if (written<0 || written>=MAX_PATH) { puts("FIXTURE_FAIL format=missing"); goto done; }
    wmemset(oversized,L'x',MAX_PATH); oversized[MAX_PATH]=0;

    good=check_pick(picker,selected,S_OK,selected,"existing-unicode-folder");
    if (!SetEnvironmentVariableW(L"WINE_UWP_PICKER_NODIALOG",NULL)) { good=0; goto done; }
    good=check_pick(picker,root,S_OK,root,"override-with-dialogs-enabled") && good;
    good=check_pick(picker,missing,E_FAIL,NULL,"missing-folder") && good;
    good=check_pick(picker,file,HRESULT_FROM_WIN32(ERROR_DIRECTORY),NULL,"file") && good;
    good=check_pick(picker,L"relative",HRESULT_FROM_WIN32(ERROR_INVALID_NAME),NULL,"relative") && good;
    good=check_pick(picker,L"C:relative",HRESULT_FROM_WIN32(ERROR_INVALID_NAME),NULL,"drive-relative") && good;
    good=check_pick(picker,L"\\rooted",HRESULT_FROM_WIN32(ERROR_INVALID_NAME),NULL,"rooted") && good;
    good=check_pick(picker,oversized,HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE),NULL,"oversized") && good;
    if (!SetEnvironmentVariableW(L"WINE_UWP_PICKER_NODIALOG",L"1")) { good=0; goto done; }
    good=check_pick(picker,NULL,S_OK,NULL,"unset-no-dialog") && good;
    good=(IFolderPicker_PickSingleFolderAsync(picker,NULL)==E_POINTER) && good;
    good=(GetFileAttributesW(missing)==INVALID_FILE_ATTRIBUTES) && good;
 done:
    if (created_file && !DeleteFileW(file)) { WIN32_FAIL("DeleteFileW",file); good=0; }
    if (created_selected && !RemoveDirectoryW(selected)) { WIN32_FAIL("RemoveDirectoryW",selected); good=0; }
    if (created_root && !RemoveDirectoryW(root)) { WIN32_FAIL("RemoveDirectoryW",root); good=0; }
    WindowsDeleteString(all); WindowsDeleteString(name);
    if (filter) IVector_HSTRING_Release(filter);
    if (picker) IFolderPicker_Release(picker);
    if (instance) IInspectable_Release(instance);
    RoUninitialize();
    printf("FOLDER_PICKER_%s\n",good?"PASS":"FAIL");
    return good?0:1;
}

/* Host checks of the real folder-picker method with mocked Win32/COM calls.
 * Dialog routing is observable here; filesystem and ABI checks need the API probe. */
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_PATH 260
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_NAME 123
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_FILENAME_EXCED_RANGE 206
#define ERROR_DIRECTORY 267
#define HRESULT_FROM_WIN32(e) (-(int)(e))
typedef wchar_t WCHAR;
typedef unsigned DWORD;
typedef void IFolderPicker, IUnknown;
typedef struct { int refs; } IStorageFolder;
typedef struct { IStorageFolder *folder; } IAsyncOperation_StorageFolder;
static IStorageFolder selected;
static IAsyncOperation_StorageFolder completed;
static const WCHAR *override, *nodialog;
static DWORD last_error;
static int dialog_calls, storage_calls, async_calls, dialog_accept=1, async_fail;
static HRESULT storage_error;
static const WCHAR valid[] = L"C:\\probe folder Ω";
static void SetLastError(DWORD error) { last_error=error; }
static DWORD GetLastError(void) { return last_error; }
static DWORD GetEnvironmentVariableW(const WCHAR *name,WCHAR *buffer,DWORD size)
{
    const WCHAR *value=!wcscmp(name,L"WINE_UWP_PICKER_FOLDER")?override:nodialog;
    if (!value) { last_error=ERROR_ENVVAR_NOT_FOUND; return 0; }
    DWORD length=wcslen(value);
    if (length>=size) return length+1;
    wcscpy(buffer,value); return length;
}
static int folder_dialog(WCHAR *path)
{
    ++dialog_calls;
    if (dialog_accept) wcscpy(path,valid);
    return dialog_accept;
}
static HRESULT storage_folder_from_path(const WCHAR *path,IStorageFolder **folder)
{
    ++storage_calls;
    assert(!wcscmp(path,override?override:valid));
    *folder=NULL;
    if (storage_error) return storage_error;
    ++selected.refs; *folder=&selected; return S_OK;
}
static void IStorageFolder_Release(IStorageFolder *folder) { assert(--folder->refs>=0); }
#define return_result NULL
static HRESULT async_operation_folder_create(IUnknown *iface,IUnknown *param,void *callback,IAsyncOperation_StorageFolder **out)
{
    ++async_calls; *out=NULL;
    if (async_fail) return E_OUTOFMEMORY;
    completed.folder=(IStorageFolder *)param;
    if (completed.folder) ++completed.folder->refs;
    *out=&completed; return S_OK;
}
/* PRODUCTION_METHODS */
int main(int argc,char **argv)
{
    int kind=atoi(argv[1]),expected_dialog=0,expected_folder=0;
    HRESULT expected=S_OK;
    WCHAR oversized[MAX_PATH+1];
    IAsyncOperation_StorageFolder *operation=(void *)1;
    override=valid;
    switch (kind)
    {
    case 0: expected_folder=1; break;
    case 1: nodialog=L"1"; expected_folder=1; break;
    case 2: override=L"C:\\missing"; expected=storage_error=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); break;
    case 3: override=L"C:\\a-file"; expected=storage_error=HRESULT_FROM_WIN32(ERROR_DIRECTORY); break;
    case 4: override=L"relative"; expected=HRESULT_FROM_WIN32(ERROR_INVALID_NAME); break;
    case 5: override=L"C:relative"; expected=HRESULT_FROM_WIN32(ERROR_INVALID_NAME); break;
    case 6: override=L"\\rooted"; expected=HRESULT_FROM_WIN32(ERROR_INVALID_NAME); break;
    case 7: override=L""; expected=HRESULT_FROM_WIN32(ERROR_INVALID_NAME); break;
    case 8:
        wmemset(oversized,L'a',MAX_PATH); oversized[MAX_PATH]=0;
        override=oversized; expected=HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE); break;
    case 9: override=NULL; expected_dialog=expected_folder=1; break;
    case 10: override=NULL; dialog_accept=0; expected_dialog=1; break;
    case 11: override=NULL; nodialog=L"1"; break;
    case 12:
        assert(folder_picker_PickSingleFolderAsync(NULL,NULL)==E_POINTER);
        assert(!dialog_calls && !storage_calls && !async_calls); return 0;
    case 13: expected=storage_error=HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); break;
    case 14: async_fail=1; expected=E_OUTOFMEMORY; break;
    case 15: override=L"\\\\server\\share\\folder"; expected_folder=1; break;
    default: abort();
    }
    assert(folder_picker_PickSingleFolderAsync(NULL,&operation)==expected);
    assert(dialog_calls==expected_dialog);
    if (FAILED(expected)) assert(!operation);
    else
    {
        assert(operation==&completed && !!operation->folder==expected_folder);
        if (operation->folder) IStorageFolder_Release(operation->folder);
    }
    if (FAILED(expected) && !async_fail) assert(!async_calls);
    if (kind>=4 && kind<=8) assert(!storage_calls);
    assert(!selected.refs);
    puts("folder picker routing PASS"); return 0;
}

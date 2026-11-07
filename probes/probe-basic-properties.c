#include "storage-probe.h"
static const GUID files_iid={0x5984c710,0xdaf2,0x43c8,{0x8b,0xb4,0xa4,0xd3,0xea,0xcf,0xd0,0x3f}};

static const GUID folder_iid={0x08f327ff,0x85d5,0x48b9,{0xae,0xe9,0x28,0x51,0x1e,0x33,0x9f,0x9f}};
static const GUID item_iid={0x4207a996,0xca2f,0x42f7,{0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30}};
static const GUID props_iid={0xd05d55db,0x785e,0x4a66,{0xbe,0x02,0x9b,0xee,0xc5,0x8a,0xea,0x81}};
static int check(const WCHAR *path,int folder){object *f=NULL,*op=NULL,*storage=NULL,*item=NULL,*props=NULL,*typed=NULL;HSTRING name=NULL;HRESULT hr=E_FAIL;UINT64 size=~(UINT64)0;INT64 modified=0;WIN32_FILE_ATTRIBUTE_DATA attr;int ok=0;
if(!GetFileAttributesExW(path,GetFileExInfoStandard,&attr))goto done;
if(FAILED(factory(folder?L"Windows.Storage.StorageFolder":L"Windows.Storage.StorageFile",folder?&folder_iid:&files_iid,&f)))goto done;
WindowsCreateString(path,lstrlenW(path),&name);hr=((HRESULT(WINAPI*)(object*,HSTRING,object**))f->vtable[6])(f,name,&op);if(FAILED(hr)||FAILED(await_op(op)))goto done;
hr=((HRESULT(WINAPI*)(object*,object**))op->vtable[8])(op,&storage);release(op);op=NULL;if(FAILED(hr)||FAILED(qi(storage,&item_iid,&item)))goto done;
hr=((HRESULT(WINAPI*)(object*,object**))item->vtable[10])(item,&op);printf("BASIC_PROPERTIES folder=%d hr=%08lx operation=%d\n",folder,(unsigned long)hr,op!=NULL);if(FAILED(hr)||FAILED(await_op(op)))goto done;
hr=((HRESULT(WINAPI*)(object*,object**))op->vtable[8])(op,&props);if(FAILED(hr)||!props||FAILED(qi(props,&props_iid,&typed)))goto done;
if(FAILED(((HRESULT(WINAPI*)(object*,UINT64*))props->vtable[6])(props,&size)))goto done;
if(FAILED(((HRESULT(WINAPI*)(object*,INT64*))props->vtable[7])(props,&modified)))goto done;
if(size!=(folder?0:(((UINT64)attr.nFileSizeHigh<<32)|attr.nFileSizeLow)))goto done;
if(modified!=(INT64)(((UINT64)attr.ftLastWriteTime.dwHighDateTime<<32)|attr.ftLastWriteTime.dwLowDateTime))goto done;
printf("BASIC_PROPERTIES_PASS folder=%d size=%llu modified_matches_win32=1\n",folder,(unsigned long long)size);ok=1;
done:release(typed);release(props);release(item);release(storage);release(op);release(f);if(name)WindowsDeleteString(name);return ok;}
int main(void){WCHAR folder[MAX_PATH],path[MAX_PATH];BYTE data[257];DWORD written;HANDLE h;int good_file,good_folder;RoInitialize(RO_INIT_MULTITHREADED);GetFullPathNameW(L"fileprops-probe-only",MAX_PATH,folder,NULL);CreateDirectoryW(folder,NULL);swprintf(path,MAX_PATH,L"%ls\\data.bin",folder);h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);if(h==INVALID_HANDLE_VALUE)return 2;memset(data,0x5a,sizeof(data));WriteFile(h,data,sizeof(data),&written,NULL);CloseHandle(h);good_file=check(path,0);good_folder=check(folder,1);DeleteFileW(path);RemoveDirectoryW(folder);RoUninitialize();return good_file&&good_folder?0:1;}

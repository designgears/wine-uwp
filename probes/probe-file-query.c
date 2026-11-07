#include "storage-probe.h"
static const GUID folder_iid={0x08f327ff,0x85d5,0x48b9,{0xae,0xe9,0x28,0x51,0x1e,0x33,0x9f,0x9f}};
static const GUID query_iid={0xcb43ccc9,0x446b,0x4a4f,{0xbe,0x97,0x75,0x77,0x71,0xbe,0x52,0x03}};
static const GUID item_iid={0x4207a996,0xca2f,0x42f7,{0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30}};
static int check_query(const WCHAR *path,UINT32 expected,INT32 kind,UINT32 start,UINT32 limit,BOOL ranged){
 object *f=NULL,*op=NULL,*folder=NULL,*queries=NULL,*query=NULL,*view=NULL;
 HSTRING name=NULL; HRESULT hr=E_FAIL; UINT32 count=0; int ok=0; const char *phase="factory";
 if(FAILED(hr=factory(L"Windows.Storage.StorageFolder",&folder_iid,&f)))goto done;
 WindowsCreateString(path,lstrlenW(path),&name);phase="folder_request";
 if(FAILED(hr=((HRESULT(WINAPI*)(object*,HSTRING,object**))f->vtable[6])(f,name,&op)))goto done;
 phase="folder_wait";if(FAILED(hr=await_op(op)))goto done;
 phase="folder_result";if(FAILED(hr=((HRESULT(WINAPI*)(object*,object**))op->vtable[8])(op,&folder)))goto done;
 release(op);op=NULL;phase="query_interface";if(FAILED(hr=qi(folder,&query_iid,&queries)))goto done;
 phase="CreateFileQuery_1";hr=((HRESULT(WINAPI*)(object*,INT32,object**))queries->vtable[8])(queries,kind,&query);
 printf("FILE_QUERY_CREATE expected=%u hr=%08lx query=%d\n",expected,(unsigned long)hr,query!=NULL);
 if(FAILED(hr)||!query)goto done;
 phase="GetFilesAsync";
 if(ranged)hr=((HRESULT(WINAPI*)(object*,UINT32,UINT32,object**))query->vtable[6])(query,start,limit,&op);
 else hr=((HRESULT(WINAPI*)(object*,object**))query->vtable[7])(query,&op);
 if(FAILED(hr))goto done;
 phase="query_wait";if(FAILED(hr=await_op(op)))goto done;
 phase="query_result";if(FAILED(hr=((HRESULT(WINAPI*)(object*,object**))op->vtable[8])(op,&view)))goto done;
 phase="count";if(FAILED(hr=((HRESULT(WINAPI*)(object*,UINT32*))view->vtable[7])(view,&count)))goto done;
 printf("FILE_QUERY_COUNT expected=%u actual=%u\n",expected,count);
 ok=count==expected;
 for(UINT32 i=0;i<count;i++){
  object *file=NULL,*item=NULL;HSTRING filename=NULL;
  hr=((HRESULT(WINAPI*)(object*,UINT32,object**))view->vtable[6])(view,i,&file);
  if(SUCCEEDED(hr))hr=qi(file,&item_iid,&item);
  if(SUCCEEDED(hr))hr=((HRESULT(WINAPI*)(object*,HSTRING*))item->vtable[11])(item,&filename);
  if(SUCCEEDED(hr)){
   const WCHAR *text=WindowsGetStringRawBuffer(filename,NULL);
   printf("FILE_QUERY_FILE index=%u name=%ls\n",i,text);
   if(kind==1&&wcscmp(text,(i+start)==0?L"a.bin":L"b.bin"))ok=0;
   if(kind==0&&wcscmp(text,L"b.bin"))ok=0;
  }else ok=0;
  if(filename)WindowsDeleteString(filename);release(item);release(file);
 }
 phase="complete";
 done:printf("FILE_QUERY_%s expected=%u phase=%s hr=%08lx\n",ok?"PASS":"FAIL",expected,phase,(unsigned long)hr);
 release(view);release(query);release(queries);release(folder);release(op);release(f);if(name)WindowsDeleteString(name);return ok;
}
static int putfile(const WCHAR *path){HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);DWORD n=0;if(h==INVALID_HANDLE_VALUE)return 0;BOOL ok=WriteFile(h,"abc",3,&n,NULL);CloseHandle(h);return ok&&n==3;}
int main(void){
 WCHAR root[MAX_PATH],nested[MAX_PATH],empty[MAX_PATH],first[MAX_PATH],second[MAX_PATH],name[80];int good=0;
 swprintf(name,80,L"file-query-probe-%lu-%lu",GetCurrentProcessId(),GetTickCount());GetFullPathNameW(name,MAX_PATH,root,NULL);
 if(!CreateDirectoryW(root,NULL))return 2;
 swprintf(nested,MAX_PATH,L"%ls\\nested",root);swprintf(empty,MAX_PATH,L"%ls\\empty",root);
 swprintf(first,MAX_PATH,L"%ls\\b.bin",root);swprintf(second,MAX_PATH,L"%ls\\a.bin",nested);
 if(!CreateDirectoryW(nested,NULL)||!CreateDirectoryW(empty,NULL)||!putfile(first)||!putfile(second))goto done;
 RoInitialize(RO_INIT_MULTITHREADED);good=check_query(root,2,1,0,0,FALSE);
 good=check_query(empty,0,1,0,0,FALSE)&&good;
 good=check_query(root,1,1,1,1,TRUE)&&good;
 good=check_query(root,0,1,2,1,TRUE)&&good;
 good=check_query(root,0,1,0,0,TRUE)&&good;
 good=check_query(root,1,0,0,0,FALSE)&&good;RoUninitialize();
 done:DeleteFileW(second);DeleteFileW(first);RemoveDirectoryW(empty);RemoveDirectoryW(nested);RemoveDirectoryW(root);return good?0:1;
}

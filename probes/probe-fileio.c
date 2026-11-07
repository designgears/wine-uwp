#include "storage-probe.h"
static const GUID fileio_iid={0x887411eb,0x7f54,0x4732,{0xa5,0xf0,0x5e,0x43,0xe3,0xb8,0xc2,0xf5}};
static const GUID files_iid={0x5984c710,0xdaf2,0x43c8,{0x8b,0xb4,0xa4,0xd3,0xea,0xcf,0xd0,0x3f}};
static const GUID buffer_iid={0x71af914d,0xc10f,0x484b,{0xbc,0x50,0x14,0xbc,0x62,0x3b,0x3a,0x27}};
static const GUID bytes_iid={0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static HRESULT write_buffer(object *io,object *file,object *bf,const BYTE *data,UINT32 len){object *b=NULL,*access=NULL,*op=NULL;BYTE *raw=NULL;HRESULT hr;hr=((HRESULT(WINAPI*)(object*,UINT32,object**))bf->vtable[6])(bf,len+16,&b);if(FAILED(hr))return hr;if(SUCCEEDED(hr=qi(b,&bytes_iid,&access))){hr=((HRESULT(WINAPI*)(object*,BYTE**))access->vtable[3])(access,&raw);if(SUCCEEDED(hr)){memset(raw,0xcc,len+16);if(len)memcpy(raw,data,len);}release(access);}if(SUCCEEDED(hr))hr=((HRESULT(WINAPI*)(object*,UINT32))b->vtable[8])(b,len);if(SUCCEEDED(hr))hr=((HRESULT(WINAPI*)(object*,object*,object*,object**))io->vtable[19])(io,file,b,&op);release(b);printf("FILEIO_WRITE length=%u hr=%08lx operation=%d\n",len,(unsigned long)hr,op!=NULL);fflush(stdout);if(SUCCEEDED(hr))hr=await_op(op);if(SUCCEEDED(hr))hr=((HRESULT(WINAPI*)(object*))op->vtable[8])(op);release(op);return hr;}
static int check_bytes(const WCHAR *path,const BYTE *expected,UINT32 len){BYTE data[512];DWORD got=0;HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;BOOL ok=ReadFile(h,data,sizeof(data),&got,NULL);CloseHandle(h);return ok&&got==len&&!memcmp(data,expected,len);}
int main(void){object *io=NULL,*sf=NULL,*bf=NULL,*file=NULL,*op=NULL,*readbuf=NULL,*access=NULL;BYTE data[257],*raw=NULL;WCHAR path[MAX_PATH];HSTRING name=NULL;HRESULT hr;UINT32 len=0;int rc=1;HANDLE h;
RoInitialize(RO_INIT_MULTITHREADED);GetFullPathNameW(L"fileio-probe-only.bin",MAX_PATH,path,NULL);h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);if(h==INVALID_HANDLE_VALUE)return 2;CloseHandle(h);
if(FAILED(factory(L"Windows.Storage.FileIO",&fileio_iid,&io))||FAILED(factory(L"Windows.Storage.StorageFile",&files_iid,&sf))||FAILED(factory(L"Windows.Storage.Streams.Buffer",&buffer_iid,&bf)))goto done;
WindowsCreateString(path,lstrlenW(path),&name);hr=((HRESULT(WINAPI*)(object*,HSTRING,object**))sf->vtable[6])(sf,name,&op);if(FAILED(hr)||FAILED(await_op(op)))goto done;hr=((HRESULT(WINAPI*)(object*,object**))op->vtable[8])(op,&file);release(op);op=NULL;if(FAILED(hr))goto done;
for(unsigned i=0;i<sizeof(data);i++)data[i]=(BYTE)(i*37);
hr=write_buffer(io,file,bf,data,sizeof(data));if(FAILED(hr)||!check_bytes(path,data,sizeof(data)))goto done;
hr=((HRESULT(WINAPI*)(object*,object*,object**))io->vtable[18])(io,file,&op);if(FAILED(hr)||FAILED(await_op(op)))goto done;hr=((HRESULT(WINAPI*)(object*,object**))op->vtable[8])(op,&readbuf);release(op);op=NULL;if(FAILED(hr)||!readbuf)goto done;
if(FAILED(((HRESULT(WINAPI*)(object*,UINT32*))readbuf->vtable[7])(readbuf,&len))||len!=sizeof(data)||FAILED(qi(readbuf,&bytes_iid,&access)))goto done;
if(FAILED(((HRESULT(WINAPI*)(object*,BYTE**))access->vtable[3])(access,&raw))||memcmp(raw,data,len))goto done;
release(access);access=NULL;release(readbuf);readbuf=NULL;
if(FAILED(write_buffer(io,file,bf,data,5))||!check_bytes(path,data,5))goto done;
if(FAILED(write_buffer(io,file,bf,data,0))||!check_bytes(path,data,0))goto done;
hr=((HRESULT(WINAPI*)(object*,object*,UINT32,BYTE*,object**))io->vtable[20])(io,file,9,data,&op);
printf("FILEIO_WRITE_BYTES hr=%08lx operation=%d\n",(unsigned long)hr,op!=NULL);
if(FAILED(hr)||FAILED(await_op(op)))goto done;
hr=((HRESULT(WINAPI*)(object*))op->vtable[8])(op);release(op);op=NULL;
if(FAILED(hr)||!check_bytes(path,data,9))goto done;
if(FAILED(write_buffer(io,file,bf,data,0)))goto done;
SetFileAttributesW(path,FILE_ATTRIBUTE_READONLY);hr=write_buffer(io,file,bf,data,5);SetFileAttributesW(path,FILE_ATTRIBUTE_NORMAL);if(SUCCEEDED(hr)||!check_bytes(path,data,0))goto done;
printf("FILEIO_PASS bits=%u binary_roundtrip=257 truncate=5 empty=0 readonly_error=%08lx\n",(unsigned)(8*sizeof(void*)),(unsigned long)hr);rc=0;
done:release(access);release(readbuf);release(op);release(file);release(bf);release(sf);release(io);if(name)WindowsDeleteString(name);SetFileAttributesW(path,FILE_ATTRIBUTE_NORMAL);DeleteFileW(path);RoUninitialize();return rc;}

#define COBJMACROS
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <stdio.h>
typedef struct object { void **vtable; } object;
static const GUID statics_iid = {0x9c06ee5f,0x15c0,0x4e72,{0x93,0x30,0xd6,0x19,0x1c,0xeb,0xd1,0x9c}};
static const GUID info_iid = {0x36,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID view_iid = {0x971c3ea6,0x4388,0x5a38,{0xae,0x13,0x49,0x29,0xb6,0xd6,0xd7,0x80}};
static const GUID iterable_iid = {0x6b076c51,0x849e,0x5ec5,{0xae,0xd5,0x9b,0x05,0x85,0x59,0x19,0x02}};
static void release(object *p) { if (p) ((ULONG(WINAPI*)(object*))p->vtable[2])(p); }
static HRESULT qi(object *p, const GUID *iid, object **out) { return ((HRESULT(WINAPI*)(object*,const GUID*,void**))p->vtable[0])(p,iid,(void**)out); }
int main(void) {
    HSTRING name = NULL;
    object *factory=NULL, *context=NULL, *operation=NULL, *info=NULL, *view=NULL, *typed=NULL, *iterable=NULL, *iterator=NULL, *item=NULL;
    HRESULT hr; UINT32 size=99; INT32 status=0; unsigned char has_current=1;
    RoInitialize(RO_INIT_MULTITHREADED);
    const WCHAR *class_name=L"Windows.Services.Store.StoreContext";
    WindowsCreateString(class_name,lstrlenW(class_name),&name);
    hr=RoGetActivationFactory(name,&statics_iid,(void**)&factory);
    if (FAILED(hr)) { printf("STORE_FACTORY hr=%08lx\n",(unsigned long)hr); return 1; }
    hr=((HRESULT(WINAPI*)(object*,object**))factory->vtable[6])(factory,&context);
    if (FAILED(hr)) return 2;
    hr=((HRESULT(WINAPI*)(object*,object**))context->vtable[23])(context,&operation);
    printf("STORE_UPDATES hr=%08lx present=%d\n",(unsigned long)hr,operation!=NULL); fflush(stdout);
    if (FAILED(hr) || !operation) return 3;
    if (FAILED(qi(operation,&info_iid,&info))) return 4;
    for (int i=0;i<100 && status==0;i++) {
        hr=((HRESULT(WINAPI*)(object*,INT32*))info->vtable[7])(info,&status);
        if (FAILED(hr)) return 5;
        if (status==0) Sleep(10);
    }
    if (status!=1) return 6;
    hr=((HRESULT(WINAPI*)(object*,object**))operation->vtable[8])(operation,&view);
    if (FAILED(hr) || !view || FAILED(qi(view,&view_iid,&typed))) return 7;
    hr=((HRESULT(WINAPI*)(object*,UINT32*))view->vtable[7])(view,&size);
    if (FAILED(hr) || size) return 8;
    if (FAILED(qi(view,&iterable_iid,&iterable))) return 9;
    hr=((HRESULT(WINAPI*)(object*,object**))iterable->vtable[6])(iterable,&iterator);
    if (FAILED(hr) || !iterator) return 10;
    hr=((HRESULT(WINAPI*)(object*,unsigned char*))iterator->vtable[7])(iterator,&has_current);
    if (FAILED(hr) || has_current) return 11;
    hr=((HRESULT(WINAPI*)(object*,UINT32,object**))view->vtable[6])(view,0,&item);
    if (hr!=(HRESULT)0x8000000b || item) return 12;
    printf("STORE_UPDATES_PASS status=%d count=%u iterable_empty=1\n",status,size);
    release(iterator); release(iterable); release(typed); release(view); release(info); release(operation); release(context); release(factory);
    WindowsDeleteString(name); RoUninitialize(); return 0;
}

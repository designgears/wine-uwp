/* Push-notification checks from BedrockNative/WineGDK b532683, tools/uwp/platform-probe.c.
 * LGPL-2.1-or-later. Only direct factory loading is adapted for this probe.
 */
#define COBJMACROS
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Networking_PushNotifications
#include <stdio.h>
#include "windef.h"
#include "winbase.h"
#include "roapi.h"
#include "winstring.h"
#include "initguid.h"
#include "windows.networking.pushnotifications.h"
static int failures;
static HMODULE module;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)
static HRESULT get_factory(const WCHAR *name,REFIID iid,void **out)
{
 HSTRING str=NULL; IActivationFactory *factory=NULL;
 HRESULT (WINAPI *get)(HSTRING,IActivationFactory **)=module?(void *)GetProcAddress(module,"DllGetActivationFactory"):NULL;
 HRESULT hr=WindowsCreateString(name,wcslen(name),&str);
 if(SUCCEEDED(hr)) {
  if(module) {
   hr=get?get(str,&factory):E_NOINTERFACE;
   if(SUCCEEDED(hr)) {
    if(!factory) hr=E_UNEXPECTED;
    else {hr=IActivationFactory_QueryInterface(factory,iid,out);IActivationFactory_Release(factory);}
   }
  } else hr=RoGetActivationFactory(str,iid,out);
  WindowsDeleteString(str);
 }
 printf("PUSH_FACTORY hr=%08lx\n",hr);return hr;
}
int main(int argc,char **argv)
{
 CHECK(SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED)));
 if(argc>1) {module=LoadLibraryA(argv[1]);if(!module){printf("PUSH_LOAD_FAIL %lu\n",GetLastError());return 2;}}
 {
  IPushNotificationChannelManagerStatics *manager=NULL; IAsyncOperation_PushNotificationChannel *op=NULL;
  IPushNotificationChannel *channel=NULL; IAsyncInfo *info=NULL; AsyncStatus status=Started; HRESULT error=S_OK;
  CHECK(get_factory(L"Windows.Networking.PushNotifications.PushNotificationChannelManager",&IID_IPushNotificationChannelManagerStatics,(void **)&manager)==S_OK);
  CHECK(manager != NULL);
  if(manager) {
   CHECK(IPushNotificationChannelManagerStatics_CreatePushNotificationChannelForApplicationAsync(manager,&op)==S_OK);
   CHECK(op != NULL);
   if(op) {
    CHECK(IAsyncOperation_PushNotificationChannel_QueryInterface(op,&IID_IAsyncInfo,(void **)&info)==S_OK);
    CHECK(info != NULL);
    if(info) {
     for(int i=0;i<100 && status==Started;i++) { CHECK(IAsyncInfo_get_Status(info,&status)==S_OK); if(status==Started) Sleep(10); }
     CHECK(status==Error); CHECK(IAsyncInfo_get_ErrorCode(info,&error)==S_OK && error==WPN_E_PLATFORM_UNAVAILABLE);
     IAsyncInfo_Release(info);
    }
    CHECK(IAsyncOperation_PushNotificationChannel_GetResults(op,&channel)==WPN_E_PLATFORM_UNAVAILABLE && !channel);
    IAsyncOperation_PushNotificationChannel_Release(op);
   }
   IPushNotificationChannelManagerStatics_Release(manager);
  }
 }
 RoUninitialize(); printf("PUSH_ASYNC_%s failures=%d\n",failures?"FAIL":"PASS",failures); return !!failures;
}

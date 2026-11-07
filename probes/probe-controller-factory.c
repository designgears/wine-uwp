/* Public custom-controller registration and dispatch probe.
 * Build with Wine's generated WinRT headers, -lole32 -lruntimeobject -luuid.
 * Run without arguments for registration, or --controller for an already
 * connected controller. Exit 77 means no controller; it is not a pass.
 * --sta-hotplug VID PID and --agile-hotplug VID PID wait for an external
 * neutral device fixture, then check callback apartments and MTA lookup.
 * This probe neither reads nor injects controller input. */
#define COBJMACROS
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Gaming_Input
#define WIDL_using_Windows_Gaming_Input_Custom
#define WIDL_using_Windows_System
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windows.h"
#include "initguid.h"
#include "objbase.h"
#include "roapi.h"
#include "winstring.h"
#include "windows.gaming.input.h"
#include "windows.gaming.input.custom.h"

static const GUID private_impl_iid={0x06e58977,0x7684,0x4dc5,{0xba,0xd1,0xcd,0xa5,0x2a,0x4a,0xa0,0x6d}};
static LONG failures, factory_refs=1, created, added, removed, private_queries, wireless_calls;
static BOOL hotplug, agile=TRUE;
static DWORD owner_thread;
static UINT16 registered_vendor, registered_product;
static HANDLE added_event, removed_event;
static IGameControllerFactoryManagerStatics *callback_manager;
static IAgileReference *test_controller_ref, *test_factory_ref;
static ICustomGameControllerFactory factory;
static void reenter_manager(IGameController *value,BOOL present);
#define CHECK(x) do { if (!(x)) { printf("FAIL line=%d %s\n",__LINE__,#x); InterlockedIncrement(&failures); } } while (0)
#define CALL(x) do { hr=(x); if (FAILED(hr)) { printf("FAIL line=%d %s hr=%08lx\n",__LINE__,#x,(unsigned long)hr); InterlockedIncrement(&failures); goto done; } } while (0)

static void check_apartment(const char *where)
{
    APTTYPE type;
    APTTYPEQUALIFIER qualifier;
    HRESULT hr;
    if (!hotplug) return;
    hr=CoGetApartmentType(&type,&qualifier);
    CHECK(SUCCEEDED(hr));
    if (SUCCEEDED(hr))
    {
        CHECK(qualifier!=APTTYPEQUALIFIER_IMPLICIT_MTA);
        if (!agile) CHECK(type==APTTYPE_STA || type==APTTYPE_MAINSTA);
    }
    if (!agile) CHECK(GetCurrentThreadId()==owner_thread);
    printf("CALLBACK where=%s thread=%lu owner=%lu hr=%08lx\n",where,
           (unsigned long)GetCurrentThreadId(),(unsigned long)owner_thread,(unsigned long)hr);
    fflush(stdout);
}

struct controller { IGameController iface; LONG refs; };
static struct controller *impl_from_controller(IGameController *iface)
{
    return CONTAINING_RECORD(iface,struct controller,iface);
}
static ULONG WINAPI controller_addref(IGameController *iface) { return InterlockedIncrement(&impl_from_controller(iface)->refs); }
static ULONG WINAPI controller_release(IGameController *iface)
{
    struct controller *impl=impl_from_controller(iface);
    ULONG refs=InterlockedDecrement(&impl->refs);
    if (!refs) free(impl);
    return refs;
}
static HRESULT WINAPI controller_query(IGameController *iface,REFIID iid,void **out)
{
    *out=NULL;
    if (!agile && IsEqualGUID(iid,&IID_IAgileObject)) return E_NOINTERFACE;
    if (IsEqualGUID(iid,&private_impl_iid)) InterlockedIncrement(&private_queries);
    if (!IsEqualGUID(iid,&IID_IUnknown) && !IsEqualGUID(iid,&IID_IInspectable) &&
        !IsEqualGUID(iid,&IID_IAgileObject) && !IsEqualGUID(iid,&IID_IGameController)) return E_NOINTERFACE;
    *out=iface; controller_addref(iface); return S_OK;
}
#define INSPECTABLE_STUBS(prefix,type) \
static HRESULT WINAPI prefix##_iids(type *iface,ULONG *count,IID **iids) { *count=0; *iids=NULL; return S_OK; } \
static HRESULT WINAPI prefix##_name(type *iface,HSTRING *name) { *name=NULL; return E_NOTIMPL; } \
static HRESULT WINAPI prefix##_trust(type *iface,TrustLevel *level) { *level=BaseTrust; return S_OK; }
INSPECTABLE_STUBS(controller,IGameController)
static HRESULT WINAPI headset_add(IGameController *iface,ITypedEventHandler_IGameController_Headset *handler,EventRegistrationToken *token) { return E_NOTIMPL; }
static HRESULT WINAPI event_remove(IGameController *iface,EventRegistrationToken token) { return E_NOTIMPL; }
static HRESULT WINAPI user_add(IGameController *iface,ITypedEventHandler_IGameController_UserChangedEventArgs *handler,EventRegistrationToken *token) { return E_NOTIMPL; }
static HRESULT WINAPI headset_get(IGameController *iface,IHeadset **value) { *value=NULL; return E_NOTIMPL; }
static HRESULT WINAPI wireless_get(IGameController *iface,boolean *value)
{
    check_apartment("controller-IsWireless");
    InterlockedIncrement(&wireless_calls);
    *value=FALSE; return S_OK;
}
static HRESULT WINAPI user_get(IGameController *iface,IUser **value) { *value=NULL; return E_NOTIMPL; }
static const IGameControllerVtbl controller_vtbl={
    controller_query,controller_addref,controller_release,controller_iids,controller_name,controller_trust,
    headset_add,event_remove,headset_add,event_remove,user_add,event_remove,headset_get,wireless_get,user_get
};

static ULONG WINAPI factory_addref(ICustomGameControllerFactory *iface) { return InterlockedIncrement(&factory_refs); }
static ULONG WINAPI factory_release(ICustomGameControllerFactory *iface) { return InterlockedDecrement(&factory_refs); }
static HRESULT WINAPI factory_query(ICustomGameControllerFactory *iface,REFIID iid,void **out)
{
    *out=NULL;
    if (!agile && IsEqualGUID(iid,&IID_IAgileObject)) return E_NOINTERFACE;
    if (!IsEqualGUID(iid,&IID_IUnknown) && !IsEqualGUID(iid,&IID_IInspectable) &&
        !IsEqualGUID(iid,&IID_IAgileObject) && !IsEqualGUID(iid,&IID_ICustomGameControllerFactory)) return E_NOINTERFACE;
    *out=iface; factory_addref(iface); return S_OK;
}
INSPECTABLE_STUBS(factory,ICustomGameControllerFactory)
static HRESULT WINAPI factory_create(ICustomGameControllerFactory *iface,IGameControllerProvider *provider,IInspectable **value)
{
    struct controller *impl;
    check_apartment("CreateGameController");
    if (hotplug)
    {
        CHECK(SUCCEEDED(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(
            callback_manager,iface,registered_vendor,registered_product)));
    }
    impl=calloc(1,sizeof(*impl));
    *value=NULL;
    if (!impl) return E_OUTOFMEMORY;
    impl->iface.lpVtbl=&controller_vtbl; impl->refs=1;
    *value=(IInspectable *)&impl->iface;
    InterlockedIncrement(&created);
    return S_OK;
}
static HRESULT WINAPI factory_added(ICustomGameControllerFactory *iface,IGameController *value)
{
    check_apartment("OnGameControllerAdded");
    CHECK(value && value->lpVtbl==&controller_vtbl);
    if (hotplug)
    {
        reenter_manager(value,TRUE);
        CHECK(SUCCEEDED(RoGetAgileReference(AGILEREFERENCE_DEFAULT,&IID_IGameController,
            (IUnknown *)value,&test_controller_ref)));
    }
    InterlockedIncrement(&added);
    if (added_event) SetEvent(added_event);
    return S_OK;
}
static HRESULT WINAPI factory_removed(ICustomGameControllerFactory *iface,IGameController *value)
{
    check_apartment("OnGameControllerRemoved");
    CHECK(value && value->lpVtbl==&controller_vtbl);
    if (hotplug) reenter_manager(value,FALSE);
    InterlockedIncrement(&removed);
    if (removed_event) SetEvent(removed_event);
    return S_OK;
}
static const ICustomGameControllerFactoryVtbl factory_vtbl={
    factory_query,factory_addref,factory_release,factory_iids,factory_name,factory_trust,
    factory_create,factory_added,factory_removed
};
static ICustomGameControllerFactory factory={&factory_vtbl};

static HRESULT activate(const WCHAR *class_name,REFIID iid,void **out)
{
    HSTRING name=NULL;
    HRESULT hr=WindowsCreateString(class_name,lstrlenW(class_name),&name);
    if (SUCCEEDED(hr)) hr=RoGetActivationFactory(name,iid,out);
    WindowsDeleteString(name); return hr;
}
/* These calls occur inside the factory callbacks. A manager lock held by the
 * monitor across STA marshaling would deadlock here. */
static void reenter_manager(IGameController *value,BOOL present)
{
    IGameControllerFactoryManagerStatics *local=NULL;
    IGameControllerFactoryManagerStatics2 *manager2=NULL;
    IGameController *found=NULL;
    CHECK(SUCCEEDED(activate(L"Windows.Gaming.Input.Custom.GameControllerFactoryManager",
        &IID_IGameControllerFactoryManagerStatics,(void **)&local)));
    if (!local) return;
    CHECK(SUCCEEDED(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(
        local,&factory,registered_vendor,registered_product)));
    CHECK(SUCCEEDED(IGameControllerFactoryManagerStatics_QueryInterface(local,
        &IID_IGameControllerFactoryManagerStatics2,(void **)&manager2)));
    IGameControllerFactoryManagerStatics_Release(local);
    if (!manager2) return;
    CHECK(SUCCEEDED(IGameControllerFactoryManagerStatics2_TryGetFactoryControllerFromGameController(
        manager2,&factory,value,&found)));
    CHECK(present?found==value:!found);
    if (found) IGameController_Release(found);
    IGameControllerFactoryManagerStatics2_Release(manager2);
}

static DWORD WINAPI lookup_thread(void *unused)
{
    ICustomGameControllerFactory *resolved_factory=NULL;
    IGameControllerFactoryManagerStatics *manager=NULL;
    IGameControllerFactoryManagerStatics2 *manager2=NULL;
    IRawGameControllerStatics *raw_statics=NULL;
    IRawGameController *raw=NULL;
    IGameController *controller=NULL,*found=NULL;
    IUnknown *a=NULL,*b=NULL;
    boolean wireless=TRUE;
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return 1;
    CALL(IAgileReference_Resolve(test_factory_ref,&IID_ICustomGameControllerFactory,(void **)&resolved_factory));
    CALL(IAgileReference_Resolve(test_controller_ref,&IID_IGameController,(void **)&controller));
    CALL(activate(L"Windows.Gaming.Input.Custom.GameControllerFactoryManager",&IID_IGameControllerFactoryManagerStatics,(void **)&manager));
    CALL(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(manager,resolved_factory,registered_vendor,registered_product));
    CALL(IGameControllerFactoryManagerStatics_QueryInterface(manager,&IID_IGameControllerFactoryManagerStatics2,(void **)&manager2));
    CALL(IGameControllerFactoryManagerStatics2_TryGetFactoryControllerFromGameController(manager2,resolved_factory,controller,&found));
    CHECK(found!=NULL);
    if (!found) goto done;
    CALL(IGameController_QueryInterface(found,&IID_IUnknown,(void **)&a));
    CALL(IGameController_QueryInterface(controller,&IID_IUnknown,(void **)&b));
    CHECK(a==b);
    CALL(IGameController_get_IsWireless(found,&wireless));
    CHECK(!wireless);
    CALL(activate(L"Windows.Gaming.Input.RawGameController",&IID_IRawGameControllerStatics,(void **)&raw_statics));
    CALL(IRawGameControllerStatics_FromGameController(raw_statics,found,&raw));
    CHECK(raw!=NULL);
 done:
    if (raw) IRawGameController_Release(raw);
    if (raw_statics) IRawGameControllerStatics_Release(raw_statics);
    if (a) IUnknown_Release(a);
    if (b) IUnknown_Release(b);
    if (found) IGameController_Release(found);
    if (controller) IGameController_Release(controller);
    if (resolved_factory) ICustomGameControllerFactory_Release(resolved_factory);
    if (manager2) IGameControllerFactoryManagerStatics2_Release(manager2);
    if (manager) IGameControllerFactoryManagerStatics_Release(manager);
    RoUninitialize();
    return 0;
}

static BOOL pump_until(HANDLE handle)
{
    DWORD index;
    /* Wine 11.20 pumps classic STA COM messages with its default wait flags. */
    HRESULT hr=CoWaitForMultipleHandles(COWAIT_DEFAULT,30000,1,&handle,&index);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) printf("WAIT_FAIL hr=%08lx\n",(unsigned long)hr);
    return SUCCEEDED(hr);
}

static int run_hotplug(void)
{
    HANDLE worker=NULL;
    HRESULT hr;
    int result=1;
    added_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    removed_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!added_event || !removed_event) { CHECK(FALSE); goto done; }
    CALL(activate(L"Windows.Gaming.Input.Custom.GameControllerFactoryManager",&IID_IGameControllerFactoryManagerStatics,(void **)&callback_manager));
    CALL(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(callback_manager,&factory,registered_vendor,registered_product));
    CALL(RoGetAgileReference(AGILEREFERENCE_DEFAULT,&IID_ICustomGameControllerFactory,(IUnknown *)&factory,&test_factory_ref));
    CHECK(!created && !added); /* The fixture must be attached only after READY_ADD. */
    if (created || added) goto done;
    printf("READY_ADD mode=%s vendor=%u product=%u\n",agile?"agile":"STA",registered_vendor,registered_product); fflush(stdout);
    if (!pump_until(added_event) || !test_controller_ref) goto done;
    worker=CreateThread(NULL,0,lookup_thread,NULL,0,NULL);
    CHECK(worker!=NULL);
    if (!worker) goto done;
    /* Do not uninitialize the STA while a worker still needs it; external timeout handles deadlocks. */
    if (!pump_until(worker)) ExitProcess(1);
    CHECK(created==1 && added==1 && wireless_calls>0 && !private_queries);
    puts("READY_REMOVE"); fflush(stdout);
    if (!pump_until(removed_event)) goto done;
    CHECK(removed==1 && created==1 && added==1);
    result=failures?1:0;
 done:
    if (worker) CloseHandle(worker);
    if (test_controller_ref) IAgileReference_Release(test_controller_ref);
    if (test_factory_ref) IAgileReference_Release(test_factory_ref);
    if (callback_manager) IGameControllerFactoryManagerStatics_Release(callback_manager);
    if (added_event) CloseHandle(added_event);
    if (removed_event) CloseHandle(removed_event);
    printf("CONTROLLER_APARTMENT_%s mode=%s created=%ld added=%ld removed=%ld private_queries=%ld failures=%ld\n",
        result?"FAIL":"PASS",agile?"agile":"STA",created,added,removed,private_queries,failures);
    return result;
}

int main(int argc,char **argv)
{
    IGameControllerFactoryManagerStatics *manager=NULL;
    IGameControllerFactoryManagerStatics2 *manager2=NULL;
    IRawGameControllerStatics *raw_statics=NULL;
    IVectorView_RawGameController *view=NULL;
    IRawGameController *raw=NULL,*roundtrip=NULL;
    IGameController *base=NULL,*custom=NULL,*identity=NULL;
    UINT16 vendor=0xffff,product=0xffff;
    UINT32 count=0;
    LONG expected_refs,expected_creates,expected_added;
    HRESULT hr;
    int result=1;
    BOOL matching=argc==2 && !strcmp(argv[1],"--controller");
    hotplug=argc==4 && (!strcmp(argv[1],"--sta-hotplug") || !strcmp(argv[1],"--agile-hotplug"));
    if (argc>1 && !matching && !hotplug)
    {
        puts("usage: probe-controller-factory.exe [--controller | --sta-hotplug VID PID | --agile-hotplug VID PID]");
        return 2;
    }
    if (hotplug)
    {
        char *end;
        unsigned long number=strtoul(argv[2],&end,0);
        if (*end || number>65535) return 2;
        registered_vendor=number;
        number=strtoul(argv[3],&end,0);
        if (*end || number>65535) return 2;
        registered_product=number;
        agile=!strcmp(argv[1],"--agile-hotplug");
    }
    owner_thread=GetCurrentThreadId();
    hr=RoInitialize(agile?RO_INIT_MULTITHREADED:RO_INIT_SINGLETHREADED);
    if (FAILED(hr)) return 1;
    if (hotplug) { result=run_hotplug(); RoUninitialize(); return result; }
    CALL(activate(L"Windows.Gaming.Input.Custom.GameControllerFactoryManager",&IID_IGameControllerFactoryManagerStatics,(void **)&manager));
    if (matching)
    {
        CALL(activate(L"Windows.Gaming.Input.RawGameController",&IID_IRawGameControllerStatics,(void **)&raw_statics));
        for (unsigned i=0;i<50;i++)
        {
            CALL(IRawGameControllerStatics_get_RawGameControllers(raw_statics,&view));
            CALL(IVectorView_RawGameController_get_Size(view,&count));
            if (count) break;
            IVectorView_RawGameController_Release(view); view=NULL; Sleep(100);
        }
        if (!count) { puts("SKIP no controller after 5 seconds"); result=77; goto done; }
        CALL(IVectorView_RawGameController_GetAt(view,0,&raw));
        CALL(IRawGameController_get_HardwareVendorId(raw,&vendor));
        CALL(IRawGameController_get_HardwareProductId(raw,&product));
        CALL(IRawGameController_QueryInterface(raw,&IID_IGameController,(void **)&base));
    }
    CALL(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(manager,&factory,vendor,product));
    expected_refs=factory_refs; expected_creates=created; expected_added=added;
    CHECK(factory_refs>=2);
    CALL(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(manager,&factory,vendor,product));
    CHECK(factory_refs==expected_refs && created==expected_creates && added==expected_added);
    CHECK(IGameControllerFactoryManagerStatics_RegisterCustomFactoryForHardwareId(manager,NULL,vendor,product)==E_INVALIDARG);
    if (matching)
    {
        CHECK(created>0 && added==created && !private_queries);
        CALL(IGameControllerFactoryManagerStatics_QueryInterface(manager,&IID_IGameControllerFactoryManagerStatics2,(void **)&manager2));
        CALL(IGameControllerFactoryManagerStatics2_TryGetFactoryControllerFromGameController(manager2,&factory,base,&custom));
        CHECK(custom && custom->lpVtbl==&controller_vtbl);
        if (!custom) goto done;
        CALL(IGameController_QueryInterface(custom,&IID_IGameController,(void **)&identity));
        CHECK(identity==custom);
        CALL(IRawGameControllerStatics_FromGameController(raw_statics,custom,&roundtrip));
        CHECK(roundtrip==raw);
    }
    else CHECK(!created && !added && factory_refs==2);
    result=failures?1:0;
done:
    if (identity) IGameController_Release(identity);
    if (custom) IGameController_Release(custom);
    if (base) IGameController_Release(base);
    if (roundtrip) IRawGameController_Release(roundtrip);
    if (raw) IRawGameController_Release(raw);
    if (view) IVectorView_RawGameController_Release(view);
    if (raw_statics) IRawGameControllerStatics_Release(raw_statics);
    if (manager2) IGameControllerFactoryManagerStatics2_Release(manager2);
    if (manager) IGameControllerFactoryManagerStatics_Release(manager);
    RoUninitialize();
    printf("CONTROLLER_FACTORY_%s matching=%d created=%ld added=%ld removed=%ld private_queries=%ld failures=%ld\n",
           result==77?"SKIP":result?"FAIL":"PASS",matching,created,added,removed,private_queries,failures);
    return result;
}

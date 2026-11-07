/* Host fixture for extracted controller-manager functions. COM and providers are
 * mocks; the manager's registration, creation, lookup and removal code is real.
 * This does not check the Windows ABI or raw HID/XUSB report delivery. */
#include <string.h>
#define WARN(...) ((void)0)
#define E_INVALIDARG (-5)
#define E_NOINTERFACE (-6)
#define TRUE 1
#define FALSE 0
typedef unsigned ULONG;
typedef void IUnknown;
typedef struct agile_reference IAgileReference;
#define E_PENDING (-7)
#define AGILEREFERENCE_DEFAULT 0
static ULONG IAgileReference_Release(IAgileReference *ref);
typedef unsigned long long UINT64;
typedef const int *REFIID;
typedef void IGameControllerFactoryManagerStatics, IGameControllerFactoryManagerStatics2;
typedef struct object IInspectable, IGameController, IGameControllerBatteryInfo;
typedef struct object IGameControllerImpl, IGameControllerInputSink;
struct object_vtbl {
    HRESULT (*QueryInterface)(struct object *, REFIID, void **);
    ULONG (*AddRef)(struct object *);
    ULONG (*Release)(struct object *);
};
struct object { const struct object_vtbl *lpVtbl; };
static const int IID_IUnknown=1, IID_IInspectable=2, IID_IAgileObject=3, IID_IGameController=4;
static const int IID_IGameControllerBatteryInfo=5, IID_IGameControllerImpl=6;
static const int IID_IGameControllerInputSink=7, IID_IWineGameControllerProvider=8, IID_ICustomGameControllerFactory=9;
#define IsEqualGUID(a,b) (*(a)==*(b))
#define InterlockedIncrement(p) (++*(p))
#define InterlockedDecrement(p) (--*(p))
#define IInspectable_QueryInterface(o,i,p) ((o)->lpVtbl->QueryInterface(o,i,p))
#define IInspectable_AddRef(o) (((IInspectable *)(o))->lpVtbl->AddRef((IInspectable *)(o)))
#define IInspectable_Release(o) ((o)->lpVtbl->Release(o))
#define IGameController_QueryInterface IInspectable_QueryInterface
#define IGameController_AddRef IInspectable_AddRef
#define IGameController_Release IInspectable_Release
#define IGameControllerImpl_Release IInspectable_Release
#define IGameControllerInputSink_Release IInspectable_Release

struct list { struct list *next, *prev; };
#define LIST_INIT(h) { &(h), &(h) }
#define LIST_ENTRY(p,t,m) CONTAINING_RECORD(p,t,m)
#define LIST_FOR_EACH_ENTRY(p,h,t,m) \
    for (struct list *cursor=(h)->next; cursor!=(h) && ((p)=LIST_ENTRY(cursor,t,m),1); cursor=cursor->next)
#define LIST_FOR_EACH_ENTRY_SAFE(p,n,h,t,m) \
    for (struct list *cursor=(h)->next,*after=cursor->next; cursor!=(h) && \
         ((p)=LIST_ENTRY(cursor,t,m),(n)=after==(h)?NULL:LIST_ENTRY(after,t,m),1); cursor=after,after=cursor->next)
#define LIST_FOR_EACH_SAFE(p,n,h) for ((p)=(h)->next,(n)=(p)->next;(p)!=(h);(p)=(n),(n)=(p)->next)
static int list_empty(struct list *h) { return h->next==h; }
static struct list *list_tail(struct list *h) { return h->prev; }
static void list_add_tail(struct list *h,struct list *p) {
    p->next=h; p->prev=h->prev; h->prev->next=p; h->prev=p;
}
static void list_remove(struct list *p) { p->prev->next=p->next; p->next->prev=p->prev; }
static struct list controller_list=LIST_INIT(controller_list);
static pthread_mutex_t manager_cs;
static _Thread_local int manager_depth;
static void EnterCriticalSection(pthread_mutex_t *cs) { pthread_mutex_lock(cs); ++manager_depth; }
static void LeaveCriticalSection(pthread_mutex_t *cs) { --manager_depth; pthread_mutex_unlock(cs); }

enum { WineGameControllerType_Joystick, WineGameControllerType_Gamepad, WineGameControllerType_RacingWheel };
typedef int WineGameControllerType;
typedef struct { int refs; UINT16 vendor, product; WineGameControllerType type; } IGameControllerProvider;
typedef IGameControllerProvider IWineGameControllerProvider;
#define IGameControllerProvider_AddRef(p) (++(p)->refs)
#define IGameControllerProvider_Release(p) (--(p)->refs)
#define IGameControllerProvider_get_HardwareVendorId(p,v) (*(v)=(p)->vendor,S_OK)
#define IGameControllerProvider_get_HardwareProductId(p,v) (*(v)=(p)->product,S_OK)
#define IGameControllerProvider_QueryInterface(p,i,o) (++(p)->refs,*(o)=(p),S_OK)
#define IWineGameControllerProvider_get_Type(p,t) (*(t)=(p)->type,S_OK)
#define IWineGameControllerProvider_Release IGameControllerProvider_Release
static UINT64 GetTickCount64(void) { return 123; }

typedef struct custom_factory {
    int refs, builtin, creates, added, removed, resumed, suspended, destroyed, private_queries, initialized;
    int fail_create, null_result, fail_private, reenter, fail_public, null_interface;
    int remove_during_create, remove_during_add;
    IGameControllerProvider *remove_provider;
    IGameController *last;
    struct custom_factory *register_during_add;
} ICustomGameControllerFactory;
static ICustomGameControllerFactory raw={.refs=1,.builtin=1}, pad={.refs=1,.builtin=1}, wheel={.refs=1,.builtin=1};
static ICustomGameControllerFactory *controller_factory=&raw, *gamepad_factory=&pad, *racing_wheel_factory=&wheel;
#define ICustomGameControllerFactory_AddRef(f) (++(f)->refs)
#define ICustomGameControllerFactory_Release(f) (--(f)->refs)
void manager_on_provider_removed(IGameControllerProvider *);
static HRESULT WINAPI statics_RegisterCustomFactoryForHardwareId(IGameControllerFactoryManagerStatics *,ICustomGameControllerFactory *,UINT16,UINT16);

struct custom_object {
    IInspectable inspectable;
    IGameController controller;
    int refs;
    ICustomGameControllerFactory *owner;
};
static struct custom_object *from_inspectable(IInspectable *o) { return CONTAINING_RECORD(o,struct custom_object,inspectable); }
static struct custom_object *from_controller(IGameController *o) { return CONTAINING_RECORD(o,struct custom_object,controller); }
static HRESULT custom_query(struct custom_object *o,REFIID iid,void **out) {
    *out=NULL;
    if (*iid==IID_IGameController && o->owner->fail_public) return E_NOINTERFACE;
    if (*iid==IID_IGameController && o->owner->null_interface) return S_OK;
    if (*iid==IID_IGameControllerImpl) {
        ++o->owner->private_queries;
        if (!o->owner->builtin || o->owner->fail_private) return E_NOINTERFACE;
        *out=&o->inspectable;
    } else if (*iid==IID_IGameController || *iid==IID_IGameControllerInputSink) *out=&o->controller;
    else if (*iid==IID_IUnknown || *iid==IID_IInspectable) *out=&o->inspectable;
    else return E_NOINTERFACE;
    ++o->refs; return S_OK;
}
static ULONG custom_release(struct custom_object *o) {
    int refs=--o->refs;
    if (!refs) { ++o->owner->destroyed; free(o); }
    return refs;
}
static HRESULT custom_query_inner(IInspectable *o,REFIID iid,void **out) { return custom_query(from_inspectable(o),iid,out); }
static ULONG custom_addref_inner(IInspectable *o) { return ++from_inspectable(o)->refs; }
static ULONG custom_release_inner(IInspectable *o) { return custom_release(from_inspectable(o)); }
static HRESULT custom_query_controller(IGameController *o,REFIID iid,void **out) { return custom_query(from_controller(o),iid,out); }
static ULONG custom_addref_controller(IGameController *o) { return ++from_controller(o)->refs; }
static ULONG custom_release_controller(IGameController *o) { return custom_release(from_controller(o)); }
static const struct object_vtbl custom_inner_vtbl={custom_query_inner,custom_addref_inner,custom_release_inner};
static const struct object_vtbl custom_controller_vtbl={custom_query_controller,custom_addref_controller,custom_release_controller};
static HRESULT ICustomGameControllerFactory_CreateGameController(ICustomGameControllerFactory *f,IGameControllerProvider *p,IInspectable **out) {
    assert(!manager_depth);
    ++f->creates; *out=NULL; f->remove_provider=p;
    if (f->remove_during_create) manager_on_provider_removed(p);
    if (f->fail_create) return E_FAIL;
    if (f->null_result) return S_OK;
    struct custom_object *o=calloc(1,sizeof(*o)); assert(o);
    o->inspectable.lpVtbl=&custom_inner_vtbl; o->controller.lpVtbl=&custom_controller_vtbl;
    o->refs=1; o->owner=f; *out=&o->inspectable; return S_OK;
}
static HRESULT IGameControllerImpl_Initialize(IGameControllerImpl *o,IGameController *outer,IGameControllerProvider *p) {
    assert(from_inspectable(o)->owner->builtin); ++from_inspectable(o)->owner->initialized; return S_OK;
}
static HRESULT IGameControllerInputSink_OnInputResumed(IGameControllerInputSink *o,UINT64 time) {
    ++from_controller(o)->owner->resumed; return S_OK;
}
static HRESULT IGameControllerInputSink_OnInputSuspended(IGameControllerInputSink *o,UINT64 time) {
    ++from_controller(o)->owner->suspended; return S_OK;
}
static HRESULT ICustomGameControllerFactory_OnGameControllerAdded(ICustomGameControllerFactory *f,IGameController *o) {
    assert(!manager_depth);
    ++f->added; f->last=o;
    if (f->remove_during_add) manager_on_provider_removed(f->remove_provider);
    if (!f->builtin) {
        assert(o->lpVtbl==&custom_controller_vtbl); /* Publish the external object's public interface. */
        assert(from_controller(o)->owner==f);
        if (f->reenter) assert(statics_RegisterCustomFactoryForHardwareId(NULL,f,0x1234,0x5678)==S_OK);
    }
    if (f->register_during_add)
        assert(statics_RegisterCustomFactoryForHardwareId(NULL,f->register_during_add,0x1234,0x5678)==S_OK);
    return S_OK;
}
static HRESULT ICustomGameControllerFactory_OnGameControllerRemoved(ICustomGameControllerFactory *f,IGameController *o) {
    assert(!manager_depth);
    ++f->removed;
    if (!f->builtin) assert(o->lpVtbl==&custom_controller_vtbl && from_controller(o)->owner==f);
    return S_OK;
}

/* These refs model ownership only. Actual apartment behavior is checked by the WinRT probe. */
struct agile_reference { void *object; int factory, refs; };
static HRESULT RoGetAgileReference(int options,REFIID iid,void *object,IAgileReference **out) {
    assert(!manager_depth);
    IAgileReference *ref=calloc(1,sizeof(*ref)); assert(ref);
    ref->object=object; ref->factory=*iid==IID_ICustomGameControllerFactory; ref->refs=1;
    if (ref->factory) ICustomGameControllerFactory_AddRef((ICustomGameControllerFactory *)object);
    else IGameController_AddRef((IGameController *)object);
    *out=ref; return S_OK;
}
static ULONG IAgileReference_Release(IAgileReference *ref) {
    assert(!manager_depth);
    int count=--ref->refs;
    if (!count) {
        if (ref->factory) ICustomGameControllerFactory_Release((ICustomGameControllerFactory *)ref->object);
        else IGameController_Release((IGameController *)ref->object);
        free(ref);
    }
    return count;
}
static HRESULT IAgileReference_Resolve(IAgileReference *ref,REFIID iid,void **out) {
    assert(!manager_depth);
    if (ref->factory) ICustomGameControllerFactory_AddRef((ICustomGameControllerFactory *)ref->object);
    else IGameController_AddRef((IGameController *)ref->object);
    *out=ref->object; return S_OK;
}
static BOOL object_matches(void *left,void *right) { assert(!manager_depth); return left==right; }

/* PRODUCTION_STRUCTS */
static inline struct controller *impl_from_IGameController(IGameController *o) {
    return CONTAINING_RECORD(o,struct controller,IGameController_iface);
}
/* PRODUCTION_REFS */
static const struct object_vtbl controller_vtbl={controller_QueryInterface,controller_AddRef,controller_Release};
static const struct object_vtbl battery_vtbl={0};
static HRESULT controller_create(ICustomGameControllerFactory *,IGameControllerProvider *,struct controller **);
/* PRODUCTION_HELPERS */
/* PRODUCTION_METHODS */

static void check_lookup(ICustomGameControllerFactory *factory,IGameController *from,IGameController *expected) {
    IGameController *out=(void *)1;
    assert(statics2_TryGetFactoryControllerFromGameController(NULL,factory,from,&out)==S_OK);
    assert(out==expected);
    if (out) IGameController_Release(out);
}
int main(int argc,char **argv) {
    int kind=atoi(argv[1]);
    pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&manager_cs,&attr); pthread_mutexattr_destroy(&attr);
    ICustomGameControllerFactory custom={.refs=1}, other={.refs=1};
    IGameControllerProvider provider={1,0x1234,0x5678,WineGameControllerType_Gamepad};
    IGameControllerProvider unmatched={1,0x1234,0x5679,WineGameControllerType_Gamepad};
    struct controller *created=NULL;
    if (kind==0) {
        /* A native activation factory registers controllers before returning its factory. */
        ICustomGameControllerFactory *activation_result=NULL;
        HRESULT hr=statics_RegisterCustomFactoryForHardwareId(NULL,&custom,0x1234,0x5678);
        if (SUCCEEDED(hr)) activation_result=&custom;
        assert(hr==S_OK && activation_result && custom.refs==2 && !custom.creates);
        assert(statics_RegisterCustomFactoryForHardwareId(NULL,&custom,0x1234,0x5678)==S_OK);
        assert(custom.refs==2);
        assert(statics_RegisterCustomFactoryForHardwareId(NULL,NULL,0x1234,0x5678)==E_INVALIDARG);
    } else if (kind==6) {
        raw.fail_private=1;
        assert(controller_create(&raw,&provider,&created)==E_NOINTERFACE);
        assert(!created && raw.destroyed==1 && raw.refs==1 && provider.refs==1);
    } else if (kind==11 || kind==12) {
        manager_on_provider_created(&provider);
        custom.remove_during_create=kind==11; custom.remove_during_add=kind==12;
        assert(statics_RegisterCustomFactoryForHardwareId(NULL,&custom,0x1234,0x5678)==S_OK);
        assert(custom.creates==1 && custom.destroyed==1 && !custom.private_queries);
        assert(custom.added==(kind==12) && custom.removed==(kind==12));
        assert(custom.resumed==(kind==12) && custom.suspended==(kind==12));
        assert(list_empty(&controller_list) && provider.refs==1 && custom.refs==2);
    } else {
        if (kind==4) custom.fail_create=1;
        if (kind==5) custom.null_result=1;
        if (kind==7) custom.reenter=1;
        if (kind==8) raw.register_during_add=&custom;
        if (kind==9) custom.fail_public=1;
        if (kind==10) custom.null_interface=1;
        if (kind==1 || kind==4 || kind==5 || kind==7 || kind>=9) manager_on_provider_created(&provider);
        if (kind==3) manager_on_provider_created(&unmatched);
        if (kind!=8) assert(statics_RegisterCustomFactoryForHardwareId(NULL,&custom,0x1234,0x5678)==S_OK);
        if (kind==2 || kind==3 || kind==8) {
            assert(!custom.creates);
            manager_on_provider_created(&provider);
        }
        assert(custom.creates==1 && custom.private_queries==0);
        if (kind==4 || kind==5 || kind>=9) {
            assert(!custom.added && !custom.resumed && custom.refs==2);
            if (kind>=9) assert(custom.destroyed==1);
            check_lookup(&custom,raw.last,NULL);
        } else {
            assert(custom.added==1 && custom.resumed==1);
            assert(statics_RegisterCustomFactoryForHardwareId(NULL,&custom,0x1234,0x5678)==S_OK);
            assert(custom.creates==1 && custom.added==1);
            check_lookup(&custom,raw.last,custom.last);
            check_lookup(&raw,custom.last,raw.last);
            assert(statics_RegisterCustomFactoryForHardwareId(NULL,&other,0x1234,0x5678)==S_OK);
            assert(other.creates==1 && other.added==1 && other.last!=custom.last);
            check_lookup(&other,custom.last,other.last);
            IGameController_AddRef(custom.last); /* The caller can outlive device removal. */
        }
        manager_on_provider_removed(&provider);
        if (kind==3) manager_on_provider_removed(&unmatched);
        if (kind!=4 && kind!=5 && kind<9) {
            assert(custom.removed==1 && custom.suspended==1 && custom.destroyed==0);
            assert(other.removed==1 && other.suspended==1 && other.destroyed==1);
            IGameController_Release(custom.last);
            assert(custom.destroyed==1 && custom.refs==2 && other.refs==2);
        }
        assert(provider.refs==1 && unmatched.refs==1 && list_empty(&controller_list));
        assert(raw.refs==1 && pad.refs==1 && raw.initialized==raw.creates && pad.initialized==pad.creates);
    }
    /* Registrations have process lifetime. Release the fixture's registry only for leak checking. */
    /* RELEASE_REGISTRATIONS */
    assert(custom.refs==1 && other.refs==1);
    puts("controller factory lifecycle PASS"); return 0;
}

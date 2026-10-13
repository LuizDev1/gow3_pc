/* SPDX-License-Identifier: GPL-2.0-or-later
 * Attribute ABI/defaults adapted from compatibility-source/src/runtime_thread.c.
 * Joinable guest threads use Windows workers; PS4 scheduling remains unresolved. */
#include "windows_thread.h"
#include "windows_tls.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define ABI __attribute__((sysv_abi))
#define INVALID ((int32_t)0x80020016)
#define NO_THREAD ((int32_t)0x80020003)
#define NOMEM ((int32_t)0x8002000c)
#define LIMIT 256
typedef struct { uint64_t id,stack,guard,affinity; int detach,policy,priority,inherit; } Attr;
static Attr attrs[LIMIT];
static uint64_t next_id=1;
static SRWLOCK lock=SRWLOCK_INIT;
static Attr *find(const uint64_t *handle) {
    if(handle && *handle) for(int i=0;i<LIMIT;i++) if(attrs[i].id==*handle) return &attrs[i];
    return NULL;
}
static ABI int32_t attr_init(uint64_t *out) {
    if(!out) return INVALID;
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(!attrs[i].id && next_id) {
        attrs[i]=(Attr){.id=next_id++,.stack=1024*1024,.guard=4096,.affinity=0x7f,.policy=1,.priority=700,.inherit=4};
        *out=attrs[i].id;ReleaseSRWLockExclusive(&lock);return 0;
    }
    ReleaseSRWLockExclusive(&lock);return NOMEM;
}
static ABI int32_t attr_destroy(uint64_t *handle) {
    AcquireSRWLockExclusive(&lock);Attr *a=find(handle);
    if(!a) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    *a=(Attr){0};*handle=0;ReleaseSRWLockExclusive(&lock);return 0;
}
#define SETTER(fn,field,type,valid) \
static ABI int32_t fn(uint64_t *handle,type value) { \
    if(!(valid)) return INVALID; \
    AcquireSRWLockExclusive(&lock);Attr *a=find(handle); \
    if(a) a->field=value; \
    ReleaseSRWLockExclusive(&lock);return a?0:INVALID; \
}
#define GETTER(fn,field,type) \
static ABI int32_t fn(uint64_t *handle,type *out) { \
    if(!out) return INVALID; \
    AcquireSRWLockExclusive(&lock);Attr *a=find(handle); \
    if(a) *out=a->field; \
    ReleaseSRWLockExclusive(&lock);return a?0:INVALID; \
}
SETTER(set_stack,stack,uint64_t,value>=16384)
SETTER(set_guard,guard,uint64_t,1)
SETTER(set_detach,detach,int,value==0 || value==1)
SETTER(set_policy,policy,int,value>=1 && value<=3)
SETTER(set_inherit,inherit,int,value==0 || value==4)
SETTER(set_affinity,affinity,uint64_t,1)
GETTER(get_stack,stack,uint64_t)
GETTER(get_detach,detach,int)
GETTER(get_priority,priority,int)
GETTER(get_affinity,affinity,uint64_t)
static ABI int32_t set_priority(uint64_t *handle,const int *value) {
    if(!value) return INVALID;
    AcquireSRWLockExclusive(&lock);Attr *a=find(handle);
    if(a) a->priority=*value;
    ReleaseSRWLockExclusive(&lock);return a?0:INVALID;
}
static ABI void *thread_self(void) { return windows_tls_current_thread(); }
static ABI int thread_equal(void *a,void *b) {
    /* Compare opaque values, including equal sentinel values, as the reference
     * runtime does. Equality does not validate that a handle is live. */
    int result=a==b;
    if(getenv("TLG_TRACE_SYNC")) fprintf(stderr,"Thread equality: a=%p b=%p result=%d host=%lu\n",a,b,result,(unsigned long)GetCurrentThreadId());
    return result;
}
static ABI int32_t attr_get(void *thread,uint64_t *out) {
    if(!thread || !out) return INVALID;
    /* No arbitrary handle dereference. Other-thread snapshots require a future
     * registry and lifecycle synchronization; only the current thread is valid. */
    if(thread!=thread_self()) return NO_THREAD;
    ULONG_PTR low=0,high=0;
    GetCurrentThreadStackLimits(&low,&high);
    MEMORY_BASIC_INFORMATION memory={0};
    if(!VirtualQuery(&memory,&memory,sizeof(memory)) || high<=(uintptr_t)memory.AllocationBase) return INVALID;
    uint64_t stack=high-(uintptr_t)memory.AllocationBase;
    AcquireSRWLockExclusive(&lock);Attr *a=find(out);
    if(!a) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    uint64_t id=a->id;
    /* These scheduling fields describe the guest's default profile, not an
     * emulated PS4 scheduler. Host-owned threads have no runtime guard request. */
    *a=(Attr){.id=id,.stack=stack,.affinity=0x7f,.policy=1,.priority=700,.inherit=4};
    ReleaseSRWLockExclusive(&lock);return 0;
}

typedef void *(ABI *GuestEntry)(void *);
typedef struct Thread { void *tcb; HANDLE host; GuestEntry entry; void *argument,*result;
    Attr attr; int finished,joining,joined; struct Thread *next; } Thread;
static Thread *threads;
static unsigned thread_count;
static Thread *find_thread(void *handle) {
    for(Thread *t=threads;t;t=t->next) if(t==handle) return t;return NULL;
}
static DWORD WINAPI run_guest_thread(void *argument) {
    Thread *t=argument;
    if(windows_tls_attach_main() || windows_tls_bind_thread(t)) {
        fputs("STOP: guest worker TLS initialization failed\n",stderr);ExitProcess(21);
    }
    void *result=t->entry(t->argument);
    windows_tls_detach();
    AcquireSRWLockExclusive(&lock);t->tcb=NULL;t->result=result;t->finished=1;
    ReleaseSRWLockExclusive(&lock);return 0;
}
static ABI int32_t thread_create(Thread **out,uint64_t *attribute,GuestEntry entry,void *argument,const char *name) {
    (void)name;
    if(!out || !entry) return INVALID;
    AcquireSRWLockExclusive(&lock);Attr *a=attribute?find(attribute):NULL;
    if(attribute && !a) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    Attr selected=a?*a:(Attr){.stack=1024*1024,.guard=4096,.affinity=0x7f,.policy=1,.priority=700,.inherit=4};
    if(selected.detach || selected.guard!=4096) {ReleaseSRWLockExclusive(&lock);return (int32_t)0x8002002d;}
    if(selected.stack<16384 || selected.stack>64*1024*1024 || thread_count>=256) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    Thread *t=calloc(1,sizeof(*t));if(!t) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    t->entry=entry;t->argument=argument;t->attr=selected;
    t->host=CreateThread(NULL,(SIZE_T)selected.stack,run_guest_thread,t,CREATE_SUSPENDED,NULL);
    if(!t->host) {free(t);ReleaseSRWLockExclusive(&lock);return NOMEM;}
    t->next=threads;threads=t;++thread_count;*out=t;
    HANDLE host=t->host;
    ReleaseSRWLockExclusive(&lock);
    if(ResumeThread(host)==(DWORD)-1) {fputs("STOP: cannot resume guest worker\n",stderr);ExitProcess(21);}
    return 0;
}
static ABI int32_t thread_join(void *handle,void **result) {
    AcquireSRWLockExclusive(&lock);Thread *t=find_thread(handle);
    if(!t || t->joined) {ReleaseSRWLockExclusive(&lock);return NO_THREAD;}
    if(handle==thread_self()) {ReleaseSRWLockExclusive(&lock);return (int32_t)0x8002000b;}
    if(t->joining) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    t->joining=1;HANDLE host=t->host;ReleaseSRWLockExclusive(&lock);
    if(WaitForSingleObject(host,INFINITE)!=WAIT_OBJECT_0) ExitProcess(21);
    AcquireSRWLockExclusive(&lock);
    if(result) *result=t->result;
    CloseHandle(t->host);t->host=NULL;t->joined=1;
    /* Retain a bounded tombstone so pointer handles cannot acquire new meaning. */
    ReleaseSRWLockExclusive(&lock);return 0;
}
static void *test_main_identity;
static ABI void *synthetic_worker(void *argument) {
    uint64_t index[2]={1,0};uint64_t *value=windows_tls_get_addr(index);
    Thread *self=thread_self();
    if(!self || self==(Thread *)test_main_identity || !self->tcb || !value || *value!=42) return NULL;
    *value=99;return argument;
}
int windows_thread_create_selftest(void) {
    static const uint64_t data=42;
    if(windows_tls_configure_main(&data,8,16,16) || windows_tls_attach_main()) return 1;
    test_main_identity=thread_self();if(!thread_equal(test_main_identity,thread_self()) || !thread_equal(NULL,NULL) || thread_equal(test_main_identity,NULL)) return 1;Thread *worker=NULL;void *result=NULL;
    if(thread_create(NULL,NULL,synthetic_worker,NULL,NULL)!=INVALID ||
        thread_create(&worker,NULL,NULL,NULL,NULL)!=INVALID ||
        thread_create(&worker,NULL,synthetic_worker,(void *)(uintptr_t)123,"test") ||
        thread_join(worker,&result) || result!=(void *)(uintptr_t)123 || thread_join(worker,NULL)!=NO_THREAD) return 1;
    uint64_t index[2]={1,0};uint64_t *value=windows_tls_get_addr(index);
    if(!value || *value!=42 || thread_self()!=test_main_identity) return 1;
    windows_tls_detach();puts("Guest threads: SysV entry, private TLS, stable identity, join result and stale join passed");return 0;
}
uintptr_t windows_thread_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
    EXPORT("3PtV6p3QNX4",thread_equal);
    EXPORT("6UgtwV+0zb4",thread_create);EXPORT("onNY9Byn-W8",thread_join);
    EXPORT("aI+OeCz8xrQ",thread_self);EXPORT("x1X76arYMxU",attr_get);
    EXPORT("nsYoNRywwNg",attr_init);EXPORT("62KCwEMmzcM",attr_destroy);
    EXPORT("UTXzJbWhhTE",set_stack);EXPORT("-Wreprtu0Qs",set_detach);
    EXPORT("4+h9EzwKF4I",set_policy);EXPORT("DzES9hQF4f4",set_priority);
    EXPORT("3qxgM4ezETA",set_affinity);EXPORT("eXbUSpEaTsA",set_inherit);
    EXPORT("JaRMy+QcpeU",get_detach);EXPORT("-fA+7ZlGDQs",get_stack);
    EXPORT("FXPWHNk8Of0",get_priority);EXPORT("El+cQ20DynU",set_guard);
    EXPORT("8+s5BzZjxSg",get_affinity);
#undef EXPORT
    return 0;
}
int windows_thread_selftest(void) {
#define CHECK(c) do {if(!(c)) {fprintf(stderr,"thread attribute check failed at %d\n",__LINE__);return 1;}}while(0)
    uint64_t a=0,b=0,size=0,mask=0;int value=0;
    CHECK(thread_self()==NULL);
    CHECK(!windows_tls_attach(NULL,0,16,16));
    CHECK(thread_self()!=NULL && thread_self()==windows_tls_current_thread());
    CHECK(windows_thread_resolve("aI+OeCz8xrQ#U#H")==(uintptr_t)thread_self);
    CHECK(!windows_thread_resolve("aI+OeCz8xrQ#other"));
    uint64_t snapshot=0,independent=0;
    CHECK(!attr_init(&snapshot));CHECK(!attr_init(&independent));
    CHECK(!set_stack(&snapshot,16384));CHECK(!attr_get(thread_self(),&snapshot));
    CHECK(!get_stack(&snapshot,&size) && size>=16384);
    ULONG_PTR low=0,high=0;GetCurrentThreadStackLimits(&low,&high);
    MEMORY_BASIC_INFORMATION memory={0};CHECK(VirtualQuery(&memory,&memory,sizeof(memory))!=0);
    CHECK(size==high-(uintptr_t)memory.AllocationBase);
    CHECK(!get_stack(&independent,&mask) && mask==1024*1024);
    CHECK(attr_get((void *)(uintptr_t)1,&snapshot)==NO_THREAD);
    CHECK(attr_get(NULL,&snapshot)==INVALID);CHECK(attr_get(thread_self(),NULL)==INVALID);
    uint64_t bogus=UINT64_MAX;CHECK(attr_get(thread_self(),&bogus)==INVALID);
    CHECK(!attr_destroy(&snapshot));CHECK(!attr_destroy(&independent));
    windows_tls_detach();CHECK(thread_self()==NULL);
    puts("Thread snapshot: actual reserved stack, output isolation and invalid handles passed");
    CHECK(attr_init(NULL)==INVALID);CHECK(!attr_init(&a));CHECK(!attr_init(&b));CHECK(a!=b);
    CHECK(!get_stack(&a,&size) && size==1024*1024);CHECK(!get_priority(&a,&value) && value==700);
    CHECK(set_stack(&a,16383)==INVALID);CHECK(!set_stack(&a,65536));CHECK(!get_stack(&a,&size) && size==65536);
    CHECK(!get_stack(&b,&size) && size==1024*1024);CHECK(set_detach(&a,2)==INVALID);
    CHECK(!set_detach(&a,1));CHECK(!get_detach(&a,&value) && value==1);
    CHECK(set_policy(&a,0)==INVALID);CHECK(!set_policy(&a,3));CHECK(set_inherit(&a,1)==INVALID);
    CHECK(!set_inherit(&a,0));value=500;CHECK(!set_priority(&a,&value));CHECK(!get_priority(&a,&value) && value==500);
    CHECK(set_priority(&a,NULL)==INVALID);CHECK(!set_guard(&a,8192));CHECK(!set_affinity(&a,3));
    CHECK(!get_affinity(&a,&mask) && mask==3);CHECK(!get_affinity(&b,&mask) && mask==0x7f);
    uint64_t stale=a;CHECK(!attr_destroy(&a) && !a);CHECK(!attr_init(&a));
    CHECK(set_stack(&stale,65536)==INVALID);CHECK(attr_destroy(&stale)==INVALID);
    CHECK(!attr_destroy(&a));CHECK(!attr_destroy(&b));CHECK(!windows_thread_resolve("nsYoNRywwNg#wrong"));
    puts("Thread attributes: defaults, validation, isolation, round trips and stale handles passed");return 0;
}

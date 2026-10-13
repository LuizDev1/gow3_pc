/* SPDX-License-Identifier: GPL-2.0-or-later
 * Attribute ABI/defaults adapted from compatibility-source/src/runtime_thread.c.
 * Settings are stored only: guest thread creation and scheduling are unresolved. */
#include "windows_thread.h"
#include "windows_tls.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
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

uintptr_t windows_thread_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
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

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest mutex attributes and locks. Priority scheduling remains unresolved.
 * The guest passes pointers to opaque handles, as in the inherited runtime.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "windows_mutex.h"
#define ABI __attribute__((sysv_abi))
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define BUSY ((int32_t)UINT32_C(0x80020010))
#define DEADLOCK ((int32_t)UINT32_C(0x8002000b))
#define NOT_OWNER ((int32_t)UINT32_C(0x80020001))
#define AGAIN ((int32_t)UINT32_C(0x80020023))
typedef struct Attr { int type,protocol; struct Attr *next; } Attr;
static Attr *attributes;
static SRWLOCK attr_lock = SRWLOCK_INIT;
typedef struct CondAttr { struct CondAttr *next; } CondAttr;
typedef struct Cond {
    unsigned waiters;
    void *bound_mutex;
    CONDITION_VARIABLE native;
    struct Cond *next;
} Cond;
static CondAttr *cond_attributes;
static Cond *conditions;
static CondAttr **find_cond_attr(CondAttr *handle) {
    CondAttr **at = &cond_attributes;
    while (*at && *at != handle) at = &(*at)->next;
    return at;
}
static ABI int32_t condattr_init(CondAttr **out) {
    if (!out) return INVALID;
    CondAttr *a = calloc(1, sizeof(*a));
    if (!a) return NO_MEMORY;
    AcquireSRWLockExclusive(&attr_lock);
    a->next = cond_attributes; cond_attributes = a;
    *out = a;
    ReleaseSRWLockExclusive(&attr_lock);
    return 0;
}
static ABI int32_t condattr_destroy(CondAttr **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    CondAttr **at = find_cond_attr(*handle), *a = *at;
    if (a) { *at = a->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&attr_lock);
    if (!a) return INVALID;
    free(a);
    return 0;
}
static ABI int32_t cond_init(Cond **out, CondAttr **attr, const char *name) {
    (void)name;
    if (!out) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    if (attr && !*find_cond_attr(*attr)) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    Cond *c = calloc(1, sizeof(*c));
    if (!c) { ReleaseSRWLockExclusive(&attr_lock); return NO_MEMORY; }
    InitializeConditionVariable(&c->native);
    c->next = conditions; conditions = c;
    *out = c;
    ReleaseSRWLockExclusive(&attr_lock);
    return 0;
}
static ABI int32_t cond_destroy(Cond **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Cond **at = &conditions;
    while (*at && *at != *handle) at = &(*at)->next;
    Cond *c = *at;
    if (c && c->waiters) { ReleaseSRWLockExclusive(&attr_lock); return BUSY; }
    if (c) { *at = c->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&attr_lock);
    if (!c) return INVALID;
    free(c);
    return 0;
}
typedef struct Mutex {
    int type,protocol;
    DWORD owner;
    uint32_t depth, waiters;
    CONDITION_VARIABLE changed;
    struct Mutex *next;
} Mutex;
static Mutex *mutexes;
static Mutex **find_mutex(Mutex *handle) {
    Mutex **at = &mutexes;
    while (*at && *at != handle) at = &(*at)->next;
    return at;
}
static Attr **find_attr(Attr *handle) {
    Attr **at = &attributes;
    while (*at && *at != handle) at = &(*at)->next;
    return at;
}
static ABI int32_t attr_init(Attr **out) {
    if (!out) return INVALID;
    Attr *attr = malloc(sizeof(*attr));
    if (!attr) return NO_MEMORY;
    attr->type = 1; attr->protocol=0;
    AcquireSRWLockExclusive(&attr_lock);
    attr->next = attributes;
    attributes = attr;
    ReleaseSRWLockExclusive(&attr_lock);
    *out = attr;
    return 0;
}
static ABI int32_t attr_type(Attr **handle, int type) {
    if (!handle || type < 1 || type > 4) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr *attr = *find_attr(*handle);
    if (attr) attr->type = type;
    ReleaseSRWLockExclusive(&attr_lock);
    return attr ? 0 : INVALID;
}
/* Experimental fallback preserves exclusion but does not implement priority
 * donation. Explicitly opt in; priority ceilings are never accepted. */
static int priority_fallback(void) {
    const char *value=getenv("TLG_MUTEX_PRIORITY_FALLBACK");
    return value && !strcmp(value,"1");
}
static ABI int32_t attr_protocol(Attr **handle,int protocol) {
    if (!handle || protocol<0 || protocol>2) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr *attr=*find_attr(*handle);
    if (!attr) {ReleaseSRWLockExclusive(&attr_lock);return INVALID;}
    if (protocol==2 || (protocol==1 && !priority_fallback())) {
        ReleaseSRWLockExclusive(&attr_lock);
        fprintf(stderr,"Unsupported mutex priority protocol: %d\n",protocol);
        return (int32_t)UINT32_C(0x8002002d);
    }
    attr->protocol=protocol;
    ReleaseSRWLockExclusive(&attr_lock);return 0;
}
static ABI int32_t attr_destroy(Attr **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr **at = find_attr(*handle);
    Attr *attr = *at;
    if (attr) { *at = attr->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&attr_lock);
    if (!attr) return INVALID;
    free(attr);
    return 0;
}
static ABI int32_t mutex_init(Mutex **out, Attr **attr, const char *name) {
    (void)name;
    if (!out) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr *a = attr ? *find_attr(*attr) : NULL;
    if (attr && !a) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    Mutex *m = calloc(1, sizeof(*m));
    if (!m) { ReleaseSRWLockExclusive(&attr_lock); return NO_MEMORY; }
    m->type = a ? a->type : 1;
    m->protocol=a?a->protocol:0;
    if(m->protocol==1) puts("Runtime: EXPERIMENTAL mutex fallback: exclusion enabled, priority inheritance unavailable");
    InitializeConditionVariable(&m->changed);
    m->next = mutexes;
    mutexes = m;
    *out = m;
    ReleaseSRWLockExclusive(&attr_lock);
    return 0;
}
static int32_t acquire_mutex(Mutex **handle, int try_only) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Mutex *m = *find_mutex(*handle);
    if (!m) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    DWORD caller = GetCurrentThreadId();
    if (m->owner == caller && m->type == 2) {
        int32_t result = m->depth == UINT32_MAX ? AGAIN : 0;
        if (!result) ++m->depth;
        ReleaseSRWLockExclusive(&attr_lock);
        return result;
    }
    if (m->owner && (try_only || (m->owner == caller && m->type != 3))) {
        int32_t result = try_only ? BUSY : DEADLOCK;
        ReleaseSRWLockExclusive(&attr_lock);
        return result;
    }
    while (m->owner) {
        ++m->waiters;
        BOOL ok = SleepConditionVariableSRW(&m->changed, &attr_lock, INFINITE, 0);
        --m->waiters;
        if (!ok) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    }
    m->owner = caller;
    m->depth = 1;
    ReleaseSRWLockExclusive(&attr_lock);
    return 0;
}
static ABI int32_t mutex_lock(Mutex **m) { return acquire_mutex(m, 0); }
static ABI int32_t mutex_trylock(Mutex **m) { return acquire_mutex(m, 1); }
static ABI int32_t mutex_unlock(Mutex **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Mutex *m = *find_mutex(*handle);
    int32_t result = !m ? INVALID : m->owner != GetCurrentThreadId() ? NOT_OWNER : 0;
    if (!result && !--m->depth) {
        m->owner = 0;
        WakeConditionVariable(&m->changed);
    }
    ReleaseSRWLockExclusive(&attr_lock);
    return result;
}
static ABI int32_t mutex_destroy(Mutex **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Mutex **at = find_mutex(*handle);
    Mutex *m = *at;
    int32_t result = !m ? INVALID : (m->owner || m->waiters) ? BUSY : 0;
    if (!result) { *at = m->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&attr_lock);
    if (!result) free(m);
    return result;
}
static Cond *find_condition(Cond **handle) {
    if(handle) for(Cond *c=conditions;c;c=c->next) if(c==*handle) return c;return NULL;
}
static ABI int32_t cond_wait(Cond **condition,Mutex **mutex) {
    AcquireSRWLockExclusive(&attr_lock);
    Cond *c=find_condition(condition);Mutex *m=mutex?*find_mutex(*mutex):NULL;
    if(!c || !m) {ReleaseSRWLockExclusive(&attr_lock);return INVALID;}
    DWORD caller=GetCurrentThreadId();
    if(m->owner!=caller) {ReleaseSRWLockExclusive(&attr_lock);return NOT_OWNER;}
    if(m->depth!=1 || (c->waiters && c->bound_mutex!=m)) {ReleaseSRWLockExclusive(&attr_lock);return INVALID;}
    ++c->waiters;c->bound_mutex=m;++m->waiters;
    m->owner=0;m->depth=0;WakeConditionVariable(&m->changed);
    /* Same state lock protects release and sleep registration: a signaler
     * cannot observe the released mutex before the waiter has registered. */
    BOOL ok=SleepConditionVariableSRW(&c->native,&attr_lock,INFINITE,0);
    while(m->owner) if(!SleepConditionVariableSRW(&m->changed,&attr_lock,INFINITE,0)) ExitProcess(21);
    m->owner=caller;m->depth=1;--m->waiters;
    if(!--c->waiters) c->bound_mutex=NULL;
    ReleaseSRWLockExclusive(&attr_lock);return ok?0:INVALID;
}
static int32_t cond_wake(Cond **handle,int all) {
    AcquireSRWLockExclusive(&attr_lock);Cond *c=find_condition(handle);
    if(!c) {ReleaseSRWLockExclusive(&attr_lock);return INVALID;}
    if(all) WakeAllConditionVariable(&c->native);else WakeConditionVariable(&c->native);
    ReleaseSRWLockExclusive(&attr_lock);return 0;
}
static ABI int32_t cond_signal(Cond **c) {return cond_wake(c,0);}
static ABI int32_t cond_broadcast(Cond **c) {return cond_wake(c,1);}
typedef struct {Mutex *mutex;Cond *condition;int ready,result,completed;} CondTest;
static DWORD WINAPI condition_worker(void *argument) {
    CondTest *t=argument;
    if(mutex_lock(&t->mutex)) {t->result=1;return 0;}
    while(!t->ready) if(cond_wait(&t->condition,&t->mutex)) {t->result=1;break;}
    ++t->completed;if(mutex_unlock(&t->mutex)) t->result=1;return 0;
}
static int selftest_condition_wait(void) {
    CondTest t={0};
    if(mutex_init(&t.mutex,NULL,NULL) || cond_init(&t.condition,NULL,NULL) ||
        cond_wait(&t.condition,&t.mutex)!=NOT_OWNER || cond_signal(NULL)!=INVALID) return 1;
    HANDLE threads[2];
    for(unsigned i=0;i<2;i++) {threads[i]=CreateThread(NULL,0,condition_worker,&t,0,NULL);if(!threads[i]) return 1;}
    int registered=0;
    for(unsigned i=0;i<5000;i++) {
        AcquireSRWLockShared(&attr_lock);registered=t.condition->waiters==2;ReleaseSRWLockShared(&attr_lock);
        if(registered) break;Sleep(1);
    }
    if(!registered || cond_destroy(&t.condition)!=BUSY || mutex_destroy(&t.mutex)!=BUSY || mutex_lock(&t.mutex)) return 1;
    t.ready=1;
    if(cond_broadcast(&t.condition) || WaitForSingleObject(threads[0],0)!=WAIT_TIMEOUT || mutex_unlock(&t.mutex)) return 1;
    if(WaitForMultipleObjects(2,threads,TRUE,5000)!=WAIT_OBJECT_0 || t.result || t.completed!=2) return 1;
    CloseHandle(threads[0]);CloseHandle(threads[1]);
    t.ready=0;t.completed=0;
    HANDLE thread=CreateThread(NULL,0,condition_worker,&t,0,NULL);if(!thread) return 1;
    registered=0;
    for(unsigned i=0;i<5000;i++) {AcquireSRWLockShared(&attr_lock);registered=t.condition->waiters==1;ReleaseSRWLockShared(&attr_lock);if(registered) break;Sleep(1);}
    if(!registered || mutex_lock(&t.mutex)) return 1;t.ready=1;
    if(cond_signal(&t.condition) || mutex_unlock(&t.mutex) || WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0 || t.result || t.completed!=1) return 1;
    CloseHandle(thread);
    Cond *stale=t.condition;
    if(cond_destroy(&t.condition) || cond_broadcast(&stale)!=INVALID || mutex_destroy(&t.mutex)) return 1;
    puts("conditions: atomic mutex release, signal, broadcast, reacquisition and busy lifetime passed");return 0;
}
/* Counted readers and exclusive writers; handles never reuse an ID. Timed
 * operations and custom attributes remain unresolved. Writer preference is
 * used except that an existing reader may recursively acquire a read lock. */
typedef struct { DWORD thread; uint32_t count; } Reader;
typedef struct { uint64_t id; DWORD writer; unsigned readers,waiting_readers,waiting_writers; Reader owners[64]; CONDITION_VARIABLE changed; } Rwlock;
static Rwlock rwlocks[128];
static uint64_t next_rwlock=1;
static SRWLOCK rw_lock=SRWLOCK_INIT;
static Rwlock *find_rw(const uint64_t *handle) {
    if(handle && *handle) for(unsigned i=0;i<128;i++) if(rwlocks[i].id==*handle) return &rwlocks[i];
    return NULL;
}
static ABI int32_t rw_init(uint64_t *out,const void *attr,const char *name) {
    (void)name;if(!out) return INVALID;if(attr) return (int32_t)0x8002002d;
    AcquireSRWLockExclusive(&rw_lock);
    for(unsigned i=0;i<128;i++) if(!rwlocks[i].id && next_rwlock) {
        rwlocks[i]=(Rwlock){.id=next_rwlock++};InitializeConditionVariable(&rwlocks[i].changed);
        *out=rwlocks[i].id;ReleaseSRWLockExclusive(&rw_lock);return 0;
    }
    ReleaseSRWLockExclusive(&rw_lock);return NO_MEMORY;
}
static Reader *reader(Rwlock *rw,DWORD thread) {
    for(unsigned i=0;i<64;i++) if(rw->owners[i].thread==thread && rw->owners[i].count) return &rw->owners[i];
    return NULL;
}
static int32_t rw_acquire(uint64_t *handle,int write,int try_only) {
    AcquireSRWLockExclusive(&rw_lock);Rwlock *rw=find_rw(handle);DWORD self=GetCurrentThreadId();
    if(!rw) {ReleaseSRWLockExclusive(&rw_lock);return INVALID;}
    Reader *own=reader(rw,self);
    if(rw->writer==self || (write && own)) {ReleaseSRWLockExclusive(&rw_lock);return DEADLOCK;}
    unsigned *waiting=write?&rw->waiting_writers:&rw->waiting_readers;
    while(rw->writer || (write?rw->readers:(!own && rw->waiting_writers))) {
        if(try_only) {ReleaseSRWLockExclusive(&rw_lock);return BUSY;}
        ++*waiting;
        if(!SleepConditionVariableSRW(&rw->changed,&rw_lock,INFINITE,0)) ExitProcess(21);
        --*waiting;
    }
    if(write) rw->writer=self;
    else {
        if(!own) for(unsigned i=0;i<64;i++) if(!rw->owners[i].count) {own=&rw->owners[i];break;}
        if(!own || own->count==UINT32_MAX || rw->readers==UINT32_MAX) {ReleaseSRWLockExclusive(&rw_lock);return AGAIN;}
        own->thread=self;++own->count;++rw->readers;
    }
    ReleaseSRWLockExclusive(&rw_lock);return 0;
}
static ABI int32_t rw_read(uint64_t *h) {return rw_acquire(h,0,0);}
static ABI int32_t rw_write(uint64_t *h) {return rw_acquire(h,1,0);}
static ABI int32_t rw_tryread(uint64_t *h) {return rw_acquire(h,0,1);}
static ABI int32_t rw_trywrite(uint64_t *h) {return rw_acquire(h,1,1);}
static ABI int32_t rw_unlock(uint64_t *handle) {
    AcquireSRWLockExclusive(&rw_lock);Rwlock *rw=find_rw(handle);DWORD self=GetCurrentThreadId();
    if(!rw) {ReleaseSRWLockExclusive(&rw_lock);return INVALID;}
    if(rw->writer==self) rw->writer=0;
    else {Reader *own=reader(rw,self);if(!own) {ReleaseSRWLockExclusive(&rw_lock);return NOT_OWNER;}
        if(!--own->count) own->thread=0;--rw->readers;}
    WakeAllConditionVariable(&rw->changed);ReleaseSRWLockExclusive(&rw_lock);return 0;
}
static ABI int32_t rw_destroy(uint64_t *handle) {
    AcquireSRWLockExclusive(&rw_lock);Rwlock *rw=find_rw(handle);
    if(!rw) {ReleaseSRWLockExclusive(&rw_lock);return INVALID;}
    if(rw->writer || rw->readers || rw->waiting_readers || rw->waiting_writers) {ReleaseSRWLockExclusive(&rw_lock);return BUSY;}
    *rw=(Rwlock){0};*handle=0;ReleaseSRWLockExclusive(&rw_lock);return 0;
}
typedef struct {uint64_t *handle;HANDLE ready;int result;} RwTest;
static DWORD WINAPI rw_worker(void *argument) {
    RwTest *test=argument;
    test->result=rw_unlock(test->handle)!=NOT_OWNER || rw_trywrite(test->handle)!=BUSY ||
        rw_tryread(test->handle) || rw_unlock(test->handle);
    SetEvent(test->ready);
    if(rw_write(test->handle) || rw_unlock(test->handle)) test->result=1;
    return 0;
}
static int selftest_rwlocks(void) {
    uint64_t h=0,other=0;
    if(rw_init(NULL,NULL,NULL)!=INVALID || rw_init(&h,NULL,"test") || rw_init(&other,NULL,"other") ||
       rw_read(&h) || rw_read(&h) || rw_trywrite(&h)!=DEADLOCK || rw_destroy(&h)!=BUSY) return 1;
    HANDLE ready=CreateEventW(NULL,TRUE,FALSE,NULL);RwTest test={&h,ready,0};
    HANDLE thread=CreateThread(NULL,0,rw_worker,&test,0,NULL);
    if(!ready || !thread || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 || test.result ||
       WaitForSingleObject(thread,0)!=WAIT_TIMEOUT || rw_write(&other) || rw_unlock(&other) ||
       rw_unlock(&h) || rw_unlock(&h) || WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0 || test.result) return 1;
    CloseHandle(thread);CloseHandle(ready);
    if(rw_write(&h) || rw_tryread(&h)!=DEADLOCK || rw_destroy(&h)!=BUSY || rw_unlock(&h)) return 1;
    uint64_t stale=h;if(rw_destroy(&h) || rw_init(&h,NULL,NULL) || rw_read(&stale)!=INVALID ||
        rw_unlock(&h)!=NOT_OWNER || rw_destroy(&h) || rw_destroy(&other)) return 1;
    puts("rwlocks: concurrent readers, exclusive writer, ownership, recursion and stale handles passed");return 0;
}
uintptr_t windows_mutex_resolve(const char *name) {
    if (!strcmp(name,"WKAXJ4XBPQ4#U#H")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"kDh-NfxgMtE#U#H")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#U#H")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"6ULAa0fq4jA#U#H")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#U#H")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#U#H")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#U#H")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#U#H")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#U#H")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#U#H")) return (uintptr_t)rw_unlock;
    if (!strcmp(name, "1FGvU0i9saQ#U#H")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "m5-2bsNfv7s#U#H")) return (uintptr_t)condattr_init;
    if (!strcmp(name, "waPcxYiR3WA#U#H")) return (uintptr_t)condattr_destroy;
    if (!strcmp(name, "2Tb92quprl0#U#H")) return (uintptr_t)cond_init;
    if (!strcmp(name, "g+PZd2hiacg#U#H")) return (uintptr_t)cond_destroy;
    if (!strcmp(name, "F8bUHwAG284#U#H")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#U#H")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#U#H")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "cmo1RIYva9o#U#H")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#U#H")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#U#H")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#U#H")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#U#H")) return (uintptr_t)mutex_destroy;
    return 0;
}
typedef struct { Mutex **mutex; HANDLE attempted; int result; } ContentionTest;
static DWORD WINAPI contention_worker(void *arg) {
    ContentionTest *test = arg;
    test->result = mutex_trylock(test->mutex) != BUSY || mutex_unlock(test->mutex) != NOT_OWNER;
    SetEvent(test->attempted);
    if (!test->result) test->result = mutex_lock(test->mutex) || mutex_unlock(test->mutex);
    return 0;
}
int windows_mutex_selftest(void) {
    if (selftest_rwlocks() || selftest_condition_wait()) return 1;
    Attr *a = NULL, *b = NULL;
    if (attr_init(NULL) != INVALID || attr_init(&a) || attr_init(&b) || a == b || a->type != 1) return 1;
    if (attr_type(&a, 2) || a->type != 2 || b->type != 1 ||
        attr_type(&a, 0) != INVALID || attr_type(&a, 5) != INVALID || a->type != 2) return 1;
    Attr *stale = a, *unknown = (Attr *)(uintptr_t)1;
    if (attr_destroy(&a) || a || attr_type(&stale, 1) != INVALID ||
        attr_destroy(&stale) != INVALID || attr_type(&unknown, 1) != INVALID ||
        attr_destroy(NULL) != INVALID || attr_destroy(&b) || attributes) return 1;
    if (windows_mutex_resolve("F8bUHwAG284#p#J")) return 1;
    Attr *protocol_attr=NULL,*invalid_attr=(Attr *)(uintptr_t)1;
    if (attr_init(&protocol_attr) || attr_protocol(&protocol_attr,0) ||
        attr_protocol(&protocol_attr,-1)!=INVALID || attr_protocol(&protocol_attr,3)!=INVALID ||
        attr_protocol(&invalid_attr,0)!=INVALID || attr_protocol(NULL,0)!=INVALID ||
        attr_protocol(&protocol_attr,1)!=(priority_fallback()?0:(int32_t)UINT32_C(0x8002002d)) ||
        attr_protocol(&protocol_attr,2)!=(int32_t)UINT32_C(0x8002002d)) return 1;
    Mutex *profile_mutex=NULL;
    if(mutex_init(&profile_mutex,&protocol_attr,"profile") || profile_mutex->protocol!=(priority_fallback()?1:0) ||
        mutex_lock(&profile_mutex) || mutex_unlock(&profile_mutex) || mutex_destroy(&profile_mutex) || attr_destroy(&protocol_attr)) return 1;
    puts(priority_fallback()?"mutex protocol: explicit fallback selected, stored on lock and priority ceilings rejected":"mutex protocol: NONE supported, priority protocols explicitly rejected and handles validated");
    puts("mutex attributes: defaults, type validation, isolation and stale handles passed");
    Mutex *m = NULL;
    if (mutex_init(&m, NULL, "test") || mutex_lock(&m) || mutex_lock(&m) != DEADLOCK ||
        mutex_trylock(&m) != BUSY || mutex_destroy(&m) != BUSY) return 1;
    ContentionTest test = {&m, CreateEventW(NULL, TRUE, FALSE, NULL), 0};
    if (!test.attempted) return 1;
    HANDLE thread = CreateThread(NULL, 0, contention_worker, &test, 0, NULL);
    if (!thread || WaitForSingleObject(test.attempted, 5000) != WAIT_OBJECT_0 || mutex_unlock(&m) ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || test.result) return 1;
    CloseHandle(thread); CloseHandle(test.attempted);
    Mutex *stale_mutex = m;
    if (mutex_destroy(&m) || m || mutex_lock(&stale_mutex) != INVALID || mutex_unlock(&m) != INVALID) return 1;
    if (attr_init(&a) || attr_type(&a, 2) || mutex_init(&m, &a, "recursive") || attr_destroy(&a) ||
        mutex_lock(&m) || mutex_lock(&m) || mutex_unlock(&m) || mutex_destroy(&m) != BUSY ||
        mutex_unlock(&m) || mutex_destroy(&m)) return 1;
    puts("mutex locks: ownership, recursion, contention, busy destruction and stale handles passed");
    CondAttr *ca = NULL;
    Cond *c = NULL, *d = NULL;
    if (condattr_init(NULL) != INVALID || cond_init(NULL, NULL, NULL) != INVALID ||
        condattr_init(&ca) || cond_init(&c, &ca, "condition") ||
        cond_init(&d, NULL, "default") || c == d || condattr_destroy(&ca) || ca) return 1;
    CondAttr *bad = (CondAttr *)(uintptr_t)1;
    Cond *unchanged = NULL;
    if (cond_init(&unchanged, &bad, NULL) != INVALID || unchanged ||
        condattr_destroy(&bad) != INVALID || condattr_destroy(&ca) != INVALID) return 1;
    Cond *stale_cond = c;
    if (cond_destroy(&c) || c || cond_destroy(&stale_cond) != INVALID ||
        cond_destroy(&d) || conditions || cond_attributes) return 1;
    if (!windows_mutex_resolve("WKAXJ4XBPQ4#U#H")) return 1;
    puts("condition lifecycle: attributes, defaults, isolation and stale handles passed");
    return 0;
}

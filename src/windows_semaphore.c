/* SPDX-License-Identifier: GPL-2.0-or-later
 * ABI/errors adapted from compatibility-source/src/runtime_sema.c.
 * FIFO counted waits using Windows SRW locks and condition variables.
 * Priority ordering and non-null creation options are not implemented. */
#include "windows_semaphore.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ABI __attribute__((sysv_abi))
#define ERR(n) ((int32_t)(0x80020000u|(n)))
typedef struct Waiter { int need,done; int32_t result; CONDITION_VARIABLE changed; struct Waiter *next; } Waiter;
typedef struct Sema { uint32_t id; int count,maximum,deleted; unsigned active; Waiter *first; struct Sema *next; } Sema;
static Sema *registry;
static uint32_t next_id=1;
static SRWLOCK lock=SRWLOCK_INIT;
static Sema *find(uint32_t id) { for(Sema *s=registry;s;s=s->next) if(s->id==id) return s;return NULL; }
static uint64_t now_us(void) {
    LARGE_INTEGER now,hz;
    if(!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&hz) || hz.QuadPart<=0) ExitProcess(21);
    return (uint64_t)(((__uint128_t)(uint64_t)now.QuadPart*1000000)/(uint64_t)hz.QuadPart);
}
static void dispatch(Sema *s) {
    while(s->first && s->first->need<=s->count) {
        Waiter *w=s->first;s->first=w->next;s->count-=w->need;w->done=1;WakeConditionVariable(&w->changed);
    }
}
static ABI int32_t create(uint32_t *out,const char *name,uint32_t attr,int32_t initial,int32_t maximum,const void *options) {
    if(!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if(attr==2 || options) {fprintf(stderr,"Unsupported semaphore profile: attr=%u options=%p\n",attr,options);return ERR(45);}
    Sema *s=calloc(1,sizeof(*s));if(!s) return ERR(12);
    AcquireSRWLockExclusive(&lock);
    if(!next_id) {ReleaseSRWLockExclusive(&lock);free(s);return ERR(28);}
    s->id=next_id++;s->count=initial;s->maximum=maximum;s->next=registry;registry=s;*out=s->id;
    ReleaseSRWLockExclusive(&lock);
    printf("Runtime: FIFO semaphore created, id=%u count=%d maximum=%d\n",*out,initial,maximum);return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    AcquireSRWLockExclusive(&lock);Sema *s=find(id);
    if(!s || need<=0 || need>s->maximum) {ReleaseSRWLockExclusive(&lock);return s?ERR(22):ERR(3);}
    if(!s->first && need<=s->count) {s->count-=need;ReleaseSRWLockExclusive(&lock);return 0;}
    if(!block || (timeout && !*timeout)) {ReleaseSRWLockExclusive(&lock);return block?ERR(60):ERR(16);}
    Waiter w={.need=need};InitializeConditionVariable(&w.changed);
    Waiter **tail=&s->first;while(*tail) tail=&(*tail)->next;*tail=&w;++s->active;
    uint64_t deadline=timeout?now_us()+*timeout:0;
    while(!w.done) {
        uint64_t now=timeout?now_us():0;
        if(timeout && now>=deadline) {
            Waiter **at=&s->first;while(*at && *at!=&w) at=&(*at)->next;
            if(*at) *at=w.next;
            w.done=1;w.result=ERR(60);dispatch(s);break;
        }
        DWORD ms=timeout?(DWORD)((deadline-now+999)/1000):INFINITE;
        if(!SleepConditionVariableSRW(&w.changed,&lock,ms,0) && GetLastError()!=ERROR_TIMEOUT) ExitProcess(21);
    }
    if(timeout) {uint64_t now=now_us();*timeout=now<deadline?(uint32_t)(deadline-now):0;}
    int32_t result=w.result;
    if(!--s->active && s->deleted) free(s);
    ReleaseSRWLockExclusive(&lock);return result;
}
static ABI int32_t wait(uint32_t id,int32_t need,uint32_t *timeout) {return wait_count(id,need,timeout,1);}
static ABI int32_t poll(uint32_t id,int32_t need) {return wait_count(id,need,NULL,0);}
static ABI int32_t signal(uint32_t id,int32_t count) {
    AcquireSRWLockExclusive(&lock);Sema *s=find(id);
    if(!s || count<=0 || count>s->maximum-s->count) {ReleaseSRWLockExclusive(&lock);return s?ERR(22):ERR(3);}
    s->count+=count;dispatch(s);ReleaseSRWLockExclusive(&lock);return 0;
}
static ABI int32_t destroy(uint32_t id) {
    AcquireSRWLockExclusive(&lock);Sema **at=&registry;while(*at && (*at)->id!=id) at=&(*at)->next;
    if(!*at) {ReleaseSRWLockExclusive(&lock);return ERR(3);}
    Sema *s=*at;*at=s->next;s->deleted=1;
    while(s->first) {Waiter *w=s->first;s->first=w->next;w->done=1;w->result=ERR(13);WakeConditionVariable(&w->changed);}
    if(!s->active) free(s);
    ReleaseSRWLockExclusive(&lock);return 0;
}
uintptr_t windows_semaphore_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
    EXPORT("188x57JYp0g",create);EXPORT("R1Jvn8bSCW8",destroy);
    EXPORT("Zxa0VhQVTsk",wait);EXPORT("4czppHBiriw",signal);EXPORT("12wOHk8ywb0",poll);
#undef EXPORT
    return 0;
}
typedef struct {uint32_t id;int32_t result;} Test;
static DWORD WINAPI worker(void *argument) {Test *t=argument;t->result=wait(t->id,2,NULL);return 0;}
static int queued(uint32_t id) {
    for(unsigned i=0;i<5000;i++) {AcquireSRWLockShared(&lock);Sema *s=find(id);int present=s && s->first;
        ReleaseSRWLockShared(&lock);if(present) return 1;Sleep(1);}
    return 0;
}
int windows_semaphore_selftest(void) {
#define CHECK(c) do {if(!(c)) {fprintf(stderr,"semaphore check failed at %d\n",__LINE__);return 1;}}while(0)
    uint32_t id=0,other=0,timeout=1000;
    CHECK(create(NULL,"test",0,0,2,NULL)==ERR(22));CHECK(create(&id,"test",2,0,2,NULL)==ERR(45));
    CHECK(!create(&id,"test",1,1,2,NULL));CHECK(!create(&other,"other",0,0,2,NULL));
    CHECK(!poll(id,1));CHECK(poll(id,1)==ERR(16));CHECK(poll(other,1)==ERR(16));
    CHECK(wait(id,1,&timeout)==ERR(60) && !timeout);CHECK(signal(id,3)==ERR(22));
    Test t={id,123};HANDLE thread=CreateThread(NULL,0,worker,&t,0,NULL);CHECK(thread && queued(id));
    CHECK(!signal(id,1));CHECK(poll(id,1)==ERR(16));CHECK(!signal(id,1));
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && !t.result);CloseHandle(thread);
    t.result=123;thread=CreateThread(NULL,0,worker,&t,0,NULL);CHECK(thread && queued(id));
    CHECK(!destroy(id));CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && t.result==ERR(13));CloseHandle(thread);
    CHECK(destroy(id)==ERR(3));CHECK(signal(id,1)==ERR(3));CHECK(!destroy(other));
    CHECK(!windows_semaphore_resolve("188x57JYp0g#wrong"));
    puts("Semaphores: counted FIFO wait, timeout, isolation, overflow and deletion wake passed");return 0;
}

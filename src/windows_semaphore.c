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
/* Event-flag ABI and bit modes follow shadPS4's GPL-2.0-or-later
 * src/core/libraries/kernel/threads/event_flag.cpp (2024-2026 contributors).
 * Windows implementation uses numeric handles and retains deleted objects
 * until every awakened waiter has stopped accessing them. */
typedef struct FlagWaiter {
    uint64_t bits,result;
    uint32_t mode;
    int done;
    int32_t error;
    CONDITION_VARIABLE changed;
    struct FlagWaiter *next;
} FlagWaiter;
typedef struct Flag {
    uint64_t id,bits;
    int multi,deleted;
    unsigned active;
    FlagWaiter *first;
    struct Flag *next;
} Flag;
static Flag *flags;
static uint64_t next_flag=0x100000000;
static Flag *find_flag(uint64_t id) {
    for(Flag *f=flags;f;f=f->next) if(f->id==id) return f;
    return NULL;
}
static int flag_matches(Flag *f,FlagWaiter *w) {
    return (w->mode&15)==1 ? (f->bits&w->bits)==w->bits : (f->bits&w->bits)!=0;
}
static void flag_consume(Flag *f,FlagWaiter *w) {
    w->result=f->bits;
    if((w->mode&0xf0)==0x10) f->bits=0;
    else if((w->mode&0xf0)==0x20) f->bits&=~w->bits;
    w->done=1;
}
static void flag_dispatch(Flag *f) {
    FlagWaiter **at=&f->first;
    while(*at) {
        FlagWaiter *w=*at;
        if(flag_matches(f,w)) {
            *at=w->next;flag_consume(f,w);WakeConditionVariable(&w->changed);
        } else at=&w->next;
    }
}
static ABI int32_t flag_create(uint64_t *out,const char *name,uint32_t attr,uint64_t bits,const void *options) {
    if(!out || !name || (attr&~0x33u) || (attr&15)>2 || (attr&0x30)==0x30) return ERR(22);
    if(strlen(name)>=32) return ERR(63);
    if(options || (attr&15)==2) return ERR(45);
    Flag *f=calloc(1,sizeof(*f));if(!f) return ERR(12);
    AcquireSRWLockExclusive(&lock);
    if(!next_flag) {ReleaseSRWLockExclusive(&lock);free(f);return ERR(28);}
    f->id=next_flag++;f->bits=bits;f->multi=(attr&0x30)==0x20;
    f->next=flags;flags=f;*out=f->id;
    ReleaseSRWLockExclusive(&lock);
    printf("Runtime: FIFO event flag created, id=%llu attr=0x%x\n",(unsigned long long)*out,attr);
    return 0;
}
static int32_t flag_wait_impl(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    if(!bits || (mode&~0x33u) || ((mode&15)!=1 && (mode&15)!=2) || (mode&0x30)==0x30) return ERR(22);
    AcquireSRWLockExclusive(&lock);Flag *f=find_flag(id);
    if(!f) {ReleaseSRWLockExclusive(&lock);return ERR(3);}
    if(!f->multi && f->first) {ReleaseSRWLockExclusive(&lock);return ERR(1);}
    FlagWaiter w={.bits=bits,.mode=mode};
    uint64_t deadline=timeout?now_us()+*timeout:0;
    if(flag_matches(f,&w)) {
        flag_consume(f,&w);if(result) *result=w.result;
        ReleaseSRWLockExclusive(&lock);return 0;
    }
    if(!block || (timeout && !*timeout)) {
        if(result) *result=f->bits;
        ReleaseSRWLockExclusive(&lock);return block?ERR(60):ERR(16);
    }
    InitializeConditionVariable(&w.changed);
    FlagWaiter **tail=&f->first;while(*tail) tail=&(*tail)->next;*tail=&w;++f->active;
    while(!w.done) {
        uint64_t now=timeout?now_us():0;
        if(timeout && now>=deadline) {
            FlagWaiter **at=&f->first;while(*at && *at!=&w) at=&(*at)->next;
            if(*at) *at=w.next;
            w.result=f->bits;w.error=ERR(60);w.done=1;break;
        }
        DWORD ms=timeout?(DWORD)((deadline-now+999)/1000):INFINITE;
        if(!SleepConditionVariableSRW(&w.changed,&lock,ms,0) && GetLastError()!=ERROR_TIMEOUT) ExitProcess(21);
    }
    if(timeout) {uint64_t now=now_us();*timeout=now<deadline?(uint32_t)(deadline-now):0;}
    if(result) *result=w.result;
    if(!--f->active && f->deleted) free(f);
    ReleaseSRWLockExclusive(&lock);return w.error;
}
static ABI int32_t flag_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return flag_wait_impl(id,bits,mode,result,timeout,1);
}
static ABI int32_t flag_poll(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result) {
    return flag_wait_impl(id,bits,mode,result,NULL,0);
}
static ABI int32_t flag_set(uint64_t id,uint64_t bits) {
    AcquireSRWLockExclusive(&lock);Flag *f=find_flag(id);
    if(f) {f->bits|=bits;flag_dispatch(f);}
    ReleaseSRWLockExclusive(&lock);return f?0:ERR(3);
}
static ABI int32_t flag_clear(uint64_t id,uint64_t keep) {
    AcquireSRWLockExclusive(&lock);Flag *f=find_flag(id);
    if(f) f->bits&=keep;
    ReleaseSRWLockExclusive(&lock);return f?0:ERR(3);
}
static void flag_wake_all(Flag *f,int32_t error) {
    while(f->first) {
        FlagWaiter *w=f->first;f->first=w->next;
        w->result=f->bits;w->error=error;w->done=1;WakeConditionVariable(&w->changed);
    }
}
static ABI int32_t flag_cancel(uint64_t id,uint64_t bits,int32_t *count) {
    AcquireSRWLockExclusive(&lock);Flag *f=find_flag(id);
    if(!f) {ReleaseSRWLockExclusive(&lock);return ERR(3);}
    int32_t n=0;for(FlagWaiter *w=f->first;w;w=w->next) ++n;
    if(count) *count=n;
    f->bits=bits;flag_wake_all(f,ERR(85));
    ReleaseSRWLockExclusive(&lock);return 0;
}
static ABI int32_t flag_delete(uint64_t id) {
    AcquireSRWLockExclusive(&lock);Flag **at=&flags;
    while(*at && (*at)->id!=id) at=&(*at)->next;
    Flag *f=*at;
    if(!f) {ReleaseSRWLockExclusive(&lock);return ERR(3);}
    *at=f->next;f->deleted=1;flag_wake_all(f,ERR(13));
    if(!f->active) free(f);
    ReleaseSRWLockExclusive(&lock);return 0;
}
uintptr_t windows_semaphore_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
    EXPORT("188x57JYp0g",create);EXPORT("R1Jvn8bSCW8",destroy);
    EXPORT("Zxa0VhQVTsk",wait);EXPORT("4czppHBiriw",signal);EXPORT("12wOHk8ywb0",poll);
    EXPORT("BpFoboUJoZU",flag_create);EXPORT("8mql9OcQnd4",flag_delete);
    EXPORT("JTvBflhYazQ",flag_wait);EXPORT("9lvj5DjHZiA",flag_poll);
    EXPORT("IOnSvHzqu6A",flag_set);EXPORT("7uhBFWRAS60",flag_clear);EXPORT("PZku4ZrXJqg",flag_cancel);
#undef EXPORT
    return 0;
}
typedef struct {uint32_t id;int32_t result;} Test;
typedef struct {uint64_t id,bits,result;uint32_t mode;int32_t error;} FlagTest;
static DWORD WINAPI flag_worker(void *argument) {
    FlagTest *t=argument;t->error=flag_wait(t->id,t->bits,t->mode,&t->result,NULL);return 0;
}
static int flag_queued(uint64_t id,unsigned expected) {
    for(unsigned i=0;i<5000;i++) {
        AcquireSRWLockShared(&lock);Flag *f=find_flag(id);unsigned n=0;
        if(f) for(FlagWaiter *w=f->first;w;w=w->next) ++n;
        ReleaseSRWLockShared(&lock);if(n==expected) return 1;Sleep(1);
    }
    return 0;
}
int windows_eventflag_selftest(void) {
#define FLAG_CHECK(c) do {if(!(c)) {fprintf(stderr,"event flag check failed at %d\n",__LINE__);return 1;}}while(0)
    uint64_t id=0,single=0,result=0;uint32_t timeout=1000;int32_t canceled=0;
    FLAG_CHECK(flag_create(NULL,"test",0x21,0,NULL)==ERR(22));
    FLAG_CHECK(flag_create(&id,"test",0x22,0,NULL)==ERR(45));
    FLAG_CHECK(flag_create(&id,"test",0x31,0,NULL)==ERR(22));
    FLAG_CHECK(!flag_create(&id,"test",0x21,5,NULL));
    FLAG_CHECK(!flag_poll(id,1,1,&result) && result==5);
    FLAG_CHECK(flag_poll(id,3,1,&result)==ERR(16) && result==5);
    FLAG_CHECK(!flag_poll(id,3,0x22,&result) && result==5);
    FLAG_CHECK(flag_poll(id,1,1,NULL)==ERR(16));
    FLAG_CHECK(!flag_clear(id,0) && flag_poll(id,4,1,NULL)==ERR(16));
    FLAG_CHECK(!flag_set(id,8) && !flag_poll(id,8,0x11,&result) && result==8);
    FLAG_CHECK(flag_poll(id,8,1,NULL)==ERR(16));
    FLAG_CHECK(flag_poll(id,0,1,NULL)==ERR(22) && flag_poll(id,1,3,NULL)==ERR(22));
    FLAG_CHECK(flag_wait(id,1,1,&result,&timeout)==ERR(60) && !timeout);
    /* FIFO matching waiters consume bits in registration order. */
    FlagTest a={.id=id,.bits=1,.mode=0x21,.error=123},b=a;
    HANDLE ta=CreateThread(NULL,0,flag_worker,&a,0,NULL);FLAG_CHECK(ta && flag_queued(id,1));
    HANDLE tb=CreateThread(NULL,0,flag_worker,&b,0,NULL);FLAG_CHECK(tb && flag_queued(id,2));
    FLAG_CHECK(!flag_set(id,1) && WaitForSingleObject(ta,5000)==WAIT_OBJECT_0 && !a.error && a.result==1);
    FLAG_CHECK(WaitForSingleObject(tb,0)==WAIT_TIMEOUT && flag_queued(id,1));CloseHandle(ta);
    FLAG_CHECK(!flag_cancel(id,4,&canceled) && canceled==1);
    FLAG_CHECK(WaitForSingleObject(tb,5000)==WAIT_OBJECT_0 && b.error==ERR(85) && b.result==4);CloseHandle(tb);
    a.bits=2;a.error=123;ta=CreateThread(NULL,0,flag_worker,&a,0,NULL);FLAG_CHECK(ta && flag_queued(id,1));
    FLAG_CHECK(!flag_delete(id) && WaitForSingleObject(ta,5000)==WAIT_OBJECT_0 && a.error==ERR(13));CloseHandle(ta);
    FLAG_CHECK(flag_delete(id)==ERR(3) && flag_set(id,1)==ERR(3));
    FLAG_CHECK(!flag_create(&single,"single",0x11,0,NULL));a.id=single;a.bits=1;
    ta=CreateThread(NULL,0,flag_worker,&a,0,NULL);FLAG_CHECK(ta && flag_queued(single,1));
    FLAG_CHECK(flag_poll(single,1,1,NULL)==ERR(1));FLAG_CHECK(!flag_set(single,1));
    FLAG_CHECK(WaitForSingleObject(ta,5000)==WAIT_OBJECT_0 && !a.error);CloseHandle(ta);
    FLAG_CHECK(!flag_delete(single) && !windows_semaphore_resolve("BpFoboUJoZU#other"));
    puts("Event flags: AND/OR bits, FIFO consumption, single waiter, timeout, cancellation and deletion wake passed");
    return 0;
#undef FLAG_CHECK
}
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

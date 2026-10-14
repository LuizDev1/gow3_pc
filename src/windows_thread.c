/* SPDX-License-Identifier: GPL-2.0-or-later
 * Attribute ABI/defaults adapted from compatibility-source/src/runtime_thread.c.
 * Joinable guest threads use Windows workers; PS4 scheduling remains unresolved. */
#include "windows_thread.h"
#include "windows_tls.h"
#include "windows_memory.h"
#include "windows_renderer_runtime.h"
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#define ABI __attribute__((sysv_abi))
#define INVALID ((int32_t)0x80020016)
#define NO_THREAD ((int32_t)0x80020003)
#define NOMEM ((int32_t)0x8002000c)
#define LIMIT 256
typedef struct { uint64_t id,stack,guard,affinity; void *stack_address; int detach,policy,priority,inherit; } Attr;
static _Thread_local const Attr *active_guest_attr;
static _Thread_local int current_priority=700;
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
SETTER(set_stack_address,stack_address,void *,value!=NULL)
SETTER(set_guard,guard,uint64_t,1)
SETTER(set_detach,detach,int,value==0 || value==1)
SETTER(set_policy,policy,int,value>=1 && value<=3)
SETTER(set_inherit,inherit,int,value==0 || value==4)
SETTER(set_affinity,affinity,uint64_t,1)
GETTER(get_stack,stack,uint64_t)
GETTER(get_stack_address,stack_address,void *)
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
    void *stack_address=memory.AllocationBase;
    if(active_guest_attr && active_guest_attr->stack_address) {
        stack=active_guest_attr->stack;stack_address=active_guest_attr->stack_address;
    }
    AcquireSRWLockExclusive(&lock);Attr *a=find(out);
    if(!a) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    uint64_t id=a->id;
    /* These scheduling fields describe the guest's default profile, not an
     * emulated PS4 scheduler. Host-owned threads have no runtime guard request. */
    *a=(Attr){.id=id,.stack=stack,.stack_address=stack_address,.affinity=0x7f,.policy=1,
        .priority=active_guest_attr?active_guest_attr->priority:current_priority,.inherit=4};
    ReleaseSRWLockExclusive(&lock);return 0;
}

typedef void *(ABI *GuestEntry)(void *);
/* The caller owns and keeps the entire fixed stack mapped through join. Its
 * existing guest memory registration remains in place; it is never freed here.
 * Save the Win64 nonvolatile state before calling a SysV entry on that stack.
 * Windows exception/stack queries must see the bounds of the active stack.
 * No unwinding through this bridge or automatic growth of caller storage is
 * supported. Normal returns restore the host stack before thread cleanup. */
_Static_assert(offsetof(NT_TIB,StackBase)==8 && offsetof(NT_TIB,StackLimit)==16,
               "Win64 NT_TIB stack layout");
/* GetCurrentThreadStackLimits uses DeallocationStack as its lower bound.
 * The x64 field immediately precedes TlsSlots (Wine include/winternl.h and
 * dlls/kernelbase/thread.c document the layout/query). Validate the layout and
 * the running Windows API before using this private Windows ABI field. */
_Static_assert(offsetof(TEB,TlsSlots)-sizeof(void *)==0x1478,
               "Win64 TEB DeallocationStack layout");
void *windows_call_guest_stack(GuestEntry entry,void *argument,void *base,uint64_t size);
__asm__(".text\n"
        ".globl windows_call_guest_stack\n"
        ".def windows_call_guest_stack; .scl 2; .type 32; .endef\n"
        "windows_call_guest_stack:\n"
        "push %rbx\n push %rbp\n push %rdi\n push %rsi\n"
        "push %r12\n push %r13\n push %r14\n push %r15\n sub $168,%rsp\n"
        "movups %xmm6,0(%rsp)\n movups %xmm7,16(%rsp)\n movups %xmm8,32(%rsp)\n"
        "movups %xmm9,48(%rsp)\n movups %xmm10,64(%rsp)\n movups %xmm11,80(%rsp)\n"
        "movups %xmm12,96(%rsp)\n movups %xmm13,112(%rsp)\n movups %xmm14,128(%rsp)\n"
        "movups %xmm15,144(%rsp)\n stmxcsr 160(%rsp)\n fnstcw 164(%rsp)\n"
        "mov %rsp,%rbx\n mov %gs:8,%r12\n mov %gs:16,%r13\n mov %gs:0x1478,%r14\n"
        "mov %rcx,%rax\n mov %rdx,%rdi\n mov %r8,%gs:16\n mov %r8,%gs:0x1478\n"
        "add %r9,%r8\n mov %r8,%gs:8\n and $-16,%r8\n mov %r8,%rsp\n"
        "call *%rax\n"
        "mov %rbx,%rsp\n mov %r12,%gs:8\n mov %r13,%gs:16\n mov %r14,%gs:0x1478\n"
        "movups 0(%rsp),%xmm6\n movups 16(%rsp),%xmm7\n movups 32(%rsp),%xmm8\n"
        "movups 48(%rsp),%xmm9\n movups 64(%rsp),%xmm10\n movups 80(%rsp),%xmm11\n"
        "movups 96(%rsp),%xmm12\n movups 112(%rsp),%xmm13\n movups 128(%rsp),%xmm14\n"
        "movups 144(%rsp),%xmm15\n ldmxcsr 160(%rsp)\n fldcw 164(%rsp)\n"
        "add $168,%rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n"
        "pop %rsi\n pop %rdi\n pop %rbp\n pop %rbx\n ret\n");

static int valid_guest_stack(void *base,uint64_t size) {
    uintptr_t address=(uintptr_t)base;
    if(!base || !size || size>UINTPTR_MAX-address ||
       !runtime_memory_is_mapped(address,size)) return 0;
    uintptr_t end=address+size;
    while(address<end) {
        MEMORY_BASIC_INFORMATION info;
        if(!VirtualQuery((void *)address,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&PAGE_GUARD) ||
           !(info.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return 0;
        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        if(next<=address) return 0;
        address=next<end?next:end;
    }
    return 1;
}
typedef struct Thread { void *tcb; HANDLE host; GuestEntry entry; void *argument,*result;
    Attr attr; int finished,joining,joined; struct Thread *next; } Thread;
static Thread *threads;
static unsigned thread_count;
/* Decoder/HLE workers may enter guest callbacks. They need the configured guest
 * TLS and a stable thread record; these bounded host records live to process exit. */
void runtime_thread_attach_host(const char *name) {
    (void)name;
    if(!windows_memory_register_current_stack()) ExitProcess(21);
    if(windows_tls_current_thread()) return;
    AcquireSRWLockExclusive(&lock);
    Thread *t=thread_count<LIMIT?calloc(1,sizeof(*t)):NULL;
    if(!t) {ReleaseSRWLockExclusive(&lock);ExitProcess(21);}
    t->attr=(Attr){.stack=1024*1024,.guard=4096,.affinity=0x7f,.policy=1,.priority=700,.inherit=4};
    t->joined=1;t->next=threads;threads=t;thread_count++;
    ReleaseSRWLockExclusive(&lock);
    if(windows_tls_attach_main() || windows_tls_bind_thread(t)) ExitProcess(21);
}
void runtime_thread_detach_host(void) {
    windows_memory_unregister_current_stack();windows_tls_detach();
}
static Thread *find_thread(void *handle) {
    for(Thread *t=threads;t;t=t->next) if(t==handle) return t;return NULL;
}
/* PS4 CPU bits map to the same low host logical CPU bits. A host that cannot
 * satisfy the requested mask returns an error; no scheduler success is faked. */
static ABI int32_t thread_set_affinity(void *handle,uint64_t mask) {
    if(!handle || !mask || (mask&~UINT64_C(0xff))) return INVALID;
    AcquireSRWLockExclusive(&lock);Thread *t=find_thread(handle);
    HANDLE host=handle==thread_self()?GetCurrentThread():
        t && !t->finished && !t->joined?t->host:NULL;
    if(!host) {ReleaseSRWLockExclusive(&lock);return NO_THREAD;}
    DWORD_PTR previous=SetThreadAffinityMask(host,(DWORD_PTR)mask);
    if(previous && t) t->attr.affinity=mask;
    ReleaseSRWLockExclusive(&lock);return previous?0:INVALID;
}
/* Coarse Windows relative levels preserve the guest priority ordering around
 * the default 700. This is not an emulation of the PS4 FIFO scheduler. */
static int host_priority(int guest) {
    return guest<512?THREAD_PRIORITY_HIGHEST:guest<700?THREAD_PRIORITY_ABOVE_NORMAL:
        guest==700?THREAD_PRIORITY_NORMAL:THREAD_PRIORITY_BELOW_NORMAL;
}
static ABI int32_t thread_set_priority(void *handle,int priority) {
    if(!handle || priority<256 || priority>767) return INVALID;
    AcquireSRWLockExclusive(&lock);Thread *t=find_thread(handle);
    int current=handle==thread_self();
    HANDLE host=current?GetCurrentThread():t && !t->finished && !t->joined?t->host:NULL;
    if(!host) {ReleaseSRWLockExclusive(&lock);return NO_THREAD;}
    int ok=SetThreadPriority(host,host_priority(priority));
    if(ok) {if(t) t->attr.priority=priority;if(current) current_priority=priority;}
    ReleaseSRWLockExclusive(&lock);return ok?0:INVALID;
}
static ABI int32_t thread_get_priority(void *handle,int *out) {
    if(!handle || !out) return INVALID;
    AcquireSRWLockShared(&lock);Thread *t=find_thread(handle);
    int current=handle==thread_self();
    int valid=current || (t && t->host && !t->finished && !t->joined);
    if(valid) *out=t?t->attr.priority:current_priority;
    ReleaseSRWLockShared(&lock);return valid?0:NO_THREAD;
}
static DWORD WINAPI run_guest_thread(void *argument) {
    Thread *t=argument;
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    void *host_base=tib->StackBase,*host_limit=tib->StackLimit;
    ULONG_PTR host_low=0,host_high=0;
    GetCurrentThreadStackLimits(&host_low,&host_high);
    void *host_allocation=*(void **)((unsigned char *)tib+0x1478);
    if(t->attr.stack_address && (host_low!=(ULONG_PTR)host_allocation || host_high!=(ULONG_PTR)host_base)) {
        fputs("STOP: Windows custom-stack ABI is not supported on this system\n",stderr);ExitProcess(21);
    }
    if(!windows_memory_register_current_stack()) ExitProcess(21);
    if(windows_tls_attach_main() || windows_tls_bind_thread(t)) {
        fputs("STOP: guest worker TLS initialization failed\n",stderr);ExitProcess(21);
    }
    active_guest_attr=&t->attr;
    void *result=t->attr.stack_address?
        windows_call_guest_stack(t->entry,t->argument,t->attr.stack_address,t->attr.stack):
        t->entry(t->argument);
    active_guest_attr=NULL;
    ULONG_PTR restored_low=0,restored_high=0;
    GetCurrentThreadStackLimits(&restored_low,&restored_high);
    if(tib->StackBase!=host_base || tib->StackLimit!=host_limit ||
       restored_low!=host_low || restored_high!=host_high) {
        fputs("STOP: guest worker did not restore Windows stack bounds\n",stderr);ExitProcess(21);
    }
    windows_memory_unregister_current_stack();
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
    if(selected.detach || (!selected.stack_address && selected.guard!=4096)) {ReleaseSRWLockExclusive(&lock);return (int32_t)0x8002002d;}
    if(selected.stack<16384 || selected.stack>64*1024*1024 || thread_count>=256) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    if(selected.stack_address) {
        if(!valid_guest_stack(selected.stack_address,selected.stack)) {ReleaseSRWLockExclusive(&lock);return INVALID;}
        uintptr_t low=(uintptr_t)selected.stack_address,high=low+selected.stack;
        for(Thread *live=threads;live;live=live->next) if(!live->finished && live->attr.stack_address) {
            uintptr_t other=(uintptr_t)live->attr.stack_address;
            if(low<other+live->attr.stack && other<high) {ReleaseSRWLockExclusive(&lock);return INVALID;}
        }
    }
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
/* Named POSIX variants from the reference GoW3 runtime_thread.c. */
static int32_t posix_result(int32_t result) {return result?(int32_t)((uint32_t)result&0xffff):0;}
static ABI int32_t posix_attr_init(uint64_t *a) {return posix_result(attr_init(a));}
static ABI int32_t posix_attr_destroy(uint64_t *a) {return posix_result(attr_destroy(a));}
static ABI int32_t posix_stack(uint64_t *a,uint64_t size) {return posix_result(set_stack(a,size));}
static ABI int32_t posix_detach(uint64_t *a,int state) {return posix_result(set_detach(a,state));}
static ABI int32_t posix_create_name(Thread **out,uint64_t *a,GuestEntry entry,void *argument,const char *name) {
    return posix_result(thread_create(out,a,entry,argument,name));
}
static ABI int32_t posix_join(void *thread,void **out) {return posix_result(thread_join(thread,out));}
static void *test_main_identity;
static ABI void *synthetic_worker(void *argument) {
    uint64_t index[2]={1,0};uint64_t *value=windows_tls_get_addr(index);
    Thread *self=thread_self();
    if(!self || self==(Thread *)test_main_identity || !self->tcb || !value || *value!=42) return NULL;
    *value=99;return argument;
}
static DWORD WINAPI test_host_worker(void *argument) {
    runtime_thread_attach_host("tlg:test");
    void *identity=thread_self();
    runtime_thread_attach_host("tlg:test-again");
    void *result=synthetic_worker(argument);
    if(identity!=thread_self()) result=NULL;
    runtime_thread_detach_host();
    return result==argument?0:1;
}
typedef struct { void *base,*fault_page; HANDLE ready,proceed; int recovered; } StackTest;
static LONG WINAPI test_stack_fault(EXCEPTION_POINTERS *exception) {
    return windows_renderer_recover(exception)?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;
}
static ABI void *synthetic_stack_worker(void *argument) {
    StackTest *test=argument;volatile unsigned char local=17;
    uintptr_t low=(uintptr_t)test->base,high=low+32768;
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    ULONG_PTR api_low=0,api_high=0;
    GetCurrentThreadStackLimits(&api_low,&api_high);
    uint64_t snapshot=0,size=0;void *address=NULL;
    if((uintptr_t)&local<low || (uintptr_t)&local>=high ||
       tib->StackBase!=(void *)high || tib->StackLimit!=test->base ||
       api_low!=low || api_high!=high ||
       !runtime_memory_is_mapped(low,32768) || attr_init(&snapshot) ||
       attr_get(thread_self(),&snapshot) || get_stack(&snapshot,&size) || size!=32768 ||
       get_stack_address(&snapshot,&address) || address!=test->base || attr_destroy(&snapshot)) return NULL;
    if(synthetic_worker(argument)!=argument) return NULL;
    TlgRecovery recovery;
    if(!gow3_setjmp(recovery)) {
        runtime_fault_recover=&recovery;
        local=*(volatile unsigned char *)test->fault_page;
        runtime_fault_recover=NULL;return NULL;
    }
    if(runtime_fault_recover || local!=17) return NULL;
    test->recovered=1;
    /* Deliberately clobber SysV-volatile registers that Win64 callers preserve. */
    __asm__ volatile("pxor %%xmm6,%%xmm6\n pxor %%xmm15,%%xmm15\n"
                     : : : "xmm6","xmm15","rdi","rsi");
    SetEvent(test->ready);
    if(WaitForSingleObject(test->proceed,10000)!=WAIT_OBJECT_0) return NULL;
    return argument;
}
static int custom_stack_selftest(void) {
#define STACK_CHECK(c) do {if(!(c)) {fprintf(stderr,"custom stack check failed at %d\n",__LINE__);return 1;}}while(0)
    typedef int32_t (ABI *MapFlexible)(void **,uint64_t,int,int);
    typedef int32_t (ABI *UnmapFlexible)(void *,uint64_t);
    MapFlexible map=(MapFlexible)windows_memory_resolve("IWIBBdTHit4#U#H");
    UnmapFlexible unmap=(UnmapFlexible)windows_memory_resolve("teiItL2boFw#U#H");
    void *storage=NULL;uint64_t attr=0;
    STACK_CHECK(map && unmap && !map(&storage,65536,3,0) && !attr_init(&attr));
    memset(storage,0xa5,65536);
    StackTest test={.base=(unsigned char *)storage+16384,
        .fault_page=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS),
        .ready=CreateEventW(NULL,TRUE,FALSE,NULL),.proceed=CreateEventW(NULL,TRUE,FALSE,NULL)};
    void *handler=AddVectoredExceptionHandler(1,test_stack_fault);
    STACK_CHECK(test.fault_page && test.ready && test.proceed && handler);
    STACK_CHECK(!set_stack(&attr,32768) && !set_guard(&attr,0));
    STACK_CHECK(set_stack_address(&attr,NULL)==INVALID && set_stack_address(NULL,test.base)==INVALID);
    STACK_CHECK(!set_stack_address(&attr,(void *)(UINTPTR_MAX-16383)));
    Thread *worker=(Thread *)(uintptr_t)123;
    STACK_CHECK(thread_create(&worker,&attr,synthetic_stack_worker,&test,"invalid-stack")==INVALID &&
                worker==(Thread *)(uintptr_t)123);
    STACK_CHECK(!set_stack_address(&attr,(void *)(uintptr_t)4096));
    STACK_CHECK(thread_create(&worker,&attr,synthetic_stack_worker,&test,"unmapped-stack")==INVALID);
    DWORD old_protection;
    STACK_CHECK(VirtualProtect(test.base,32768,PAGE_READONLY,&old_protection));
    STACK_CHECK(!set_stack_address(&attr,test.base));
    STACK_CHECK(thread_create(&worker,&attr,synthetic_stack_worker,&test,"readonly-stack")==INVALID);
    STACK_CHECK(VirtualProtect(test.base,32768,old_protection,&old_protection));
    for(int pass=0;pass<2;pass++) {
        ResetEvent(test.ready);ResetEvent(test.proceed);test.recovered=0;
        STACK_CHECK(!thread_create(&worker,&attr,synthetic_stack_worker,&test,"caller-stack"));
        STACK_CHECK(WaitForSingleObject(test.ready,10000)==WAIT_OBJECT_0 && test.recovered);
        int priority=123;
        STACK_CHECK(!thread_set_priority(worker,550) && GetThreadPriority(worker->host)==THREAD_PRIORITY_ABOVE_NORMAL);
        STACK_CHECK(!thread_get_priority(worker,&priority) && priority==550);
        Thread *collision=(Thread *)(uintptr_t)123;
        STACK_CHECK(thread_create(&collision,&attr,synthetic_stack_worker,&test,"overlap")==INVALID &&
                    collision==(Thread *)(uintptr_t)123);
        SetEvent(test.proceed);void *result=NULL;
        STACK_CHECK(!thread_join(worker,&result) && result==&test);
        STACK_CHECK(thread_set_priority(worker,550)==NO_THREAD);
        priority=123;STACK_CHECK(thread_get_priority(worker,&priority)==NO_THREAD && priority==123);
        STACK_CHECK(runtime_memory_is_mapped((uintptr_t)storage,65536));
    }
    for(unsigned i=0;i<16384;i++) {
        STACK_CHECK(((unsigned char *)storage)[i]==0xa5 && ((unsigned char *)storage)[49152+i]==0xa5);
    }
    STACK_CHECK(!attr_destroy(&attr) && !unmap(storage,65536));
    RemoveVectoredExceptionHandler(handler);VirtualFree(test.fault_page,0,MEM_RELEASE);
    CloseHandle(test.ready);CloseHandle(test.proceed);
    puts("Caller-owned stacks: actual stack and Windows bounds, TLS, fault recovery, overlap rejection and reuse passed");
    return 0;
#undef STACK_CHECK
}
int windows_thread_create_selftest(void) {
    static const uint64_t data=42;
    if(windows_tls_configure_main(&data,8,16,16) || windows_tls_attach_main()) return 1;
    test_main_identity=thread_self();if(!thread_equal(test_main_identity,thread_self()) || !thread_equal(NULL,NULL) || thread_equal(test_main_identity,NULL)) return 1;Thread *worker=NULL;void *result=NULL;
    uint64_t posix_attr=0;
    if(posix_attr_init(NULL)!=22 || posix_attr_init(&posix_attr) ||
       posix_stack(&posix_attr,1)!=22 || posix_stack(&posix_attr,131072) ||
       posix_detach(&posix_attr,2)!=22 || posix_detach(&posix_attr,0) ||
       posix_create_name(&worker,&posix_attr,synthetic_worker,(void *)(uintptr_t)123,"posix") ||
       posix_attr_destroy(&posix_attr) || posix_join(worker,&result) || result!=(void *)(uintptr_t)123 ||
       posix_join(worker,NULL)!=3 || posix_attr_destroy(&posix_attr)!=22) return 1;
    puts("POSIX threads: named worker, private TLS, join result and positive errno passed");
    if(thread_create(NULL,NULL,synthetic_worker,NULL,NULL)!=INVALID ||
        thread_create(&worker,NULL,NULL,NULL,NULL)!=INVALID ||
        thread_create(&worker,NULL,synthetic_worker,(void *)(uintptr_t)123,"test") ||
        thread_join(worker,&result) || result!=(void *)(uintptr_t)123 || thread_join(worker,NULL)!=NO_THREAD) return 1;
    uint64_t index[2]={1,0};uint64_t *value=windows_tls_get_addr(index);
    if(!value || *value!=42 || thread_self()!=test_main_identity) return 1;
    HANDLE host=CreateThread(NULL,0,test_host_worker,(void *)(uintptr_t)0x1234,0,NULL);
    DWORD status=1;
    if(!host || WaitForSingleObject(host,10000)!=WAIT_OBJECT_0 || !GetExitCodeThread(host,&status) || status) return 1;
    CloseHandle(host);
    if(custom_stack_selftest()) return 1;
    windows_tls_detach();puts("Host renderer workers: private guest TLS and idempotent attachment passed");
    puts("Guest threads: SysV entry, private TLS, stable identity, join result and stale join passed");return 0;
}
uintptr_t windows_thread_resolve(const char *name) {
#define POSIX(n,f) if(!strcmp(name,n "#posix")) return (uintptr_t)f
    POSIX("wtkt-teR1so",posix_attr_init);POSIX("zHchY8ft5pk",posix_attr_destroy);
    POSIX("2Q0z6rnBrTE",posix_stack);POSIX("E+tyo3lp5Lw",posix_detach);
    POSIX("Jmi+9w9u0E4",posix_create_name);POSIX("h9CcP3J0oVM",posix_join);
    POSIX("EotR8a3ASf4",thread_self);
#undef POSIX
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
    EXPORT("3PtV6p3QNX4",thread_equal);
    EXPORT("bt3CTBKmGyI",thread_set_affinity);
    EXPORT("W0Hpm2X0uPE",thread_set_priority);EXPORT("1tKyG7RlMJo",thread_get_priority);
    EXPORT("6UgtwV+0zb4",thread_create);EXPORT("onNY9Byn-W8",thread_join);
    EXPORT("aI+OeCz8xrQ",thread_self);EXPORT("x1X76arYMxU",attr_get);
    EXPORT("nsYoNRywwNg",attr_init);EXPORT("62KCwEMmzcM",attr_destroy);
    EXPORT("UTXzJbWhhTE",set_stack);EXPORT("-Wreprtu0Qs",set_detach);
    EXPORT("F+yfmduIBB8",set_stack_address);EXPORT("Ru36fiTtJzA",get_stack_address);
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
    GROUP_AFFINITY original={0},changed={0};DWORD_PTR allowed=0,system=0;
    CHECK(GetThreadGroupAffinity(GetCurrentThread(),&original));
    CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    uint64_t available=(uint64_t)allowed&0xff;
    CHECK(available);
    uint64_t selected=available&(~available+1);
    CHECK(thread_set_affinity(NULL,selected)==INVALID && thread_set_affinity(thread_self(),0)==INVALID);
    CHECK(thread_set_affinity((void *)(uintptr_t)1,selected)==NO_THREAD);
    CHECK(thread_set_affinity(thread_self(),0x100)==INVALID);
    CHECK(!thread_set_affinity(thread_self(),selected));
    CHECK(GetThreadGroupAffinity(GetCurrentThread(),&changed) && changed.Mask==selected);
    CHECK(SetThreadAffinityMask(GetCurrentThread(),original.Mask)!=0);
    puts("Thread affinity: actual Windows CPU mask, restoration and invalid-handle rejection passed");
    int original_priority=GetThreadPriority(GetCurrentThread()),requested=123;
    CHECK(original_priority!=THREAD_PRIORITY_ERROR_RETURN);
    CHECK(thread_set_priority(NULL,550)==INVALID && thread_set_priority(thread_self(),255)==INVALID);
    CHECK(thread_set_priority(thread_self(),768)==INVALID);
    CHECK(thread_set_priority((void *)(uintptr_t)1,550)==NO_THREAD);
    CHECK(thread_get_priority((void *)(uintptr_t)1,&requested)==NO_THREAD && requested==123);
    CHECK(thread_get_priority(thread_self(),NULL)==INVALID);
    CHECK(!thread_set_priority(thread_self(),550));
    CHECK(GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_ABOVE_NORMAL);
    CHECK(!thread_get_priority(thread_self(),&requested) && requested==550);
    uint64_t priority_snapshot=0;
    CHECK(!attr_init(&priority_snapshot) && !attr_get(thread_self(),&priority_snapshot));
    CHECK(!get_priority(&priority_snapshot,&requested) && requested==550 && !attr_destroy(&priority_snapshot));
    CHECK(!thread_set_priority(thread_self(),700) && GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_NORMAL);
    CHECK(SetThreadPriority(GetCurrentThread(),original_priority));
    puts("Thread priority: Windows relative level, guest-value query, restoration and invalid-handle rejection passed");
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
    void *stack_address=(void *)(uintptr_t)123;
    CHECK(!get_stack_address(&a,&stack_address) && !stack_address);
    CHECK(set_stack_address(&stale,(void *)(uintptr_t)4096)==INVALID);
    CHECK(get_stack_address(&stale,&stack_address)==INVALID && !stack_address);
    CHECK(!set_stack_address(&a,(void *)(uintptr_t)4096));
    CHECK(!get_stack_address(&a,&stack_address) && stack_address==(void *)(uintptr_t)4096);
    CHECK(!get_stack_address(&b,&stack_address) && !stack_address);
    CHECK(windows_thread_resolve("F+yfmduIBB8#U#H")== (uintptr_t)set_stack_address);
    CHECK(!windows_thread_resolve("F+yfmduIBB8#wrong"));
    CHECK(set_stack(&stale,65536)==INVALID);CHECK(attr_destroy(&stale)==INVALID);
    CHECK(!attr_destroy(&a));CHECK(!attr_destroy(&b));CHECK(!windows_thread_resolve("nsYoNRywwNg#wrong"));
    puts("Thread attributes: defaults, validation, isolation, round trips and stale handles passed");return 0;
}

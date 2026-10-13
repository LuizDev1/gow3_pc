/* Windows direct-memory diagnostic backend. PS4 allocation contracts follow
 * compatibility-source/src/runtime_memory.c (GPL-2.0); mappings here use
 * independent pagefile sections so aliases share actual storage. */
#include "windows_memory.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define ABI __attribute__((sysv_abi))
#define PAGE UINT64_C(16384)
#define POOL (UINT64_C(5056)*1024*1024)
#define INVALID ((int32_t)0x80020016)
#define NOMEM ((int32_t)0x8002000c)
#define NOSPACE ((int32_t)0x80020023)
#define BUSY ((int32_t)0x80020010)
#define LIMIT 256
#define FLEXIBLE (UINT64_C(448)*1024*1024)
static uint64_t flexible_bytes;
static SRWLOCK lock=SRWLOCK_INIT;
/* Serialize mapping mutations and notifications, without holding the table lock
 * during renderer callbacks (which may query/protect the same mappings). */
static SRWLOCK mutation_lock=SRWLOCK_INIT;
static WindowsGpuRange gpu_map,gpu_unmap;
typedef struct { uint64_t start,size; HANDLE section; } Block;
typedef struct { void *address; uint64_t size; HANDLE section; int protection,kind; } View;
/* kind: 0 direct allocation; 1 flexible section; 2 image; 3 growable host stack. */
static Block blocks[LIMIT];
static View views[LIMIT];
/* The caller holds the shared mapping lock. A gap ends at the next tracked
 * view; past the last view there is no region and outputs stay untouched. */
static int region_locked(uintptr_t address,uintptr_t *start,uintptr_t *end,int *mapped) {
    uintptr_t previous=0,next=UINTPTR_MAX;
    int found=0;
    for(int i=0;i<LIMIT;i++) if(views[i].address) {
        uintptr_t a=(uintptr_t)views[i].address,b=a+views[i].size;
        if(address>=a && address<b) {
            if(views[i].kind==3) {
                MEMORY_BASIC_INFORMATION info;
                if(!VirtualQuery((void *)address,&info,sizeof(info))) return 0;
                uintptr_t region=(uintptr_t)info.BaseAddress,limit=region+info.RegionSize;
                if(region>a) a=region;
                if(limit<b) b=limit;
                *mapped=info.State==MEM_COMMIT && !(info.Protect&PAGE_GUARD);
            } else *mapped=1;
            *start=a;*end=b;return 1;
        }
        if(b<=address && b>previous) previous=b;
        if(a>address && (!found || a<next)) {next=a;found=1;}
    }
    if(found) {*start=previous;*end=next;*mapped=0;}
    return found;
}
int runtime_memory_region(uintptr_t address,uintptr_t *start,uintptr_t *end,int *mapped) {
    if(!start || !end || !mapped) return 0;
    AcquireSRWLockShared(&lock);
    int result=region_locked(address,start,end,mapped);
    ReleaseSRWLockShared(&lock);
    return result;
}
uint64_t runtime_memory_clamp(uintptr_t address,uint64_t size) {
    if(!size || size>UINTPTR_MAX-address) return 0;
    uint64_t length=0;
    AcquireSRWLockShared(&lock);
    while(length<size) {
        uintptr_t start,end;int mapped;
        if(!region_locked(address+length,&start,&end,&mapped) || !mapped) break;
        uint64_t available=end-(address+length);
        length+=available<size-length?available:size-length;
    }
    ReleaseSRWLockShared(&lock);
    return length;
}
int runtime_memory_is_mapped(uintptr_t address,uint64_t size) {
    /* Empty/overflowing ranges are rejected rather than treated as GPU storage. */
    return size && size<=UINTPTR_MAX-address && runtime_memory_clamp(address,size)==size;
}
static int aligned(uint64_t n) { return n>=PAGE && !(n&(n-1)); }
static ABI uint64_t direct_size(void) { return POOL; }
static ABI int32_t allocate(int64_t low,int64_t high,uint64_t size,uint64_t alignment,int type,int64_t *out) {
    if (!alignment) alignment=PAGE;
    if (!out || low<0 || high<0 || !size || size%PAGE || !aligned(alignment) || type<0 || type>10) return INVALID;
    uint64_t end=(uint64_t)high<POOL?(uint64_t)high:POOL;
    if ((uint64_t)low>=end || size>end-(uint64_t)low || alignment>POOL) return NOSPACE;
    AcquireSRWLockExclusive(&lock);
    int slot=-1; uint64_t start=((uint64_t)low+alignment-1)&~(alignment-1);
    for (int i=0;i<LIMIT;i++) if (!blocks[i].section) {slot=i;break;}
    for (;;) {
        if (slot<0 || start>end || size>end-start) {ReleaseSRWLockExclusive(&lock);return NOSPACE;}
        int overlap=0;
        for(int i=0;i<LIMIT;i++) if(blocks[i].section && start<blocks[i].start+blocks[i].size && blocks[i].start<start+size) {
            start=(blocks[i].start+blocks[i].size+alignment-1)&~(alignment-1);overlap=1;break;
        }
        if(!overlap) break;
    }
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,(DWORD)(size>>32),(DWORD)size,NULL);
    if(!section) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    blocks[slot]=(Block){start,size,section};*out=(int64_t)start;
    ReleaseSRWLockExclusive(&lock);return 0;
}
/* Complete allocations, CPU RW storage with optional GPU permission metadata.
 * GPU permissions do not implement GPU access. Fixed replacement is unresolved. */
static int32_t map_storage(void **out,uint64_t size,int prot,int flags,int64_t physical,uint64_t alignment) {
    if(!out || physical<0 || !size || size%PAGE || (alignment && !aligned(alignment)) || alignment>POOL || (flags&~0x80) || (prot&~0x33) || (prot&3)!=3) {
        fprintf(stderr,"Unsupported direct mapping: size=%llu protection=0x%x flags=0x%x\n",(unsigned long long)size,prot,flags);return INVALID;
    }
    AcquireSRWLockExclusive(&lock);
    HANDLE section=NULL;int slot=-1;
    for(int i=0;i<LIMIT;i++) {
        if(blocks[i].section && blocks[i].start==(uint64_t)physical && blocks[i].size==size) section=blocks[i].section;
        if(slot<0 && !views[i].address) slot=i;
    }
    if(!section || slot<0) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    void *address=NULL;
    uint64_t step=alignment>65536?alignment:65536;
    uint64_t start=(uintptr_t)*out;
    if(start<UINT64_C(0x1000000000) || start>=UINT64_C(0xfc00000000)-size) start=UINT64_C(0x1000000000);
    start=(start+step-1)&~(step-1);
    for(uint64_t candidate=start, attempts=0;candidate<UINT64_C(0xfc00000000)-size && attempts<4096;candidate+=step,attempts++) {
        address=MapViewOfFileEx(section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,(SIZE_T)size,(void *)(uintptr_t)candidate);
        if(address) break;
    }
    if(!address) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    views[slot]=(View){address,size,section,prot,0};*out=address;
    ReleaseSRWLockExclusive(&lock);
    printf("Runtime: direct memory mapped (%llu bytes, protection 0x%x; GPU not connected)\n",(unsigned long long)size,prot);return 0;
}
static int32_t unmap_storage(void *address,uint64_t size) {
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(views[i].address==address && address && views[i].size==size && views[i].kind<2) {
        if(!UnmapViewOfFile(address)) {ReleaseSRWLockExclusive(&lock);return INVALID;}
        if (views[i].kind==1) {flexible_bytes-=views[i].size;CloseHandle(views[i].section);}
        views[i]=(View){0};ReleaseSRWLockExclusive(&lock);return 0;
    }
    ReleaseSRWLockExclusive(&lock);return INVALID;
}
/* Flexible allocations own private zero-initialized Windows pages. CPU protection
 * bits are translated exactly; GPU mappings and replacement of occupied ranges are deferred. */
static int32_t map_flexible_storage(void **out,uint64_t size,int prot,int flags) {
    static const DWORD protection[8]={PAGE_NOACCESS,PAGE_READONLY,0,PAGE_READWRITE,
        PAGE_EXECUTE,PAGE_EXECUTE_READ,0,PAGE_EXECUTE_READWRITE};
    if(!out || !size || size%PAGE || (prot&~7) || !protection[prot] || (flags&~0x10)) {
        fprintf(stderr,"Unsupported flexible mapping: size=%llu prot=0x%x flags=0x%x address=%p\n",(unsigned long long)size,prot,flags,out?*out:NULL);
        return INVALID;
    }
    AcquireSRWLockExclusive(&lock);
    if(size>FLEXIBLE-flexible_bytes) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    int slot=-1;
    for(int i=0;i<LIMIT;i++) if(!views[i].address) {slot=i;break;}
    if(slot<0) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    uintptr_t requested=(uintptr_t)*out;
    if((flags&0x10) && (requested<UINT64_C(0x1000000000) || requested%65536 || requested>UINT64_C(0xfc00000000)-size)) {
        ReleaseSRWLockExclusive(&lock);return INVALID;
    }
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,
        (prot&4)?PAGE_EXECUTE_READWRITE:PAGE_READWRITE,(DWORD)(size>>32),(DWORD)size,NULL);
    if(!section) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    void *address=NULL;
    for(uint64_t candidate=(flags&0x10)?requested:UINT64_C(0x1000000000),attempts=0;attempts<((flags&0x10)?1u:4096u);candidate+=65536,attempts++) {
        address=MapViewOfFileEx(section,FILE_MAP_READ|FILE_MAP_WRITE|((prot&4)?FILE_MAP_EXECUTE:0),
            0,0,(SIZE_T)size,(void *)(uintptr_t)candidate);
        if(address) break;
    }
    DWORD old;
    if(!address || !VirtualProtect(address,(SIZE_T)size,protection[prot],&old)) {
        if(address) UnmapViewOfFile(address);
        CloseHandle(section);ReleaseSRWLockExclusive(&lock);return NOMEM;
    }
    views[slot]=(View){address,size,section,prot,1};flexible_bytes+=size;*out=address;
    ReleaseSRWLockExclusive(&lock);
    printf("Runtime: flexible memory mapped (%llu bytes, protection 0x%x)\n",(unsigned long long)size,prot);
    return 0;
}
static int32_t release_flexible_storage(void *address,uint64_t size) {
    /* Keep lookup and release atomic; never release a direct-memory view here. */
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(address && views[i].address==address && views[i].size==size && views[i].kind==1) {
        if(!UnmapViewOfFile(address)) {ReleaseSRWLockExclusive(&lock);return INVALID;}
        CloseHandle(views[i].section);
        flexible_bytes-=size;views[i]=(View){0};ReleaseSRWLockExclusive(&lock);return 0;
    }
    ReleaseSRWLockExclusive(&lock);return INVALID;
}
static ABI int32_t map(void **out,uint64_t size,int prot,int flags,int64_t physical,uint64_t alignment) {
    AcquireSRWLockExclusive(&mutation_lock);
    int32_t result=map_storage(out,size,prot,flags,physical,alignment);
    if(!result && gpu_map) gpu_map((uintptr_t)*out,size);
    ReleaseSRWLockExclusive(&mutation_lock);return result;
}
static ABI int32_t map_flexible(void **out,uint64_t size,int prot,int flags) {
    AcquireSRWLockExclusive(&mutation_lock);
    int32_t result=map_flexible_storage(out,size,prot,flags);
    if(!result && gpu_map) gpu_map((uintptr_t)*out,size);
    ReleaseSRWLockExclusive(&mutation_lock);return result;
}
static ABI int32_t map_named_flexible(void **out,uint64_t size,int prot,int flags,const char *name) {
    (void)name;return map_flexible(out,size,prot,flags);
}
static int32_t remove_view(void *address,uint64_t size,int flexible_only) {
    AcquireSRWLockExclusive(&mutation_lock);
    int found=0;
    AcquireSRWLockShared(&lock);
    for(int i=0;i<LIMIT;i++) if(address && views[i].address==address && views[i].size==size &&
        (flexible_only?views[i].kind==1:views[i].kind<2)) {found=1;break;}
    ReleaseSRWLockShared(&lock);
    if(found && gpu_unmap) gpu_unmap((uintptr_t)address,size);
    int32_t result=flexible_only?release_flexible_storage(address,size):unmap_storage(address,size);
    if(found && result && gpu_unmap) {
        fputs("STOP: failed to release a renderer-tracked view\n",stderr);ExitProcess(21);
    }
    ReleaseSRWLockExclusive(&mutation_lock);return result;
}
static ABI int32_t unmap(void *address,uint64_t size) {return remove_view(address,size,0);}
static ABI int32_t release_flexible(void *address,uint64_t size) {return remove_view(address,size,1);}

static int register_external(void *address,uint64_t size,int protection,int kind) {
    uintptr_t at=(uintptr_t)address;
    if(!address || !size || size>UINTPTR_MAX-at || at%4096 || size%4096 ||
        (protection&~7) || (protection&3)==2) return 0;
    AcquireSRWLockExclusive(&mutation_lock);AcquireSRWLockExclusive(&lock);
    int slot=-1,overlap=0;
    for(int i=0;i<LIMIT;i++) {
        if(!views[i].address && slot<0) slot=i;
        if(views[i].address && at<(uintptr_t)views[i].address+views[i].size &&
            (uintptr_t)views[i].address<at+size) overlap=1;
    }
    int result=slot>=0 && !overlap;
    if(result) views[slot]=(View){address,size,NULL,protection,kind};
    ReleaseSRWLockExclusive(&lock);
    if(result && gpu_map) gpu_map(at,size);
    ReleaseSRWLockExclusive(&mutation_lock);return result;
}
int windows_memory_register_external(void *address,uint64_t size,int protection) {
    return register_external(address,size,protection,2);
}
static _Thread_local void *registered_stack;
int windows_memory_register_current_stack(void) {
    if(registered_stack) return 1;
    typedef void (WINAPI *GetLimits)(PULONG_PTR,PULONG_PTR);
    GetLimits limits=(GetLimits)(uintptr_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetCurrentThreadStackLimits");
    MEMORY_BASIC_INFORMATION info={0};ULONG_PTR low,high;
    if(!limits || !VirtualQuery(&info,&info,sizeof(info))) return 0;
    limits(&low,&high);low=(ULONG_PTR)info.AllocationBase;
    if(high<=low || !register_external((void *)low,high-low,3,3)) return 0;
    registered_stack=(void *)low;return 1;
}
void windows_memory_unregister_current_stack(void) {
    if(!registered_stack) return;
    AcquireSRWLockExclusive(&mutation_lock);
    int slot=-1;uint64_t size=0;
    AcquireSRWLockShared(&lock);
    for(int i=0;i<LIMIT;i++) if(views[i].kind==3 && views[i].address==registered_stack) {slot=i;size=views[i].size;break;}
    ReleaseSRWLockShared(&lock);
    if(slot>=0) {
        if(gpu_unmap) gpu_unmap((uintptr_t)registered_stack,size);
        AcquireSRWLockExclusive(&lock);views[slot]=(View){0};ReleaseSRWLockExclusive(&lock);
    }
    registered_stack=NULL;ReleaseSRWLockExclusive(&mutation_lock);
}

void runtime_memory_set_gpu_hooks(WindowsGpuRange map_hook,WindowsGpuRange unmap_hook,WindowsGpuRange invalidate_hook) {
    AcquireSRWLockExclusive(&mutation_lock);
    /* Installation is serialized with mappings. Callbacks must not mutate the
     * guest mapping table; query and protection callbacks are safe here. */
    if(gpu_unmap) for(int i=0;i<LIMIT;i++) if(views[i].address)
        gpu_unmap((uintptr_t)views[i].address,views[i].size);
    gpu_map=map_hook;gpu_unmap=unmap_hook;
    /* CPU writes are handled by the renderer's access-violation dispatcher.
     * This backend currently emits map/unmap events, not explicit invalidations. */
    (void)invalidate_hook;
    if(gpu_map) for(int i=0;i<LIMIT;i++) if(views[i].address)
        gpu_map((uintptr_t)views[i].address,views[i].size);
    ReleaseSRWLockExclusive(&mutation_lock);
}

int runtime_memory_write_backing(uintptr_t address,const void *data,uint64_t size) {
    if(!data || !size || size>UINTPTR_MAX-address) return 0;
    AcquireSRWLockShared(&lock);
    /* Validate the entire range before changing storage; external regions have
     * no alias and return failure so the renderer can use its tracked path. */
    uint64_t done=0;
    while(done<size) {
        View *view=NULL;
        for(int i=0;i<LIMIT;i++) if(views[i].address && address+done>=(uintptr_t)views[i].address &&
            address+done-(uintptr_t)views[i].address<views[i].size) {view=&views[i];break;}
        if(!view || !view->section) {ReleaseSRWLockShared(&lock);return 0;}
        uint64_t n=view->size-(address+done-(uintptr_t)view->address);
        done+=n<size-done?n:size-done;
    }
    done=0;int ok=1;
    while(done<size) {
        View *view=NULL;
        for(int i=0;i<LIMIT;i++) if(views[i].address && address+done>=(uintptr_t)views[i].address &&
            address+done-(uintptr_t)views[i].address<views[i].size) {view=&views[i];break;}
        uint64_t offset=address+done-(uintptr_t)view->address,n=view->size-offset;
        if(n>size-done) n=size-done;
        unsigned char *alias=MapViewOfFile(view->section,FILE_MAP_WRITE,0,0,(SIZE_T)view->size);
        if(!alias) {ok=0;break;}
        memcpy(alias+offset,(const unsigned char *)data+done,(size_t)n);
        if(!UnmapViewOfFile(alias)) {ok=0;break;}
        done+=n;
    }
    ReleaseSRWLockShared(&lock);return ok;
}

void runtime_memory_gpu_protect(uintptr_t address,uint64_t size,int read,int write) {
    static const DWORD modes[8]={PAGE_NOACCESS,PAGE_READONLY,0,PAGE_READWRITE,
        PAGE_EXECUTE,PAGE_EXECUTE_READ,0,PAGE_EXECUTE_READWRITE};
    if(!size || size>UINTPTR_MAX-address) return;
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(views[i].address) {
        uintptr_t a=(uintptr_t)views[i].address,b=a+views[i].size;
        if(a>=address+size || b<=address) continue;
        if(a<address) a=address;
        if(b>address+size) b=address+size;
        int guest=views[i].protection&7;
        int wanted=(read||write?1:0)|(write?2:0)|(guest&4);
        DWORD old;
        if(views[i].kind==3) {
            /* Never change a stack's reserved pages or its Windows growth guard. */
            while(a<b) {
                MEMORY_BASIC_INFORMATION info;
                if(!VirtualQuery((void *)a,&info,sizeof(info))) break;
                uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
                if(next>b) next=b;
                if(info.State==MEM_COMMIT && !(info.Protect&PAGE_GUARD) &&
                    !VirtualProtect((void *)a,(SIZE_T)(next-a),modes[guest&wanted],&old)) ExitProcess(21);
                a=next;
            }
        } else if(!VirtualProtect((void *)a,(SIZE_T)(b-a),modes[guest&wanted],&old)) {
            ReleaseSRWLockExclusive(&lock);
            fputs("STOP: GPU page protection failed\n",stderr);ExitProcess(21);
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
static ABI int32_t release(int64_t physical,uint64_t size) {
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(blocks[i].section && blocks[i].start==(uint64_t)physical && blocks[i].size==size) {
        for(int j=0;j<LIMIT;j++) if(views[j].section==blocks[i].section) {ReleaseSRWLockExclusive(&lock);return BUSY;}
        CloseHandle(blocks[i].section);blocks[i]=(Block){0};ReleaseSRWLockExclusive(&lock);return 0;
    }
    ReleaseSRWLockExclusive(&lock);return INVALID;
}
/* Signature/NID confirmed in shadPS4 kernel/memory.cpp. */
static ABI int32_t available_flexible(uint64_t *out) {
    if(!out) return INVALID;
    AcquireSRWLockShared(&lock);*out=FLEXIBLE-flexible_bytes;ReleaseSRWLockShared(&lock);
    return 0;
}
uintptr_t windows_memory_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
    EXPORT("aNz11fnnzi4",available_flexible);
    EXPORT("IWIBBdTHit4",map_flexible);EXPORT("teiItL2boFw",release_flexible);
    EXPORT("mL8NDH86iQI",map_named_flexible);
    EXPORT("pO96TwzOm5E",direct_size);EXPORT("rTXw65xmLIA",allocate);
    EXPORT("L-Q3LEjIbgA",map);EXPORT("MBuItvba6z8",release);EXPORT("cQke9UuBQOk",unmap);
#undef EXPORT
    return 0;
}
static int test_maps,test_unmaps;
static void test_map_hook(uintptr_t address,uint64_t size) {
    if(!runtime_memory_is_mapped(address,size)) ExitProcess(21);
    test_maps++;
}
static void test_unmap_hook(uintptr_t address,uint64_t size) {
    if(!runtime_memory_is_mapped(address,size)) ExitProcess(21);
    test_unmaps++;
}
int windows_memory_selftest(void) {
    int64_t a=-1,b=-1;void *x=NULL,*y=NULL;
#define CHECK(c) do {if(!(c)) {fprintf(stderr,"memory check failed at %d\n",__LINE__);return 1;}}while(0)
    CHECK(direct_size()==POOL);CHECK(allocate(0,POOL,1,0,0,&a)==INVALID);
    CHECK(!allocate(0,POOL,PAGE,0,0,&a));CHECK(!allocate(0,POOL,PAGE,0,0,&b));CHECK(a!=b);
    CHECK(!map(&x,PAGE,3,0,a,0));CHECK(!map(&y,PAGE,0x33,0x80,a,0));
    runtime_memory_set_gpu_hooks(test_map_hook,test_unmap_hook,NULL);
    CHECK(test_maps==2 && !test_unmaps);
    runtime_memory_gpu_protect((uintptr_t)x,PAGE,1,0);
    MEMORY_BASIC_INFORMATION info;
    CHECK(VirtualQuery(x,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    uint64_t backing_value=UINT64_C(0x445566778899aabb);
    CHECK(runtime_memory_write_backing((uintptr_t)x,&backing_value,sizeof(backing_value)));
    CHECK(*(volatile uint64_t *)y==backing_value && *(volatile uint64_t *)x==backing_value);
    CHECK(VirtualQuery(x,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(!runtime_memory_write_backing((uintptr_t)x+PAGE-1,&backing_value,2));
    runtime_memory_gpu_protect((uintptr_t)x,PAGE,1,1);
    uintptr_t start=0,end=0;int mapped_view=0;
    CHECK(runtime_memory_region((uintptr_t)x+1,&start,&end,&mapped_view));
    CHECK(start==(uintptr_t)x && end==(uintptr_t)x+PAGE && mapped_view);
    CHECK(runtime_memory_is_mapped((uintptr_t)x,PAGE));
    CHECK(runtime_memory_clamp((uintptr_t)x+PAGE-1,2)==1);
    CHECK(!runtime_memory_is_mapped((uintptr_t)x,PAGE+1));
    CHECK(runtime_memory_region(0,&start,&end,&mapped_view) && !mapped_view && !start);
    CHECK(end==((uintptr_t)x<(uintptr_t)y?(uintptr_t)x:(uintptr_t)y));
    CHECK(!runtime_memory_is_mapped(0,1));CHECK(!runtime_memory_is_mapped((uintptr_t)x,0));
    CHECK(!runtime_memory_clamp(UINTPTR_MAX-1,4));
    start=11;end=22;mapped_view=33;
    CHECK(!runtime_memory_region(UINTPTR_MAX,&start,&end,&mapped_view));
    CHECK(start==11 && end==22 && mapped_view==33);
    CHECK(!runtime_memory_region((uintptr_t)x,NULL,&end,&mapped_view));
    *(volatile uint64_t *)x=UINT64_C(0x123456789abcdef0);CHECK(*(volatile uint64_t *)y==UINT64_C(0x123456789abcdef0));
    CHECK(release_flexible(x,PAGE)==INVALID);
    CHECK((uintptr_t)x<UINT64_C(0x10000000000));CHECK(release(a,PAGE)==BUSY);
    CHECK(unmap(x,PAGE*2)==INVALID);CHECK(!unmap(x,PAGE));CHECK(!unmap(y,PAGE));
    CHECK(test_unmaps==2);runtime_memory_set_gpu_hooks(NULL,NULL,NULL);
    CHECK(!runtime_memory_is_mapped((uintptr_t)x,1));
    CHECK(!runtime_memory_is_mapped((uintptr_t)y,1));
    CHECK(!release(a,PAGE));CHECK(release(a,PAGE)==INVALID);CHECK(!release(b,PAGE));
    CHECK(!windows_memory_resolve("pO96TwzOm5E#other"));
    uint64_t available=0;CHECK(available_flexible(NULL)==INVALID);CHECK(!available_flexible(&available) && available==FLEXIBLE);
    void *f=NULL,*g=NULL,*unchanged=(void *)(uintptr_t)123;
    CHECK(map_flexible(&unchanged,FLEXIBLE+PAGE,3,0)==NOMEM);CHECK(unchanged==(void *)(uintptr_t)123);
    CHECK(map_flexible(&f,1,3,0)==INVALID);CHECK(map_flexible(&f,PAGE,3,16)==INVALID);
    CHECK(!map_flexible(&f,PAGE,3,0));CHECK(!map_named_flexible(&g,PAGE,3,0,"test"));
    CHECK(f!=g && *(volatile uint64_t *)f==0 && *(volatile uint64_t *)g==0);
    runtime_memory_gpu_protect((uintptr_t)f,PAGE,0,0);
    CHECK(VirtualQuery(f,&info,sizeof(info)) && info.Protect==PAGE_NOACCESS);
    CHECK(runtime_memory_write_backing((uintptr_t)f,&backing_value,sizeof(backing_value)));
    CHECK(VirtualQuery(f,&info,sizeof(info)) && info.Protect==PAGE_NOACCESS);
    runtime_memory_gpu_protect((uintptr_t)f,PAGE,1,1);
    CHECK(*(volatile uint64_t *)f==backing_value);
    CHECK(runtime_memory_is_mapped((uintptr_t)f,PAGE));
    CHECK(runtime_memory_clamp((uintptr_t)g+PAGE-8,16)==8);
    *(volatile uint64_t *)f=42;CHECK(*(volatile uint64_t *)g==0);CHECK(flexible_bytes==PAGE*2);CHECK(!available_flexible(&available) && available==FLEXIBLE-PAGE*2);
    CHECK(release_flexible(f,PAGE*2)==INVALID);CHECK(!release_flexible(f,PAGE));
    CHECK(release_flexible(f,PAGE)==INVALID);
    void *fixed=f;CHECK(!map_flexible(&fixed,PAGE,3,0x10));CHECK(fixed==f);
    void *collision=f;CHECK(map_flexible(&collision,PAGE,3,0x10)==NOMEM);CHECK(collision==f);
    CHECK(!release_flexible(fixed,PAGE));CHECK(!unmap(g,PAGE));CHECK(!flexible_bytes);CHECK(!available_flexible(&available) && available==FLEXIBLE);
    /* Two allocation-granularity views exercise crossing a real view boundary. */
    f=NULL;g=NULL;CHECK(!map_flexible(&f,PAGE*4,3,0));
    g=(void *)((uintptr_t)f+PAGE*4);CHECK(!map_flexible(&g,PAGE*4,3,0x10));
    CHECK(runtime_memory_is_mapped((uintptr_t)f,PAGE*8));
    CHECK(runtime_memory_clamp((uintptr_t)f+PAGE*4-1,PAGE*4+2)==PAGE*4+1);
    CHECK(!release_flexible(f,PAGE*4));CHECK(!runtime_memory_is_mapped((uintptr_t)f,PAGE*8));
    CHECK(runtime_memory_is_mapped((uintptr_t)g,PAGE*4));CHECK(!release_flexible(g,PAGE*4));
    f=NULL;CHECK(!map_flexible(&f,PAGE,1,0));
    runtime_memory_gpu_protect((uintptr_t)f,PAGE,1,1);
    CHECK(VirtualQuery(f,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(!release_flexible(f,PAGE));
    CHECK(windows_memory_register_current_stack());
    CHECK(windows_memory_register_current_stack());
    CHECK(runtime_memory_is_mapped((uintptr_t)&start,sizeof(start)));
    CHECK(!runtime_memory_write_backing((uintptr_t)&start,&backing_value,sizeof(backing_value)));
    CHECK(VirtualQuery(&start,&info,sizeof(info)));
    uintptr_t stack_base=(uintptr_t)info.AllocationBase;
    CHECK(runtime_memory_region(stack_base,&start,&end,&mapped_view));
    CHECK(end>stack_base);
    windows_memory_unregister_current_stack();
    CHECK(!runtime_memory_is_mapped((uintptr_t)&start,sizeof(start)));
    puts("Flexible availability: initial budget, allocation debit and release credit passed");
    puts("Renderer memory queries: view bounds, gaps, overflow and unmap invalidation passed");
    puts("Renderer memory bridge: hook replay, pre-unmap notification, backing aliases and guest protection limits passed");
    puts("Renderer stacks: current-frame query, idempotent registration and detach invalidation passed");
    puts("Flexible memory: zero-fill, isolation, budget, invalid requests and release passed");
    puts("Direct mapping: no-overwrite alias placement and GPU permission metadata passed");
    puts("Direct memory: allocation isolation, shared aliases, low addresses and lifecycle passed");return 0;
}

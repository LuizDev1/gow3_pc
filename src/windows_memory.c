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
static SRWLOCK lock=SRWLOCK_INIT;
typedef struct { uint64_t start,size; HANDLE section; } Block;
typedef struct { void *address; uint64_t size; HANDLE section; } View;
static Block blocks[LIMIT];
static View views[LIMIT];
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
/* Initial contract: complete allocations, RW CPU mappings, no placement flags.
 * Subranges, GPU-only protections and fixed replacement remain unresolved. */
static ABI int32_t map(void **out,uint64_t size,int prot,int flags,int64_t physical,uint64_t alignment) {
    if(!out || physical<0 || !size || size%PAGE || (alignment && !aligned(alignment)) || alignment>POOL || flags || prot!=3 || *out) return INVALID;
    AcquireSRWLockExclusive(&lock);
    HANDLE section=NULL;int slot=-1;
    for(int i=0;i<LIMIT;i++) {
        if(blocks[i].section && blocks[i].start==(uint64_t)physical && blocks[i].size==size) section=blocks[i].section;
        if(slot<0 && !views[i].address) slot=i;
    }
    if(!section || slot<0) {ReleaseSRWLockExclusive(&lock);return INVALID;}
    void *address=NULL;
    uint64_t step=alignment>65536?alignment:65536;
    for(uint64_t candidate=UINT64_C(0x1000000000), attempts=0;candidate<UINT64_C(0xfc00000000)-size && attempts<4096;candidate+=step,attempts++) {
        address=MapViewOfFileEx(section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,(SIZE_T)size,(void *)(uintptr_t)candidate);
        if(address) break;
    }
    if(!address) {ReleaseSRWLockExclusive(&lock);return NOMEM;}
    views[slot]=(View){address,size,section};*out=address;
    ReleaseSRWLockExclusive(&lock);return 0;
}
static ABI int32_t unmap(void *address,uint64_t size) {
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(views[i].address==address && address && views[i].size==size) {
        if(!UnmapViewOfFile(address)) {ReleaseSRWLockExclusive(&lock);return INVALID;}
        views[i]=(View){0};ReleaseSRWLockExclusive(&lock);return 0;
    }
    ReleaseSRWLockExclusive(&lock);return INVALID;
}
static ABI int32_t release(int64_t physical,uint64_t size) {
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<LIMIT;i++) if(blocks[i].section && blocks[i].start==(uint64_t)physical && blocks[i].size==size) {
        for(int j=0;j<LIMIT;j++) if(views[j].section==blocks[i].section) {ReleaseSRWLockExclusive(&lock);return BUSY;}
        CloseHandle(blocks[i].section);blocks[i]=(Block){0};ReleaseSRWLockExclusive(&lock);return 0;
    }
    ReleaseSRWLockExclusive(&lock);return INVALID;
}
uintptr_t windows_memory_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#U#H")) return (uintptr_t)f
    EXPORT("pO96TwzOm5E",direct_size);EXPORT("rTXw65xmLIA",allocate);
    EXPORT("L-Q3LEjIbgA",map);EXPORT("MBuItvba6z8",release);EXPORT("cQke9UuBQOk",unmap);
#undef EXPORT
    return 0;
}
int windows_memory_selftest(void) {
    int64_t a=-1,b=-1;void *x=NULL,*y=NULL;
#define CHECK(c) do {if(!(c)) {fprintf(stderr,"memory check failed at %d\n",__LINE__);return 1;}}while(0)
    CHECK(direct_size()==POOL);CHECK(allocate(0,POOL,1,0,0,&a)==INVALID);
    CHECK(!allocate(0,POOL,PAGE,0,0,&a));CHECK(!allocate(0,POOL,PAGE,0,0,&b));CHECK(a!=b);
    CHECK(!map(&x,PAGE,3,0,a,0));CHECK(!map(&y,PAGE,3,0,a,0));
    *(volatile uint64_t *)x=UINT64_C(0x123456789abcdef0);CHECK(*(volatile uint64_t *)y==UINT64_C(0x123456789abcdef0));
    CHECK((uintptr_t)x<UINT64_C(0x10000000000));CHECK(release(a,PAGE)==BUSY);
    CHECK(unmap(x,PAGE*2)==INVALID);CHECK(!unmap(x,PAGE));CHECK(!unmap(y,PAGE));
    CHECK(!release(a,PAGE));CHECK(release(a,PAGE)==INVALID);CHECK(!release(b,PAGE));
    CHECK(!windows_memory_resolve("pO96TwzOm5E#other"));
    puts("Direct memory: allocation isolation, shared aliases, low addresses and lifecycle passed");return 0;
}

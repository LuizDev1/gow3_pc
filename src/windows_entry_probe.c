/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows x86-64 diagnostic with native-module startup and minimal contracts.
 * Accepts BBPROBE2/TLGWIN01 and linked TLGWIN02, never Linux linked images.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <x86intrin.h>
#include "windows_equeue.h"
#include "windows_mutex.h"
#include "windows_tls.h"
#include "windows_memory.h"
#include "windows_renderer_runtime.h"
#ifdef TLG_WITH_RENDERER
#include "../platform/windows/gow3-base/gpu/gow3gpu.h"
#endif
#include "windows_thread.h"
#include "windows_semaphore.h"
#include "windows_pad.h"
#if !defined(__x86_64__) || !defined(__clang__)
#error Build with x86-64 Clang (SysV guest ABI required).
#endif
#define ABI __attribute__((sysv_abi))
typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static unsigned char *image, *data_traps;
static char (*names)[128];
static uint64_t image_size, import_count;
static int gpu_requested;
#ifdef TLG_WITH_RENDERER
static int gpu_active;
#endif
typedef struct {
    char nid[12], library[64];
    uint32_t library_version;
    char module[64];
    uint32_t module_version, kind;
} ImportIdentity;
_Static_assert(sizeof(ImportIdentity) == 152, "canonical identity layout");
static ImportIdentity *identities;
typedef struct { uint64_t base,size,init,tls_address,tls_memsz,tls_filesz,tls_module; } NativeModule;
static NativeModule native_modules[15];
static uint64_t native_count;
static void *process_parameter;
static void **application_heap_api;
/* Heap trace ABI adapted from shadPS4's GPL-2.0-or-later implementation:
 * src/core/libraries/libc_internal/libc_internal_memory.cpp
 * Copyright 2024 shadPS4 Emulator Project. Keep shared storage: libc writes it.
 */
typedef struct {
    uint64_t size;
    uint32_t flag, get_segment_info;
    uint64_t *atomic_mask, *state_table;
} HeapTraceInfo;
_Static_assert(sizeof(HeapTraceInfo)==32,"heap trace ABI");
static uint64_t heap_trace_mask, heap_trace_states[64];
static void __attribute__((sysv_abi)) heap_get_trace_info(HeapTraceInfo *info) {
    if (!info || info->size<sizeof(*info)) {
        fputs("STOP: invalid heap trace info buffer\n",stderr);
        ExitProcess(21);
    }
    info->atomic_mask=&heap_trace_mask;
    info->state_table=heap_trace_states;
    info->get_segment_info=0;
}
static int selftest_heap_trace(void) {
    HeapTraceInfo a={.size=sizeof(a),.flag=7,.get_segment_info=123}, b={.size=sizeof(b)};
    heap_get_trace_info(&a); heap_get_trace_info(&b);
    if (a.size!=32 || a.flag!=7 || a.get_segment_info || !a.atomic_mask || !a.state_table ||
        a.atomic_mask!=b.atomic_mask || a.state_table!=b.state_table) return 1;
    *a.atomic_mask=8; a.state_table[63]=42;
    if (*b.atomic_mask!=8 || b.state_table[63]!=42) return 1;
    *a.atomic_mask=0; a.state_table[63]=0;
    puts("heap trace: ABI fields, shared writable mask and 64-entry table passed");
    return 0;
}
static void __attribute__((sysv_abi)) register_application_heap(void **api) {
    application_heap_api=api;
    printf("Runtime: application heap API registered at %p\n",(void *)application_heap_api);
}
static int entered_game;
/* The diagnostic models a base PS4, independent of the host GPU/CPU model. */
static int __attribute__((sysv_abi)) is_neo_mode(void) { return 0; }
static void * __attribute__((sysv_abi)) get_process_parameter(void) { return process_parameter; }
static size_t page_size;
static uint64_t init_env_calls;
static uint64_t stack_canary;
static const struct { const char *nid, *label; } diagnostic_names[] = {
#include "import_names.inc"
};
static const char *diagnostic_name(const char *name) {
    const char *hash = strchr(name, '#');
    size_t length = hash ? (size_t)(hash - name) : strlen(name);
    /* NID-only lookup is for labels, never for resolving executable contracts. */
    for (size_t i = 0; i < sizeof(diagnostic_names)/sizeof(*diagnostic_names); ++i)
        if (!strncmp(name, diagnostic_names[i].nid, length) && diagnostic_names[i].nid[length] == '#')
            return diagnostic_names[i].label;
    return "unknown";
}
typedef void (ABI *ExitCallback)(void);
typedef struct {
    union { ExitCallback plain; void (ABI *with_arg)(void *); } callback;
    void *argument, *dso;
    int with_arg;
} ExitHandler;
static ExitHandler *exit_callbacks;
static size_t exit_count, exit_capacity;
static SRWLOCK exit_lock = SRWLOCK_INIT;
static int register_exit(ExitHandler handler) {
    AcquireSRWLockExclusive(&exit_lock);
    if (exit_count == exit_capacity) {
        size_t capacity = exit_capacity ? exit_capacity * 2 : 32;
        ExitHandler *next = capacity <= 1024 * 1024
                           ? realloc(exit_callbacks, capacity * sizeof(*next)) : NULL;
        if (!next) { ReleaseSRWLockExclusive(&exit_lock); return -1; }
        exit_callbacks = next;
        exit_capacity = capacity;
    }
    exit_callbacks[exit_count++] = handler;
    ReleaseSRWLockExclusive(&exit_lock);
    return 0;
}
static ABI int guest_atexit(ExitCallback callback) {
    if (!callback) return -1;
    return register_exit((ExitHandler){.callback.plain=callback});
}
static ABI int guest_cxa_atexit(void (ABI *callback)(void *), void *argument, void *dso) {
    if (!callback) return -1;
    return register_exit((ExitHandler){.callback.with_arg=callback, .argument=argument,
                                      .dso=dso, .with_arg=1});
}
static void finalize_callbacks(void) {
    for (;;) {
        AcquireSRWLockExclusive(&exit_lock);
        if (!exit_count) { ReleaseSRWLockExclusive(&exit_lock); return; }
        ExitHandler handler = exit_callbacks[--exit_count];
        ReleaseSRWLockExclusive(&exit_lock);
        /* Pop before calling, and release the lock: callbacks can register more. */
        if (handler.with_arg) handler.callback.with_arg(handler.argument);
        else handler.callback.plain();
    }
}
static unsigned selftest_order;
static ABI void test_first(void) { selftest_order = selftest_order * 10 + 1; }
static ABI void test_cxa(void *argument) { *(unsigned *)argument += 7; }
static ABI void test_last(void) {
    selftest_order = selftest_order * 10 + 2;
    if (guest_atexit(test_first)) ExitProcess(1);
}
static int selftest_atexit(void) {
    if (guest_atexit(NULL) == 0 || guest_atexit(test_first) || guest_atexit(test_last)) return 1;
    finalize_callbacks();
    if (selftest_order != 211 || exit_count) return 1;
    finalize_callbacks();
    if (selftest_order != 211) return 1;
    unsigned argument = 0;
    if (guest_cxa_atexit(NULL, NULL, NULL) == 0 || guest_cxa_atexit(test_cxa, &argument, (void *)1)) return 1;
    finalize_callbacks(); finalize_callbacks();
    if (argument != 7) return 1;
    puts("atexit: reverse order, registration during finalization and exactly-once checks passed");
    puts("C++ destructors: argument forwarding and exactly-once checks passed");
    free(exit_callbacks);
    return 0;
}

static ABI void init_env_ret(void) {
    ++init_env_calls;
    puts("Runtime: _init_env returned (prepared libc export verified as RET)");
}
/* Keep the real x86 counter: guest code can also execute RDTSC directly.
 * Calibrate once against Windows QPC instead of inventing a PS4 frequency. */
static INIT_ONCE clock_once=INIT_ONCE_STATIC_INIT;
static uint64_t counter_hz, qpc_hz, process_start;
static BOOL CALLBACK initialize_clock(PINIT_ONCE once,PVOID argument,PVOID *context) {
    (void)once;(void)argument;(void)context;
    LARGE_INTEGER frequency,a,b;
    if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 || !QueryPerformanceCounter(&a)) return FALSE;
    uint64_t begin=__rdtsc();Sleep(20);uint64_t end=__rdtsc();
    if(!QueryPerformanceCounter(&b) || b.QuadPart<=a.QuadPart || end<=begin) return FALSE;
    qpc_hz=(uint64_t)frequency.QuadPart;
    counter_hz=(uint64_t)(((__uint128_t)(end-begin)*qpc_hz)/(uint64_t)(b.QuadPart-a.QuadPart));
    process_start=(uint64_t)a.QuadPart;
    return counter_hz!=0;
}
static void require_clock(void) {
    if(!InitOnceExecuteOnce(&clock_once,initialize_clock,NULL,NULL)) {
        fputs("STOP: Windows clock initialization failed\n",stderr);ExitProcess(21);
    }
}
static ABI uint64_t guest_read_tsc(void) { return __rdtsc(); }
static ABI uint64_t guest_tsc_frequency(void) { require_clock();return counter_hz; }
static ABI uint64_t guest_process_time(void) {
    require_clock();LARGE_INTEGER now;
    if(!QueryPerformanceCounter(&now) || (uint64_t)now.QuadPart<process_start) ExitProcess(21);
    return (uint64_t)(((__uint128_t)((uint64_t)now.QuadPart-process_start)*1000000)/qpc_hz);
}
uint64_t runtime_process_time_us(void) {return guest_process_time();}
uint64_t runtime_process_time_counter(void) {
    require_clock();LARGE_INTEGER now;
    if(!QueryPerformanceCounter(&now) || (uint64_t)now.QuadPart<process_start) ExitProcess(21);
    return (uint64_t)(((__uint128_t)((uint64_t)now.QuadPart-process_start)*1000000000)/qpc_hz);
}
uint64_t runtime_tsc_frequency(void) {return guest_tsc_frequency();}
static int selftest_clock(void) {
    uint64_t hz=guest_tsc_frequency(),a=guest_read_tsc(),before=guest_process_time();
    uint64_t counter_before=runtime_process_time_counter();
    Sleep(30);uint64_t b=guest_read_tsc(),after=guest_process_time();
    uint64_t counter_after=runtime_process_time_counter();
    if(!hz || guest_tsc_frequency()!=hz || b<=a || after<=before) return 1;
    uint64_t elapsed=(uint64_t)(((__uint128_t)(b-a)*1000000)/hz);
    uint64_t expected=after-before;
    if(elapsed<expected/2 || elapsed>expected*2) return 1;
    uint64_t counter_us=(counter_after-counter_before)/1000;
    if(counter_after<=counter_before || counter_us<expected/2 || counter_us>expected*2) return 1;
    puts("Timing: advancing hardware counter, stable frequency and elapsed-time agreement passed");return 0;
}
/* POSIX console writes only. File descriptors are not host Win32 handles. */
static _Thread_local int32_t console_errno;
int32_t *runtime_errno(void) {return &console_errno;}
typedef struct { int64_t seconds,nanoseconds; } GuestTimespec;
_Static_assert(sizeof(GuestTimespec)==16,"guest timespec layout");
static ABI int guest_clock_gettime(uint32_t id,GuestTimespec *out) {
    if(!out) {console_errno=14;return -1;}
    GuestTimespec result;
    switch(id) {
    case 0: case 9: case 10: case 13: {
        FILETIME filetime;GetSystemTimePreciseAsFileTime(&filetime);
        uint64_t ticks=((uint64_t)filetime.dwHighDateTime<<32)|filetime.dwLowDateTime;
        const uint64_t epoch=UINT64_C(116444736000000000);
        if(ticks<epoch) {console_errno=5;return -1;}
        ticks-=epoch;result.seconds=(int64_t)(ticks/10000000);
        result.nanoseconds=id==13?0:(int64_t)(ticks%10000000)*100;
        break;
    }
    case 4: case 5: case 7: case 8: case 11: case 12: {
        require_clock();LARGE_INTEGER now;
        if(!QueryPerformanceCounter(&now) || now.QuadPart<0) {console_errno=5;return -1;}
        uint64_t ticks=(uint64_t)now.QuadPart;
        result.seconds=(int64_t)(ticks/qpc_hz);
        result.nanoseconds=(int64_t)(((__uint128_t)(ticks%qpc_hz)*1000000000)/qpc_hz);
        break;
    }
    default:console_errno=22;return -1; /* CPU/network clocks require their own semantics. */
    }
    *out=result;return 0;
}
static int selftest_timespec(void) {
    GuestTimespec wall={0},seconds={0},a={0},b={0},unchanged={123,456};
    if(guest_clock_gettime(0,&wall) || wall.seconds<0 || wall.nanoseconds<0 || wall.nanoseconds>=1000000000 ||
       guest_clock_gettime(13,&seconds) || seconds.nanoseconds!=0 ||
       seconds.seconds<wall.seconds || seconds.seconds>wall.seconds+1) return 1;
    if(guest_clock_gettime(4,&a)) return 1;
    Sleep(10);if(guest_clock_gettime(4,&b) || b.seconds<a.seconds ||
        (b.seconds==a.seconds && b.nanoseconds<=a.nanoseconds)) return 1;
    if(guest_clock_gettime(UINT32_MAX,&unchanged)!=-1 || console_errno!=22 ||
        unchanged.seconds!=123 || unchanged.nanoseconds!=456 ||
        guest_clock_gettime(0,NULL)!=-1 || console_errno!=14) return 1;
    static const uint32_t aliases[]={5,7,8,9,10,11,12};
    for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);i++)
        if(guest_clock_gettime(aliases[i],&a) || a.nanoseconds<0 || a.nanoseconds>=1000000000) return 1;
    puts("Guest clocks: Unix epoch, SECOND truncation, monotonic advance, aliases and invalid inputs passed");return 0;
}
static ABI int32_t *guest_console_error(void) { return &console_errno; }
static ABI int64_t guest_console_write(int fd,const void *buffer,uint64_t size) {
    if(fd!=1 && fd!=2) {console_errno=9;return -1;}
    if(size>INT64_MAX) {console_errno=22;return -1;}
    if(size && !buffer) {console_errno=14;return -1;}
    if(!size) return 0;
    HANDLE handle=GetStdHandle(fd==1?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
    DWORD written=0;
    DWORD count=size>UINT32_MAX?UINT32_MAX:(DWORD)size;
    if(!WriteFile(handle,buffer,count,&written,NULL)) {console_errno=5;return -1;}
    return written; /* Partial writes are reported, not silently discarded. */
}
static int selftest_console(void) {
    static const unsigned char bytes[]={'T','L','G',0,'\n'};
    if(guest_console_write(99,bytes,sizeof(bytes))!=-1 || *guest_console_error()!=9 ||
       guest_console_write(1,NULL,1)!=-1 || *guest_console_error()!=14 ||
       guest_console_write(1,bytes,UINT64_MAX)!=-1 || *guest_console_error()!=22 ||
       guest_console_write(1,NULL,0)!=0) return 1;
    if(guest_console_write(1,bytes,sizeof(bytes))!=(int64_t)sizeof(bytes) ||
       guest_console_write(2,"console stderr\n",15)!=15) return 1;
    return 0;
}
/* Read-only /app0 mount. Guest descriptors never expose host HANDLE values. */
static SRWLOCK file_lock=SRWLOCK_INIT;
static struct { int id; HANDLE handle; } guest_files[128];
static int next_file=3;
#define FILE_ERROR(n) (-(int64_t)0x100000000+(int64_t)(0x80020000u|(n)))
static int file_error(DWORD error) {
    return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?2:
        error==ERROR_ACCESS_DENIED?13:5;
}
static int translate_path(const char *path,char *host,size_t size) {
    if(!path || !host) return 14;
    const char *relative=NULL;
    if(!strncmp(path,"/app0/",6)) relative=path+6;
    else if(!strncmp(path,"/hostapp/",9)) relative=path+9;
    else if(path[0]!='/') relative=path;
    if(!relative || !*relative || strlen(relative)>1024) return 22;
    for(const char *at=relative;*at;) {
        const char *end=strchr(at,'/');size_t n=end?(size_t)(end-at):strlen(at);
        if(!n || (n==1 && at[0]=='.') || (n==2 && at[0]=='.' && at[1]=='.')) return 22;
        for(size_t i=0;i<n;i++) if(at[i]=='\\' || at[i]==':' || at[i]=='*' || at[i]=='?') return 22;
        if(!end) break;at=end+1;
    }
    const char *root=getenv("TLG_GAME_ROOT");
    if(!root || !*root) return 2;
    if(strlen(root)+strlen(relative)+2>size) return 63;
    snprintf(host,size,"%s/%s",root,relative);return 0;
}
int runtime_file_translate(const char *path,char *host,size_t size) {
    int result=translate_path(path,host,size);
    if(result) console_errno=result;
    return result?-1:0;
}
static ABI int64_t guest_file_open(const char *path,int flags,int mode) {
    (void)mode;
    if(!path) return FILE_ERROR(14);
    printf("Runtime: file open %s (flags 0x%x)\n",path,flags);
    if(flags) return FILE_ERROR(45);
    char host[2048];
    int result=translate_path(path,host,sizeof(host));
    if(result) return FILE_ERROR(result);
    HANDLE h=CreateFileA(host,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(h==INVALID_HANDLE_VALUE) return FILE_ERROR(file_error(GetLastError()));
    AcquireSRWLockExclusive(&file_lock);
    for(unsigned i=0;i<128;i++) if(!guest_files[i].id && next_file<INT_MAX) {
        int id=next_file++;guest_files[i].id=id;guest_files[i].handle=h;
        ReleaseSRWLockExclusive(&file_lock);return id;
    }
    ReleaseSRWLockExclusive(&file_lock);CloseHandle(h);return FILE_ERROR(24);
}
static HANDLE find_file(int id) {
    for(unsigned i=0;i<128;i++) if(guest_files[i].id==id && id>=3) return guest_files[i].handle;
    return INVALID_HANDLE_VALUE;
}
/* The PS4 reader can request UINT32_MAX to consume the rest of a small file.
 * Windows may probe the entire requested buffer and reject it before EOF.
 * All descriptors here are synchronous disk handles, serialized by file_lock. */
static int64_t bounded_read_size(HANDLE h,uint64_t requested,DWORD *count) {
    LARGE_INTEGER end,zero={0},position;
    if(!GetFileSizeEx(h,&end) || !SetFilePointerEx(h,zero,&position,FILE_CURRENT) ||
        end.QuadPart<0 || position.QuadPart<0) return FILE_ERROR(5);
    uint64_t remaining=position.QuadPart<end.QuadPart?(uint64_t)(end.QuadPart-position.QuadPart):0;
    uint64_t bounded=requested<remaining?requested:remaining;
    *count=bounded>UINT32_MAX?UINT32_MAX:(DWORD)bounded;return 0;
}
static ABI int64_t guest_file_read(int fd,void *buffer,uint64_t size) {
    if((size && !buffer) || size>INT64_MAX) return FILE_ERROR(22);
    AcquireSRWLockExclusive(&file_lock);HANDLE h=find_file(fd);DWORD count=0,amount=0;
    int64_t result=h==INVALID_HANDLE_VALUE?FILE_ERROR(9):bounded_read_size(h,size,&amount);
    if(!result && amount) result=ReadFile(h,buffer,amount,&count,NULL)?(int64_t)count:FILE_ERROR(file_error(GetLastError()));
    if(getenv("TLG_TRACE_FILES")) fprintf(stderr,"File read: fd=%d requested=%llu result=%lld\n",
        fd,(unsigned long long)size,(long long)result);
    ReleaseSRWLockExclusive(&file_lock);return result;
}
static ABI int64_t guest_file_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    if((size && !buffer) || size>INT64_MAX || offset<0) return FILE_ERROR(22);
    AcquireSRWLockExclusive(&file_lock);HANDLE h=find_file(fd);
    if(h==INVALID_HANDLE_VALUE) {ReleaseSRWLockExclusive(&file_lock);return FILE_ERROR(9);}
    LARGE_INTEGER zero={0},saved,target={.QuadPart=offset};DWORD count=0;
    if(!SetFilePointerEx(h,zero,&saved,FILE_CURRENT)) {
        int64_t error=FILE_ERROR(file_error(GetLastError()));ReleaseSRWLockExclusive(&file_lock);return error;
    }
    int64_t result;
    if(!SetFilePointerEx(h,target,NULL,FILE_BEGIN)) result=FILE_ERROR(file_error(GetLastError()));
    else {
        DWORD amount=0;result=bounded_read_size(h,size,&amount);
        if(!result && amount) result=ReadFile(h,buffer,amount,&count,NULL)?
            (int64_t)count:FILE_ERROR(file_error(GetLastError()));
    }
    /* Synchronous Windows handles have one shared cursor. Serialize seek/read
     * and restore it before any guest read/seek/close can observe the change. */
    if(!SetFilePointerEx(h,saved,NULL,FILE_BEGIN)) result=FILE_ERROR(5);
    if(getenv("TLG_TRACE_FILES")) fprintf(stderr,"File pread: fd=%d requested=%llu offset=%lld result=%lld\n",
        fd,(unsigned long long)size,(long long)offset,(long long)result);
    ReleaseSRWLockExclusive(&file_lock);return result;
}
static ABI int64_t guest_file_seek(int fd,int64_t offset,int whence) {
    if(whence<0 || whence>2) return FILE_ERROR(22);
    AcquireSRWLockExclusive(&file_lock);HANDLE h=find_file(fd);LARGE_INTEGER move={.QuadPart=offset},position;
    int64_t result=h==INVALID_HANDLE_VALUE?FILE_ERROR(9):
        SetFilePointerEx(h,move,&position,(DWORD)whence)?position.QuadPart:FILE_ERROR(22);
    ReleaseSRWLockExclusive(&file_lock);return result;
}
static ABI int64_t guest_file_close(int fd) {
    AcquireSRWLockExclusive(&file_lock);
    for(unsigned i=0;i<128;i++) if(guest_files[i].id==fd && fd>=3) {
        if(!CloseHandle(guest_files[i].handle)) {ReleaseSRWLockExclusive(&file_lock);return FILE_ERROR(5);}
        guest_files[i].id=0;ReleaseSRWLockExclusive(&file_lock);return 0;
    }
    ReleaseSRWLockExclusive(&file_lock);return FILE_ERROR(9);
}
/* Explicit FreeBSD/PS4 stat layout, from the existing runtime_file.c. */
typedef struct {
    uint32_t dev,ino;uint16_t mode,nlink;uint32_t uid,gid,rdev;
    GuestTimespec atime,mtime,ctime;int64_t size,blocks;
    uint32_t blksize,flags,gen;int32_t spare;GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"guest stat ABI");
static GuestTimespec stat_time(LARGE_INTEGER time) {
    int64_t ticks=time.QuadPart-INT64_C(116444736000000000);
    int64_t seconds=ticks/10000000,remainder=ticks%10000000;
    if(remainder<0) {--seconds;remainder+=10000000;}
    return (GuestTimespec){seconds,remainder*100};
}
static int64_t stat_handle(HANDLE h,GuestStat *out) {
    FILE_BASIC_INFO basic;FILE_STANDARD_INFO standard;BY_HANDLE_FILE_INFORMATION identity;
    if(!GetFileInformationByHandleEx(h,FileBasicInfo,&basic,sizeof(basic)) ||
       !GetFileInformationByHandleEx(h,FileStandardInfo,&standard,sizeof(standard)) ||
       !GetFileInformationByHandle(h,&identity)) return FILE_ERROR(file_error(GetLastError()));
    GuestStat value={0};
    value.dev=identity.dwVolumeSerialNumber;value.ino=identity.nFileIndexLow;
    value.mode=(uint16_t)(standard.Directory?0040555:0100444);
    value.nlink=(uint16_t)(standard.NumberOfLinks>UINT16_MAX?UINT16_MAX:standard.NumberOfLinks);
    value.atime=stat_time(basic.LastAccessTime);value.mtime=stat_time(basic.LastWriteTime);
    value.ctime=stat_time(basic.ChangeTime);value.birthtime=stat_time(basic.CreationTime);
    value.size=standard.EndOfFile.QuadPart;value.blocks=(standard.AllocationSize.QuadPart+511)/512;
    value.blksize=16384;*out=value;return 0;
}
static ABI int64_t guest_file_fstat(int fd,GuestStat *out) {
    if(!out) return FILE_ERROR(14);
    AcquireSRWLockExclusive(&file_lock);HANDLE h=find_file(fd);
    int64_t result=h==INVALID_HANDLE_VALUE?FILE_ERROR(9):stat_handle(h,out);
    if(getenv("TLG_TRACE_FILES")) fprintf(stderr,"File fstat: fd=%d result=%lld size=%lld\n",
        fd,(long long)result,result?0:(long long)out->size);
    ReleaseSRWLockExclusive(&file_lock);return result;
}
static ABI int64_t guest_file_stat(const char *path,GuestStat *out) {
    if(!out) return FILE_ERROR(14);
    int64_t fd=guest_file_open(path,0,0);if(fd<0) return fd;
    int64_t result=guest_file_fstat((int)fd,out);guest_file_close((int)fd);return result;
}
static int selftest_files(void) {
    if(guest_file_open("/app0/../fixture.bin",0,0)!=FILE_ERROR(22) ||
       guest_file_open("/app0/fixture.bin",1,0)!=FILE_ERROR(45)) return 1;
    GuestStat stat={0},descriptor={0},unchanged;memset(&unchanged,0x55,sizeof(unchanged));
    GuestStat before=unchanged;
    if(guest_file_stat("/app0/fixture.bin",&stat) || stat.size!=5 || (stat.mode&0170000)!=0100000 ||
        stat.mtime.nanoseconds<0 || stat.mtime.nanoseconds>=1000000000 ||
        guest_file_stat("/app0/missing.bin",&unchanged)!=FILE_ERROR(2) || memcmp(&unchanged,&before,sizeof(before)) ||
        guest_file_stat("/app0/fixture.bin",NULL)!=FILE_ERROR(14)) return 1;
    int64_t fd=guest_file_open("/app0/fixture.bin",0,0);char bytes[8]={0};
    if(fd<3 || guest_file_fstat((int)fd,&descriptor) || descriptor.size!=stat.size || descriptor.ino!=stat.ino ||
        guest_file_fstat(-1,&unchanged)!=FILE_ERROR(9) || memcmp(&unchanged,&before,sizeof(before))) return 1;
    puts("File metadata: 120-byte ABI, size, type, timestamp normalization and unchanged errors passed");
    if(fd<3 || guest_file_read((int)fd,bytes,UINT32_MAX)!=5 || memcmp(bytes,"ABCDE",5) ||
       guest_file_read((int)fd,bytes,UINT32_MAX)!=0 || guest_file_seek((int)fd,0,0)!=0 ||
       guest_file_pread((int)fd,bytes,UINT32_MAX,2)!=3 || memcmp(bytes,"CDE",3) || guest_file_seek((int)fd,0,1)!=0 ||
       guest_file_read((int)fd,bytes,3)!=3 || memcmp(bytes,"ABC",3) ||
       guest_file_pread((int)fd,bytes,2,1)!=2 || memcmp(bytes,"BC",2) ||
       guest_file_seek((int)fd,0,1)!=3 || guest_file_pread((int)fd,bytes,8,4)!=1 || bytes[0]!='E' ||
       guest_file_pread((int)fd,bytes,1,99)!=0 || guest_file_seek((int)fd,0,1)!=3 ||
       guest_file_pread((int)fd,bytes,1,-1)!=FILE_ERROR(22) || guest_file_pread((int)fd,NULL,1,0)!=FILE_ERROR(22) ||
       guest_file_seek((int)fd,0,1)!=3 ||
       guest_file_seek((int)fd,-2,2)!=3 || guest_file_read((int)fd,bytes,8)!=2 || memcmp(bytes,"DE",2) ||
       guest_file_read((int)fd,bytes,1)!=0 || guest_file_close((int)fd) ||
       guest_file_close((int)fd)!=FILE_ERROR(9) || guest_file_read((int)fd,bytes,1)!=FILE_ERROR(9)) return 1;
    puts("Files: read-only mount, exact bytes, seek, EOF and stale descriptors passed");return 0;
}
/* ABI layouts and lifecycle adapted from src/runtime_content.c (GPL-2.0-or-later).
 * Narrow AppContent provider: lifecycle and reserved-zero initialization only.
 * No DLC entitlement, package mounting or guessed application parameters. */
static SRWLOCK content_lock=SRWLOCK_INIT;
static unsigned content_refs,content_initialized;
#define MODULE_INVALID ((int32_t)0x805a1000)
#define MODULE_UNLOADED ((int32_t)0x805a1001)
#define CONTENT_PARAMETER ((int32_t)0x80d90002)
#define CONTENT_BUSY ((int32_t)0x80d90003)
/* Limited dialog provider: lifecycle only, no UI/open/result implementation.
 * Error/status conventions follow src/runtime_services.c. */
static unsigned dialog_refs,common_initialized,message_initialized;
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80b80003)
#define DIALOG_ALREADY_INITIALIZED ((int32_t)0x80b80004)
static int32_t dialog_module(int action) {
    AcquireSRWLockExclusive(&content_lock);int32_t result=0;
    if(action==1) {if(dialog_refs==UINT_MAX) result=CONTENT_BUSY;else ++dialog_refs;}
    else if(!dialog_refs) result=MODULE_UNLOADED;
    else if(action==-1 && !--dialog_refs) message_initialized=0;
    ReleaseSRWLockExclusive(&content_lock);return result;
}
static ABI int32_t guest_common_dialog_init(void) {
    AcquireSRWLockExclusive(&content_lock);
    int32_t result=common_initialized?DIALOG_ALREADY_INITIALIZED:0;
    if(!result) common_initialized=1;
    ReleaseSRWLockExclusive(&content_lock);return result;
}
static ABI int32_t guest_message_dialog_init(void) {
    AcquireSRWLockExclusive(&content_lock);
    int32_t result=!dialog_refs?MODULE_UNLOADED:!common_initialized?DIALOG_NOT_INITIALIZED:
        message_initialized?DIALOG_ALREADY_INITIALIZED:0;
    if(!result) message_initialized=1;
    ReleaseSRWLockExclusive(&content_lock);return result;
}
static ABI int32_t guest_message_dialog_status(void) {
    AcquireSRWLockShared(&content_lock);int32_t result=message_initialized?1:0;
    ReleaseSRWLockShared(&content_lock);return result;
}
static ABI int32_t guest_message_dialog_term(void) {
    AcquireSRWLockExclusive(&content_lock);int32_t result=message_initialized?0:DIALOG_NOT_INITIALIZED;
    if(!result) message_initialized=0;
    ReleaseSRWLockExclusive(&content_lock);return result;
}
static int selftest_dialog(void) {
    if(guest_message_dialog_init()!=MODULE_UNLOADED || dialog_module(0)!=MODULE_UNLOADED ||
       dialog_module(1) || guest_message_dialog_init()!=DIALOG_NOT_INITIALIZED ||
       guest_common_dialog_init() || guest_common_dialog_init()!=DIALOG_ALREADY_INITIALIZED ||
       guest_message_dialog_init() || guest_message_dialog_init()!=DIALOG_ALREADY_INITIALIZED ||
       guest_message_dialog_status()!=1 || guest_message_dialog_term() || guest_message_dialog_status()!=0 ||
       guest_message_dialog_term()!=DIALOG_NOT_INITIALIZED || dialog_module(1) || dialog_module(-1) ||
       dialog_module(0) || guest_message_dialog_init() || dialog_module(-1) || guest_message_dialog_status()!=0 ||
       dialog_module(-1)!=MODULE_UNLOADED) return 1;
    puts("Dialogs: load references, initialization order, duplicate init and final unload reset passed");return 0;
}
static ABI int32_t guest_module_load(uint16_t id) {
    if(id==0xa4) {puts("Runtime: limited MESSAGE_DIALOG lifecycle provider loaded (no UI)");return dialog_module(1);}
    if(id!=0xb4) {fprintf(stderr,"Unsupported module load: id=0x%x\n",id);return MODULE_INVALID;}
    AcquireSRWLockExclusive(&content_lock);
    if(content_refs==UINT_MAX) {ReleaseSRWLockExclusive(&content_lock);return CONTENT_BUSY;}
    ++content_refs;ReleaseSRWLockExclusive(&content_lock);
    puts("Runtime: limited AppContent host provider loaded");return 0;
}
static ABI int32_t guest_module_loaded(uint16_t id) {
    if(id==0xa4) return dialog_module(0);
    if(id!=0xb4) return MODULE_INVALID;
    AcquireSRWLockShared(&content_lock);int32_t result=content_refs?0:MODULE_UNLOADED;
    ReleaseSRWLockShared(&content_lock);return result;
}
static ABI int32_t guest_module_unload(uint16_t id) {
    if(id==0xa4) return dialog_module(-1);
    if(id!=0xb4) return MODULE_INVALID;
    AcquireSRWLockExclusive(&content_lock);
    if(!content_refs) {ReleaseSRWLockExclusive(&content_lock);return MODULE_UNLOADED;}
    if(!--content_refs) content_initialized=0;
    ReleaseSRWLockExclusive(&content_lock);return 0;
}
static ABI int32_t guest_content_init(const unsigned char *init,unsigned char *boot) {
    if(!init || !boot) return CONTENT_PARAMETER;
    AcquireSRWLockExclusive(&content_lock);
    if(!content_refs) {ReleaseSRWLockExclusive(&content_lock);return MODULE_UNLOADED;}
    if(content_initialized) {ReleaseSRWLockExclusive(&content_lock);return CONTENT_BUSY;}
    for(unsigned i=0;i<32;i++) if(init[i]) {ReleaseSRWLockExclusive(&content_lock);return CONTENT_PARAMETER;}
    memset(boot,0,40);content_initialized=1;
    ReleaseSRWLockExclusive(&content_lock);return 0;
}
static uint32_t content_parameters[4];
static int parameters_loaded;
static int load_content_parameters(void) {
    const char *path=getenv("TLG_CONTENT_PARAMETERS");
    if(!path) path="out/tlg/content-parameters.bin";
    FILE *f=fopen(path,"rb");unsigned char data[48];
    if(!f) return -1;
    size_t count=fread(data,1,sizeof(data),f);int extra=fgetc(f);fclose(f);
    static const unsigned char title[16]="CUSA03627",version[8]="01.03";
    if(count!=sizeof(data) || extra!=EOF || memcmp(data,"TLGPAR01",8) ||
        memcmp(data+8,title,16) || memcmp(data+24,version,8)) return -1;
    memcpy(content_parameters,data+32,16);parameters_loaded=1;return 0;
}
static ABI int32_t guest_content_parameter(uint32_t id,int32_t *out) {
    if(!out || id<1 || id>4) return CONTENT_PARAMETER;
    AcquireSRWLockExclusive(&content_lock);
    if(!content_refs || !content_initialized) {ReleaseSRWLockExclusive(&content_lock);return MODULE_UNLOADED;}
    if(!parameters_loaded && load_content_parameters()) {
        ReleaseSRWLockExclusive(&content_lock);
        fputs("STOP: prepare validated TLG content parameters before querying AppContent\n",stderr);ExitProcess(21);
    }
    memcpy(out,&content_parameters[id-1],4);
    ReleaseSRWLockExclusive(&content_lock);
    printf("Runtime: AppContent metadata parameter %u = %d\n",id,*out);return 0;
}
static int selftest_content(void) {
    unsigned char init[32]={0},boot[42];memset(boot,0x55,sizeof(boot));
    if(guest_module_loaded(0xb4)!=MODULE_UNLOADED || guest_module_load(0xb5)!=MODULE_INVALID ||
       guest_content_init(init,boot+1)!=MODULE_UNLOADED || guest_module_load(0xb4) || guest_module_load(0xb4)) return 1;
    init[31]=1;if(guest_content_init(init,boot+1)!=CONTENT_PARAMETER || boot[1]!=0x55) return 1;
    init[31]=0;if(guest_content_init(init,boot+1) || boot[0]!=0x55 || boot[41]!=0x55) return 1;
    for(unsigned i=1;i<=40;i++) if(boot[i]) return 1;
    parameters_loaded=1;content_parameters[0]=1;content_parameters[1]=1612131828;content_parameters[2]=0;content_parameters[3]=0;
    int32_t value=-1;
    if(guest_content_parameter(1,&value) || value!=1 || guest_content_parameter(2,&value) || value!=1612131828 ||
        guest_content_parameter(4,&value) || value!=0) return 1;
    value=123;if(guest_content_parameter(0,&value)!=CONTENT_PARAMETER || value!=123 ||
        guest_content_parameter(5,&value)!=CONTENT_PARAMETER || guest_content_parameter(1,NULL)!=CONTENT_PARAMETER) return 1;
    if(guest_content_init(init,boot+1)!=CONTENT_BUSY || guest_module_unload(0xb4) ||
       guest_module_loaded(0xb4) || guest_module_unload(0xb4) || guest_module_loaded(0xb4)!=MODULE_UNLOADED ||
       guest_module_unload(0xb4)!=MODULE_UNLOADED || guest_module_load(0xb4) ||
       guest_content_init(init,boot+1) || guest_module_unload(0xb4)) return 1;
    puts("AppContent: references, initialization bounds, invalid profile and unload reset passed");return 0;
}
/* System parameter IDs follow shadPS4 systemservice.h. The diagnostic
 * explicitly selects English US (1); this is not inferred from Windows locale. */
#define SYSTEM_PARAMETER ((int32_t)0x80a10003)
static ABI int32_t guest_system_parameter(int32_t id,int32_t *out) {
    if(!out || id!=1) return SYSTEM_PARAMETER;
    *out=1;puts("Runtime: system language profile = English US");return 0;
}
static int selftest_system_parameter(void) {
    int32_t value=123;
    if(guest_system_parameter(2,&value)!=SYSTEM_PARAMETER || value!=123 ||
       guest_system_parameter(-1,&value)!=SYSTEM_PARAMETER ||
       guest_system_parameter(1,NULL)!=SYSTEM_PARAMETER || guest_system_parameter(1,&value) || value!=1) return 1;
    puts("System parameters: explicit language profile and invalid-output preservation passed");return 0;
}
/* ABI and errors: shadPS4 playgo_types.h, GPL-2.0-or-later,
 * Copyright 2024 shadPS4 Emulator Project. Metadata availability is not proof
 * that every installation chunk is present; locus/progress remain unresolved. */
typedef struct { const void *buffer; uint32_t size,reserved; } PlayGoInit;
_Static_assert(sizeof(PlayGoInit)==16,"PlayGo initialization ABI");
#define PLAYGO_ERROR(n) ((int32_t)(0x80b20000u|(n)))
static SRWLOCK playgo_lock=SRWLOCK_INIT;
static unsigned playgo_initialized,playgo_opened,playgo_chunks;
static ABI int32_t guest_playgo_init(const PlayGoInit *params) {
    if(!params || !params->buffer) return PLAYGO_ERROR(10);
    if(params->size<0x200000) return PLAYGO_ERROR(11);
    if(params->reserved) return PLAYGO_ERROR(4);
    AcquireSRWLockExclusive(&playgo_lock);
    if(playgo_initialized) {ReleaseSRWLockExclusive(&playgo_lock);return PLAYGO_ERROR(6);}
    int64_t fd=guest_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[20];int valid=0;
    if(fd>=3) {
        int64_t length=guest_file_seek((int)fd,0,2);
        if(guest_file_seek((int)fd,0,0)==0 && guest_file_read((int)fd,header,sizeof(header))==sizeof(header)) {
            uint32_t declared;memcpy(&declared,header+16,4);
            unsigned count=(unsigned)header[10]|((unsigned)header[11]<<8);
            valid=!memcmp(header,"plgo",4) && length==declared && count>0;
            if(valid) playgo_chunks=count;
        }
        guest_file_close((int)fd);
    }
    if(!valid) {ReleaseSRWLockExclusive(&playgo_lock);return PLAYGO_ERROR(4);}
    playgo_initialized=1;ReleaseSRWLockExclusive(&playgo_lock);
    printf("Runtime: PlayGo metadata initialized (%u declared chunks; availability unverified)\n",playgo_chunks);return 0;
}
static ABI int32_t guest_playgo_open(uint32_t *out,const void *params) {
    if(!out) return PLAYGO_ERROR(10);
    if(params) return PLAYGO_ERROR(4);
    AcquireSRWLockExclusive(&playgo_lock);
    if(!playgo_initialized) {ReleaseSRWLockExclusive(&playgo_lock);return PLAYGO_ERROR(5);}
    if(playgo_opened) {ReleaseSRWLockExclusive(&playgo_lock);return PLAYGO_ERROR(7);}
    playgo_opened=1;*out=1;ReleaseSRWLockExclusive(&playgo_lock);return 0;
}
/* Explicit experimental offline profile, never inferred from required_locus.
 * This setting assumes a complete extracted copy; it does not verify package
 * regions, file integrity, or installation completeness. Default is unsupported. */
static int playgo_assume_local(void) {
    const char *value=getenv("TLG_PLAYGO_ASSUME_LOCAL");
    return value && !strcmp(value,"1");
}
static ABI int32_t guest_playgo_locus(uint32_t handle,const uint16_t *ids,uint32_t count,int8_t *out) {
    if(!ids || !out) return PLAYGO_ERROR(10);
    if(!count) return PLAYGO_ERROR(11);
    AcquireSRWLockShared(&playgo_lock);
    if(!playgo_initialized) {ReleaseSRWLockShared(&playgo_lock);return PLAYGO_ERROR(5);}
    if(!playgo_opened || handle!=1) {ReleaseSRWLockShared(&playgo_lock);return PLAYGO_ERROR(9);}
    for(uint32_t i=0;i<count;i++) if(ids[i]>=playgo_chunks) {
        ReleaseSRWLockShared(&playgo_lock);return PLAYGO_ERROR(12);
    }
    if(!playgo_assume_local()) {
        ReleaseSRWLockShared(&playgo_lock);
        fputs("PlayGo availability is unverified; offline assumption profile is disabled\n",stderr);
        return PLAYGO_ERROR(14);
    }
    memset(out,3,count);ReleaseSRWLockShared(&playgo_lock);
    printf("Runtime: PlayGo OFFLINE ASSUMPTION: %u requested chunks reported LocalFast (first=%u)\n",count,ids[0]);
    return 0;
}
static int selftest_playgo(void) {
    uint32_t handle=123;PlayGoInit params={0};
    if(guest_playgo_open(&handle,NULL)!=PLAYGO_ERROR(5) || handle!=123 ||
        guest_playgo_init(NULL)!=PLAYGO_ERROR(10) || guest_playgo_init(&params)!=PLAYGO_ERROR(10)) return 1;
    void *buffer=calloc(1,0x200000);if(!buffer) return 1;params.buffer=buffer;params.size=1;
    if(guest_playgo_init(&params)!=PLAYGO_ERROR(11)) return 1;
    params.size=0x200000;params.reserved=1;if(guest_playgo_init(&params)!=PLAYGO_ERROR(4)) return 1;
    params.reserved=0;
    if(guest_playgo_init(&params) || guest_playgo_init(&params)!=PLAYGO_ERROR(6) ||
        guest_playgo_open(NULL,NULL)!=PLAYGO_ERROR(10) || guest_playgo_open(&handle,buffer)!=PLAYGO_ERROR(4) ||
        guest_playgo_open(&handle,NULL) || handle!=1 || guest_playgo_open(&handle,NULL)!=PLAYGO_ERROR(7)) return 1;
    uint16_t ids[2]={0,(uint16_t)(playgo_chunks-1)};int8_t loci[2]={77,88};
    if(guest_playgo_locus(999,ids,2,loci)!=PLAYGO_ERROR(9) ||
        guest_playgo_locus(handle,NULL,2,loci)!=PLAYGO_ERROR(10) ||
        guest_playgo_locus(handle,ids,0,loci)!=PLAYGO_ERROR(11)) return 1;
    uint16_t invalid[2]={0,(uint16_t)playgo_chunks};
    if(guest_playgo_locus(handle,invalid,2,loci)!=PLAYGO_ERROR(12) || loci[0]!=77 || loci[1]!=88) return 1;
    int32_t result=guest_playgo_locus(handle,ids,2,loci);
    if(playgo_assume_local()) {if(result || loci[0]!=3 || loci[1]!=3) return 1;}
    else if(result!=PLAYGO_ERROR(14) || loci[0]!=77 || loci[1]!=88) return 1;
    puts("PlayGo locus: explicit profile, invalid IDs and unchanged error outputs passed");
    free(buffer);puts("PlayGo: metadata initialization, ABI validation and open lifecycle passed");return 0;
}
/* SaveData lifecycle adapted from the reference GoW3 runtime_savedata.c.
 * Only the observed Initialize3(NULL) contract and termination are exposed.
 * Mounts, file persistence and save-dialog completion remain unsupported. */
#define SAVE_PARAMETER ((int32_t)0x809f0000)
#define SAVE_NOT_INITIALIZED ((int32_t)0x809f0001)
static SRWLOCK save_lock=SRWLOCK_INIT;
static int save_initialized;
static HANDLE save_memory_file=INVALID_HANDLE_VALUE;
static uint64_t save_memory_size;
#define SAVE_INTERNAL ((int32_t)0x809f000b)
#define SAVE_MEMORY_NOT_READY ((int32_t)0x809f0012)
/* Observed SDK setup/get/set layouts from shadPS4 savedata.cpp (GPL-2.0-or-later).
 * Narrow local user 1 / slot 0 memory backend; icons and metadata are unresolved. */
typedef struct {uint32_t option;int32_t user;uint64_t size,icon_size;
    const void *param,*icon;uint32_t slot;unsigned char reserved[20];} SaveMemorySetup;
typedef struct {uint64_t existed_size;unsigned char reserved[16];} SaveMemoryResult;
typedef struct {void *buffer;uint64_t size;int64_t offset;unsigned char reserved[40];} SaveMemoryData;
typedef struct {int32_t user;uint32_t pad;SaveMemoryData *data;void *param,*icon;
    uint32_t slot;unsigned char reserved[28];} SaveMemoryGet;
typedef struct {int32_t user;uint32_t pad;const SaveMemoryData *data;const void *param,*icon;
    uint32_t count,slot;unsigned char reserved[32];} SaveMemorySet;
_Static_assert(sizeof(SaveMemorySetup)==64 && sizeof(SaveMemoryResult)==24 &&
    sizeof(SaveMemoryData)==64 && sizeof(SaveMemoryGet)==64 && sizeof(SaveMemorySet)==72,"save memory ABI");
static int all_zero(const void *data,size_t size) {
    const unsigned char *p=data;for(size_t i=0;i<size;i++) if(p[i]) return 0;return 1;
}
static ABI int32_t guest_save_memory_setup(const SaveMemorySetup *setup,SaveMemoryResult *out) {
    AcquireSRWLockExclusive(&save_lock);
    if(!save_initialized) {ReleaseSRWLockExclusive(&save_lock);return SAVE_NOT_INITIALIZED;}
    if(!setup || setup->user!=1 || setup->slot || setup->option>1 || setup->param || setup->icon ||
        !setup->size || setup->size>16*1024*1024 || setup->icon_size>1024*1024 ||
        !all_zero(setup->reserved,sizeof(setup->reserved))) {
        ReleaseSRWLockExclusive(&save_lock);return SAVE_PARAMETER;
    }
    const char *root=getenv("TLG_SAVE_ROOT");char path[2048];
    if(!root || !*root || strlen(root)>1900) {ReleaseSRWLockExclusive(&save_lock);return SAVE_INTERNAL;}
    snprintf(path,sizeof(path),"%s/CUSA03627-user1-slot0.memory",root);
    if(save_memory_file!=INVALID_HANDLE_VALUE) {
        int32_t result=save_memory_size==setup->size?0:(int32_t)0x809f0003;
        if(!result && out) *out=(SaveMemoryResult){.existed_size=save_memory_size};
        ReleaseSRWLockExclusive(&save_lock);return result;
    }
    HANDLE h=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    LARGE_INTEGER existing;
    if(h==INVALID_HANDLE_VALUE || !GetFileSizeEx(h,&existing) || existing.QuadPart<0 || existing.QuadPart>16*1024*1024) {
        if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);
        ReleaseSRWLockExclusive(&save_lock);return SAVE_INTERNAL;
    }
    /* Never truncate an existing save; explicitly zero-fill newly grown bytes. */
    LARGE_INTEGER end=existing;unsigned char zero[4096]={0};int ok=SetFilePointerEx(h,end,NULL,FILE_BEGIN);
    uint64_t position=(uint64_t)existing.QuadPart;
    while(ok && position<setup->size) {
        DWORD amount=(DWORD)(setup->size-position>sizeof(zero)?sizeof(zero):setup->size-position),written=0;
        ok=WriteFile(h,zero,amount,&written,NULL) && written==amount;position+=written;
    }
    if(!ok || !FlushFileBuffers(h)) {CloseHandle(h);ReleaseSRWLockExclusive(&save_lock);return SAVE_INTERNAL;}
    save_memory_file=h;save_memory_size=setup->size;
    if(out) *out=(SaveMemoryResult){.existed_size=(uint64_t)existing.QuadPart};
    ReleaseSRWLockExclusive(&save_lock);
    printf("Runtime: local save memory ready (%llu bytes; user 1, slot 0; icons/metadata unsupported)\n",
        (unsigned long long)setup->size);return 0;
}
static int32_t save_memory_io(int32_t user,uint32_t slot,const SaveMemoryData *data,int writing) {
    AcquireSRWLockExclusive(&save_lock);int32_t result=0;
    if(!save_initialized) result=SAVE_NOT_INITIALIZED;
    else if(user!=1 || slot || !data || (!data->buffer && data->size) || data->offset<0 ||
        !all_zero(data->reserved,sizeof(data->reserved))) result=SAVE_PARAMETER;
    else if(save_memory_file==INVALID_HANDLE_VALUE) result=SAVE_MEMORY_NOT_READY;
    else if((uint64_t)data->offset>save_memory_size || data->size>save_memory_size-(uint64_t)data->offset) result=SAVE_PARAMETER;
    else {
        LARGE_INTEGER offset={.QuadPart=data->offset};DWORD amount=(DWORD)data->size,done=0;
        int ok=SetFilePointerEx(save_memory_file,offset,NULL,FILE_BEGIN) &&
            (writing?WriteFile(save_memory_file,data->buffer,amount,&done,NULL):
                     ReadFile(save_memory_file,data->buffer,amount,&done,NULL)) && done==amount;
        if(ok && writing) ok=FlushFileBuffers(save_memory_file);
        if(!ok) result=SAVE_INTERNAL;
    }
    ReleaseSRWLockExclusive(&save_lock);return result;
}
static ABI int32_t guest_save_memory_get(const SaveMemoryGet *get) {
    if(!get || get->pad || get->param || get->icon || !all_zero(get->reserved,sizeof(get->reserved))) {
        fprintf(stderr,"Unsupported save-memory get profile: parameter present=%d metadata=%d icon=%d\n",
            get!=NULL,get && get->param!=NULL,get && get->icon!=NULL);return SAVE_PARAMETER;
    }
    return save_memory_io(get->user,get->slot,get->data,0);
}
static ABI int32_t guest_save_memory_set(const SaveMemorySet *set) {
    /* SDK 4.x uses one data descriptor with dataNum left zero; 1 is also
     * accepted. Multi-descriptor requests remain outside this profile. */
    if(!set || set->pad || set->param || set->icon || set->count>1 || !all_zero(set->reserved,sizeof(set->reserved))) {
        fprintf(stderr,"Unsupported save-memory set profile: parameter present=%d metadata=%d icon=%d\n",
            set!=NULL,set && set->param!=NULL,set && set->icon!=NULL);return SAVE_PARAMETER;
    }
    return save_memory_io(set->user,set->slot,set->data,1);
}
static ABI int32_t guest_save_init3(const void *params) {
    if(params) return SAVE_PARAMETER;
    AcquireSRWLockExclusive(&save_lock);save_initialized=1;ReleaseSRWLockExclusive(&save_lock);
    puts("Runtime: SaveData lifecycle initialized (mount/read/write not implemented)");return 0;
}
static ABI int32_t guest_save_term(void) {
    AcquireSRWLockExclusive(&save_lock);
    if(!save_initialized) {ReleaseSRWLockExclusive(&save_lock);return SAVE_NOT_INITIALIZED;}
    if(save_memory_file!=INVALID_HANDLE_VALUE) {
        if(!FlushFileBuffers(save_memory_file) || !CloseHandle(save_memory_file)) {ReleaseSRWLockExclusive(&save_lock);return SAVE_INTERNAL;}
        save_memory_file=INVALID_HANDLE_VALUE;save_memory_size=0;
    }
    save_initialized=0;ReleaseSRWLockExclusive(&save_lock);return 0;
}
static int selftest_save_lifecycle(void) {
    int opaque=123;
    if(guest_save_term()!=SAVE_NOT_INITIALIZED ||
       guest_save_init3(&opaque)!=SAVE_PARAMETER || save_initialized || opaque!=123 ||
       guest_save_init3(NULL) || !save_initialized ||
       guest_save_init3(&opaque)!=SAVE_PARAMETER || !save_initialized ||
       guest_save_init3(NULL) || guest_save_term() || save_initialized ||
       guest_save_term()!=SAVE_NOT_INITIALIZED || guest_save_init3(NULL) || guest_save_term()) return 1;
    puts("SaveData lifecycle: NULL initialization, idempotency, opaque parameter rejection and termination passed");return 0;
}
static int selftest_save_memory(int writing) {
    SaveMemorySetup setup={.option=1,.user=1,.size=64};
    SaveMemoryResult result;memset(&result,0x55,sizeof(result));SaveMemoryResult before=result;
    if(guest_save_memory_setup(&setup,&result)!=SAVE_NOT_INITIALIZED || memcmp(&before,&result,sizeof(result)) ||
        guest_save_init3(NULL)) return 1;
    setup.user=2;
    if(guest_save_memory_setup(&setup,&result)!=SAVE_PARAMETER || memcmp(&before,&result,sizeof(result))) return 1;
    setup.user=1;
    if(guest_save_memory_setup(&setup,&result) || result.existed_size!=(writing?16u:64u)) return 1;
    unsigned char bytes[64];memset(bytes,0x55,sizeof(bytes));
    SaveMemoryData data={.buffer=bytes,.size=64};SaveMemoryGet get={.user=1,.data=&data};
    if(guest_save_memory_get(&get) || memcmp(bytes,"KEEP",4)) return 1;
    for(unsigned i=4;i<64;i++) if(bytes[i]!=(unsigned char)(!writing && i>=16 && i<20?"DATA"[i-16]:0)) return 1;
    data.offset=INT64_MAX;
    if(guest_save_memory_get(&get)!=SAVE_PARAMETER) return 1;
    data.offset=0;data.size=UINT64_MAX;
    if(guest_save_memory_get(&get)!=SAVE_PARAMETER) return 1;
    if(writing) {
        data=(SaveMemoryData){.buffer=(void *)"DATA",.size=4,.offset=16};
        SaveMemorySet set={.user=1,.data=&data,.count=0};
        if(guest_save_memory_set(&set)) return 1;
    }
    if(guest_save_term() || save_memory_file!=INVALID_HANDLE_VALUE) return 1;
    puts("Save memory: preserved existing bytes, explicit zero-fill, bounded I/O and persisted restart passed");return 0;
}
/* Offline local UserService profile: one local user, not a Windows/PSN login.
 * ABI follows the bundled shadPS4 system/userservice.h; error values follow
 * shadPS4 system/userservice_error.h (GPL-2.0-or-later). No online account or
 * dialog completion is reported. Unknown services still trap. */
#define USER_ERROR(n) ((int32_t)(UINT32_C(0x80960000)|(n)))
static SRWLOCK user_lock=SRWLOCK_INIT;
static int user_initialized,user_login_pending;
typedef struct {int32_t priority;} UserInitialize;
typedef struct {int32_t type,id;} UserEvent;
_Static_assert(sizeof(UserInitialize)==4 && sizeof(UserEvent)==8,"UserService ABI");
static ABI int32_t guest_user_init(const UserInitialize *params) {
    if(params && (params->priority<256 || params->priority>767)) return USER_ERROR(5);
    AcquireSRWLockExclusive(&user_lock);
    if(user_initialized) {ReleaseSRWLockExclusive(&user_lock);return USER_ERROR(3);}
    user_initialized=1;user_login_pending=1;
    ReleaseSRWLockExclusive(&user_lock);
    puts("Runtime: UserService initialized (offline local profile: user 1, Player)");return 0;
}
static ABI int32_t guest_user_term(void) {
    AcquireSRWLockExclusive(&user_lock);
    if(!user_initialized) {ReleaseSRWLockExclusive(&user_lock);return USER_ERROR(2);}
    user_initialized=0;user_login_pending=0;
    ReleaseSRWLockExclusive(&user_lock);return 0;
}
static ABI int32_t guest_user_initial(int32_t *out) {
    if(!out) return USER_ERROR(5);
    AcquireSRWLockShared(&user_lock);
    int32_t result=user_initialized?0:USER_ERROR(2);
    if(!result) *out=1;
    ReleaseSRWLockShared(&user_lock);return result;
}
static ABI int32_t guest_user_list(int32_t *out) {
    if(!out) return USER_ERROR(5);
    AcquireSRWLockShared(&user_lock);
    int32_t result=user_initialized?0:USER_ERROR(2);
    if(!result) {out[0]=1;out[1]=out[2]=out[3]=-1;}
    ReleaseSRWLockShared(&user_lock);return result;
}
static ABI int32_t guest_user_name(int32_t id,char *out,uint64_t size) {
    if(!out || id<=0) return USER_ERROR(5);
    AcquireSRWLockShared(&user_lock);
    int32_t result=!user_initialized?USER_ERROR(2):id!=1?USER_ERROR(9):
                   size<sizeof("Player")?USER_ERROR(10):0;
    if(!result) memcpy(out,"Player",sizeof("Player"));
    ReleaseSRWLockShared(&user_lock);return result;
}
static ABI int32_t guest_user_event(UserEvent *out) {
    if(!out) return USER_ERROR(5);
    AcquireSRWLockExclusive(&user_lock);
    int32_t result=!user_initialized?USER_ERROR(2):!user_login_pending?USER_ERROR(7):0;
    if(!result) {*out=(UserEvent){0,1};user_login_pending=0;}
    ReleaseSRWLockExclusive(&user_lock);return result;
}
static DWORD WINAPI test_user_event_worker(void *out) {
    return (DWORD)guest_user_event(out);
}
static int selftest_user_service(void) {
#define USER_CHECK(c) do {if(!(c)) {fprintf(stderr,"UserService check failed at %d\n",__LINE__);return 1;}}while(0)
    int32_t id=123,ids[5]={11,22,33,44,55};UserEvent event={77,88};
    char name[8];memset(name,0x55,sizeof(name));UserInitialize invalid={255},valid={700};
    USER_CHECK(guest_user_initial(&id)==USER_ERROR(2) && id==123);
    USER_CHECK(guest_user_list(ids)==USER_ERROR(2) && ids[0]==11 && ids[4]==55);
    USER_CHECK(guest_user_event(&event)==USER_ERROR(2) && event.type==77 && event.id==88);
    USER_CHECK(guest_user_name(1,name,sizeof(name))==USER_ERROR(2) && name[0]==0x55);
    USER_CHECK(guest_user_term()==USER_ERROR(2) && guest_user_init(&invalid)==USER_ERROR(5));
    USER_CHECK(!guest_user_init(NULL) && guest_user_init(&valid)==USER_ERROR(3));
    USER_CHECK(guest_user_initial(NULL)==USER_ERROR(5) && !guest_user_initial(&id) && id==1);
    USER_CHECK(guest_user_list(NULL)==USER_ERROR(5) && !guest_user_list(ids));
    USER_CHECK(ids[0]==1 && ids[1]==-1 && ids[2]==-1 && ids[3]==-1 && ids[4]==55);
    USER_CHECK(guest_user_name(-1,name,8)==USER_ERROR(5) &&
               guest_user_name(2,name,8)==USER_ERROR(9) &&
               guest_user_name(1,NULL,8)==USER_ERROR(5) &&
               guest_user_name(1,name,6)==USER_ERROR(10) && name[0]==0x55);
    USER_CHECK(!guest_user_name(1,name,7) && !strcmp(name,"Player") && name[7]==0x55);
    USER_CHECK(guest_user_event(NULL)==USER_ERROR(5));
    UserEvent events[2]={{77,88},{77,88}};HANDLE workers[2];
    for(int i=0;i<2;i++) {workers[i]=CreateThread(NULL,0,test_user_event_worker,&events[i],0,NULL);USER_CHECK(workers[i]);}
    USER_CHECK(WaitForMultipleObjects(2,workers,TRUE,10000)==WAIT_OBJECT_0);
    int successes=0;
    for(int i=0;i<2;i++) {
        DWORD code=1;USER_CHECK(GetExitCodeThread(workers[i],&code));CloseHandle(workers[i]);
        if(!code) {successes++;USER_CHECK(events[i].type==0 && events[i].id==1);}
        else USER_CHECK(code==(DWORD)USER_ERROR(7) && events[i].type==77 && events[i].id==88);
    }
    USER_CHECK(successes==1 && guest_user_event(&event)==USER_ERROR(7) && event.type==77);
    USER_CHECK(!guest_user_term() && guest_user_initial(&id)==USER_ERROR(2) && id==1);
    USER_CHECK(!guest_user_init(&valid) && !guest_user_event(&event) && event.type==0 && event.id==1);
    USER_CHECK(!guest_user_term());
    puts("UserService: local profile, lifecycle, bounded outputs and exactly-once concurrent login event passed");return 0;
#undef USER_CHECK
}
/* Logical diagnostic VideoOut port. ABI/status profile follows the bundled
 * shadPS4 video_out.h/videoout_error.h, GPL-2.0-or-later. No host window or
 * renderer is attached: no flip, presentation or vblank success is simulated. */
static SRWLOCK video_lock=SRWLOCK_INIT;
static int32_t video_handle,next_video_handle=1;
#define VIDEO_INVALID ((int32_t)0x80290001)
#define VIDEO_BUSY ((int32_t)0x80290009)
#define VIDEO_BAD_HANDLE ((int32_t)0x8029000b)
typedef struct {int32_t width,height,pane_width,pane_height;uint64_t refresh;
    float inches;uint16_t flags,reserved;uint32_t padding[3];} VideoResolution;
_Static_assert(sizeof(VideoResolution)==48,"VideoOut resolution ABI");
static ABI int32_t guest_video_open(int32_t user,int32_t bus,int32_t index,const void *params) {
    (void)user;
    if(bus || index || params) return VIDEO_INVALID;
    AcquireSRWLockExclusive(&video_lock);
    if(video_handle || next_video_handle==INT_MAX) {ReleaseSRWLockExclusive(&video_lock);return VIDEO_BUSY;}
    video_handle=next_video_handle++;int32_t result=video_handle;ReleaseSRWLockExclusive(&video_lock);
    puts("Runtime: diagnostic VideoOut port opened (1280x720 profile; no window or renderer)");return result;
}
static ABI int32_t guest_video_close(int32_t handle) {
    AcquireSRWLockExclusive(&video_lock);
    if(handle<=0 || handle!=video_handle) {ReleaseSRWLockExclusive(&video_lock);return VIDEO_BAD_HANDLE;}
    video_handle=0;ReleaseSRWLockExclusive(&video_lock);return 0;
}
static ABI int32_t guest_video_resolution(int32_t handle,VideoResolution *out) {
    if(!out) return VIDEO_INVALID;
    AcquireSRWLockShared(&video_lock);
    if(handle<=0 || handle!=video_handle) {ReleaseSRWLockShared(&video_lock);return VIDEO_BAD_HANDLE;}
    VideoResolution result={0};result.width=result.pane_width=1280;result.height=result.pane_height=720;
    result.refresh=3; /* Guest enum for 59.94 Hz, a profile rather than measured presentation. */
    *out=result;ReleaseSRWLockShared(&video_lock);return 0;
}
/* One 64-bit capability word, per the bundled shadPS4 VideoOut ABI.
 * No HDR capability is advertised without an attached presenter. */
static ABI int32_t guest_video_capabilities(int32_t handle,uint64_t *out) {
    if(!out) return VIDEO_INVALID;
    AcquireSRWLockShared(&video_lock);
    if(handle<=0 || handle!=video_handle) {ReleaseSRWLockShared(&video_lock);return VIDEO_BAD_HANDLE;}
    *out=0;ReleaseSRWLockShared(&video_lock);return 0;
}
static int selftest_video(void) {
    VideoResolution value;memset(&value,0x55,sizeof(value));VideoResolution original=value;
    if(guest_video_open(255,1,0,NULL)!=VIDEO_INVALID || guest_video_resolution(0,&value)!=VIDEO_BAD_HANDLE ||
        memcmp(&value,&original,sizeof(value))) return 1;
    uint64_t capabilities[3]={123,456,789};
    if(guest_video_capabilities(0,capabilities+1)!=VIDEO_BAD_HANDLE || capabilities[1]!=456) return 1;
    int32_t handle=guest_video_open(255,0,0,NULL);
    if(guest_video_capabilities(handle,NULL)!=VIDEO_INVALID || guest_video_capabilities(handle,capabilities+1) ||
        capabilities[0]!=123 || capabilities[1]!=0 || capabilities[2]!=789) return 1;
    puts("Video capabilities: no HDR advertised, eight-byte output and invalid-handle preservation passed");
    if(handle<=0 || guest_video_open(255,0,0,NULL)!=VIDEO_BUSY || guest_video_resolution(handle,NULL)!=VIDEO_INVALID ||
        guest_video_resolution(handle,&value) || value.width!=1280 || value.height!=720 || value.refresh!=3 ||
        guest_video_close(handle) || guest_video_close(handle)!=VIDEO_BAD_HANDLE) return 1;
    int32_t reopened=guest_video_open(255,0,0,NULL);
    if(reopened<=handle || guest_video_resolution(handle,&value)!=VIDEO_BAD_HANDLE || guest_video_close(reopened)) return 1;
    puts("VideoOut: logical lifecycle, resolution ABI, duplicate open and stale handles passed");return 0;
}
/* Retail behavior from bundled shadPS4 gnmdriver.cpp/gnm_error.h:
 * debug resource registration is unavailable, not successful GPU setup. */
#define GNM_REGISTRATION_UNAVAILABLE ((int32_t)0x8eee00ff)
static ABI int32_t guest_gnm_register_owner(void *out,const char *name) {
    (void)out;(void)name;
    puts("Runtime: GNM debug owner registration unavailable in retail profile");
    return GNM_REGISTRATION_UNAVAILABLE;
}
static ABI int32_t guest_gnm_register_resource(void *out,void *owner,const void *address,
    uint64_t size,const char *name,int type,uint64_t user_data) {
    (void)out;(void)owner;(void)address;(void)size;(void)name;(void)type;(void)user_data;
    return GNM_REGISTRATION_UNAVAILABLE;
}
static int selftest_gnm_registration(void) {
    uint64_t output[2]={UINT64_C(0x123456789abcdef0),UINT64_C(0xfedcba9876543210)};
    if(guest_gnm_register_owner(output,"test")!=GNM_REGISTRATION_UNAVAILABLE ||
       guest_gnm_register_owner(NULL,NULL)!=GNM_REGISTRATION_UNAVAILABLE ||
       guest_gnm_register_resource(output,NULL,NULL,0,NULL,0,0)!=GNM_REGISTRATION_UNAVAILABLE ||
       output[0]!=UINT64_C(0x123456789abcdef0) || output[1]!=UINT64_C(0xfedcba9876543210)) return 1;
    puts("GNM registration: retail failure code and unchanged output passed");return 0;
}
/* Local NP title configuration only. ABI/errors: shadPS4 np_types.h and
 * np_error.h (GPL-2.0-or-later). Never log or persist the supplied secret. */
typedef struct {char id[13];unsigned char padding[3];} NpTitle;
typedef struct {unsigned char data[128];} NpSecret;
_Static_assert(sizeof(NpTitle)==16 && sizeof(NpSecret)==128,"NP title ABI");
static SRWLOCK np_title_lock=SRWLOCK_INIT;
static NpTitle local_np_title;
static NpSecret local_np_secret;
static int local_np_configured;
static ABI int32_t guest_np_set_title(const NpTitle *title,const NpSecret *secret) {
    if(!title || !secret) {
        fprintf(stderr,"NP local configuration rejected: title present=%d, secret present=%d\n",title!=NULL,secret!=NULL);
        return (int32_t)0x80550003;
    }
    size_t length=0;while(length<sizeof(title->id) && title->id[length]) ++length;
    /* The public ABI bounds the field; it does not establish a prefix or a
     * required 12-character format. Do not guess an online identifier schema. */
    if(!length || length==sizeof(title->id)) {
        fprintf(stderr,"NP local configuration rejected: identifier length=%zu (contents redacted)\n",length);
        return (int32_t)0x8055001e;
    }
    AcquireSRWLockExclusive(&np_title_lock);
    SecureZeroMemory(&local_np_secret,sizeof(local_np_secret));
    memcpy(local_np_title.id,title->id,sizeof(local_np_title.id));
    memset(local_np_title.padding,0,sizeof(local_np_title.padding));
    local_np_secret=*secret;local_np_configured=1;
    ReleaseSRWLockExclusive(&np_title_lock);
    puts("Runtime: NP title configuration recorded locally (offline; no PSN login/network)");return 0;
}
static int selftest_np_title(void) {
    NpTitle title={.id="NPXX00001_00"},invalid=title;NpSecret secret={{0}};
    memset(secret.data,0x55,sizeof(secret.data));
    if(guest_np_set_title(NULL,&secret)!=(int32_t)0x80550003 ||
        guest_np_set_title(&title,NULL)!=(int32_t)0x80550003 || local_np_configured ||
        guest_np_set_title(&title,&secret) || !local_np_configured ||
        memcmp(local_np_title.id,title.id,sizeof(title.id)) || memcmp(&local_np_secret,&secret,sizeof(secret))) return 1;
    invalid.id[12]='A';
    if(guest_np_set_title(&invalid,&secret)!=(int32_t)0x8055001e ||
        memcmp(local_np_title.id,title.id,sizeof(title.id))) return 1;
    memset(&secret,0x33,sizeof(secret));
    if(guest_np_set_title(&title,&secret) || memcmp(&local_np_secret,&secret,sizeof(secret))) return 1;
    title=(NpTitle){.id="CUSA00001"};
    if(guest_np_set_title(&title,&secret) || memcmp(local_np_title.id,title.id,sizeof(title.id))) return 1;
    SecureZeroMemory(&local_np_secret,sizeof(local_np_secret));
    memset(&local_np_title,0,sizeof(local_np_title));local_np_configured=0;
    puts("NP title: local configuration, validation, replacement and secret cleanup passed; no online authentication");return 0;
}
static uintptr_t resolve_verified(const char *name, int is_data, uint64_t capabilities) {
    /* prepare_tlg.py validates this target and its libc hash; prepare.py sets
     * bit 0 only after inspecting the supplied libc's _init_env as exactly C3.
     * Match the observed full scope, never an arbitrary same-NID import.
     */
    if (!is_data && (capabilities & 1) && !strcmp(name, "bzQExy189ZI#V#V"))
        return (uintptr_t)init_env_ret;
    if (!is_data && (capabilities & 1) && !strcmp(name, "8G2LB+A3rzg#V#V"))
        return (uintptr_t)guest_atexit;
    if (!is_data && (capabilities & 1) && !strcmp(name, "tsvEmnenz48#V#V"))
        return (uintptr_t)guest_cxa_atexit;
    if (!is_data && (capabilities & 1) && !strcmp(name, "vNe1w4diLCs#U#H"))
        return (uintptr_t)windows_tls_get_addr;
    if (!is_data && (capabilities & 1) && !strcmp(name, "959qrazPIrg#U#H"))
        return (uintptr_t)get_process_parameter;
    if (!is_data && (capabilities & 1) && !strcmp(name, "p5EcQeEeJAE#U#H"))
        return (uintptr_t)register_application_heap;
    if (!is_data && (capabilities & 1) && !strcmp(name, "WslcK1FQcGI#U#H"))
        return (uintptr_t)is_neo_mode;
    if (!is_data && (capabilities & 1) && !strcmp(name, "NWtTN10cJzE#heap-trace"))
        return (uintptr_t)heap_get_trace_info;
    if (is_data && (capabilities & 1) && !strcmp(name, "f7uOxY9mM1U#U#H"))
        return (uintptr_t)&stack_canary;
    if (!is_data && (capabilities & 1)) {
        uintptr_t pad=windows_pad_resolve(name);if(pad) return pad;
        const char *tag=strchr(name,'#');
        if(tag && !strcmp(tag,"#posix")) {
            uintptr_t posix=windows_mutex_resolve(name);
            return posix?posix:windows_thread_resolve(name);
        }
        if (!strcmp(name,"TywrFKCoLGY#savedata")) return (uintptr_t)guest_save_init3;
        if (!strcmp(name,"Ec63y59l9tw#npmanager")) return (uintptr_t)guest_np_set_title;
        if (!strcmp(name,"yKDy8S5yLA0#savedata")) return (uintptr_t)guest_save_term;
        if (!strcmp(name,"oQySEUfgXRA#savedata")) return (uintptr_t)guest_save_memory_setup;
        if (!strcmp(name,"QwOO7vegnV8#savedata")) return (uintptr_t)guest_save_memory_get;
        if (!strcmp(name,"cduy9v4YmT4#savedata")) return (uintptr_t)guest_save_memory_set;
        if (!strcmp(name,"ts6GlZOKRrE#playgo")) return (uintptr_t)guest_playgo_init;
        if (!strcmp(name,"uWIYLFkkwqk#playgo")) return (uintptr_t)guest_playgo_locus;
        if (!strcmp(name,"M1Gma1ocrGE#playgo")) return (uintptr_t)guest_playgo_open;
        if (!strcmp(name,"fZo48un7LK4#systemservice")) return (uintptr_t)guest_system_parameter;
        if (!strcmp(name,"j3YMu1MVNNo#userservice")) return (uintptr_t)guest_user_init;
        if (!strcmp(name,"bwFjS+bX9mA#userservice")) return (uintptr_t)guest_user_term;
        if (!strcmp(name,"CdWp0oHWGr0#userservice")) return (uintptr_t)guest_user_initial;
        if (!strcmp(name,"fPhymKNvK-A#userservice")) return (uintptr_t)guest_user_list;
        if (!strcmp(name,"1xxcMiGu2fo#userservice")) return (uintptr_t)guest_user_name;
        if (!strcmp(name,"yH17Q6NWtVg#userservice")) return (uintptr_t)guest_user_event;
        if (!strcmp(name,"uoUpLGNkygk#commondialog")) return (uintptr_t)guest_common_dialog_init;
        if (!strcmp(name,"lDqxaY1UbEo#msgdialog")) return (uintptr_t)guest_message_dialog_init;
        if (!strcmp(name,"6fIC3XKt2k0#msgdialog")) return (uintptr_t)guest_message_dialog_status;
        if (!strcmp(name,"ePw-kqZmelo#msgdialog")) return (uintptr_t)guest_message_dialog_term;
        if (!strcmp(name,"kGVLc3htQE8#videoout")) return (uintptr_t)guest_video_capabilities;
        if (!strcmp(name,"ZFqKFl23aMc#E#F")) return (uintptr_t)guest_gnm_register_owner;
        if (!strcmp(name,"nvEwfYAImTs#E#F")) return (uintptr_t)guest_gnm_register_resource;
        if (!strcmp(name,"Up36PTk687E#videoout")) return (uintptr_t)guest_video_open;
        if (!strcmp(name,"uquVH4-Du78#videoout")) return (uintptr_t)guest_video_close;
        if (!strcmp(name,"6kPnj51T62Y#videoout")) return (uintptr_t)guest_video_resolution;
        if (!strcmp(name,"g8cM39EUZ6o#sysmodule")) return (uintptr_t)guest_module_load;
        if (!strcmp(name,"fMP5NHUOaMk#sysmodule")) return (uintptr_t)guest_module_loaded;
        if (!strcmp(name,"eR2bZFAAU0Q#sysmodule")) return (uintptr_t)guest_module_unload;
        if (!strcmp(name,"99b82IKXpH4#appcontent")) return (uintptr_t)guest_content_parameter;
        if (!strcmp(name,"R9lA82OraNs#appcontent")) return (uintptr_t)guest_content_init;
        if (!strcmp(name,"eV9wAD2riIA#U#H")) return (uintptr_t)guest_file_stat;
        if (!strcmp(name,"kBwCPsYX-m4#U#H")) return (uintptr_t)guest_file_fstat;
        if (!strcmp(name,"1G3lF1Gg1k8#U#H")) return (uintptr_t)guest_file_open;
        if (!strcmp(name,"Cg4srZ6TKbU#U#H")) return (uintptr_t)guest_file_read;
        if (!strcmp(name,"+r3rMFwItV4#U#H")) return (uintptr_t)guest_file_pread;
        if (!strcmp(name,"oib76F-12fk#U#H")) return (uintptr_t)guest_file_seek;
        if (!strcmp(name,"UK2Tl2DWUns#U#H")) return (uintptr_t)guest_file_close;
        if (!strcmp(name,"lLMT9vJAck0#U#H")) return (uintptr_t)guest_clock_gettime;
        if (!strcmp(name,"-2IRUCO--PM#U#H")) return (uintptr_t)guest_read_tsc;
        if (!strcmp(name,"1j3S3n-tTW4#U#H")) return (uintptr_t)guest_tsc_frequency;
        if (!strcmp(name,"4J2sUJmuHZQ#U#H")) return (uintptr_t)guest_process_time;
        if (!strcmp(name,"FxVZqBAA7ks#U#H")) return (uintptr_t)guest_console_write;
        if (!strcmp(name,"9BcDykPmo1I#U#H")) return (uintptr_t)guest_console_error;
        uintptr_t resolved = windows_equeue_resolve(name);
        if (!resolved) resolved = windows_mutex_resolve(name);
        if (!resolved) resolved = windows_memory_resolve(name);
        if (!resolved) resolved = windows_thread_resolve(name);
        return resolved ? resolved : windows_semaphore_resolve(name);
    }
    return 0;
}
static uintptr_t resolve_identity(uint64_t index, int is_data, uint64_t capabilities) {
    if (!identities) return resolve_verified(names[index], is_data, capabilities);
    const ImportIdentity *id = &identities[index];
#ifdef TLG_WITH_RENDERER
    if(gpu_active && id->kind==(is_data?1u:2u) && id->library_version==1 &&
        ((!strcmp(id->library,"libSceGnmDriver") && !strcmp(id->module,"libSceGnmDriver") && id->module_version==257) ||
         (!strcmp(id->library,"libSceVideoOut") && !strcmp(id->module,"libSceVideoOut") && id->module_version==0) ||
         (!strcmp(id->library,"libkernel") && !strcmp(id->module,"libkernel") && id->module_version==257) ||
         (!strcmp(id->library,"libSceAvPlayer") && !strcmp(id->module,"libSceAvPlayer") && id->module_version==257))) {
        uintptr_t resolved=gow3gpu_resolve_identity(id->nid,id->library,id->library_version,id->module,is_data);
        if(resolved) return resolved;
    }
#endif
    const char *scope = NULL;
    /* Explicit contracts for the observed ABI identities. These aliases are
     * internal dispatch keys, never untrusted dump-local library IDs.
     */
    if (id->library_version == 1 && id->module_version == 257) {
        if (!strcmp(id->library, "libc") && !strcmp(id->module, "libc")) scope = "V#V";
        else if (!strcmp(id->library, "libkernel") && !strcmp(id->module, "libkernel")) scope = "U#H";
        else if (!strcmp(id->library, "libSceSystemService") && !strcmp(id->module, "libSceSystemService")) scope = "systemservice";
        else if (!strcmp(id->library,"libSceUserService") && !strcmp(id->module,"libSceUserService")) scope="userservice";
        else if (!strcmp(id->library,"libScePad") && !strcmp(id->module,"libScePad")) scope="pad";
        else if (!strcmp(id->library,"libSceSaveData") && !strcmp(id->module,"libSceSaveData")) scope="savedata";
        else if (!strcmp(id->library,"libSceNpManager") && !strcmp(id->module,"libSceNpManager")) scope="npmanager";
        else if (!strcmp(id->library,"libScePosix") && !strcmp(id->module,"libkernel")) scope="posix";
        else if (!strcmp(id->library,"libSceCommonDialog") && !strcmp(id->module,"libSceCommonDialog")) scope="commondialog";
        else if (!strcmp(id->library,"libSceMsgDialog") && !strcmp(id->module,"libSceMsgDialog")) scope="msgdialog";
        else if (!strcmp(id->library, "libSceSysmodule") && !strcmp(id->module, "libSceSysmodule")) scope = "sysmodule";
        else if (!strcmp(id->library, "libSceAppContent") && !strcmp(id->module, "libSceAppContentUtil")) scope = "appcontent";
        else if (!strcmp(id->library, "libSceGnmDriver") && !strcmp(id->module, "libSceGnmDriver")) scope = "E#F";
        else if (!strcmp(id->library, "libSceLibcInternalExt") && !strcmp(id->module, "libSceLibcInternal")) scope = "heap-trace";
    }
    if (id->library_version==1 && id->module_version==256 &&
        !strcmp(id->library,"libScePlayGo") && !strcmp(id->module,"libScePlayGo")) scope="playgo";
    if(id->library_version==1 && id->module_version==0 &&
        !strcmp(id->library,"libSceVideoOut") && !strcmp(id->module,"libSceVideoOut")) scope="videoout";
    if (!scope) return 0;
    char dispatch[128];
    snprintf(dispatch, sizeof(dispatch), "%s#%s", id->nid, scope);
    return resolve_verified(dispatch, is_data, capabilities);
}

static void fail(const char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    exit(1);
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t rounded(size_t n) { return (n + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t n) {
    void *p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
    return p;
}
static void protect(void *p, size_t n, unsigned flags) {
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = flags & 2 ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, n, mode, &old)) fail("VirtualProtect failed");
    if (!FlushInstructionCache(GetCurrentProcess(), p, n)) fail("instruction cache flush failed");
}
static int mapped(Segment *segments, uint64_t count, uint64_t addr, uint64_t size) {
    for (uint64_t i = 0; i < count; ++i)
        if (addr >= segments[i].address && size <= segments[i].size &&
            addr - segments[i].address <= segments[i].size - size) return 1;
    return 0;
}
static ABI __attribute__((noreturn)) void unsupported(uint32_t id, uintptr_t first_arg) {
    if (id >= import_count) fail("invalid import trap");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("Diagnostic API name: %s\n", diagnostic_name(names[id]));
    if (identities) printf("Identity: %s@%u / %s@%u / kind %u\n", identities[id].library,
        identities[id].library_version, identities[id].module, identities[id].module_version, identities[id].kind);
    printf("Caller offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, first_arg);
    printf("Verified _init_env calls: %" PRIu64 "\n", init_env_calls);
    printf("Registered atexit callbacks: %zu\n", exit_count);
    puts(entered_game ? "Original entry reached an unsupported import; game initialization is incomplete." :
                       "Native module initialization is incomplete; original game entry has not run.");
    fflush(NULL);
    ExitProcess(20);
}
static ABI __attribute__((noreturn)) void guest_exit(void) {
    finalize_callbacks();
    ExitProcess(24);
}
static LONG WINAPI fault(EXCEPTION_POINTERS *e) {
    if(e->ExceptionRecord->ExceptionCode==0x406d1388 &&
        !(e->ExceptionRecord->ExceptionFlags&EXCEPTION_NONCONTINUABLE) &&
        e->ExceptionRecord->NumberParameters>=3 &&
        (uint32_t)e->ExceptionRecord->ExceptionInformation[0]==0x1000)
        return EXCEPTION_CONTINUE_EXECUTION;
#ifdef TLG_WITH_RENDERER
    if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION &&
        e->ExceptionRecord->NumberParameters>=2 &&
        gow3gpu_handle_fault(e,(void *)e->ExceptionRecord->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
#endif
    if(windows_renderer_recover(e)) return EXCEPTION_CONTINUE_EXECUTION;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    printf("STOP: Windows exception 0x%08lx at 0x%" PRIxPTR,
           (unsigned long)e->ExceptionRecord->ExceptionCode, at);
    if (at >= (uintptr_t)image && at - (uintptr_t)image < image_size)
        printf(" (guest offset 0x%" PRIxPTR ")", at - (uintptr_t)image);
    puts("");
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        e->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t address = e->ExceptionRecord->ExceptionInformation[1];
        printf("Access type: %" PRIuPTR "; address: 0x%" PRIxPTR "\n",
               (uintptr_t)e->ExceptionRecord->ExceptionInformation[0], address);
        if (data_traps && address >= (uintptr_t)data_traps &&
            address - (uintptr_t)data_traps < import_count * page_size)
            printf("Unsupported data import: %s\n",
                   names[(address - (uintptr_t)data_traps) / page_size]);
    }
    fflush(NULL);
    ExitProcess(23);
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI watchdog(void *unused) {
    (void)unused;
    Sleep(gpu_requested?120000:10000);
    fprintf(stderr,"STOP: diagnostic timeout after %d seconds\n",gpu_requested?120:10);
    fflush(NULL);
    ExitProcess(124);
    return 0;
}
static int selftest_thread_notification(void) {
    void *handler=AddVectoredExceptionHandler(1,fault);
    if(!handler) return 1;
    ULONG_PTR info[3]={0x1000,(ULONG_PTR)"tlg:test",(ULONG_PTR)(DWORD)-1};
    RaiseException(0x406d1388,0,3,info);
    RemoveVectoredExceptionHandler(handler);
    puts("Windows thread notification: continuable name event returned normally passed");
    return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if(argc==2 && !strcmp(argv[1],"--selftest-thread-notification")) return selftest_thread_notification();
    if (argc == 2 && !strcmp(argv[1], "--selftest-gnm-registration")) return selftest_gnm_registration();
    if (argc == 2 && !strcmp(argv[1], "--selftest-video")) return selftest_video();
    if (argc == 2 && !strcmp(argv[1], "--selftest-dialog")) return selftest_dialog();
    if (argc == 2 && !strcmp(argv[1], "--selftest-thread-create")) return windows_thread_create_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-playgo")) return selftest_playgo();
    if (argc == 2 && !strcmp(argv[1], "--selftest-semaphore")) return windows_semaphore_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-eventflag")) return windows_eventflag_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-system-parameter")) return selftest_system_parameter();
    if (argc == 2 && !strcmp(argv[1], "--selftest-user-service")) return selftest_user_service();
    if (argc == 2 && !strcmp(argv[1], "--selftest-pad")) return windows_pad_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-save-lifecycle")) return selftest_save_lifecycle();
    if (argc == 2 && !strcmp(argv[1], "--selftest-np-title")) return selftest_np_title();
    if (argc == 2 && !strcmp(argv[1], "--selftest-save-memory-write")) return selftest_save_memory(1);
    if (argc == 2 && !strcmp(argv[1], "--selftest-save-memory-read")) return selftest_save_memory(0);
    if (argc == 2 && !strcmp(argv[1], "--selftest-content")) return selftest_content();
    if (argc == 2 && !strcmp(argv[1], "--selftest-files")) return selftest_files();
    if (argc == 2 && !strcmp(argv[1], "--selftest-timespec")) return selftest_timespec();
    if (argc == 2 && !strcmp(argv[1], "--selftest-clock")) return selftest_clock();
    if (argc == 2 && !strcmp(argv[1], "--selftest-console")) return selftest_console();
    if (argc == 2 && !strcmp(argv[1], "--selftest-atexit")) return selftest_atexit();
    if (argc == 2 && !strcmp(argv[1], "--selftest-equeue")) return windows_equeue_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-mutex")) return windows_mutex_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-thread")) return windows_thread_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-memory")) return windows_memory_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-renderer-recovery")) return windows_renderer_recovery_selftest();
#ifdef TLG_WITH_RENDERER
    if (argc == 2 && !strcmp(argv[1], "--selftest-renderer-link")) {
        printf("Renderer linked: %u symbols before initialization; no GPU started\n",gow3gpu_symbol_count());
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--selftest-renderer-identity")) {
        int result=gow3gpu_selftest_identity();
        if(!result) puts("Renderer identity: library, module, version and object/function isolation passed");
        return result;
    }
    if(argc==2 && !strcmp(argv[1],"--selftest-renderer-profile")) {
        int result=gow3gpu_selftest_profile();
        if(!result) puts("TLG graphics profile: native targets, precise readbacks and GoW3 enhancements disabled passed");
        return result;
    }
#endif
    if (argc == 2 && !strcmp(argv[1], "--selftest-tls")) return windows_tls_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-heap-trace")) return selftest_heap_trace();
    const char *boot_path=argc==2?argv[1]:NULL;
    if(argc==3 && !strcmp(argv[1],"--gpu")) {
#ifdef TLG_WITH_RENDERER
        gpu_requested=1;boot_path=argv[2];
#else
        fputs("GPU mode requires the linked tlg-renderer-probe executable\n",stderr);return 1;
#endif
    }
    if (!boot_path) {
#ifdef TLG_WITH_RENDERER
        fputs("Usage: tlg-renderer-probe.exe [--gpu] boot.bin\nExperimental diagnostic; no gameplay guarantee.\n",stderr);
#else
        fputs("Usage: tlg-entry-probe.exe boot.bin\nDiagnostic only; no graphics or PS4 runtime.\n", stderr);
#endif
        return 1;
    }
    SYSTEM_INFO system; GetSystemInfo(&system); page_size = system.dwPageSize;
    FILE *f = fopen(boot_path, "rb");
    if (!f) fail("cannot open boot file");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE2", 8) && memcmp(magic, "TLGWIN01", 8) && memcmp(magic,"TLGWIN02",8)))
        fail("expected BBPROBE2, TLGWIN01 or TLGWIN02; Linux linked images are not supported");
    image_size = read64(f);
    uint64_t entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = read64(f);
    int linked=!memcmp(magic,"TLGWIN02",8);
    if(gpu_requested && !linked) fail("GPU mode requires a canonical linked Windows image");
    if (!image_size || image_size > 512 * 1024 * 1024 || entry >= image_size ||
        !ns || ns > 64 || nr > 1000000 || import_count > 100000 || capabilities > 1)
        fail("boot file limits exceeded");
    uint64_t main_tls[4]={0}, procparam=0, site_count=0, *sites=NULL;
    uint64_t *bindings=calloc((size_t)(import_count?import_count:1),sizeof(*bindings));
    uint64_t *binding_kinds=calloc((size_t)(import_count?import_count:1),sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("binding allocation failed");
    if (linked) {
        if (!(capabilities&1)) fail("native modules require preparation evidence");
        procparam=read64(f);
        for (unsigned i=0;i<4;++i) main_tls[i]=read64(f);
        native_count=read64(f);
        if (native_count>15) fail("too many native modules");
        for (uint64_t i=0;i<native_count;++i) {
            NativeModule *m=&native_modules[i];
            m->base=read64(f); m->size=read64(f); m->init=read64(f);
            m->tls_address=read64(f); m->tls_memsz=read64(f);
            m->tls_filesz=read64(f); m->tls_module=read64(f);
            if (m->base>=image_size || m->size>image_size-m->base || m->init<m->base ||
                m->init-m->base>=m->size || m->tls_filesz>m->tls_memsz || m->tls_memsz>1024*1024 ||
                (m->tls_module && (m->tls_module<2 || m->tls_module>=16))) fail("invalid native module");
        }
        uint64_t count=read64(f);
        if (count>import_count) fail("too many native bindings");
        for (uint64_t i=0;i<count;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            if (index>=import_count || !address || address>=image_size || bindings[index] ||
                (kind!=1 && kind!=2)) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
        site_count=read64(f);
        if (site_count>1000000) fail("too many TLS sites");
        sites=calloc((size_t)(site_count?site_count:1),sizeof(*sites));
        if (!sites) fail("TLS site allocation failed");
        for (uint64_t i=0;i<site_count;++i) sites[i]=read64(f);
    }
    if (BCryptGenRandom(NULL, (PUCHAR)&stack_canary, sizeof(stack_canary),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) fail("stack canary generation failed");
    stack_canary &= ~UINT64_C(255);
    Segment *segments = calloc((size_t)ns, sizeof(*segments));
    Reloc *relocs = calloc((size_t)(nr ? nr : 1), sizeof(*relocs));
    names = calloc((size_t)(import_count ? import_count : 1), sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        Segment *s = &segments[i];
        s->address = read64(f); s->size = read64(f); s->flags = read64(f);
        if (!s->size || s->address >= image_size || s->size > image_size - s->address ||
            s->address % page_size || s->flags > 7) fail("invalid segment");
        for (uint64_t j = 0; j < i; ++j)
            if (s->address < segments[j].address + rounded((size_t)segments[j].size) &&
                segments[j].address < s->address + rounded((size_t)s->size)) fail("overlapping segments");
        if ((s->flags & 1) && entry >= s->address && entry - s->address < s->size) executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    if (fread(names, 128, (size_t)import_count, f) != import_count) fail("truncated imports");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import");
    if (!memcmp(magic, "TLGWIN01", 8) || linked) {
        identities = calloc((size_t)(import_count ? import_count : 1), sizeof(*identities));
        if (!identities || fread(identities, sizeof(*identities), (size_t)import_count, f) != import_count)
            fail("truncated canonical identities");
        for (uint64_t i = 0; i < import_count; ++i) {
            ImportIdentity *id = &identities[i];
            if (!memchr(id->nid, 0, sizeof(id->nid)) || !memchr(id->library, 0, sizeof(id->library)) ||
                !memchr(id->module, 0, sizeof(id->module)) || !id->nid[0] || !id->library[0] || !id->module[0] ||
                id->library_version > 65535 || id->module_version > 65535 || (id->kind != 1 && id->kind != 2))
                fail("invalid canonical identity");
            size_t length = strcspn(names[i], "#");
            if (length != strlen(id->nid) || memcmp(names[i], id->nid, length)) fail("import NID mismatch");
        }
    }
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        r->target = read64(f); r->kind = read64(f); r->value = read64(f); r->addend = read64(f);
        if (!mapped(segments, ns, r->target, 8) || r->kind > 2 ||
            (r->kind && r->value >= import_count) ||
            (r->kind != 2 && r->addend) || r->addend >= page_size) fail("invalid relocation");
        if (r->kind && identities && identities[r->value].kind != (r->kind == 2 ? 1u : 2u))
            fail("canonical symbol kind mismatch");
    }
    image = allocate(rounded((size_t)image_size));
    if (fread(image, 1, (size_t)image_size, f) != image_size || fgetc(f) != EOF) fail("incorrect image size");
    fclose(f);
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || !mapped(segments,ns,procparam,64) ||
            (main_tls[2] && !mapped(segments,ns,main_tls[0],main_tls[2]))) fail("invalid main TLS metadata");
        for (uint64_t i=0;i<native_count;++i) {
            NativeModule *m=&native_modules[i];
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && m->init>=segments[s].address &&
                    m->init-segments[s].address<segments[s].size) executable=1;
            if (!executable || (m->tls_module && !mapped(segments,ns,m->tls_address,m->tls_memsz)))
                fail("unmapped module initializer or TLS");
        }
        for (uint64_t i=0;i<import_count;++i) if (bindings[i]) {
            if (!mapped(segments,ns,bindings[i],1) || identities[i].kind!=(binding_kinds[i]==2?1u:2u))
                fail("invalid native binding identity");
            if (binding_kinds[i]==1) {
                int executable=0;
                for (uint64_t s=0;s<ns;++s)
                    if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                        bindings[i]-segments[s].address<segments[s].size) executable=1;
                if (!executable) fail("native function is not executable");
            }
        }
        uint32_t offset;
        if (windows_tls_slot_offset(&offset)) fail("Windows TLS slot unavailable");
        const unsigned char expected[]={0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
        for (uint64_t i=0;i<site_count;++i) {
            uint64_t at=sites[i]; int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && at>=segments[s].address && segments[s].size>=9 &&
                    at-segments[s].address<=segments[s].size-9) executable=1;
            if (!executable || at>image_size-9 || memcmp(image+at,expected,9)) fail("invalid Windows TLS patch site");
            image[at]=0x65;
            memcpy(image+at+5,&offset,4);
        }
    }
    unsigned char *traps = allocate(rounded((size_t)(import_count + 1) * 32));
    data_traps = allocate((size_t)(import_count + 1) * page_size);
    protect(data_traps, (size_t)(import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        uint32_t id = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unsupported;
        t[0]=0x48; t[1]=0x89; t[2]=0xfe; /* mov rsi,rdi */
        t[3]=0xbf; memcpy(t+4, &id, 4); /* mov edi,id */
        t[8]=0x48; t[9]=0xb8; memcpy(t+10, &handler, 8);
        t[18]=0xff; t[19]=0xe0; /* jmp rax */
    }
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        uintptr_t value = r->kind == 2 ? (uintptr_t)(data_traps + page_size*r->value + r->addend)
                        : r->kind == 1 ? (uintptr_t)(traps + 32*r->value)
                        : (uintptr_t)image + r->value;
        if (r->kind) {
            uintptr_t resolved = resolve_identity(r->value, r->kind == 2, capabilities);
            if (resolved) value = resolved + r->addend;
            else if (bindings[r->value]) value=(uintptr_t)image+bindings[r->value]+r->addend;
        }
        memcpy(image + r->target, &value, 8);
    }
    protect(traps, rounded((size_t)(import_count + 1)*32), 5);
    protect(image, rounded((size_t)image_size), 0);
    for (uint64_t i = 0; i < ns; ++i)
        protect(image + segments[i].address, rounded((size_t)segments[i].size), (unsigned)segments[i].flags);
    for(uint64_t i=0;i<ns;i++) {
        unsigned flags=(unsigned)segments[i].flags;
        int protection=((flags&4)?1:0)|((flags&2)?2:0)|((flags&1)?4:0);
        if(!windows_memory_register_external(image+segments[i].address,
            rounded((size_t)segments[i].size),protection)) fail("cannot register image memory for renderer");
    }
    require_clock();
    if (!AddVectoredExceptionHandler(1, fault)) fail("exception handler unavailable");
    HANDLE timer = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (!timer) fail("watchdog unavailable");
    CloseHandle(timer);
    if (linked) {
        process_parameter=image+procparam;
        if (windows_tls_configure_main(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]?main_tls[3]:16) || windows_tls_attach_main())
            fail("main Windows TLS initialization failed");
        for (uint64_t i=0;i<native_count;++i) {
            NativeModule *m=&native_modules[i];
            if (m->tls_module && windows_tls_set_module(m->tls_module,image+m->tls_address,m->tls_filesz,m->tls_memsz))
                fail("native module TLS configuration failed");
        }
#ifdef TLG_WITH_RENDERER
        if(gpu_requested) {
            if(!windows_memory_register_current_stack()) fail("cannot register main stack for GPU");
            const char *path=getenv("TLG_GPU_PARAMETERS");
            FILE *metadata=fopen(path?path:"out/tlg/gpu-parameters.bin","rb");
            unsigned char data[36];
            if(!metadata || fread(data,1,sizeof(data),metadata)!=sizeof(data) || fgetc(metadata)!=EOF ||
                memcmp(data,"TLGGPU01",8) || memcmp(data+8,"CUSA03627",10) || memcmp(data+24,"01.03",5))
                fail("GPU mode requires validated CUSA03627 01.03 parameters");
            fclose(metadata);
            uint32_t sdk,attributes;memcpy(&sdk,image+procparam+16,4);memcpy(&attributes,data+32,4);
            CreateDirectoryA("out/tlg/gpu-user",NULL);
            Gow3GpuConfig config={"The Last Guardian - Windows diagnostic","CUSA03627","out/tlg/gpu-user",sdk,attributes,1280,720};
            printf("Starting Vulkan renderer: SDK 0x%08x, SFO attributes 0x%08x\n",sdk,attributes);
            gow3gpu_register_kernel();
            if(gow3gpu_init(&config)) fail("GPU initialization failed");
            gpu_active=1;
            uint64_t rebound=0;
            for(uint64_t i=0;i<nr;i++) if(relocs[i].kind) {
                Reloc *r=&relocs[i];
                uintptr_t resolved=resolve_identity(r->value,r->kind==2,capabilities);
                if(!resolved) continue;
                uintptr_t previous,value=resolved+r->addend;
                memcpy(&previous,image+r->target,8);
                if(previous==value) continue;
                DWORD old,ignored;
                if(!VirtualProtect(image+r->target,8,PAGE_READWRITE,&old)) fail("GPU relocation protection failed");
                memcpy(image+r->target,&value,8);
                if(!VirtualProtect(image+r->target,8,old,&ignored) ||
                    !FlushInstructionCache(GetCurrentProcess(),image+r->target,8)) fail("GPU relocation restore failed");
                rebound++;
            }
            printf("GPU imports connected: %llu relocations; %u registered symbols\n",(unsigned long long)rebound,gow3gpu_symbol_count());
        }
#endif
        for (uint64_t i=0;i<native_count;++i) {
            printf("Starting native module %" PRIu64 " at guest offset 0x%" PRIx64 "\n",i,native_modules[i].init);
            typedef int (ABI *Initialize)(uint64_t,void *,void *);
            int result=((Initialize)(image+native_modules[i].init))(0,NULL,NULL);
            printf("Native module %" PRIu64 " returned %d\n",i,result);
            if (result) fail("native module initializer failed");
        }
    }
    printf("Windows entry diagnostic: mapped %" PRIu64 " bytes; %" PRIu64 " relocations\n", image_size, nr);
    puts(gpu_requested ? "Windows native modules, TLS and experimental Vulkan GPU enabled." : linked ? "Windows native modules and TLS enabled; no GPU integration." :
                  "Startup contracts enabled; no native modules, GPU or TLS rewrite.");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
    fail("entry unexpectedly returned");
}

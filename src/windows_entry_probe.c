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
#include "windows_equeue.h"
#include "windows_mutex.h"
#include "windows_tls.h"
#include "windows_memory.h"
#include "windows_thread.h"
#if !defined(__x86_64__) || !defined(__clang__)
#error Build with x86-64 Clang (SysV guest ABI required).
#endif
#define ABI __attribute__((sysv_abi))
typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static unsigned char *image, *data_traps;
static char (*names)[128];
static uint64_t image_size, import_count;
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
/* POSIX console writes only. File descriptors are not host Win32 handles. */
static _Thread_local int32_t console_errno;
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
        if (!strcmp(name,"FxVZqBAA7ks#U#H")) return (uintptr_t)guest_console_write;
        if (!strcmp(name,"9BcDykPmo1I#U#H")) return (uintptr_t)guest_console_error;
        uintptr_t resolved = windows_equeue_resolve(name);
        if (!resolved) resolved = windows_mutex_resolve(name);
        if (!resolved) resolved = windows_memory_resolve(name);
        return resolved ? resolved : windows_thread_resolve(name);
    }
    return 0;
}
static uintptr_t resolve_identity(uint64_t index, int is_data, uint64_t capabilities) {
    if (!identities) return resolve_verified(names[index], is_data, capabilities);
    const ImportIdentity *id = &identities[index];
    const char *scope = NULL;
    /* Explicit contracts for the observed ABI identities. These aliases are
     * internal dispatch keys, never untrusted dump-local library IDs.
     */
    if (id->library_version == 1 && id->module_version == 257) {
        if (!strcmp(id->library, "libc") && !strcmp(id->module, "libc")) scope = "V#V";
        else if (!strcmp(id->library, "libkernel") && !strcmp(id->module, "libkernel")) scope = "U#H";
        else if (!strcmp(id->library, "libSceGnmDriver") && !strcmp(id->module, "libSceGnmDriver")) scope = "E#F";
        else if (!strcmp(id->library, "libSceLibcInternalExt") && !strcmp(id->module, "libSceLibcInternal")) scope = "heap-trace";
    }
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
    Sleep(10000);
    fputs("STOP: diagnostic timeout after 10 seconds\n", stderr);
    fflush(NULL);
    ExitProcess(124);
    return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--selftest-console")) return selftest_console();
    if (argc == 2 && !strcmp(argv[1], "--selftest-atexit")) return selftest_atexit();
    if (argc == 2 && !strcmp(argv[1], "--selftest-equeue")) return windows_equeue_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-mutex")) return windows_mutex_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-thread")) return windows_thread_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-memory")) return windows_memory_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-tls")) return windows_tls_selftest();
    if (argc == 2 && !strcmp(argv[1], "--selftest-heap-trace")) return selftest_heap_trace();
    if (argc != 2) {
        fputs("Usage: tlg-entry-probe.exe boot.bin\nDiagnostic only; no graphics or PS4 runtime.\n", stderr);
        return 1;
    }
    SYSTEM_INFO system; GetSystemInfo(&system); page_size = system.dwPageSize;
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE2", 8) && memcmp(magic, "TLGWIN01", 8) && memcmp(magic,"TLGWIN02",8)))
        fail("expected BBPROBE2, TLGWIN01 or TLGWIN02; Linux linked images are not supported");
    image_size = read64(f);
    uint64_t entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = read64(f);
    int linked=!memcmp(magic,"TLGWIN02",8);
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
    if (!AddVectoredExceptionHandler(1, fault)) fail("exception handler unavailable");
    HANDLE timer = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (!timer) fail("watchdog unavailable");
    CloseHandle(timer);
    if (linked) {
        process_parameter=image+procparam;
        if (windows_tls_attach(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]?main_tls[3]:16))
            fail("main Windows TLS initialization failed");
        for (uint64_t i=0;i<native_count;++i) {
            NativeModule *m=&native_modules[i];
            if (m->tls_module && windows_tls_set_module(m->tls_module,image+m->tls_address,m->tls_filesz,m->tls_memsz))
                fail("native module TLS configuration failed");
        }
        for (uint64_t i=0;i<native_count;++i) {
            printf("Starting native module %" PRIu64 " at guest offset 0x%" PRIx64 "\n",i,native_modules[i].init);
            typedef int (ABI *Initialize)(uint64_t,void *,void *);
            int result=((Initialize)(image+native_modules[i].init))(0,NULL,NULL);
            printf("Native module %" PRIu64 " returned %d\n",i,result);
            if (result) fail("native module initializer failed");
        }
    }
    printf("Windows entry diagnostic: mapped %" PRIu64 " bytes; %" PRIu64 " relocations\n", image_size, nr);
    puts(linked ? "Windows native modules and TLS enabled; no GPU integration." :
                  "Startup contracts enabled; no native modules, GPU or TLS rewrite.");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
    fail("entry unexpectedly returned");
}

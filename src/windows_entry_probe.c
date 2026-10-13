/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows x86-64 entry diagnostic. One verified libc contract; no native modules,
 * GPU or game patches.
 * Accepts only unlinked BBPROBE2 images, never Linux FS-to-GS linked images.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#if !defined(__x86_64__) || !defined(__clang__)
#error Build with x86-64 Clang (SysV guest ABI required).
#endif
#define ABI __attribute__((sysv_abi))
typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static unsigned char *image, *data_traps;
static char (*names)[128];
static uint64_t image_size, import_count;
static size_t page_size;
static uint64_t init_env_calls;

static ABI void init_env_ret(void) {
    ++init_env_calls;
    puts("Runtime: _init_env returned (prepared libc export verified as RET)");
}
static uintptr_t resolve_verified(const char *name, int is_data, uint64_t capabilities) {
    /* prepare_tlg.py validates this target and its libc hash; prepare.py sets
     * bit 0 only after inspecting the supplied libc's _init_env as exactly C3.
     * Match the observed full scope, never an arbitrary same-NID import.
     */
    if (!is_data && (capabilities & 1) && !strcmp(name, "bzQExy189ZI#V#V"))
        return (uintptr_t)init_env_ret;
    return 0;
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
    printf("Caller offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, first_arg);
    printf("Verified _init_env calls: %" PRIu64 "\n", init_env_calls);
    puts("Original entry reached an unsupported import; game initialization is incomplete.");
    fflush(NULL);
    ExitProcess(20);
}
static ABI __attribute__((noreturn)) void guest_exit(void) { ExitProcess(24); }
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
    if (argc != 2) {
        fputs("Usage: tlg-entry-probe.exe boot.bin\nDiagnostic only; no graphics or PS4 runtime.\n", stderr);
        return 1;
    }
    SYSTEM_INFO system; GetSystemInfo(&system); page_size = system.dwPageSize;
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "BBPROBE2", 8))
        fail("expected unlinked BBPROBE2; Linux linked images are not supported");
    image_size = read64(f);
    uint64_t entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = read64(f);
    if (!image_size || image_size > 512 * 1024 * 1024 || entry >= image_size ||
        !ns || ns > 64 || nr > 1000000 || import_count > 100000 || capabilities > 1)
        fail("boot file limits exceeded");
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
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        r->target = read64(f); r->kind = read64(f); r->value = read64(f); r->addend = read64(f);
        if (!mapped(segments, ns, r->target, 8) || r->kind > 2 ||
            (r->kind && r->value >= import_count) ||
            (r->kind != 2 && r->addend) || r->addend >= page_size) fail("invalid relocation");
    }
    image = allocate(rounded((size_t)image_size));
    if (fread(image, 1, (size_t)image_size, f) != image_size || fgetc(f) != EOF) fail("incorrect image size");
    fclose(f);
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
            uintptr_t resolved = resolve_verified(names[r->value], r->kind == 2, capabilities);
            if (resolved) value = resolved + r->addend;
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
    printf("Windows entry diagnostic: mapped %" PRIu64 " bytes; %" PRIu64 " relocations\n", image_size, nr);
    puts("Only verified _init_env RET enabled; no native modules, GPU, TLS rewrite or game patches.");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
    fail("entry unexpectedly returned");
}

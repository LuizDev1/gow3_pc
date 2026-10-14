/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows static TLS and per-thread dynamic-module blocks via __tls_get_addr.
 * Opaque guest thread identities are stored in the TCB; direct-DTV access is incomplete.
 * TEB TLS slot addressing follows the existing GoW Windows runtime approach.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include "windows_tls.h"
static INIT_ONCE initialized = INIT_ONCE_STATIC_INIT;
static DWORD slot = TLS_OUT_OF_INDEXES;
static _Thread_local void *allocation;
static _Thread_local uint64_t dtv[3];
static struct { const void *data; uint64_t filesz, memsz; } modules[16];
static _Thread_local void *module_blocks[16];
static _Thread_local uint64_t main_size;
int windows_tls_set_module(uint64_t id, const void *data, uint64_t filesz, uint64_t memsz) {
    /* Configure templates on the loader thread before starting guest threads. */
    if (id < 2 || id >= 16 || modules[id].memsz || !memsz || memsz > 1024*1024 ||
        filesz > memsz || (filesz && !data)) return -1;
    modules[id].data=data; modules[id].filesz=filesz; modules[id].memsz=memsz;
    return 0;
}
void * __attribute__((sysv_abi)) windows_tls_get_addr(const uint64_t *index) {
    if (!index || !allocation || index[0] >= 16 || !index[0]) return NULL;
    uint64_t id=index[0], offset=index[1];
    if (id == 1) return offset < main_size ? (unsigned char *)allocation+offset : NULL;
    if (offset >= modules[id].memsz) return NULL;
    if (!module_blocks[id]) {
        module_blocks[id]=calloc(1,(size_t)modules[id].memsz);
        if (!module_blocks[id]) return NULL;
        if (modules[id].filesz) memcpy(module_blocks[id],modules[id].data,(size_t)modules[id].filesz);
    }
    return (unsigned char *)module_blocks[id]+offset;
}
static BOOL CALLBACK allocate_slot(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    DWORD candidate = TlsAlloc();
    if (candidate >= 64) {
        if (candidate != TLS_OUT_OF_INDEXES) TlsFree(candidate);
        return FALSE;
    }
    slot = candidate;
    return TRUE;
}
int windows_tls_slot_offset(uint32_t *offset) {
    if (!offset || !InitOnceExecuteOnce(&initialized, allocate_slot, NULL, NULL)) return -1;
    *offset = 0x1480 + 8 * slot;
    return 0;
}
int windows_tls_attach(const void *data, uint64_t filesz, uint64_t memsz, uint64_t alignment) {
    uint32_t offset;
    if (allocation || filesz > memsz || memsz > 1024 * 1024 || (filesz && !data) ||
        !alignment || (alignment & (alignment - 1)) || alignment > 65536 ||
        windows_tls_slot_offset(&offset)) return -1;
    size_t aligned = (size_t)((memsz + alignment - 1) & ~(alignment - 1));
    void *block = _aligned_malloc(aligned + 256, alignment < 64 ? 64 : (size_t)alignment);
    if (!block) return -1;
    memset(block, 0, aligned + 256);
    if (filesz) memcpy(block, data, (size_t)filesz);
    uint64_t *tcb = (uint64_t *)((unsigned char *)block + aligned);
    tcb[0] = (uintptr_t)tcb;
    tcb[1] = (uintptr_t)dtv;
    /* Opaque record lifetime matches this TLS attachment. The first field is
     * its TCB, matching the reference runtime; further layout is private. */
    uint64_t *record=tcb+8;
    record[0]=(uintptr_t)tcb;
    record[1]=GetCurrentThreadId();
    tcb[2]=(uintptr_t)record;
    /* Dynamic-module DTV entries remain unimplemented. */
    if (!TlsSetValue(slot, tcb)) { _aligned_free(block); return -1; }
    allocation = block;
    main_size = memsz;
    if(getenv("TLG_TRACE_SYNC")) fprintf(stderr,
        "TLS attach: block=%p bytes=%llu tcb=%p record=%p host=%lu\n",
        block,(unsigned long long)(aligned+256),(void *)tcb,(void *)record,(unsigned long)GetCurrentThreadId());
    return 0;
}
static struct { const void *data; uint64_t filesz,memsz,alignment; int configured; } main_template;
int windows_tls_configure_main(const void *data,uint64_t filesz,uint64_t memsz,uint64_t alignment) {
    if(main_template.configured || filesz>memsz || memsz>1024*1024 || (filesz && !data) ||
        !alignment || (alignment&(alignment-1)) || alignment>65536) return -1;
    main_template.data=data;main_template.filesz=filesz;main_template.memsz=memsz;
    main_template.alignment=alignment;main_template.configured=1;return 0;
}
int windows_tls_attach_main(void) {
    return !main_template.configured?-1:windows_tls_attach(main_template.data,main_template.filesz,main_template.memsz,main_template.alignment);
}
int windows_tls_bind_thread(void *record) {
    if(!allocation || !record) return -1;
    uint64_t *tcb=TlsGetValue(slot);if(!tcb) return -1;
    *(uintptr_t *)record=(uintptr_t)tcb;tcb[2]=(uintptr_t)record;return 0;
}
void *windows_tls_current_thread(void) {
    if (!allocation) return NULL;
    uint64_t *tcb=TlsGetValue(slot);
    return tcb ? (void *)(uintptr_t)tcb[2] : NULL;
}
void windows_tls_detach(void) {
    if (!allocation) return;
    if (!TlsSetValue(slot, NULL)) return; /* Never free memory while the slot still refers to it. */
    _aligned_free(allocation);
    allocation = NULL;
    main_size = 0;
    for (unsigned i=2;i<16;++i) { free(module_blocks[i]); module_blocks[i]=NULL; }
    memset(dtv, 0, sizeof(dtv));
}
typedef void *(__attribute__((sysv_abi)) *ReadTcb)(void);
typedef struct { ReadTcb read; void *main_tcb; void *main_identity; int result; } Test;
static DWORD WINAPI isolated_thread(void *argument) {
    Test *test = argument;
    uint64_t value = 77;
    if (windows_tls_attach(&value, 8, 16, 16)) { test->result = 1; return 0; }
    unsigned char *tcb = test->read();
    test->result = !tcb || tcb == test->main_tcb || *(uint64_t *)tcb != (uintptr_t)tcb ||
                   *(uint64_t *)(tcb - 16) != value;
    void *identity=windows_tls_current_thread();
    if (!identity || identity!=windows_tls_current_thread() || identity==test->main_identity ||
        (uintptr_t)identity!=((uint64_t *)tcb)[2] || *(uintptr_t *)identity!=(uintptr_t)tcb) test->result=1;
    uint64_t index[]={2,0};
    uint64_t *dynamic=windows_tls_get_addr(index);
    if (!dynamic || *dynamic != 99) test->result=1;
    if (dynamic) *dynamic=123;
    windows_tls_detach();
    return 0;
}
int windows_tls_selftest(void) {
    uint32_t offset;
    uint64_t value = 42;
    static const uint64_t template=99;
    if (windows_tls_set_module(2,&template,8,16)) return 1;
    if (windows_tls_slot_offset(&offset) || windows_tls_attach(&value, 8, 16, 3) == 0 ||
        windows_tls_attach(NULL, 8, 16, 16) == 0 || windows_tls_attach(&value, 8, 16, 16)) return 1;
    unsigned char instruction[] = {0x65,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
    memcpy(instruction + 5, &offset, 4);
    void *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!code) return 1;
    memcpy(code, instruction, sizeof(instruction));
    DWORD previous;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), code, sizeof(instruction))) return 1;
    ReadTcb read = (ReadTcb)code;
    uint64_t index[]={2,0};
    uint64_t *dynamic=windows_tls_get_addr(index);
    if (!dynamic || *dynamic != 99) return 1;
    unsigned char *tcb = read();
    if (!tcb || *(uint64_t *)tcb != (uintptr_t)tcb || *(uint64_t *)(tcb - 16) != value ||
        *(uint64_t *)(tcb - 8) != 0 || windows_tls_attach(NULL, 0, 0, 16) == 0) return 1;
    void *identity=windows_tls_current_thread();
    if (!identity || identity!=windows_tls_current_thread() || (uintptr_t)identity!=((uint64_t *)tcb)[2] ||
        *(uintptr_t *)identity!=(uintptr_t)tcb) return 1;
    Test test = {read, tcb, identity, 0};
    HANDLE thread = CreateThread(NULL, 0, isolated_thread, &test, 0, NULL);
    if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || test.result || read() != tcb || *dynamic != 99) return 1;
    CloseHandle(thread);
    windows_tls_detach();
    if (read() != NULL || windows_tls_current_thread() != NULL) return 1;
    VirtualFree(code, 0, MEM_RELEASE);
    puts("Thread identity: stable self, TCB agreement, host-thread isolation and detach passed");
    puts("Windows TLS: actual GS slot read, template, zero-fill, thread isolation and detach passed");
    return 0;
}

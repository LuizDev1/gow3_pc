/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows static-TLS foundation. No dynamic-module TLS or guest thread record yet.
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
    /* tcb_thread and dynamic-module DTV entries remain unimplemented. */
    if (!TlsSetValue(slot, tcb)) { _aligned_free(block); return -1; }
    allocation = block;
    return 0;
}
void windows_tls_detach(void) {
    if (!allocation) return;
    if (!TlsSetValue(slot, NULL)) return; /* Never free memory while the slot still refers to it. */
    _aligned_free(allocation);
    allocation = NULL;
    memset(dtv, 0, sizeof(dtv));
}
typedef void *(__attribute__((sysv_abi)) *ReadTcb)(void);
typedef struct { ReadTcb read; void *main_tcb; int result; } Test;
static DWORD WINAPI isolated_thread(void *argument) {
    Test *test = argument;
    uint64_t value = 77;
    if (windows_tls_attach(&value, 8, 16, 16)) { test->result = 1; return 0; }
    unsigned char *tcb = test->read();
    test->result = !tcb || tcb == test->main_tcb || *(uint64_t *)tcb != (uintptr_t)tcb ||
                   *(uint64_t *)(tcb - 16) != value;
    windows_tls_detach();
    return 0;
}
int windows_tls_selftest(void) {
    uint32_t offset;
    uint64_t value = 42;
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
    unsigned char *tcb = read();
    if (!tcb || *(uint64_t *)tcb != (uintptr_t)tcb || *(uint64_t *)(tcb - 16) != value ||
        *(uint64_t *)(tcb - 8) != 0 || windows_tls_attach(NULL, 0, 0, 16) == 0) return 1;
    Test test = {read, tcb, 0};
    HANDLE thread = CreateThread(NULL, 0, isolated_thread, &test, 0, NULL);
    if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || test.result || read() != tcb) return 1;
    CloseHandle(thread);
    windows_tls_detach();
    if (read() != NULL) return 1;
    VirtualFree(code, 0, MEM_RELEASE);
    puts("Windows TLS: actual GS slot read, template, zero-fill, thread isolation and detach passed");
    return 0;
}

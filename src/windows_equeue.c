/* SPDX-License-Identifier: GPL-2.0-or-later
 * Event queue lifecycle only. Event registration/waiting remain unresolved.
 * Guest handle width and errors follow the inherited kernel equeue interface.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "windows_equeue.h"
#define ABI __attribute__((sysv_abi))
#define ERR_INVALID ((int32_t)UINT32_C(0x80020016))
#define ERR_BAD_HANDLE ((int32_t)UINT32_C(0x80020009))
#define ERR_NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ERR_LONG_NAME ((int32_t)UINT32_C(0x8002003f))
typedef struct Queue {
    int64_t handle;
    char name[33];
    struct Queue *next;
} Queue;
static SRWLOCK registry_lock = SRWLOCK_INIT;
static Queue *queues;
static int64_t next_handle = 0x40000;
static unsigned count;

static ABI int32_t create_queue(int64_t *out, const char *name) {
    if (!out || !name) return ERR_INVALID;
    size_t length = 0;
    while (length <= 32 && name[length]) ++length;
    if (length > 32) return ERR_LONG_NAME;
    Queue *q = calloc(1, sizeof(*q));
    if (!q) return ERR_NO_MEMORY;
    memcpy(q->name, name, length);
    AcquireSRWLockExclusive(&registry_lock);
    if (count >= 4096 || next_handle == INT64_MAX) {
        ReleaseSRWLockExclusive(&registry_lock);
        free(q);
        return ERR_NO_MEMORY;
    }
    q->handle = next_handle++;
    q->next = queues;
    queues = q;
    ++count;
    int64_t handle = q->handle;
    ReleaseSRWLockExclusive(&registry_lock);
    *out = handle;
    printf("Runtime: event queue created: %s (handle 0x%" PRIx64 ")\n", name, (uint64_t)handle);
    return 0;
}
static ABI int32_t delete_queue(int64_t handle) {
    AcquireSRWLockExclusive(&registry_lock);
    Queue **at = &queues;
    while (*at && (*at)->handle != handle) at = &(*at)->next;
    if (!*at) { ReleaseSRWLockExclusive(&registry_lock); return ERR_BAD_HANDLE; }
    Queue *q = *at;
    *at = q->next;
    --count;
    ReleaseSRWLockExclusive(&registry_lock);
    free(q);
    return 0;
}
uintptr_t windows_equeue_resolve(const char *name) {
    if (!strcmp(name, "D0OdFMjp46I#U#H")) return (uintptr_t)create_queue;
    if (!strcmp(name, "jpFjmgAC5AE#U#H")) return (uintptr_t)delete_queue;
    return 0;
}
int windows_equeue_selftest(void) {
    int64_t a = -1, b = -1;
    if (create_queue(NULL, "test") != ERR_INVALID || create_queue(&a, NULL) != ERR_INVALID ||
        create_queue(&a, "12345678901234567890123456789012345") != ERR_LONG_NAME || a != -1)
        return 1;
    if (create_queue(&a, "first") || create_queue(&b, "second") || a == b || count != 2) return 1;
    if (delete_queue(-1) != ERR_BAD_HANDLE || delete_queue(a) ||
        delete_queue(a) != ERR_BAD_HANDLE || delete_queue(b) || count) return 1;
    if (windows_equeue_resolve("D0OdFMjp46I#p#J") ||
        windows_equeue_resolve("fzyMKs9kim0#U#H")) return 1;
    puts("equeue: lifecycle, invalid handles, invalid inputs and exact scope checks passed");
    return 0;
}

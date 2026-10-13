/* SPDX-License-Identifier: GPL-2.0-or-later
 * Queue lifecycle and GPU event subscriptions. Waiting remains unresolved.
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
typedef struct GpuEvent {
    uint64_t id, pending;
    void *userdata;
    struct GpuEvent *next;
} GpuEvent;
typedef struct Queue {
    int64_t handle;
    char name[33];
    GpuEvent *events;
    unsigned event_count;
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
    while (q->events) {
        GpuEvent *event = q->events;
        q->events = event->next;
        free(event);
    }
    free(q);
    return 0;
}
static Queue *find_queue(int64_t handle) {
    for (Queue *q = queues; q; q = q->next) if (q->handle == handle) return q;
    return NULL;
}
static ABI int32_t add_gpu_event(int64_t handle, uint64_t id, void *userdata) {
    AcquireSRWLockExclusive(&registry_lock);
    Queue *q = find_queue(handle);
    if (!q) { ReleaseSRWLockExclusive(&registry_lock); return ERR_BAD_HANDLE; }
    GpuEvent *event = q->events;
    while (event && event->id != id) event = event->next;
    if (!event) {
        if (q->event_count >= 4096) { ReleaseSRWLockExclusive(&registry_lock); return ERR_NO_MEMORY; }
        event = calloc(1, sizeof(*event));
        if (!event) { ReleaseSRWLockExclusive(&registry_lock); return ERR_NO_MEMORY; }
        event->id = id;
        event->next = q->events;
        q->events = event;
        ++q->event_count;
    }
    event->userdata = userdata;
    event->pending = 0;
    ReleaseSRWLockExclusive(&registry_lock);
    printf("Runtime: GPU event subscribed: queue 0x%" PRIx64 ", id 0x%" PRIx64 "\n", (uint64_t)handle, id);
    return 0;
}
static ABI int32_t delete_gpu_event(int64_t handle, uint64_t id) {
    AcquireSRWLockExclusive(&registry_lock);
    Queue *q = find_queue(handle);
    if (!q) { ReleaseSRWLockExclusive(&registry_lock); return ERR_BAD_HANDLE; }
    GpuEvent **at = &q->events;
    while (*at && (*at)->id != id) at = &(*at)->next;
    if (*at) {
        GpuEvent *event = *at;
        *at = event->next;
        --q->event_count;
        free(event);
    }
    ReleaseSRWLockExclusive(&registry_lock);
    return 0;
}
int windows_equeue_signal_gpu(int64_t handle, uint64_t id) {
    /* Future renderer completion hook. Registration alone never signals events. */
    AcquireSRWLockExclusive(&registry_lock);
    Queue *q = find_queue(handle);
    GpuEvent *event = q ? q->events : NULL;
    while (event && event->id != id) event = event->next;
    if (event && event->pending != UINT64_MAX) ++event->pending;
    int found = event != NULL;
    ReleaseSRWLockExclusive(&registry_lock);
    return found;
}
uintptr_t windows_equeue_resolve(const char *name) {
    if (!strcmp(name, "D0OdFMjp46I#U#H")) return (uintptr_t)create_queue;
    if (!strcmp(name, "jpFjmgAC5AE#U#H")) return (uintptr_t)delete_queue;
    if (!strcmp(name, "b0xyllnVY-I#E#F")) return (uintptr_t)add_gpu_event;
    if (!strcmp(name, "PVT+fuoS9gU#E#F")) return (uintptr_t)delete_gpu_event;
    return 0;
}
int windows_equeue_selftest(void) {
    int64_t a = -1, b = -1;
    if (create_queue(NULL, "test") != ERR_INVALID || create_queue(&a, NULL) != ERR_INVALID ||
        create_queue(&a, "12345678901234567890123456789012345") != ERR_LONG_NAME || a != -1)
        return 1;
    if (create_queue(&a, "first") || create_queue(&b, "second") || a == b || count != 2) return 1;
    if (add_gpu_event(-1, 7, NULL) != ERR_BAD_HANDLE || add_gpu_event(a, 7, (void *)1)) return 1;
    Queue *q = find_queue(a); /* Single-threaded test; production accesses hold the lock. */
    if (!q || q->event_count != 1 || q->events->pending || q->events->userdata != (void *)1) return 1;
    if (!windows_equeue_signal_gpu(a, 7) || q->events->pending != 1 ||
        windows_equeue_signal_gpu(a, 8) || windows_equeue_signal_gpu(b, 7)) return 1;
    if (add_gpu_event(a, 7, (void *)2) || q->event_count != 1 || q->events->pending ||
        q->events->userdata != (void *)2) return 1;
    if (delete_gpu_event(-1, 7) != ERR_BAD_HANDLE || delete_gpu_event(a, 7) ||
        q->event_count || windows_equeue_signal_gpu(a, 7)) return 1;
    if (add_gpu_event(b, 8, NULL)) return 1; /* Deleting the queue must remove its subscription. */
    if (delete_queue(-1) != ERR_BAD_HANDLE || delete_queue(a) ||
        delete_queue(a) != ERR_BAD_HANDLE || delete_queue(b) || count) return 1;
    if (windows_equeue_signal_gpu(b, 8) || windows_equeue_resolve("b0xyllnVY-I#wrong#scope")) return 1;
    if (windows_equeue_resolve("D0OdFMjp46I#p#J") ||
        windows_equeue_resolve("fzyMKs9kim0#U#H")) return 1;
    puts("equeue: lifecycle, invalid handles, invalid inputs and exact scope checks passed");
    puts("GPU subscriptions: replacement, isolation, explicit signaling and deletion checks passed");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest mutex attributes and locks. Priority scheduling remains unresolved.
 * The guest passes pointers to opaque handles, as in the inherited runtime.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "windows_mutex.h"
#define ABI __attribute__((sysv_abi))
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define BUSY ((int32_t)UINT32_C(0x80020010))
#define DEADLOCK ((int32_t)UINT32_C(0x8002000b))
#define NOT_OWNER ((int32_t)UINT32_C(0x80020001))
#define AGAIN ((int32_t)UINT32_C(0x80020023))
typedef struct Attr { int type; struct Attr *next; } Attr;
static Attr *attributes;
static SRWLOCK attr_lock = SRWLOCK_INIT;
typedef struct Mutex {
    int type;
    DWORD owner;
    uint32_t depth, waiters;
    CONDITION_VARIABLE changed;
    struct Mutex *next;
} Mutex;
static Mutex *mutexes;
static Mutex **find_mutex(Mutex *handle) {
    Mutex **at = &mutexes;
    while (*at && *at != handle) at = &(*at)->next;
    return at;
}
static Attr **find_attr(Attr *handle) {
    Attr **at = &attributes;
    while (*at && *at != handle) at = &(*at)->next;
    return at;
}
static ABI int32_t attr_init(Attr **out) {
    if (!out) return INVALID;
    Attr *attr = malloc(sizeof(*attr));
    if (!attr) return NO_MEMORY;
    attr->type = 1;
    AcquireSRWLockExclusive(&attr_lock);
    attr->next = attributes;
    attributes = attr;
    ReleaseSRWLockExclusive(&attr_lock);
    *out = attr;
    return 0;
}
static ABI int32_t attr_type(Attr **handle, int type) {
    if (!handle || type < 1 || type > 4) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr *attr = *find_attr(*handle);
    if (attr) attr->type = type;
    ReleaseSRWLockExclusive(&attr_lock);
    return attr ? 0 : INVALID;
}
static ABI int32_t attr_destroy(Attr **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr **at = find_attr(*handle);
    Attr *attr = *at;
    if (attr) { *at = attr->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&attr_lock);
    if (!attr) return INVALID;
    free(attr);
    return 0;
}
static ABI int32_t mutex_init(Mutex **out, Attr **attr, const char *name) {
    (void)name;
    if (!out) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Attr *a = attr ? *find_attr(*attr) : NULL;
    if (attr && !a) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    Mutex *m = calloc(1, sizeof(*m));
    if (!m) { ReleaseSRWLockExclusive(&attr_lock); return NO_MEMORY; }
    m->type = a ? a->type : 1;
    InitializeConditionVariable(&m->changed);
    m->next = mutexes;
    mutexes = m;
    *out = m;
    ReleaseSRWLockExclusive(&attr_lock);
    return 0;
}
static int32_t acquire_mutex(Mutex **handle, int try_only) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Mutex *m = *find_mutex(*handle);
    if (!m) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    DWORD caller = GetCurrentThreadId();
    if (m->owner == caller && m->type == 2) {
        int32_t result = m->depth == UINT32_MAX ? AGAIN : 0;
        if (!result) ++m->depth;
        ReleaseSRWLockExclusive(&attr_lock);
        return result;
    }
    if (m->owner && (try_only || (m->owner == caller && m->type != 3))) {
        int32_t result = try_only ? BUSY : DEADLOCK;
        ReleaseSRWLockExclusive(&attr_lock);
        return result;
    }
    while (m->owner) {
        ++m->waiters;
        BOOL ok = SleepConditionVariableSRW(&m->changed, &attr_lock, INFINITE, 0);
        --m->waiters;
        if (!ok) { ReleaseSRWLockExclusive(&attr_lock); return INVALID; }
    }
    m->owner = caller;
    m->depth = 1;
    ReleaseSRWLockExclusive(&attr_lock);
    return 0;
}
static ABI int32_t mutex_lock(Mutex **m) { return acquire_mutex(m, 0); }
static ABI int32_t mutex_trylock(Mutex **m) { return acquire_mutex(m, 1); }
static ABI int32_t mutex_unlock(Mutex **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Mutex *m = *find_mutex(*handle);
    int32_t result = !m ? INVALID : m->owner != GetCurrentThreadId() ? NOT_OWNER : 0;
    if (!result && !--m->depth) {
        m->owner = 0;
        WakeConditionVariable(&m->changed);
    }
    ReleaseSRWLockExclusive(&attr_lock);
    return result;
}
static ABI int32_t mutex_destroy(Mutex **handle) {
    if (!handle) return INVALID;
    AcquireSRWLockExclusive(&attr_lock);
    Mutex **at = find_mutex(*handle);
    Mutex *m = *at;
    int32_t result = !m ? INVALID : (m->owner || m->waiters) ? BUSY : 0;
    if (!result) { *at = m->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&attr_lock);
    if (!result) free(m);
    return result;
}
uintptr_t windows_mutex_resolve(const char *name) {
    if (!strcmp(name, "F8bUHwAG284#U#H")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#U#H")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#U#H")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "cmo1RIYva9o#U#H")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#U#H")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#U#H")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#U#H")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#U#H")) return (uintptr_t)mutex_destroy;
    return 0;
}
typedef struct { Mutex **mutex; HANDLE attempted; int result; } ContentionTest;
static DWORD WINAPI contention_worker(void *arg) {
    ContentionTest *test = arg;
    test->result = mutex_trylock(test->mutex) != BUSY || mutex_unlock(test->mutex) != NOT_OWNER;
    SetEvent(test->attempted);
    if (!test->result) test->result = mutex_lock(test->mutex) || mutex_unlock(test->mutex);
    return 0;
}
int windows_mutex_selftest(void) {
    Attr *a = NULL, *b = NULL;
    if (attr_init(NULL) != INVALID || attr_init(&a) || attr_init(&b) || a == b || a->type != 1) return 1;
    if (attr_type(&a, 2) || a->type != 2 || b->type != 1 ||
        attr_type(&a, 0) != INVALID || attr_type(&a, 5) != INVALID || a->type != 2) return 1;
    Attr *stale = a, *unknown = (Attr *)(uintptr_t)1;
    if (attr_destroy(&a) || a || attr_type(&stale, 1) != INVALID ||
        attr_destroy(&stale) != INVALID || attr_type(&unknown, 1) != INVALID ||
        attr_destroy(NULL) != INVALID || attr_destroy(&b) || attributes) return 1;
    if (windows_mutex_resolve("F8bUHwAG284#p#J")) return 1;
    puts("mutex attributes: defaults, type validation, isolation and stale handles passed");
    Mutex *m = NULL;
    if (mutex_init(&m, NULL, "test") || mutex_lock(&m) || mutex_lock(&m) != DEADLOCK ||
        mutex_trylock(&m) != BUSY || mutex_destroy(&m) != BUSY) return 1;
    ContentionTest test = {&m, CreateEventW(NULL, TRUE, FALSE, NULL), 0};
    if (!test.attempted) return 1;
    HANDLE thread = CreateThread(NULL, 0, contention_worker, &test, 0, NULL);
    if (!thread || WaitForSingleObject(test.attempted, 5000) != WAIT_OBJECT_0 || mutex_unlock(&m) ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || test.result) return 1;
    CloseHandle(thread); CloseHandle(test.attempted);
    Mutex *stale_mutex = m;
    if (mutex_destroy(&m) || m || mutex_lock(&stale_mutex) != INVALID || mutex_unlock(&m) != INVALID) return 1;
    if (attr_init(&a) || attr_type(&a, 2) || mutex_init(&m, &a, "recursive") || attr_destroy(&a) ||
        mutex_lock(&m) || mutex_lock(&m) || mutex_unlock(&m) || mutex_destroy(&m) != BUSY ||
        mutex_unlock(&m) || mutex_destroy(&m)) return 1;
    puts("mutex locks: ownership, recursion, contention, busy destruction and stale handles passed");
    return 0;
}

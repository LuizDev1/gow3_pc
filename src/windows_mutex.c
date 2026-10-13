/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest mutex attributes only; locking and priority scheduling remain unresolved.
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
typedef struct Attr { int type; struct Attr *next; } Attr;
static Attr *attributes;
static SRWLOCK attr_lock = SRWLOCK_INIT;
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
uintptr_t windows_mutex_resolve(const char *name) {
    if (!strcmp(name, "F8bUHwAG284#U#H")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#U#H")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#U#H")) return (uintptr_t)attr_destroy;
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
    return 0;
}

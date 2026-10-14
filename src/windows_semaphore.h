#pragma once
#include <stdint.h>
uintptr_t windows_semaphore_resolve(const char *name);
int windows_semaphore_selftest(void);
int windows_eventflag_selftest(void);

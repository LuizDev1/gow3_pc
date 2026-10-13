#pragma once
#include <stdint.h>
uintptr_t windows_thread_resolve(const char *name);
int windows_thread_selftest(void);

int windows_thread_create_selftest(void);

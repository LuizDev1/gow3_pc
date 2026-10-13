/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TLG_WINDOWS_MUTEX_H
#define TLG_WINDOWS_MUTEX_H
#include <stdint.h>
uintptr_t windows_mutex_resolve(const char *name);
int windows_mutex_selftest(void);
#endif

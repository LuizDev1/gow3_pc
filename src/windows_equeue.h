/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TLG_WINDOWS_EQUEUE_H
#define TLG_WINDOWS_EQUEUE_H
#include <stdint.h>
uintptr_t windows_equeue_resolve(const char *name);
int windows_equeue_selftest(void);
int windows_equeue_signal_gpu(int64_t handle, uint64_t id);
#endif

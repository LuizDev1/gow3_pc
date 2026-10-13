/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <windows.h>
#include <stdint.h>
typedef unsigned long long TlgRecovery[32];
extern _Thread_local TlgRecovery *runtime_fault_recover;
extern uint64_t runtime_disabled_optimizations;
__attribute__((returns_twice)) int gow3_setjmp(TlgRecovery buffer);
int windows_renderer_recover(EXCEPTION_POINTERS *exception);
int windows_renderer_recovery_selftest(void);

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TLG_WINDOWS_TLS_H
#define TLG_WINDOWS_TLS_H
#include <stdint.h>
int windows_tls_selftest(void);
int windows_tls_slot_offset(uint32_t *offset);
int windows_tls_attach(const void *data, uint64_t filesz, uint64_t memsz, uint64_t alignment);
void windows_tls_detach(void);
int windows_tls_set_module(uint64_t id, const void *data, uint64_t filesz, uint64_t memsz);
void * __attribute__((sysv_abi)) windows_tls_get_addr(const uint64_t *index);
#endif

#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uintptr_t windows_memory_resolve(const char *name);
int windows_memory_selftest(void);
/* Host ABI callbacks for tracked direct/flexible views only. Image/stack regions
 * and GPU protection/backing services are not connected yet. Queries do not pin
 * storage against concurrent unmapping. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
#ifdef __cplusplus
}
#endif

#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uintptr_t windows_memory_resolve(const char *name);
int windows_memory_selftest(void);
/* Host ABI callbacks. Queries do not pin storage against concurrent unmapping. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
typedef void (*WindowsGpuRange)(uintptr_t address, uint64_t size);
void runtime_memory_set_gpu_hooks(WindowsGpuRange map, WindowsGpuRange unmap, WindowsGpuRange invalidate);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
int windows_memory_register_external(void *address, uint64_t size, int protection);
int windows_memory_register_current_stack(void);
void windows_memory_unregister_current_stack(void);
#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif
namespace BbHost {
// Diagnostic memory reads must report unreadable ranges, rather than faulting.
inline bool ReadMemory(void* destination,const void* source,std::size_t size) {
#ifdef _WIN32
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(),source,destination,size,&copied) && copied==size;
#else
    iovec local{destination,size},remote{const_cast<void*>(source),size};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==static_cast<ssize_t>(size);
#endif
}
}

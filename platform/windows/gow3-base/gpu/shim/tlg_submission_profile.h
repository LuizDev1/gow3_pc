// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>

namespace TlgSubmissionProfile {
// Keep native Vulkan recording/copies synchronous until the GoW optimizations
// are validated for TLG's queue ordering. Explicit experimental overrides remain
// available; unrelated titles retain their inherited defaults.
inline constexpr const char* options[] = {"GOW3_VK_RECORD_THREAD", "GOW3_COPY_THREADS"};
inline int Apply(const char* serial) {
    if(!serial || std::strcmp(serial,"CUSA03627")) return 0;
    for(const char* name : options) {
        const char* value=std::getenv(name);
        if(value && *value) continue;
#ifdef _WIN32
        if(_putenv_s(name,"0")) return 1;
#else
        if(setenv(name,"0",1)) return 1;
#endif
    }
    return 0;
}
}

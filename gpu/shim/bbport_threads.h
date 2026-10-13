// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {
inline unsigned CurrentId() {
#ifdef _WIN32
    return static_cast<unsigned>(GetCurrentThreadId());
#else
    return static_cast<unsigned>(gettid());
#endif
}


/// Hardware threads available to the process.
inline unsigned Available() {
#ifdef _WIN32
    DWORD_PTR process_mask=0,system_mask=0;
    if(GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask)) {
        unsigned count=0;
        for(;process_mask;process_mask>>=1) count+=static_cast<unsigned>(process_mask&1);
        return std::max(1u,count);
    }
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
#ifdef _WIN32
    // Windows has no SCHED_IDLE equivalent; this is a best-effort lower priority.
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
#else
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
#endif
}

} // namespace BbThreads

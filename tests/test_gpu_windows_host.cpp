// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shim/bbport_host_memory.h"
#include "gpu/shim/bbport_threads.h"
int main() {
    int source=42,destination=0;
    if(!BbHost::ReadMemory(&destination,&source,sizeof(source)) || destination!=42) return 1;
    if(BbHost::ReadMemory(&destination,reinterpret_cast<void*>(1),sizeof(destination))) return 1;
    if(!BbThreads::Available() || !BbThreads::CurrentId() || BbThreads::CurrentId()!=GetCurrentThreadId()) return 1;
    const unsigned flags=42;
    if(__atomic_load_n(&flags,__ATOMIC_ACQUIRE)!=42) return 1;
    BbThreads::MakeBackground();
    return 0;
}

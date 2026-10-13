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
    void* page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!page || !BbHost::ProtectMemory(page,4096,false)) return 1;
    if(BbHost::ReadMemory(&destination,page,sizeof(destination))) return 1;
    if(!BbHost::ProtectMemory(page,4096,true)) return 1;
    *static_cast<int*>(page)=77;
    if(!BbHost::ReadMemory(&destination,page,sizeof(destination)) || destination!=77) return 1;
    if(!VirtualFree(page,0,MEM_RELEASE)) return 1;
    if(BbHost::AllocateZeroed(0) || BbHost::FreeZeroed(nullptr,4096)) return 1;
    auto* bookkeeping=static_cast<unsigned char*>(BbHost::AllocateZeroed(32768));
    if(!bookkeeping) return 1;
    for(unsigned i=0;i<32768;++i) if(bookkeeping[i]) return 1;
    bookkeeping[0]=1;bookkeeping[32767]=2;
    if(!BbHost::FreeZeroed(bookkeeping,32768)) return 1;
    BbThreads::MakeBackground();
    return 0;
}

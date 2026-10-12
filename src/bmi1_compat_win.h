#ifndef GOW3_BMI1_COMPAT_WIN_H
#define GOW3_BMI1_COMPAT_WIN_H
#include <cpuid.h>
#include "bmi1_compat.h"

static int gow3_bmi1_available(void) {
    unsigned a,b,c,d;
    return __get_cpuid_count(7,0,&a,&b,&c,&d) && (b & (1u<<3));
}
static int gow3_bmi1_read(void *unused, uint64_t address, void *out, size_t size) {
    SIZE_T got=0; (void)unused;
    return ReadProcessMemory(GetCurrentProcess(),(const void *)(uintptr_t)address,
                             out,size,&got) && got==size;
}
static int gow3_bmi1_fault(EXCEPTION_POINTERS *info) {
    unsigned char code[15]; SIZE_T got=0;
    CONTEXT *c=info->ContextRecord;
    if (!ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)c->Rip,
                           code,sizeof(code),&got)) {
        /* A short valid instruction may end just before an unreadable page. */
        got=0;
        while (got<sizeof(code)) {
            SIZE_T one=0;
            if (!ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rip+got),
                                   code+got,1,&one) || one!=1) break;
            got++;
        }
    }
    if (got<5) return 0;
    Gow3BmiContext guest={
        {c->Rax,c->Rcx,c->Rdx,c->Rbx,c->Rsp,c->Rbp,c->Rsi,c->Rdi,
         c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15},c->Rip,c->EFlags};
    if (!gow3_bmi1_emulate(code,got,&guest,gow3_bmi1_read,NULL)) return 0;
    c->Rax=guest.r[0]; c->Rcx=guest.r[1]; c->Rdx=guest.r[2]; c->Rbx=guest.r[3];
    c->Rsp=guest.r[4]; c->Rbp=guest.r[5]; c->Rsi=guest.r[6]; c->Rdi=guest.r[7];
    c->R8=guest.r[8]; c->R9=guest.r[9]; c->R10=guest.r[10]; c->R11=guest.r[11];
    c->R12=guest.r[12]; c->R13=guest.r[13]; c->R14=guest.r[14]; c->R15=guest.r[15];
    c->Rip=guest.rip; c->EFlags=(DWORD)guest.flags;
    return 1;
}
#endif

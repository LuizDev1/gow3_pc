#ifndef GOW3_BMI1_COMPAT_WIN_H
#define GOW3_BMI1_COMPAT_WIN_H
#include <cpuid.h>
#include <stdio.h>
#include "bmi1_compat.h"

/* Per-thread history: never mix another worker's register state into a crash. */
typedef struct {
    Gow3BmiContext before,after;
    unsigned char code[15];
    unsigned size;
    int length;
} Gow3BmiTrace;
static __thread Gow3BmiTrace gow3_bmi_history[32];
static __thread uint64_t gow3_bmi_attempts;
static inline void gow3_bmi1_dump(uintptr_t image_base) {
    uint64_t count=gow3_bmi_attempts<32?gow3_bmi_attempts:32;
    fprintf(stderr,"BMI1 diagnostics v2: %llu attempts on faulting thread; last %llu follow\n",
            (unsigned long long)gow3_bmi_attempts,(unsigned long long)count);
    for(uint64_t i=gow3_bmi_attempts-count;i<gow3_bmi_attempts;i++) {
        const Gow3BmiTrace *t=&gow3_bmi_history[i%32];
        fprintf(stderr,"BMI1 #%llu guest=0x%llx length=%d bytes=",
                (unsigned long long)i,(unsigned long long)(t->before.rip-image_base),t->length);
        for(unsigned j=0;j<t->size;j++) fprintf(stderr,"%02x",t->code[j]);
        fprintf(stderr," flags=%llx->%llx\n",(unsigned long long)t->before.flags,(unsigned long long)t->after.flags);
        for(unsigned j=0;j<16;j++)
            fprintf(stderr," r%u=%llx->%llx%s",j,(unsigned long long)t->before.r[j],
                    (unsigned long long)t->after.r[j],j%4==3?"\n":"");
    }
}

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
    Gow3BmiTrace *trace=&gow3_bmi_history[gow3_bmi_attempts++%32];
    trace->before=guest;trace->size=(unsigned)got;
    memcpy(trace->code,code,got);
    trace->length=gow3_bmi1_emulate(code,got,&guest,gow3_bmi1_read,NULL);
    trace->after=guest;
    if (!trace->length) return 0;
    c->Rax=guest.r[0]; c->Rcx=guest.r[1]; c->Rdx=guest.r[2]; c->Rbx=guest.r[3];
    c->Rsp=guest.r[4]; c->Rbp=guest.r[5]; c->Rsi=guest.r[6]; c->Rdi=guest.r[7];
    c->R8=guest.r[8]; c->R9=guest.r[9]; c->R10=guest.r[10]; c->R11=guest.r[11];
    c->R12=guest.r[12]; c->R13=guest.r[13]; c->R14=guest.r[14]; c->R15=guest.r[15];
    c->Rip=guest.rip; c->EFlags=(DWORD)guest.flags;
    return 1;
}
#endif

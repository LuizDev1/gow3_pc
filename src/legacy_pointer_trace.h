#ifndef GOW3_LEGACY_POINTER_TRACE_H
#define GOW3_LEGACY_POINTER_TRACE_H
/* Diagnostic only: bounded, per-thread stepping inside the failing routine.
 * Never install software breakpoints or change guest register values. */
typedef struct {
    DWORD64 rip, rdx, r12, rsp, slots[3];
    unsigned char bytes[15];
    unsigned char readable;
} Gow3PointerTrace;
static __thread Gow3PointerTrace gow3_pointer_history[256];
static __thread unsigned gow3_pointer_count, gow3_pointer_steps;
static __thread int gow3_pointer_active;
static void gow3_pointer_capture(CONTEXT *c) {
    Gow3PointerTrace *t=&gow3_pointer_history[gow3_pointer_count++%256];
    memset(t,0,sizeof(*t));
    t->rip=c->Rip; t->rdx=c->Rdx; t->r12=c->R12; t->rsp=c->Rsp;
    SIZE_T got=0;
    ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)c->Rip,t->bytes,15,&got);
    t->readable=(unsigned char)got;
    /* The routine saves/restores pointers in the SysV red zone. */
    ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rsp-24),t->slots,sizeof(t->slots),&got);
}
static void gow3_pointer_begin(CONTEXT *c, uintptr_t base) {
    if (!gow3_pointer_active && gow3_pointer_steps<20000 && !(c->EFlags&0x100) &&
        c->Rip-base>=0x18ed00 && c->Rip-base<0x18f100) {
        gow3_pointer_active=1;
        gow3_pointer_capture(c);
        c->EFlags|=0x100;
    }
}
static int gow3_pointer_step(EXCEPTION_POINTERS *info, uintptr_t base) {
    if (info->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP || !gow3_pointer_active) return 0;
    CONTEXT *c=info->ContextRecord;
    gow3_pointer_capture(c);
    ++gow3_pointer_steps;
    if (gow3_pointer_steps>=20000 || c->Rip-base<0x18ed00 || c->Rip-base>=0x18f100) {
        gow3_pointer_active=0;
        c->EFlags&=~(DWORD)0x100;
    } else c->EFlags|=0x100;
    return 1;
}
static void gow3_pointer_dump(uintptr_t base) {
    unsigned count=gow3_pointer_count<256?gow3_pointer_count:256;
    fprintf(stderr,"Pointer diagnostics v3: steps=%u; last %u states (before next instruction)\n",gow3_pointer_steps,count);
    for(unsigned i=gow3_pointer_count-count;i<gow3_pointer_count;i++) {
        const Gow3PointerTrace *t=&gow3_pointer_history[i%256];
        fprintf(stderr,"PTR #%u guest=%llx rdx=%llx r12=%llx rsp=%llx stack[-24,-16,-8]=%llx,%llx,%llx bytes=",
            i,(unsigned long long)(t->rip-base),(unsigned long long)t->rdx,
            (unsigned long long)t->r12,(unsigned long long)t->rsp,
            (unsigned long long)t->slots[0],(unsigned long long)t->slots[1],(unsigned long long)t->slots[2]);
        for(unsigned j=0;j<t->readable;j++) fprintf(stderr,"%02x",t->bytes[j]);
        fputc('\n',stderr);
    }
}
#endif

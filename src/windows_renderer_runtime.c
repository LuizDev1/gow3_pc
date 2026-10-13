/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows recovery assembly adapted from the user-provided GoW3 Windows source,
 * compatibility-source/src/compat_win.c, commit
 * 97f1f8d4410ac8ad4fde29d29041f06e3af51b66 (GPL-2.0).
 * No game loader, executable hooks or game addresses are used here. */
#include "windows_renderer_runtime.h"
#include <stdio.h>
#include <stdlib.h>
_Thread_local TlgRecovery *runtime_fault_recover;
uint64_t runtime_disabled_optimizations;
void gow3_longjmp(TlgRecovery buffer,int result) __attribute__((noreturn));

/* Win64 nonvolatile integer/vector registers, post-return stack, control words. */
__asm__(".text\n"
        ".globl gow3_setjmp\n"
        ".def gow3_setjmp; .scl 2; .type 32; .endef\n"
        "gow3_setjmp:\n"
        "mov %rbx,0(%rcx)\n mov %rbp,8(%rcx)\n mov %rdi,16(%rcx)\n mov %rsi,24(%rcx)\n"
        "mov %r12,32(%rcx)\n mov %r13,40(%rcx)\n mov %r14,48(%rcx)\n mov %r15,56(%rcx)\n"
        "lea 8(%rsp),%rdx\n mov %rdx,64(%rcx)\n mov (%rsp),%rdx\n mov %rdx,72(%rcx)\n"
        "movups %xmm6,80(%rcx)\n movups %xmm7,96(%rcx)\n movups %xmm8,112(%rcx)\n"
        "movups %xmm9,128(%rcx)\n movups %xmm10,144(%rcx)\n movups %xmm11,160(%rcx)\n"
        "movups %xmm12,176(%rcx)\n movups %xmm13,192(%rcx)\n movups %xmm14,208(%rcx)\n"
        "movups %xmm15,224(%rcx)\n stmxcsr 240(%rcx)\n fnstcw 244(%rcx)\n"
        "xor %eax,%eax\n ret\n"
        ".globl gow3_longjmp\n"
        ".def gow3_longjmp; .scl 2; .type 32; .endef\n"
        "gow3_longjmp:\n"
        "mov %edx,%eax\n test %eax,%eax\n jnz 1f\n mov $1,%eax\n"
        "1: mov 0(%rcx),%rbx\n mov 8(%rcx),%rbp\n mov 16(%rcx),%rdi\n mov 24(%rcx),%rsi\n"
        "mov 32(%rcx),%r12\n mov 40(%rcx),%r13\n mov 48(%rcx),%r14\n mov 56(%rcx),%r15\n"
        "movups 80(%rcx),%xmm6\n movups 96(%rcx),%xmm7\n movups 112(%rcx),%xmm8\n"
        "movups 128(%rcx),%xmm9\n movups 144(%rcx),%xmm10\n movups 160(%rcx),%xmm11\n"
        "movups 176(%rcx),%xmm12\n movups 192(%rcx),%xmm13\n movups 208(%rcx),%xmm14\n"
        "movups 224(%rcx),%xmm15\n ldmxcsr 240(%rcx)\n fldcw 244(%rcx)\n"
        "mov 64(%rcx),%rsp\n jmp *72(%rcx)\n");

int windows_renderer_recover(EXCEPTION_POINTERS *exception) {
    if(!runtime_fault_recover || exception->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters<2 || exception->ExceptionRecord->ExceptionInformation[0]==8)
        return 0;
    /* Resume in a known Win64 landing point; never unwind through guest frames. */
    exception->ContextRecord->Rip=(DWORD64)(uintptr_t)gow3_longjmp;
    exception->ContextRecord->Rcx=(DWORD64)(uintptr_t)*runtime_fault_recover;
    exception->ContextRecord->Rdx=1;
    runtime_fault_recover=NULL;
    return 1;
}
static LONG WINAPI recover_test(EXCEPTION_POINTERS *exception) {
    return windows_renderer_recover(exception)?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;
}
int windows_renderer_recovery_selftest(void) {
    TlgRecovery recovery;
    void *page=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
    void *handler=AddVectoredExceptionHandler(1,recover_test);
    if(!page || !handler) return 1;
    int resumed=gow3_setjmp(recovery);
    if(!resumed) {
        runtime_fault_recover=&recovery;
        (void)*(volatile unsigned char *)page;
        abort();
    }
    if(resumed!=1 || runtime_fault_recover) return 1;
    RemoveVectoredExceptionHandler(handler);VirtualFree(page,0,MEM_RELEASE);
    puts("Renderer fault recovery: protected read resumed at Win64 landing point passed");
    return 0;
}

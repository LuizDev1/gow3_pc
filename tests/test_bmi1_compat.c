#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../src/bmi1_compat.h"
#ifdef _WIN32
#include <windows.h>
#include "../src/bmi1_compat_win.h"
#endif

static int memory_read(void *user, uint64_t address, void *out, size_t size) {
    if (address!=0x1000 || size>8) return 0;
    memcpy(out,user,size); return 1;
}
static uint64_t random64(uint64_t *seed) {
    *seed^=*seed<<13; *seed^=*seed>>7; *seed^=*seed<<17; return *seed;
}
int main(void) {
    const unsigned char instructions[][5]={
        {0xc4,0xe2,0x78,0xf2,0xc1}, /* andn eax,eax,ecx */
        {0xc4,0xe2,0x78,0xf7,0xc6}, /* bextr eax,esi,eax */
        {0xc4,0xe2,0x78,0xf3,0xdb}, /* blsi eax,ebx */
        {0xc4,0xe2,0x48,0xf3,0xca}, /* blsr esi,edx */
        {0xc4,0xe2,0x78,0xf3,0xd3}  /* blsmsk eax,ebx */
    };
    uint64_t seed=3770; unsigned checks=0;
    for (unsigned wide=0;wide<2;wide++) {
        unsigned bits=wide?64:32; uint64_t mask=wide?UINT64_MAX:UINT32_MAX;
        for (unsigned op=0;op<5;op++) for (unsigned trial=0;trial<10000;trial++) {
            unsigned char code[5]; memcpy(code,instructions[op],5);
            if (wide) code[2]|=128;
            Gow3BmiContext c={{0},0x2000,0xad7};
            for (unsigned i=0;i<16;i++) c.r[i]=random64(&seed);
            uint64_t value=trial<4?(trial==0?0:trial==1?1:trial==2?mask:UINT64_C(1)<<(bits-1)):random64(&seed)&mask;
            unsigned dst=op==3?6:0; uint64_t expected=0, carry=0;
            if (op==0) { c.r[1]=value; expected=(~c.r[0])&value&mask; }
            else if (op==1) {
                c.r[6]=value;
                /* Include start/length zero, width, out-of-range and 255. */
                unsigned start=trial<4?trial*bits:(unsigned)random64(&seed)&255;
                unsigned count=trial<4?255:(unsigned)random64(&seed)&255;
                c.r[0]=start|(count<<8);
                for (unsigned i=0;i<count && start+i<bits;i++) expected|=((value>>(start+i))&1)<<i;
            } else if (op==2) { c.r[3]=value; expected=value&(~value+1); carry=value!=0; }
            else if (op==3) { c.r[2]=value; expected=value&(value-1); carry=value==0; }
            else { c.r[3]=value; expected=(value^(value-1))&mask; carry=value==0; }
            Gow3BmiContext before=c;
            assert(gow3_bmi1_emulate(code,5,&c,NULL,NULL)==5);
            assert(c.r[dst]==expected && c.rip==0x2005);
            for (unsigned i=0;i<16;i++) if (i!=dst) assert(c.r[i]==before.r[i]);
            uint64_t flags=(before.flags&~UINT64_C(0x8c1))|carry;
            if (!expected) flags|=0x40;
            if (expected&(UINT64_C(1)<<(bits-1))) flags|=0x80;
            assert(c.flags==flags); checks++;
        }
    }
    const unsigned char memory[]={0xc4,0xe2,0x70,0xf7,0x80,0xc4,1,0,0};
    Gow3BmiContext c={{0},0x2000,0x202}; c.r[0]=0x1000-0x1c4; c.r[1]=0x0804;
    uint64_t value=0xabcdef;
    assert(gow3_bmi1_emulate(memory,sizeof(memory),&c,memory_read,&value)==9);
    assert(c.r[0]==0xde && c.rip==0x2009);
    const unsigned char sib[]={0xc4,0x82,0x70,0xf7,0x44,0x88,0x08};
    memset(&c,0,sizeof(c)); c.rip=0x2000; c.r[8]=0x1000-8-12;
    c.r[9]=3; c.r[1]=0x0804;
    assert(gow3_bmi1_emulate(sib,sizeof(sib),&c,memory_read,&value)==7);
    assert(c.r[0]==0xde && c.rip==0x2007);
    const unsigned char relative[]={0xc4,0xe2,0xf0,0xf7,0x05,0xf7,0x0f,0,0};
    memset(&c,0,sizeof(c)); c.r[1]=0x0804;
    assert(gow3_bmi1_emulate(relative,sizeof(relative),&c,memory_read,&value)==9);
    assert(c.r[0]==0xde && c.rip==9);
    c.r[0]=0; Gow3BmiContext before=c;
    assert(!gow3_bmi1_emulate(memory,sizeof(memory),&c,memory_read,&value));
    assert(!memcmp(&c,&before,sizeof(c)));
#ifdef _WIN32
    /* Exercise the exact Windows adapter even on a BMI1-capable test machine. */
    assert(gow3_bmi1_available()==0 || gow3_bmi1_available()==1);
    CONTEXT win={0}; win.Rax=0xff; win.Rcx=0xff00; win.EFlags=0xad7;
    win.Rip=(DWORD64)(uintptr_t)instructions[0];
    EXCEPTION_RECORD record={0}; record.ExceptionCode=EXCEPTION_ILLEGAL_INSTRUCTION;
    EXCEPTION_POINTERS info={&record,&win};
    assert(gow3_bmi1_fault(&info));
    assert(win.Rax==0xff00 && win.Rcx==0xff00);
    assert(win.Rip==(DWORD64)(uintptr_t)instructions[0]+5);
    SYSTEM_INFO system; GetSystemInfo(&system);
    unsigned char *pages=VirtualAlloc(NULL,system.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    assert(pages); DWORD previous;
    memcpy(pages+system.dwPageSize-5,instructions[0],5);
    assert(VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&previous));
    memset(&win,0,sizeof(win)); win.Rax=0xff; win.Rcx=0xff00;
    win.Rip=(DWORD64)(uintptr_t)(pages+system.dwPageSize-5);
    assert(gow3_bmi1_fault(&info));
    assert(win.Rax==0xff00 && win.Rip==(DWORD64)(uintptr_t)(pages+system.dwPageSize));
    assert(VirtualFree(pages,0,MEM_RELEASE));
#endif
    for (unsigned size=0;size<sizeof(memory);size++) {
        assert(!gow3_bmi1_emulate(memory,size,&c,memory_read,&value));
        assert(!memcmp(&c,&before,sizeof(c)));
    }
    const unsigned char unsupported[]={0xc4,0xe2,0x79,0xf7,0xc1};
    assert(!gow3_bmi1_emulate(unsupported,5,&c,NULL,NULL));
    assert(!memcmp(&c,&before,sizeof(c)));
    printf("PASS: %u BMI1 register/flag checks plus memory, truncation and unsupported-encoding checks\n",checks);
    return 0;
}

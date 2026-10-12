/* Scalar BMI1 fallback for Windows guest faults. No AVX2/BMI intrinsics required. */
#ifndef GOW3_BMI1_COMPAT_H
#define GOW3_BMI1_COMPAT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { uint64_t r[16], rip, flags; } Gow3BmiContext;
typedef int (*Gow3BmiRead)(void *, uint64_t, void *, size_t);

/* Register order is the x86 encoding order: ax,cx,dx,bx,sp,bp,si,di,r8..r15.
 * Unsupported encodings and unreadable operands leave the context untouched.
 * TZCNT is intentionally not handled: on pre-BMI CPUs it decodes as BSF and
 * does not raise an illegal-instruction exception. See CPU_COMPATIBILITY.md. */
static int gow3_bmi1_emulate(const unsigned char *code, size_t size,
                            Gow3BmiContext *context, Gow3BmiRead read, void *user) {
    if (size < 5 || code[0] != 0xc4) return 0;
    unsigned vex1=code[1], vex2=code[2], opcode=code[3], modrm=code[4];
    if ((vex1 & 31) != 2 || (vex2 & 7) != 0) return 0; /* 0F38, L=0, pp=0 */
    if (opcode != 0xf2 && opcode != 0xf3 && opcode != 0xf7) return 0;
    unsigned mod=modrm>>6, field=(modrm>>3)&7, rm=modrm&7;
    unsigned src1=(~vex2>>3)&15, dst=field+((~vex1>>7)&1)*8;
    unsigned width=(vex2&128)?64:32, length=5;
    uint64_t mask=width==64?UINT64_MAX:UINT32_MAX;
    uint64_t value, address=0;
    int rip_relative=0;
    if (opcode==0xf3) {
        if ((vex1&128)==0 || (field!=1 && field!=2 && field!=3)) return 0;
        dst=src1;
    }
    if (mod==3) value=context->r[rm+((~vex1>>5)&1)*8]&mask;
    else {
        int displacement_size=mod==1?1:mod==2?4:0;
        if (rm==4) {
            if (length>=size) return 0;
            unsigned sib=code[length++], base=sib&7, index=(sib>>3)&7;
            unsigned index_reg=index+((~vex1>>6)&1)*8;
            if (index_reg!=4) address+=context->r[index_reg]<<(sib>>6);
            if (mod==0 && base==5) displacement_size=4;
            else address+=context->r[base+((~vex1>>5)&1)*8];
        } else if (mod==0 && rm==5) {
            displacement_size=4; rip_relative=1;
        } else address=context->r[rm+((~vex1>>5)&1)*8];
        if (length+(unsigned)displacement_size>size) return 0;
        int64_t displacement=0;
        if (displacement_size==1) displacement=(int8_t)code[length];
        else if (displacement_size==4) {
            int32_t d; memcpy(&d,code+length,4); displacement=d;
        }
        length+=(unsigned)displacement_size;
        address+=(uint64_t)displacement;
        if (rip_relative) address+=context->rip+length;
        value=0;
        if (!read || !read(user,address,&value,width/8)) return 0;
        value&=mask;
    }
    uint64_t result=0, carry=0;
    if (opcode==0xf2) result=(~context->r[src1])&value&mask; /* ANDN */
    else if (opcode==0xf7) { /* BEXTR: start and length are unsigned bytes. */
        unsigned start=(unsigned)context->r[src1]&255;
        unsigned count=(unsigned)(context->r[src1]>>8)&255;
        if (start<width && count) {
            unsigned remaining=width-start;
            if (count>remaining) count=remaining;
            result=value>>start;
            if (count<64) result&=(UINT64_C(1)<<count)-1;
        }
    } else if (field==1) { result=(value&(value-1))&mask; carry=value==0; } /* BLSR */
    else if (field==2) { result=(value^(value-1))&mask; carry=value==0; } /* BLSMSK */
    else { result=(value&(~value+1))&mask; carry=value!=0; } /* BLSI */
    /* CF/OF are defined for all these operations; ZF/SF are defined except
     * SF for BEXTR. AF/PF are architecturally undefined and left unchanged. */
    uint64_t flags=context->flags&~UINT64_C(0x8c1); /* OF,SF,ZF,CF */
    flags|=carry;
    if (result==0) flags|=0x40;
    if (result&(UINT64_C(1)<<(width-1))) flags|=0x80;
    context->r[dst]=result; /* 32-bit destinations zero-extend. */
    context->flags=flags;
    context->rip+=length;
    return (int)length;
}
#endif

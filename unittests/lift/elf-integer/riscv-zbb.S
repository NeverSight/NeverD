// Preserve each Zbb result in its own XLEN-sized slot for an ABI caller.
.text
.globl eval
.type eval,@function
eval:
.set offset,0
.macro save op, args:vararg
 \op t0,\args
#if __riscv_xlen == 64
 sd t0,offset(a0)
#else
 sw t0,offset(a0)
#endif
 .set offset,offset+__riscv_xlen/8
.endm
 save andn,a1,a2
 save orn,a1,a2
 save xnor,a1,a2
 save min,a1,a2
 save minu,a1,a2
 save max,a1,a2
 save maxu,a1,a2
 save rol,a1,a2
 save ror,a1,a2
 save rori,a1,__riscv_xlen-1
 save clz,a1
 save ctz,a1
 save cpop,a1
 save sext.b,a1
 save sext.h,a1
 save zext.h,a1
 save orc.b,a1
 save rev8,a1
#if __riscv_xlen == 64
 save rolw,a1,a2
 save rorw,a1,a2
 save roriw,a1,31
 save clzw,a1
 save ctzw,a1
 save cpopw,a1
#endif
 mv a0,a1
 ret
.size eval,.-eval

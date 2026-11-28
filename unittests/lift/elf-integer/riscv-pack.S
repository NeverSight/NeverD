// Keep both source orders, zero inputs and the XLEN-specific zext.h alias.
.text
.globl eval
.type eval,@function
eval:
.set offset,0
.macro save op, left, right
 \op t0,\left,\right
#if __riscv_xlen == 64
 sd t0,offset(a0)
#else
 sw t0,offset(a0)
#endif
 .set offset,offset+__riscv_xlen/8
.endm
 pack zero,a1,a2
 save pack,a1,a2
 save pack,a2,a1
 save packh,a1,a2
 save pack,a1,zero
 save packh,zero,a2
#if __riscv_xlen == 64
 save packw,a1,a2
 save packw,a1,zero
#endif
 mv a0,a1
 ret
.size eval,.-eval

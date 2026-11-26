// Exercise each Zba and Zbs operation through a scalar ABI call.
.text
.globl eval
.type eval,@function
eval:
 sh1add t0,a0,a1
 sh2add t1,a1,t0
 sh3add t2,a0,t1
#if __riscv_xlen == 64
 add.uw t0,t0,t2
 sh1add.uw t1,a0,t0
 sh2add.uw t0,a1,t1
 sh3add.uw t2,t2,t0
 slli.uw t0,a0,33
 xor t2,t2,t0
#endif
 bset t0,t2,a1
 bclri t1,t0,__riscv_xlen-1
 binv t1,t1,a0
 bext t0,t2,a1
 bseti t0,t0,__riscv_xlen-1
 bclr t0,t0,a0
 binvi t0,t0,3
 bexti t2,t2,__riscv_xlen-1
 add a0,t1,t0
 xor a0,a0,t2
 ret
.size eval,.-eval

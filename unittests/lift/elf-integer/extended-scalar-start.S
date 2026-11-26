// Linux ELF entry for the freestanding oracle.
.text
.globl _start
.type _start,@function
#ifdef __riscv
_start:
 call check
 li a7,93
 ecall
#elif defined(__powerpc64__)
.abiversion 2
_start:
 lis 2,.TOC.@ha
 addi 2,2,.TOC.@l
 bl check
 li 0,1
 sc
#else
_start:
 bl check
 li 0,1
 sc
#endif
.size _start,.-_start

// Linux ELF entry for the freestanding oracle.
#include "scalar-elf-function.inc"
.text
begin_function _start
#ifdef __riscv
 call check
 li a7,93
 ecall
#else
 lis 2,.TOC.@ha
 addi 2,2,.TOC.@l
 bl check
 li 0,1
 sc
#endif
end_function _start

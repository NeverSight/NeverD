.set noreorder
EXIT_ISA
.section .text.start,"ax",@progbits
.globl finish
.ent finish
finish:
  addiu $2, $zero, EXIT_NR
  syscall
.end finish

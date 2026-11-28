# A jump through an absolute table with a constant index, as the Go 1.20 and
# 1.21 runtimes compile one after inlining (`mov $5, %ecx; lea table(%rip),
# %rdx; jmp *(%rdx,%rcx,8)`), linked as a static executable that runs at its
# link address.  The table's sixth word is the address the jump goes to, so
# the jump is a branch to that one label; the other labels stay unreached.

  .text
  .globl fixed_const_index
  .type fixed_const_index, @function
  .p2align 4
fixed_const_index:
  movl $5, %ecx
  leaq fixed_table(%rip), %rdx
  jmpq *(%rdx,%rcx,8)
.Lf0:
  movl $10, %eax
  retq
.Lf1:
  movl $11, %eax
  retq
.Lf2:
  movl $12, %eax
  retq
.Lf3:
  movl $13, %eax
  retq
.Lf4:
  movl $14, %eax
  retq
.Lf5:
  movl $15, %eax
  retq
.Lf6:
  movl $16, %eax
  retq
.Lf7:
  movl $17, %eax
  retq
  .size fixed_const_index, .-fixed_const_index

  .globl _start
  .type _start, @function
  .p2align 4
_start:
  callq fixed_const_index
  movl %eax, %edi
  movl $60, %eax
  syscall
  hlt
  .size _start, .-_start

  .section .rodata
  .globl fixed_table
  .type fixed_table, @object
  .p2align 3
fixed_table:
  .quad .Lf0, .Lf1, .Lf2, .Lf3, .Lf4, .Lf5, .Lf6, .Lf7
  .size fixed_table, .-fixed_table

  .section .note.GNU-stack,"",@progbits

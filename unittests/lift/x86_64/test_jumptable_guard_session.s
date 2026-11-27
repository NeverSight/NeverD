# A dispatch behind a long compare chain, the shape of pcre2's pattern
# converter: 24 controlling compares test a value that grows by a
# multiply and an add between them, and only the last compare,
# `cmpl $5, %ecx; ja` right after `movzbl (%rdi), %ecx`, bounds the
# index.  One query session over every condition runs out of its match
# allowance; the compare's own queries still decide the six slots.  The
# table ends with a zero word, which is no entry.

  .text
  .globl jt_many_guards
  .type jt_many_guards, @function
  .p2align 4
jt_many_guards:
  movl %esi, %r8d
  cmpl $7, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $1, %r8d
  cmpl $8, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $2, %r8d
  cmpl $9, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $3, %r8d
  cmpl $10, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $4, %r8d
  cmpl $11, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $5, %r8d
  cmpl $12, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $6, %r8d
  cmpl $13, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $7, %r8d
  cmpl $14, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $8, %r8d
  cmpl $15, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $9, %r8d
  cmpl $16, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $10, %r8d
  cmpl $17, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $11, %r8d
  cmpl $18, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $12, %r8d
  cmpl $19, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $13, %r8d
  cmpl $20, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $14, %r8d
  cmpl $21, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $15, %r8d
  cmpl $22, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $16, %r8d
  cmpl $23, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $17, %r8d
  cmpl $24, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $18, %r8d
  cmpl $25, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $19, %r8d
  cmpl $26, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $20, %r8d
  cmpl $27, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $21, %r8d
  cmpl $28, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $22, %r8d
  cmpl $29, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $23, %r8d
  cmpl $30, %r8d
  je .Lmg_exit
  imull $3, %r8d, %r8d
  addl $24, %r8d
  movzbl (%rdi), %ecx
  cmpl $5, %ecx
  ja .Lmg_exit
  leaq .Lmg_table(%rip), %rdx
  movslq (%rdx,%rcx,4), %rcx
  addq %rdx, %rcx
  jmpq *%rcx
.Lmg_c0:
  movl $10, %eax
  ret
.Lmg_c1:
  movl $11, %eax
  ret
.Lmg_c2:
  movl $12, %eax
  ret
.Lmg_c3:
  movl $13, %eax
  ret
.Lmg_c4:
  movl $14, %eax
  ret
.Lmg_c5:
  movl $15, %eax
  ret
.Lmg_exit:
  movl %r8d, %eax
  ret
  .size jt_many_guards, .-jt_many_guards

  .section .rodata
  .p2align 2
.Lmg_table:
  .long .Lmg_c0-.Lmg_table
  .long .Lmg_c1-.Lmg_table
  .long .Lmg_c2-.Lmg_table
  .long .Lmg_c3-.Lmg_table
  .long .Lmg_c4-.Lmg_table
  .long .Lmg_c5-.Lmg_table
  .long 0

  .section .note.GNU-stack,"",@progbits

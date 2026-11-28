# A dispatch on a structure field, as liblzma's LZMA decoder does:
# `cmpl $3, 0x10(%rdi); ja` bounds the field, `movl 0x10(%rdi), %ecx` reloads
# it into RCX through the implicit zero-extend, and the table load's own
# instruction scales RCX (`movslq (%r9,%rcx,4)`).  One case also scales an
# unrelated value by four at the end of an alternating multiply/xor chain
# deeper than any value query follows.  jt_scale_after_extend takes the scale
# straight from the extended register, so that case, decoded once the table's
# targets are, does not decide its table.  jt_scale_after_overwrite writes
# CL between the reload and the table load, which makes the index 5: its
# dispatch may not be resolved.  Each table ends with a zero word, which is
# no entry.

  .text
  .globl jt_scale_after_extend
  .type jt_scale_after_extend, @function
  .p2align 4
jt_scale_after_extend:
  cmpl $3, 0x10(%rdi)
  ja .Lse_done
  movl 0x10(%rdi), %ecx
  leaq .Lse_table(%rip), %r9
  movslq (%r9,%rcx,4), %rcx
  addq %r9, %rcx
  jmpq *%rcx
.Lse_c0:
  movl $10, %eax
  ret
.Lse_c1:
  movl $11, %eax
  ret
.Lse_c2:
  movl $12, %eax
  ret
.Lse_c3:
  movl %esi, %eax
  .rept 160
  imull $3, %eax, %eax
  xorl $0x55, %eax
  .endr
  movl (%rdx,%rax,4), %eax
  ret
.Lse_done:
  xorl %eax, %eax
  ret
  .size jt_scale_after_extend, .-jt_scale_after_extend

  .globl jt_scale_after_overwrite
  .type jt_scale_after_overwrite, @function
  .p2align 4
jt_scale_after_overwrite:
  cmpl $3, 0x10(%rdi)
  ja .Lso_done
  movl 0x10(%rdi), %ecx
  movb $5, %cl
  leaq .Lso_table(%rip), %r9
  movslq (%r9,%rcx,4), %rcx
  addq %r9, %rcx
  jmpq *%rcx
.Lso_c0:
  movl $20, %eax
  ret
.Lso_c1:
  movl $21, %eax
  ret
.Lso_c2:
  movl $22, %eax
  ret
.Lso_c3:
  movl %esi, %eax
  .rept 160
  imull $3, %eax, %eax
  xorl $0x55, %eax
  .endr
  movl (%rdx,%rax,4), %eax
  ret
.Lso_done:
  xorl %eax, %eax
  ret
  .size jt_scale_after_overwrite, .-jt_scale_after_overwrite

  .section .rodata
  .p2align 2
.Lse_table:
  .long .Lse_c0-.Lse_table
  .long .Lse_c1-.Lse_table
  .long .Lse_c2-.Lse_table
  .long .Lse_c3-.Lse_table
  .long 0
.Lso_table:
  .long .Lso_c0-.Lso_table
  .long .Lso_c1-.Lso_table
  .long .Lso_c2-.Lso_table
  .long .Lso_c3-.Lso_table
  .long 0

  .section .note.GNU-stack,"",@progbits

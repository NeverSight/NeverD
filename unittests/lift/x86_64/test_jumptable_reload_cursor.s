# A loop dispatching on the byte under a cursor (GCC -O2 shape, as in
# pcre2's _pcre2_eclass): `cmpb $5, (%rbx); ja` bounds the byte, and the index
# reloads it with `movzbl (%rbx), %eax`.  The cases advance the cursor and one
# calls out, so the byte's value at the compare has no closed form; the two
# loads still read the same address, unchanged in between.
# jt_reload_shifted reloads the next byte instead (`movzbl 1(%rbx), %eax`),
# which the compare does not bound, and jt_reload_rebased replaces the cursor
# between the compare and the reload: neither dispatch may be resolved.

  .text
  .globl jt_reload_loop
  .type jt_reload_loop, @function
  .p2align 4
jt_reload_loop:
  pushq %rbx
  pushq %r12
  pushq %r13
  movq %rdi, %rbx
  movq %rsi, %r13
  xorl %eax, %eax
  leaq .Lrl_table(%rip), %r12
.Lrl_top:
  cmpb $5, (%rbx)
  ja .Lrl_done
  movzbl (%rbx), %eax
  movslq (%r12,%rax,4), %rax
  addq %r12, %rax
  jmpq *%rax
.Lrl_c0:
  addq $1, %rbx
  jmp .Lrl_top
.Lrl_c1:
  movq %rbx, %rdi
  callq reload_visit@PLT
  addq $2, %rbx
  jmp .Lrl_top
.Lrl_c2:
  addq $3, %rbx
  jmp .Lrl_top
.Lrl_c3:
  addq $4, %rbx
  jmp .Lrl_top
.Lrl_c4:
  addq $5, %rbx
  jmp .Lrl_top
.Lrl_c5:
  addq $6, %rbx
  jmp .Lrl_top
.Lrl_done:
  movq %rbx, %rax
  popq %r13
  popq %r12
  popq %rbx
  retq
  .size jt_reload_loop, .-jt_reload_loop

  .text
  .globl jt_reload_shifted
  .type jt_reload_shifted, @function
  .p2align 4
jt_reload_shifted:
  pushq %rbx
  pushq %r12
  pushq %r13
  movq %rdi, %rbx
  movq %rsi, %r13
  xorl %eax, %eax
  leaq .Lrs_table(%rip), %r12
.Lrs_top:
  cmpb $5, (%rbx)
  ja .Lrs_done
  movzbl 1(%rbx), %eax
  movslq (%r12,%rax,4), %rax
  addq %r12, %rax
  jmpq *%rax
.Lrs_c0:
  addq $1, %rbx
  jmp .Lrs_top
.Lrs_c1:
  movq %rbx, %rdi
  callq reload_visit@PLT
  addq $2, %rbx
  jmp .Lrs_top
.Lrs_c2:
  addq $3, %rbx
  jmp .Lrs_top
.Lrs_c3:
  addq $4, %rbx
  jmp .Lrs_top
.Lrs_c4:
  addq $5, %rbx
  jmp .Lrs_top
.Lrs_c5:
  addq $6, %rbx
  jmp .Lrs_top
.Lrs_done:
  movq %rbx, %rax
  popq %r13
  popq %r12
  popq %rbx
  retq
  .size jt_reload_shifted, .-jt_reload_shifted

  .text
  .globl jt_reload_rebased
  .type jt_reload_rebased, @function
  .p2align 4
jt_reload_rebased:
  pushq %rbx
  pushq %r12
  pushq %r13
  movq %rdi, %rbx
  movq %rsi, %r13
  xorl %eax, %eax
  leaq .Lrb_table(%rip), %r12
.Lrb_top:
  cmpb $5, (%rbx)
  ja .Lrb_done
  movq %r13, %rbx
  movzbl (%rbx), %eax
  movslq (%r12,%rax,4), %rax
  addq %r12, %rax
  jmpq *%rax
.Lrb_c0:
  addq $1, %rbx
  jmp .Lrb_top
.Lrb_c1:
  movq %rbx, %rdi
  callq reload_visit@PLT
  addq $2, %rbx
  jmp .Lrb_top
.Lrb_c2:
  addq $3, %rbx
  jmp .Lrb_top
.Lrb_c3:
  addq $4, %rbx
  jmp .Lrb_top
.Lrb_c4:
  addq $5, %rbx
  jmp .Lrb_top
.Lrb_c5:
  addq $6, %rbx
  jmp .Lrb_top
.Lrb_done:
  movq %rbx, %rax
  popq %r13
  popq %r12
  popq %rbx
  retq
  .size jt_reload_rebased, .-jt_reload_rebased

  .section .rodata,"a",@progbits
  .p2align 2
.Lrl_table:
  .long .Lrl_c0-.Lrl_table
  .long .Lrl_c1-.Lrl_table
  .long .Lrl_c2-.Lrl_table
  .long .Lrl_c3-.Lrl_table
  .long .Lrl_c4-.Lrl_table
  .long .Lrl_c5-.Lrl_table
  .long 0
.Lrs_table:
  .long .Lrs_c0-.Lrs_table
  .long .Lrs_c1-.Lrs_table
  .long .Lrs_c2-.Lrs_table
  .long .Lrs_c3-.Lrs_table
  .long .Lrs_c4-.Lrs_table
  .long .Lrs_c5-.Lrs_table
  .long 0
.Lrb_table:
  .long .Lrb_c0-.Lrb_table
  .long .Lrb_c1-.Lrb_table
  .long .Lrb_c2-.Lrb_table
  .long .Lrb_c3-.Lrb_table
  .long .Lrb_c4-.Lrb_table
  .long .Lrb_c5-.Lrb_table
  .long 0
  .section .note.GNU-stack,"",@progbits

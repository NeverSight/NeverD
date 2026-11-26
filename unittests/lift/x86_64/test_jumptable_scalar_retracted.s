# A state machine whose first state is a constant (zlib's inflateBack starts
# at `mode = TYPE`): before any case is explored, the only path into the
# dispatch computes slot 0, so exact scalar recovery first takes the jump as a
# direct branch.  The cases loop back with other states, which withdraws that
# guess, and the ordinary resolver then proves the guarded four-slot table.
# jt_scalar_unbounded loads its next state from memory and has no guard: its
# withdrawn guess leaves a transfer nothing bounds.  Each table ends with a
# zero word, which is no entry.

  .text
  .globl jt_scalar_retracted
  .type jt_scalar_retracted, @function
  .p2align 4
jt_scalar_retracted:
  movl $5, %eax
.Lsr_loop:
  leal -5(%rax), %ecx
  cmpl $3, %ecx
  ja .Lsr_done
  leaq .Lsr_table(%rip), %rdx
  movslq (%rdx,%rcx,4), %rcx
  addq %rdx, %rcx
  jmpq *%rcx
.Lsr_c0:
  movl $6, %eax
  jmp .Lsr_loop
.Lsr_c1:
  movl $7, %eax
  jmp .Lsr_loop
.Lsr_c2:
  movl $8, %eax
  jmp .Lsr_loop
.Lsr_c3:
  movl $1, %eax
  ret
.Lsr_done:
  xorl %eax, %eax
  ret
  .size jt_scalar_retracted, .-jt_scalar_retracted

  .globl jt_scalar_unbounded
  .type jt_scalar_unbounded, @function
  .p2align 4
jt_scalar_unbounded:
  movl $5, %eax
.Lsu_loop:
  leal -5(%rax), %ecx
  leaq .Lsu_table(%rip), %rdx
  movslq (%rdx,%rcx,4), %rcx
  addq %rdx, %rcx
  jmpq *%rcx
.Lsu_c0:
  movl (%rdi), %eax
  jmp .Lsu_loop
.Lsu_c1:
  movl $1, %eax
  ret
  .size jt_scalar_unbounded, .-jt_scalar_unbounded

  .section .rodata
  .p2align 2
.Lsr_table:
  .long .Lsr_c0-.Lsr_table
  .long .Lsr_c1-.Lsr_table
  .long .Lsr_c2-.Lsr_table
  .long .Lsr_c3-.Lsr_table
  .long 0
.Lsu_table:
  .long .Lsu_c0-.Lsu_table
  .long .Lsu_c1-.Lsu_table
  .long 0

  .section .note.GNU-stack,"",@progbits

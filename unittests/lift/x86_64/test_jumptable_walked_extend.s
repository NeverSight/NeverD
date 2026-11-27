# A compare of the index's 32-bit lane whose zero extension lies in an
# earlier block, behind a test of the full register (GCC -O2 in a routine of
# bash's command printer): `movl 0x18(%rdi), %eax` widens %eax into %rax, a
# block tests `(%eax - 4) & ~4` and branches away on 4 and 8, and the next
# block bounds %eax by ten before the dispatch indexes with %rax.  The
# compare names the index lane itself, so its bound wins over the full-width
# test that only cuts holes: jt_walked_extend reads its ten entries.
# jt_walked_two_preds enters the compare from a second path that loads all
# of %rax, jt_walked_call calls a function between the load and the compare,
# which leaves %rax whatever the callee returns, and .data holds the address
# of jt_walked_rooted's compare block, which that entry reaches with any
# %rax: none of those dispatches may be resolved.  Every table ends with a
# zero word, which is no entry.

  .text
  .type jt_walked_ext, @function
  .p2align 4
jt_walked_ext:
  movl (%rdi), %eax
  retq
  .size jt_walked_ext, .-jt_walked_ext

  .globl jt_walked_extend
  .type jt_walked_extend, @function
  .p2align 4
jt_walked_extend:
  leaq .Lwx_table(%rip), %rcx
  movl 0x18(%rdi), %eax
  leal -4(%rax), %edx
  andl $0xfffffffb, %edx
  je .Lwx_four_or_eight
  cmpl $9, %eax
  ja .Lwx_other
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  notrack jmpq *%rax
.Lwx_c0:
  movl $10, %eax
  retq
.Lwx_c1:
  movl $11, %eax
  retq
.Lwx_c2:
  movl $12, %eax
  retq
.Lwx_c3:
  movl $13, %eax
  retq
.Lwx_c5:
  movl $15, %eax
  retq
.Lwx_c6:
  movl $16, %eax
  retq
.Lwx_c7:
  movl $17, %eax
  retq
.Lwx_c9:
  movl $19, %eax
  retq
.Lwx_four_or_eight:
  movl $48, %eax
  retq
.Lwx_other:
  movl $-1, %eax
  retq
  .size jt_walked_extend, .-jt_walked_extend

  .globl jt_walked_two_preds
  .type jt_walked_two_preds, @function
  .p2align 4
jt_walked_two_preds:
  leaq .Lwt_table(%rip), %rcx
  testl %esi, %esi
  jne .Lwt_wide
  movl 0x18(%rdi), %eax
  leal -4(%rax), %edx
  andl $0xfffffffb, %edx
  je .Lwt_other
  jmp .Lwt_compare
.Lwt_wide:
  movq 0x20(%rdi), %rax
.Lwt_compare:
  cmpl $9, %eax
  ja .Lwt_other
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  notrack jmpq *%rax
.Lwt_c0:
  movl $20, %eax
  retq
.Lwt_c1:
  movl $21, %eax
  retq
.Lwt_other:
  movl $-1, %eax
  retq
  .size jt_walked_two_preds, .-jt_walked_two_preds

  .globl jt_walked_call
  .type jt_walked_call, @function
  .p2align 4
jt_walked_call:
  pushq %rbx
  pushq %r12
  pushq %rax
  leaq .Lwc_table(%rip), %rbx
  movq %rdi, %r12
  movl 0x18(%rdi), %eax
  testl %eax, %eax
  je .Lwc_other
  movq %r12, %rdi
  callq jt_walked_ext
  cmpl $9, %eax
  ja .Lwc_other
  movslq (%rbx,%rax,4), %rax
  addq %rbx, %rax
  popq %rcx
  popq %r12
  popq %rbx
  notrack jmpq *%rax
.Lwc_c0:
  movl $30, %eax
  retq
.Lwc_c1:
  movl $31, %eax
  retq
.Lwc_other:
  popq %rcx
  popq %r12
  popq %rbx
  movl $-1, %eax
  retq
  .size jt_walked_call, .-jt_walked_call

  .globl jt_walked_rooted
  .type jt_walked_rooted, @function
  .p2align 4
jt_walked_rooted:
  leaq .Lwr_table(%rip), %rcx
  movl 0x18(%rdi), %eax
  leal -4(%rax), %edx
  andl $0xfffffffb, %edx
  je .Lwr_four_or_eight
.Lwr_compare:
  cmpl $9, %eax
  ja .Lwr_other
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  notrack jmpq *%rax
.Lwr_c0:
  movl $40, %eax
  retq
.Lwr_c1:
  movl $41, %eax
  retq
.Lwr_four_or_eight:
  movl $48, %eax
  retq
.Lwr_other:
  movl $-1, %eax
  retq
  .size jt_walked_rooted, .-jt_walked_rooted

  .section .rodata
  .p2align 2
.Lwx_table:
  .long .Lwx_c0-.Lwx_table
  .long .Lwx_c1-.Lwx_table
  .long .Lwx_c2-.Lwx_table
  .long .Lwx_c3-.Lwx_table
  .long .Lwx_four_or_eight-.Lwx_table
  .long .Lwx_c5-.Lwx_table
  .long .Lwx_c6-.Lwx_table
  .long .Lwx_c7-.Lwx_table
  .long .Lwx_four_or_eight-.Lwx_table
  .long .Lwx_c9-.Lwx_table
  .long 0
.Lwt_table:
  .long .Lwt_c0-.Lwt_table
  .long .Lwt_c1-.Lwt_table
  .long .Lwt_c0-.Lwt_table
  .long .Lwt_c1-.Lwt_table
  .long .Lwt_c0-.Lwt_table
  .long .Lwt_c1-.Lwt_table
  .long .Lwt_c0-.Lwt_table
  .long .Lwt_c1-.Lwt_table
  .long .Lwt_c0-.Lwt_table
  .long .Lwt_c1-.Lwt_table
  .long 0
.Lwc_table:
  .long .Lwc_c0-.Lwc_table
  .long .Lwc_c1-.Lwc_table
  .long .Lwc_c0-.Lwc_table
  .long .Lwc_c1-.Lwc_table
  .long .Lwc_c0-.Lwc_table
  .long .Lwc_c1-.Lwc_table
  .long .Lwc_c0-.Lwc_table
  .long .Lwc_c1-.Lwc_table
  .long .Lwc_c0-.Lwc_table
  .long .Lwc_c1-.Lwc_table
  .long 0
.Lwr_table:
  .long .Lwr_c0-.Lwr_table
  .long .Lwr_c1-.Lwr_table
  .long .Lwr_c0-.Lwr_table
  .long .Lwr_c1-.Lwr_table
  .long .Lwr_four_or_eight-.Lwr_table
  .long .Lwr_c1-.Lwr_table
  .long .Lwr_c0-.Lwr_table
  .long .Lwr_c1-.Lwr_table
  .long .Lwr_four_or_eight-.Lwr_table
  .long .Lwr_c1-.Lwr_table
  .long 0

  .data
  .p2align 3
jt_walked_rooted_entry:
  .quad .Lwr_compare

  .section .note.GNU-stack,"",@progbits

# Indirect jumps whose stack frame is restored, live or unknown when they
# jump; the tests link and strip the object.  Restored, so a tail call:
# tf_leaf jumps with nothing pushed, tf_pop_frame after popping RBX and RBP,
# tf_leave after LEAVE, tf_aligned after LEAVE undoes a realignment,
# tf_add_back after adding its frame back, tf_diamond after a push and pop on
# one of two paths, tf_lea_restore after `lea -8(%rbp), %rsp` and two pops,
# and tf_loop_balanced after a loop that pushes and pops.  On another stack,
# so a tail call too: tf_switch takes RSP from an argument and pops the
# resumed state, as a fiber switch does, and tf_resume loads RSP from
# memory, as gogo<> and longjmp do.  Live or unknown, so the jump stays an
# unresolved branch: tf_live jumps inside its frame, tf_live_table through
# an unbounded relative table inside its frame, tf_join after a push on one
# of two paths, tf_aligned_lost after a realignment it does not undo,
# tf_dynamic after moving RSP by a register, tf_frame_lost after LEAVE reads
# an RBP a call result moved, and tf_loop_push after a loop that pushes.

  .text
  .globl tf_ext
  .type tf_ext, @function
  .p2align 4
tf_ext:
  retq
  .size tf_ext, .-tf_ext

  .globl tf_leaf
  .type tf_leaf, @function
  .p2align 4
tf_leaf:
  jmpq *%rdi
  .size tf_leaf, .-tf_leaf

  .globl tf_pop_frame
  .type tf_pop_frame, @function
  .p2align 4
tf_pop_frame:
  pushq %rbp
  movq %rsp, %rbp
  pushq %rbx
  movq %rdi, %rbx
  callq tf_ext
  movq %rbx, %rax
  popq %rbx
  popq %rbp
  jmpq *%rax
  .size tf_pop_frame, .-tf_pop_frame

  .globl tf_leave
  .type tf_leave, @function
  .p2align 4
tf_leave:
  pushq %rbp
  movq %rsp, %rbp
  subq $0x20, %rsp
  movq %rdi, -8(%rbp)
  callq tf_ext
  movq -8(%rbp), %rax
  leaveq
  jmpq *%rax
  .size tf_leave, .-tf_leave

  .globl tf_aligned
  .type tf_aligned, @function
  .p2align 4
tf_aligned:
  pushq %rbp
  movq %rsp, %rbp
  andq $-32, %rsp
  subq $0x40, %rsp
  movq %rdi, (%rsp)
  callq tf_ext
  movq (%rsp), %rax
  leaveq
  jmpq *%rax
  .size tf_aligned, .-tf_aligned

  .globl tf_add_back
  .type tf_add_back, @function
  .p2align 4
tf_add_back:
  subq $0x18, %rsp
  movq %rdi, 8(%rsp)
  callq tf_ext
  movq 8(%rsp), %rax
  addq $0x18, %rsp
  jmpq *%rax
  .size tf_add_back, .-tf_add_back

  .globl tf_diamond
  .type tf_diamond, @function
  .p2align 4
tf_diamond:
  testl %esi, %esi
  je 1f
  pushq %rbx
  movq %rdx, %rbx
  popq %rbx
1:
  jmpq *%rdi
  .size tf_diamond, .-tf_diamond

  .globl tf_lea_restore
  .type tf_lea_restore, @function
  .p2align 4
tf_lea_restore:
  pushq %rbp
  movq %rsp, %rbp
  pushq %r12
  subq $0x10, %rsp
  movq %rdi, %r12
  callq tf_ext
  movq %r12, %rax
  leaq -8(%rbp), %rsp
  popq %r12
  popq %rbp
  jmpq *%rax
  .size tf_lea_restore, .-tf_lea_restore

  .globl tf_loop_balanced
  .type tf_loop_balanced, @function
  .p2align 4
tf_loop_balanced:
  movl %esi, %ecx
1:
  pushq %rcx
  popq %rcx
  decl %ecx
  jnz 1b
  jmpq *%rdi
  .size tf_loop_balanced, .-tf_loop_balanced

  .globl tf_live
  .type tf_live, @function
  .p2align 4
tf_live:
  pushq %rbp
  movq %rsp, %rbp
  movq %rdi, %rax
  jmpq *%rax
  .size tf_live, .-tf_live

  .globl tf_live_table
  .type tf_live_table, @function
  .p2align 4
tf_live_table:
  pushq %rbp
  movq %rsp, %rbp
  pushq %rbx
  leaq .Ltf_table(%rip), %rdx
  movslq (%rdx,%rdi,4), %rax
  addq %rdx, %rax
  jmpq *%rax
.Ltf_c0:
  xorl %eax, %eax
  popq %rbx
  popq %rbp
  retq
.Ltf_c1:
  movl $1, %eax
  popq %rbx
  popq %rbp
  retq
.Ltf_c2:
  movl $2, %eax
  popq %rbx
  popq %rbp
  retq
  .size tf_live_table, .-tf_live_table

  .globl tf_join
  .type tf_join, @function
  .p2align 4
tf_join:
  testl %esi, %esi
  je 1f
  pushq %rbx
1:
  jmpq *%rdi
  .size tf_join, .-tf_join

  .globl tf_aligned_lost
  .type tf_aligned_lost, @function
  .p2align 4
tf_aligned_lost:
  andq $-16, %rsp
  jmpq *%rdi
  .size tf_aligned_lost, .-tf_aligned_lost

  .globl tf_dynamic
  .type tf_dynamic, @function
  .p2align 4
tf_dynamic:
  subq %rsi, %rsp
  addq %rsi, %rsp
  jmpq *%rdi
  .size tf_dynamic, .-tf_dynamic

  .globl tf_frame_lost
  .type tf_frame_lost, @function
  .p2align 4
tf_frame_lost:
  pushq %rbp
  movq %rsp, %rbp
  pushq %rbx
  movq %rdi, %rbx
  callq tf_ext
  addq %rax, %rbp
  movq %rbx, %rax
  leaveq
  jmpq *%rax
  .size tf_frame_lost, .-tf_frame_lost

  .globl tf_switch
  .type tf_switch, @function
  .p2align 4
tf_switch:
  pushq %rbp
  pushq %rbx
  movq %rsp, (%rdi)
  movq %rsi, %rsp
  popq %rbx
  popq %rbp
  popq %rcx
  jmpq *%rcx
  .size tf_switch, .-tf_switch

  .globl tf_resume
  .type tf_resume, @function
  .p2align 4
tf_resume:
  movq (%rdi), %rsp
  movq 8(%rdi), %rbp
  jmpq *16(%rdi)
  .size tf_resume, .-tf_resume

  .globl tf_loop_push
  .type tf_loop_push, @function
  .p2align 4
tf_loop_push:
  movl %esi, %ecx
1:
  pushq %rcx
  decl %ecx
  jnz 1b
  jmpq *%rdi
  .size tf_loop_push, .-tf_loop_push

  .section .rodata,"a",@progbits
  .p2align 2
.Ltf_table:
  .long .Ltf_c0-.Ltf_table
  .long .Ltf_c1-.Ltf_table
  .long .Ltf_c2-.Ltf_table
  .long 0

  .section .note.GNU-stack,"",@progbits

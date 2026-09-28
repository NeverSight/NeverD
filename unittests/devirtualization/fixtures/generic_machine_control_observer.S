// Original single-threaded test observer. It saves the host, installs all
// 16 guest GPRs and RFLAGS, and enters a fixture by JMP. The guest's outer RET
// reaches observer_exit. Capturing flags uses the restored host stack, so
// observing a run cannot overwrite the guest's control slots or canaries.
.text
.globl generic_machine_observe
.type generic_machine_observe,@function
generic_machine_observe:
  pushfq
  pushq %rbx
  pushq %rbp
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  movq %rsp, .Lm_host_sp(%rip)
  movq %rdi, .Lm_state(%rip)
  movq %rsi, .Lm_entry(%rip)
  movq %rdi, %rax
  pushq 128(%rax)
  popfq
  movq 8(%rax), %rcx
  movq 16(%rax), %rdx
  movq 24(%rax), %rbx
  movq 32(%rax), %rsp
  movq 40(%rax), %rbp
  movq 48(%rax), %rsi
  movq 56(%rax), %rdi
  movq 64(%rax), %r8
  movq 72(%rax), %r9
  movq 80(%rax), %r10
  movq 88(%rax), %r11
  movq 96(%rax), %r12
  movq 104(%rax), %r13
  movq 112(%rax), %r14
  movq 120(%rax), %r15
  movq 0(%rax), %rax
  jmpq *.Lm_entry(%rip)
.globl generic_machine_observer_exit
generic_machine_observer_exit:
  movq %rax, .Lm_saved_rax(%rip)
  movq .Lm_state(%rip), %rax
  movq %rcx, 8(%rax)
  movq %rdx, 16(%rax)
  movq %rbx, 24(%rax)
  movq %rsp, 32(%rax)
  movq %rbp, 40(%rax)
  movq %rsi, 48(%rax)
  movq %rdi, 56(%rax)
  movq %r8, 64(%rax)
  movq %r9, 72(%rax)
  movq %r10, 80(%rax)
  movq %r11, 88(%rax)
  movq %r12, 96(%rax)
  movq %r13, 104(%rax)
  movq %r14, 112(%rax)
  movq %r15, 120(%rax)
  movq .Lm_host_sp(%rip), %rsp
  pushfq
  popq 128(%rax)
  movq .Lm_saved_rax(%rip), %rdx
  movq %rdx, 0(%rax)
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbp
  popq %rbx
  popfq
  retq
.size generic_machine_observe, .-generic_machine_observe

.bss
.p2align 3
.Lm_host_sp: .quad 0
.Lm_state: .quad 0
.Lm_entry: .quad 0
.Lm_saved_rax: .quad 0
.section .note.GNU-stack,"",@progbits

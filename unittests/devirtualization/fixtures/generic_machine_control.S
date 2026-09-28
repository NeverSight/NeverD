// Original public x64 control-transfer fixtures. Positive entries use an
// explicit register/flags/stack state; they are not ordinary C functions.
.text
.p2align 4
.globl generic_machine_indirect_call
.type generic_machine_indirect_call,@function
generic_machine_indirect_call:
  pushfq
  popq %r8
  testq $1, %rdi
  leaq generic_machine_call_zero(%rip), %rax
  leaq generic_machine_call_one(%rip), %rdx
  cmovneq %rdx, %rax
  pushq %r8
  popfq
  callq *%rax
.globl generic_machine_call_fallthrough
generic_machine_call_fallthrough:
  movq %rsp, %r11
  retq
.globl generic_machine_call_zero
generic_machine_call_zero:
  movq %rax, %rbx
  movq (%rsp), %r9
  movq %rsp, %r10
  leaq 17(%rdi,%rsi,2), %rax
  retq
.globl generic_machine_call_one
generic_machine_call_one:
  movq %rax, %rbx
  movq (%rsp), %r9
  movq %rsp, %r10
  leaq 29(%rdi,%rsi,4), %rax
  retq
.size generic_machine_indirect_call, .-generic_machine_indirect_call

.p2align 4
.globl generic_machine_return_thread
.type generic_machine_return_thread,@function
generic_machine_return_thread:
  pushfq
  popq %r8
  testq $1, %rdi
  leaq generic_machine_return_zero(%rip), %rax
  leaq generic_machine_return_one(%rip), %rdx
  cmovneq %rdx, %rax
  pushq %r8
  popfq
  pushq %rax
  retq
.globl generic_machine_return_zero
generic_machine_return_zero:
  movq %rax, %rbx
  movq %rsp, %r11
  leaq 31(%rdi,%rsi,2), %rax
  retq
.globl generic_machine_return_one
generic_machine_return_one:
  movq %rax, %rbx
  movq %rsp, %r11
  leaq 47(%rdi,%rsi,4), %rax
  retq
.size generic_machine_return_thread, .-generic_machine_return_thread

.globl generic_machine_unknown_call
.type generic_machine_unknown_call,@function
generic_machine_unknown_call:
  callq *%rax
  retq
.size generic_machine_unknown_call, .-generic_machine_unknown_call

.macro bad_finite name, is_return, null_target
.globl \name
.type \name,@function
\name:
  testq $1, %rdi
  leaq .Lm_good\@(%rip), %rax
  .if \null_target
    movq $0, %rdx
  .else
    leaq generic_machine_readonly_slot(%rip), %rdx
  .endif
  cmovneq %rdx, %rax
  .if \is_return
    pushq %rax
    retq
  .else
    callq *%rax
  .endif
  retq
.Lm_good\@:
  retq
.size \name, .-\name
.endm
bad_finite generic_machine_nonexec_call, 0, 0
bad_finite generic_machine_missing_call, 0, 1
bad_finite generic_machine_nonexec_return, 1, 0

.globl generic_machine_memory_call
.type generic_machine_memory_call,@function
generic_machine_memory_call:
  callq *generic_machine_readonly_slot(%rip)
  retq
.size generic_machine_memory_call, .-generic_machine_memory_call

.globl generic_machine_stack_memory_call
.type generic_machine_stack_memory_call,@function
generic_machine_stack_memory_call:
  callq *(%rsp)
  retq
.size generic_machine_stack_memory_call, .-generic_machine_stack_memory_call

.globl generic_machine_unknown_return
.type generic_machine_unknown_return,@function
generic_machine_unknown_return:
  pushq %rdi
  retq
.size generic_machine_unknown_return, .-generic_machine_unknown_return

.section .rodata,"a",@progbits
.p2align 3
.globl generic_machine_readonly_slot
generic_machine_readonly_slot:
  .quad generic_machine_call_zero
.section .note.GNU-stack,"",@progbits

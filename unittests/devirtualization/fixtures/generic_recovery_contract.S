// Independently authored arithmetic/control fixtures for public recovery options.
.text
.globl generic_recovery_chain
.type generic_recovery_chain,@function
generic_recovery_chain:
  movq %rdi, %r10
  xorq $90, %r10
  jmp .Lchain_middle
.Lchain_middle:
  xorq %rdi, %r10
  leaq .Lchain_exit(%rip), %r11
  subq $90, %r11
  addq %r10, %r11
  jmp *%r11
.Lchain_exit:
  leaq 17(%rdi), %rax
  ret
.size generic_recovery_chain,.-generic_recovery_chain

.globl generic_recovery_repeat
.type generic_recovery_repeat,@function
generic_recovery_repeat:
  movq %rdi, %r10
  xorq $90, %r10
  movl $2, %edx
  jmp .Lrepeat_body
.Lrepeat_body:
  xorq $1, %r10
  decl %edx
  jne .Lrepeat_body
  xorq %rdi, %r10
  leaq .Lrepeat_exit(%rip), %r11
  subq $90, %r11
  addq %r10, %r11
  jmp *%r11
.Lrepeat_exit:
  leaq 17(%rdi), %rax
  ret
.size generic_recovery_repeat,.-generic_recovery_repeat

.globl generic_recovery_bounds
.type generic_recovery_bounds,@function
generic_recovery_bounds:
  leaq -16(%rsp), %r10
  leaq -48(%rsp), %r11
  subq $80, %rsp
  cmpq %r10, %r11
  jb .Lbounds_exit
  ud2
.Lbounds_exit:
  addq $80, %rsp
  leaq 17(%rdi), %rax
  ret
.size generic_recovery_bounds,.-generic_recovery_bounds
.globl generic_recovery_external_store
.type generic_recovery_external_store,@function
generic_recovery_external_store:
  call .Lexternal_store_callee
  ret
.Lexternal_store_callee:
  movq %rdx, (%rcx)
  movq (%rcx), %rax
  ret
.size generic_recovery_external_store,.-generic_recovery_external_store

.globl generic_recovery_register_cases
.type generic_recovery_register_cases,@function
generic_recovery_register_cases:
  movl $16, %eax
  movl $32, %edx
  testb $1, %cl
  cmovzq %rdx, %rax
  testb $4, %cl
  jnz .Lregister_case_store
  movl $48, %eax
  movl $64, %edx
  testb $1, %cl
  cmovzq %rdx, %rax
  jmp .Lregister_case_store
.Lregister_case_store:
  movq %rsp, %r8
  subq %rax, %r8
  movb $90, (%r8)
  ret
.size generic_recovery_register_cases,.-generic_recovery_register_cases
.section .note.GNU-stack,"",@progbits

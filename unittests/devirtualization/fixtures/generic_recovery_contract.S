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

.globl generic_recovery_indexed_store
.type generic_recovery_indexed_store,@function
generic_recovery_indexed_store:
  subq $64, %rsp
  movq $37, 56(%rsp)
  movl $4, %edx
.Lindexed_store_body:
  decl %edx
  movl %edi, %eax
  andl $7, %eax
  movl %edx, %ecx
  andl $3, %ecx
  leaq (%rsp,%rax), %r8
  movb $90, (%r8,%rcx)
  testl %edx, %edx
  jne .Lindexed_store_body
  movq 56(%rsp), %rax
  addq $64, %rsp
  ret
.size generic_recovery_indexed_store,.-generic_recovery_indexed_store
.globl generic_recovery_carried_indexed_store
.type generic_recovery_carried_indexed_store,@function
generic_recovery_carried_indexed_store:
  subq $64, %rsp
  movq $37, 56(%rsp)
  movl %edi, %eax
  andl $7, %eax
  leaq (%rsp,%rax), %r8
  movl $4, %edx
.Lcarried_store_body:
  decl %edx
  movl %edx, %ecx
  andl $3, %ecx
  movb $90, (%r8,%rcx)
  testl %edx, %edx
  jne .Lcarried_store_body
  movq 56(%rsp), %rax
  addq $64, %rsp
  ret
.size generic_recovery_carried_indexed_store,.-generic_recovery_carried_indexed_store
.globl generic_recovery_indexed_load
.type generic_recovery_indexed_load,@function
generic_recovery_indexed_load:
  subq $24, %rsp
  movq %rdi, (%rsp)
  movq %rsi, 8(%rsp)
  movl %edx, %eax
  andl $1, %eax
  movq (%rsp,%rax,8), %rax
  movq %rcx, (%rax)
  addq $24, %rsp
  ret
.size generic_recovery_indexed_load,.-generic_recovery_indexed_load
.section .note.GNU-stack,"",@progbits

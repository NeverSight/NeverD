# NeverD owned-context differential guest, x86-64 System V ABI.
.section .text.start,"ax"
.global stage64
stage64:
 mov $0x3f00000, %rsp
 mov $__bss_start, %edi
 mov $__bss_end, %ecx
 sub %edi, %ecx
 xor %eax, %eax
 rep stosb
 call guest_main
 ud2

.text
.global pending_wait
pending_wait:
 fwait
 ret

.global run_probe
run_probe:
 push %rbx
 push %rbp
 push %r12
 push %r13
 push %r14
 push %r15
 sub $8, %rsp
 mov %rsp, resume_stack(%rip)
 movl $0, fault_vector(%rip)
 movq $0, fault_error(%rip)
 fxrstor64 (%rdx)
 mov %rdi, %rax
 mov %rsi, %rdi
 call *%rax
 fxsave64 captured(%rip)
resume:
 mov fault_vector(%rip), %eax
 add $8, %rsp
 pop %r15
 pop %r14
 pop %r13
 pop %r12
 pop %rbp
 pop %rbx
 ret

.macro fault number, error
.global fault_\number
fault_\number:
.if !\error
 pushq $0
.endif
 pushq $\number
 jmp fault_common
.endm
fault 6, 0
fault 12, 1
fault 13, 1
fault 14, 1
fault 16, 0
fault_common:
 mov (%rsp), %eax
 mov %eax, fault_vector(%rip)
 mov 8(%rsp), %rax
 mov %rax, fault_error(%rip)
 fxsave64 captured(%rip)
 lea resume(%rip), %rax
 mov %rax, 16(%rsp)
 mov resume_stack(%rip), %rax
 mov %rax, 40(%rsp)
 add $16, %rsp
 iretq
.section .note.GNU-stack,"",@progbits

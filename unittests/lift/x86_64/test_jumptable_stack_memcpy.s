// Discovery sees the later lexical COPY of 64, but a feasible predecessor
// reaches the same memcpy with 56.  The complete CALL-point proof must reject
// the merge rather than authenticating only the lexical producer.
    .text
    .globl jt_stack_memcpy_length_clobber
    .type jt_stack_memcpy_length_clobber,@function
jt_stack_memcpy_length_clobber:
    pushq %rbp
    movq %rsp, %rbp
    subq $0x60, %rsp
    movl %edi, -0x54(%rbp)
    leaq -0x50(%rbp), %rdi
    leaq jt_stack_memcpy_length_clobber_table(%rip), %rsi
    testl %edi, %edi
    jne .Llength56
    jmp .Llength64
.Llength56:
    movl $56, %edx
    jmp .Llength_ready
.Llength64:
    movl $64, %edx
.Llength_ready:
    call memcpy
    movl -0x54(%rbp), %eax
    andl $7, %eax
    movq -0x50(%rbp,%rax,8), %rax
    jmp *%rax
.Lclobber0:
    movl $20, %eax
    leave
    ret
.Lclobber1:
    movl $21, %eax
    leave
    ret
.Lclobber2:
    movl $22, %eax
    leave
    ret
.Lclobber3:
    movl $23, %eax
    leave
    ret
.Lclobber4:
    movl $24, %eax
    leave
    ret
.Lclobber5:
    movl $25, %eax
    leave
    ret
.Lclobber6:
    movl $26, %eax
    leave
    ret
.Lclobber7:
    movl $27, %eax
    leave
    ret
    .size jt_stack_memcpy_length_clobber, .-jt_stack_memcpy_length_clobber

    .section .data.rel.ro.jt_stack_memcpy_length_clobber,"aw",@progbits
    .p2align 3
    .type jt_stack_memcpy_length_clobber_table,@object
    .size jt_stack_memcpy_length_clobber_table,64
jt_stack_memcpy_length_clobber_table:
    .quad .Lclobber0, .Lclobber1, .Lclobber2, .Lclobber3
    .quad .Lclobber4, .Lclobber5, .Lclobber6, .Lclobber7

    .section .note.GNU-stack,"",@progbits

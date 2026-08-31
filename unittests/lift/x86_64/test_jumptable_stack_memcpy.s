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

// SysV passes memcpy's operands in RDI/RSI/RDX.  Carry the real destination
// through an opaque-but-runtime-identity integer operation while placing an
// otherwise convincing cdecl-shaped dst/src/length tuple in outgoing stack
// cells.  The real call still initializes the complete table, so every input
// executes defined code; the stack tuple is only decoy evidence that a
// register-argument ABI must never consume.
    .text
    .globl jt_stack_memcpy_register_abi_cdecl_decoys
    .type jt_stack_memcpy_register_abi_cdecl_decoys,@function
jt_stack_memcpy_register_abi_cdecl_decoys:
    pushq %rbp
    movq %rsp, %rbp
    subq $0xa0, %rsp
    movl %edi, -0x54(%rbp)

    leaq -0x50(%rbp), %rax
    movq %rax, 0x00(%rsp)
    leaq jt_stack_memcpy_register_abi_cdecl_decoys_table(%rip), %rax
    movq %rax, 0x08(%rsp)
    movq $64, 0x10(%rsp)

    leaq -0x50(%rbp), %rdi
    andq $-1, %rdi
    leaq jt_stack_memcpy_register_abi_cdecl_decoys_table(%rip), %rsi
    movl $64, %edx
    call memcpy

    movl -0x54(%rbp), %eax
    andl $7, %eax
    movq -0x50(%rbp,%rax,8), %rax
    jmp *%rax
.Ldecoy0:
    movl $30, %eax
    leave
    ret
.Ldecoy1:
    movl $31, %eax
    leave
    ret
.Ldecoy2:
    movl $32, %eax
    leave
    ret
.Ldecoy3:
    movl $33, %eax
    leave
    ret
.Ldecoy4:
    movl $34, %eax
    leave
    ret
.Ldecoy5:
    movl $35, %eax
    leave
    ret
.Ldecoy6:
    movl $36, %eax
    leave
    ret
.Ldecoy7:
    movl $37, %eax
    leave
    ret
    .size jt_stack_memcpy_register_abi_cdecl_decoys, .-jt_stack_memcpy_register_abi_cdecl_decoys

    .section .data.rel.ro.jt_stack_memcpy_register_abi_cdecl_decoys,"aw",@progbits
    .p2align 3
    .type jt_stack_memcpy_register_abi_cdecl_decoys_table,@object
    .size jt_stack_memcpy_register_abi_cdecl_decoys_table,64
jt_stack_memcpy_register_abi_cdecl_decoys_table:
    .quad .Ldecoy0, .Ldecoy1, .Ldecoy2, .Ldecoy3
    .quad .Ldecoy4, .Ldecoy5, .Ldecoy6, .Ldecoy7

    .section .note.GNU-stack,"",@progbits

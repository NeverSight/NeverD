        .text
        .globl  jt_inclusive_postload_clobber
        .type   jt_inclusive_postload_clobber,@function
jt_inclusive_postload_clobber:
        cmpl    $2, %edi
        ja      .Lpost_default
        # Materialize the 32-bit guarded value as the actual 64-bit address
        # index.  Using live-in %rdi directly would leave its high half
        # unconstrained and make the fixture itself an out-of-bounds program.
        movl    %edi, %edi
        leaq    .Lpost_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        movl    %esi, %edi
        jmpq    *%rcx
.Lpost_case0:
        movl    $100, %eax
        retq
.Lpost_case1:
        movl    $101, %eax
        retq
.Lpost_case2:
        movl    $102, %eax
        retq
.Lpost_default:
        movl    $199, %eax
        retq
        .size   jt_inclusive_postload_clobber, .-jt_inclusive_postload_clobber

        .section .rodata,"a",@progbits
        .p2align 2
.Lpost_table:
        .long   .Lpost_case0-.Lpost_table
        .long   .Lpost_case1-.Lpost_table
        .long   .Lpost_case2-.Lpost_table

        .section .note.GNU-stack,"",@progbits

        .text
        .globl  jt_inclusive_postload_unrelated
        .type   jt_inclusive_postload_unrelated,@function
jt_inclusive_postload_unrelated:
        # The real index has an exclusive two-entry bound.  The later guard on
        # %esi has the same immediate but is inclusive; confusing it with the
        # index would add one and expose the adjacent poison relocation.
        # Compare the same full-width view consumed by the address expression;
        # a 32-bit EDI comparison would not constrain pre-existing high RDI
        # bits and is itself an invalid guard for the 64-bit table index.
        cmpq    $2, %rdi
        jae     .Lunrelated_default
        cmpl    $2, %esi
        ja      .Lunrelated_default
        leaq    .Lunrelated_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        movl    %esi, %edi
        jmpq    *%rcx
.Lunrelated_case0:
        movl    $300, %eax
        retq
.Lunrelated_case1:
        movl    $301, %eax
        retq
.Lunrelated_poison:
        movl    $399, %eax
        retq
.Lunrelated_default:
        movl    $398, %eax
        retq
        .size   jt_inclusive_postload_unrelated, .-jt_inclusive_postload_unrelated

        .section .rodata,"a",@progbits
        .p2align 2
.Lunrelated_table:
        .long   .Lunrelated_case0-.Lunrelated_table
        .long   .Lunrelated_case1-.Lunrelated_table
        # Physically adjacent but unreachable: the real index guard is < 2.
        .long   .Lunrelated_poison-.Lunrelated_table

        .section .note.GNU-stack,"",@progbits

        .text
        .globl  jt_inclusive_preload_alias
        .type   jt_inclusive_preload_alias,@function
jt_inclusive_preload_alias:
        cmpl    $2, %edi
        ja      .Lalias_default
        movl    %edi, %esi
        leaq    .Lalias_table(%rip), %rax
        movslq  (%rax,%rsi,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lalias_case0:
        movl    $200, %eax
        retq
.Lalias_case1:
        movl    $201, %eax
        retq
.Lalias_case2:
        movl    $202, %eax
        retq
.Lalias_default:
        movl    $299, %eax
        retq
        .size   jt_inclusive_preload_alias, .-jt_inclusive_preload_alias

        .section .rodata,"a",@progbits
        .p2align 2
.Lalias_table:
        .long   .Lalias_case0-.Lalias_table
        .long   .Lalias_case1-.Lalias_table
        .long   .Lalias_case2-.Lalias_table

        .section .note.GNU-stack,"",@progbits

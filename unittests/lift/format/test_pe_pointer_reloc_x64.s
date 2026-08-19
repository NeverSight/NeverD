        .text
        .globl  main
        .p2align 4
main:
        leaq    pointer_table(%rip), %rcx
        leaq    pointer_table+16(%rip), %rdx
        xorl    %eax, %eax
.Lwalk:
        movq    (%rcx), %r8
        addl    (%r8), %eax
        addq    $8, %rcx
        cmpq    %rdx, %rcx
        jne     .Lwalk
        retq

        .section .rdata,"dr"
        .p2align 3
pointer_table:
        .quad   value_a
        .quad   value_b
value_a:
        .long   3
value_b:
        .long   4

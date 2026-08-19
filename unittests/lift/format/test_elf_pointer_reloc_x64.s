        .text
        .globl  _start
        .type   _start,@function
        .p2align 4
_start:
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
        .size   _start, .-_start

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
pointer_table:
        .quad   value_a
        .quad   value_b

        .section .rodata,"a",@progbits
        .p2align 2
value_a:
        .long   3
value_b:
        .long   4

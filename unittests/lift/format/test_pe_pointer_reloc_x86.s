        .text
        .globl  _start
        .p2align 4
_start:
        movl    $pointer_table, %ecx
        movl    $pointer_table+8, %edx
        xorl    %eax, %eax
.Lwalk:
        movl    (%ecx), %esi
        addl    (%esi), %eax
        addl    $4, %ecx
        cmpl    %edx, %ecx
        jne     .Lwalk
        retl

        .section .rdata,"dr"
        .p2align 2
pointer_table:
        .long   value_a
        .long   value_b
value_a:
        .long   3
value_b:
        .long   4

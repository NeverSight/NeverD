// A PC-relative relocation run authenticates physical table capacity, not the
// range of an attacker-controlled selector.

        .text
        .globl  jt_raw_relative_capacity
        .type   jt_raw_relative_capacity,@function
jt_raw_relative_capacity:
        movl    %edi, %r10d
        leaq    .Lraw_rel_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lraw_rel_case0:
        movl    $4600, %eax
        retq
.Lraw_rel_case1:
        movl    $4601, %eax
        retq
.Lraw_rel_case2:
        movl    $4602, %eax
        retq
.Lraw_rel_case3:
        movl    $4603, %eax
        retq
        .size   jt_raw_relative_capacity, .-jt_raw_relative_capacity

        .section .rodata,"a",@progbits
        .p2align 2
.Lraw_rel_table:
        .long .Lraw_rel_case0-.Lraw_rel_table
        .long .Lraw_rel_case1-.Lraw_rel_table
        .long .Lraw_rel_case2-.Lraw_rel_table
        .long .Lraw_rel_case3-.Lraw_rel_table

        .section .note.GNU-stack,"",@progbits

        .text
        .globl  jt_inclusive_preload_reuse
        .type   jt_inclusive_preload_reuse,@function
jt_inclusive_preload_reuse:
        movl    %esi, %r10d
        cmpl    $2, %r10d
        ja      .Lreuse_default
        # Keep the poison block in the recovered CFG without making it a
        # table-index target.  The third argument selects this independent
        # control-flow edge; slot 2 must still never point here.
        testl   %edx, %edx
        jne     .Lreuse_poison
        movl    %edi, %r10d
        cmpl    $2, %r10d
        jae     .Lreuse_default
        leaq    .Lreuse_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lreuse_case0:
        movl    $400, %eax
        retq
.Lreuse_case1:
        movl    $401, %eax
        retq
.Lreuse_poison:
        movl    $499, %eax
        retq
.Lreuse_default:
        movl    $498, %eax
        retq
        .size   jt_inclusive_preload_reuse, .-jt_inclusive_preload_reuse

        .section .rodata,"a",@progbits
        .p2align 2
.Lreuse_table:
        .long   .Lreuse_case0-.Lreuse_table
        .long   .Lreuse_case1-.Lreuse_table
        # Adjacent relocation from a different index lifetime; never reachable.
        .long   .Lreuse_poison-.Lreuse_table

        .section .note.GNU-stack,"",@progbits

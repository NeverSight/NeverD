// A proof-dependent table that is valid before its targets are explored and
// invalid after case 0 contributes a backedge.  Losing the table during normal
// CFG fixed-point revalidation must preserve the original indirect-branch
// identity; it is not a function-pointer tail call.

        .text
        .globl  jt_identity_lost_published_table
        .type   jt_identity_lost_published_table,@function
jt_identity_lost_published_table:
        leaq    .Llost_published_table(%rip), %rax
        cmpl    $1, %edi
        ja      .Llost_published_default
.Llost_published_dispatch:
        movl    %edi, %r10d
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Llost_published_case0:
        movq    %rdx, %rax
        movl    %esi, %edi
        jmp     .Llost_published_dispatch
.Llost_published_case1:
        movl    $1251, %eax
        retq
.Llost_published_default:
        movl    $1259, %eax
        retq
        .size   jt_identity_lost_published_table, .-jt_identity_lost_published_table

        .section .rodata.jt_lost_published,"a",@progbits
        .p2align 2
.Llost_published_table:
        .long .Llost_published_case0-.Llost_published_table
        .long .Llost_published_case1-.Llost_published_table

        .section .note.GNU-stack,"",@progbits

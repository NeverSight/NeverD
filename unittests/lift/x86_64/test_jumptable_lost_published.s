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

// This absolute table is valid when both case labels are reached only through
// the dispatch.  Protecting slot 0 makes case 0 an independent relocation root;
// its backedge then reaches the dispatch with an unconstrained %edi, so a fresh
// builder correctly publishes no table at all.  Module arbitration nevertheless
// has to remember that an earlier builder published this guest branch as a jump
// table instead of reclassifying it as a function-pointer tail call.
        .text
        .globl  jt_identity_fresh_published_table
        .type   jt_identity_fresh_published_table,@function
jt_identity_fresh_published_table:
        andl    $1, %edi
.Lfresh_published_dispatch:
        leaq    jt_identity_fresh_published_table_storage(%rip), %rax
        jmpq    *(%rax,%rdi,8)
.Lfresh_published_case0:
        jmp     .Lfresh_published_dispatch
.Lfresh_published_case1:
        movl    $1261, %eax
        retq
        .size   jt_identity_fresh_published_table, .-jt_identity_fresh_published_table

// An exact second consumer forces module arbitration to preserve slot 0 as an
// independent code-pointer relocation root during the fresh owner rebuild.
        .globl  jt_identity_fresh_published_observer
        .type   jt_identity_fresh_published_observer,@function
jt_identity_fresh_published_observer:
        movq    jt_identity_fresh_published_table_storage(%rip), %rax
        retq
        .size   jt_identity_fresh_published_observer, .-jt_identity_fresh_published_observer

// A seeded indirect call is not historical jump-table evidence.  This keeps
// the fresh-builder seed filter from swallowing ordinary callback semantics.
        .globl  jt_identity_fresh_published_indirect_call
        .type   jt_identity_fresh_published_indirect_call,@function
jt_identity_fresh_published_indirect_call:
        callq   *%rdi
        retq
        .size   jt_identity_fresh_published_indirect_call, .-jt_identity_fresh_published_indirect_call

        .section .data.rel.ro.jt_fresh_published,"aw",@progbits
        .p2align 3
        .globl  jt_identity_fresh_published_table_storage
        .type   jt_identity_fresh_published_table_storage,@object
jt_identity_fresh_published_table_storage:
        .quad   .Lfresh_published_case0
        .quad   .Lfresh_published_case1
        .size   jt_identity_fresh_published_table_storage, .-jt_identity_fresh_published_table_storage

        .section .note.GNU-stack,"",@progbits

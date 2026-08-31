//===- test_jumptable_singleton_closed_world.s - closure negatives -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

        .text

// A constant selector does not close the runtime domain.  Slot zero widens
// the selector and transfers through an opaque address that may re-enter the
// dispatch, where slot one is then required.
        .globl  jt_lfp_relative_singleton_opaque_reentry
        .type   jt_lfp_relative_singleton_opaque_reentry,@function
jt_lfp_relative_singleton_opaque_reentry:
        xorl    %esi, %esi
        leaq    jt_lfp_relative_singleton_opaque_reentry_table(%rip), %rdx
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_singleton_opaque_reentry_dispatch
jt_lfp_relative_singleton_opaque_reentry_dispatch:
        jmpq    *%r8
.Lopaque_t0:
        movl    $1, %esi
        jmpq    *%rdi
.Lopaque_t1:
        retq
        .size   jt_lfp_relative_singleton_opaque_reentry, .-jt_lfp_relative_singleton_opaque_reentry

// Decoding can also stop at an ownership boundary.  The slot-zero body falls
// through into an adjacent function that may re-enter with selector one.
        .globl  jt_lfp_relative_singleton_owner_gap
        .type   jt_lfp_relative_singleton_owner_gap,@function
jt_lfp_relative_singleton_owner_gap:
        xorl    %esi, %esi
        leaq    jt_lfp_relative_singleton_owner_gap_table(%rip), %rdx
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_singleton_owner_gap_dispatch
jt_lfp_relative_singleton_owner_gap_dispatch:
        jmpq    *%r8
.Lowner_t1:
        retq
.Lowner_t0:
        movl    $1, %esi
        nop
        .size   jt_lfp_relative_singleton_owner_gap, .-jt_lfp_relative_singleton_owner_gap

        .globl  jt_lfp_relative_singleton_owner_gap_reentry
        .type   jt_lfp_relative_singleton_owner_gap_reentry,@function
jt_lfp_relative_singleton_owner_gap_reentry:
        jmpq    *%rdi
        .size   jt_lfp_relative_singleton_owner_gap_reentry, .-jt_lfp_relative_singleton_owner_gap_reentry

// A direct callee is open-world as well: it may invoke a runtime callback
// into the dispatch before returning to its caller.
        .globl  jt_lfp_relative_singleton_call_reentry
        .type   jt_lfp_relative_singleton_call_reentry,@function
jt_lfp_relative_singleton_call_reentry:
        xorl    %esi, %esi
        leaq    jt_lfp_relative_singleton_call_reentry_table(%rip), %rdx
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_singleton_call_reentry_dispatch
jt_lfp_relative_singleton_call_reentry_dispatch:
        jmpq    *%r8
.Lcall_t0:
        movl    $1, %esi
        callq   jt_lfp_relative_singleton_call_reentry_helper
        retq
.Lcall_t1:
        retq
        .size   jt_lfp_relative_singleton_call_reentry, .-jt_lfp_relative_singleton_call_reentry

        .globl  jt_lfp_relative_singleton_call_reentry_helper
        .type   jt_lfp_relative_singleton_call_reentry_helper,@function
jt_lfp_relative_singleton_call_reentry_helper:
        jmpq    *%rdi
        .size   jt_lfp_relative_singleton_call_reentry_helper, .-jt_lfp_relative_singleton_call_reentry_helper

        .section .rodata.jt_lfp_singleton_closed_world,"a",@progbits
        .p2align 2
        .globl  jt_lfp_relative_singleton_opaque_reentry_table
        .type   jt_lfp_relative_singleton_opaque_reentry_table,@object
jt_lfp_relative_singleton_opaque_reentry_table:
        .long   .Lopaque_t0-jt_lfp_relative_singleton_opaque_reentry_table
        .long   .Lopaque_t1-jt_lfp_relative_singleton_opaque_reentry_table
        .size   jt_lfp_relative_singleton_opaque_reentry_table, .-jt_lfp_relative_singleton_opaque_reentry_table

        .p2align 2
        .globl  jt_lfp_relative_singleton_owner_gap_table
        .type   jt_lfp_relative_singleton_owner_gap_table,@object
jt_lfp_relative_singleton_owner_gap_table:
        .long   .Lowner_t0-jt_lfp_relative_singleton_owner_gap_table
        .long   .Lowner_t1-jt_lfp_relative_singleton_owner_gap_table
        .size   jt_lfp_relative_singleton_owner_gap_table, .-jt_lfp_relative_singleton_owner_gap_table

        .p2align 2
        .globl  jt_lfp_relative_singleton_call_reentry_table
        .type   jt_lfp_relative_singleton_call_reentry_table,@object
jt_lfp_relative_singleton_call_reentry_table:
        .long   .Lcall_t0-jt_lfp_relative_singleton_call_reentry_table
        .long   .Lcall_t1-jt_lfp_relative_singleton_call_reentry_table
        .size   jt_lfp_relative_singleton_call_reentry_table, .-jt_lfp_relative_singleton_call_reentry_table

        .section .note.GNU-stack,"",@progbits

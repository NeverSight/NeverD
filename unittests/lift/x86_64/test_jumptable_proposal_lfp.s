//===- test_jumptable_proposal_lfp.s - proposal fixed-point fixtures ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

        .text

// A sized function and a sized relocation-backed table are both authoritative,
// and every table entry names the current function entry.  The machine-level
// jump re-enters the current frame; rewriting it as a call would push a new
// continuation on every iteration, so it must remain an opaque indirect branch.
        .globl  jt_lfp_sized_self_callback
        .type   jt_lfp_sized_self_callback,@function
jt_lfp_sized_self_callback:
        andl    $1, %edi
        leaq    jt_lfp_sized_self_callback_table(%rip), %rax
        movq    (%rax,%rdi,8), %rax
        jmpq    *%rax
        .size   jt_lfp_sized_self_callback, .-jt_lfp_sized_self_callback

// These mirrors model an optimized computed-goto loop.  The table base is
// materialized only at entry and inherited by every target body.  All five
// indirect branches consume the same four relocation slots, so no candidate
// may reject a sibling merely because the sibling has not published yet in
// the current traversal order.
        .globl  jt_lfp_siblings_forward
        .type   jt_lfp_siblings_forward,@function
jt_lfp_siblings_forward:
        .globl  jt_lfp_fwd_entry_begin
jt_lfp_fwd_entry_begin:
        leaq    jt_lfp_fwd_table(%rip), %rdx
        movl    %edi, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_fwd_entry_end
jt_lfp_fwd_entry_end:

        .globl  jt_lfp_fwd_t0_begin
jt_lfp_fwd_t0_begin:
        movl    %edi, %esi
        addl    $1, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_fwd_t0_end
jt_lfp_fwd_t0_end:

        .globl  jt_lfp_fwd_t1_begin
jt_lfp_fwd_t1_begin:
        movl    %edi, %esi
        shrl    $2, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_fwd_t1_end
jt_lfp_fwd_t1_end:

        .globl  jt_lfp_fwd_t2_begin
jt_lfp_fwd_t2_begin:
        movl    %edi, %esi
        shrl    $5, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_fwd_t2_end
jt_lfp_fwd_t2_end:

        .globl  jt_lfp_fwd_t3_begin
jt_lfp_fwd_t3_begin:
        movl    %edi, %esi
        shrl    $9, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_fwd_t3_end
jt_lfp_fwd_t3_end:
        .size   jt_lfp_siblings_forward, .-jt_lfp_siblings_forward

        .globl  jt_lfp_siblings_reverse
        .type   jt_lfp_siblings_reverse,@function
jt_lfp_siblings_reverse:
        .globl  jt_lfp_rev_entry_begin
jt_lfp_rev_entry_begin:
        leaq    jt_lfp_rev_table(%rip), %rdx
        movl    %edi, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_rev_entry_end
jt_lfp_rev_entry_end:

        .globl  jt_lfp_rev_t3_begin
jt_lfp_rev_t3_begin:
        movl    %edi, %esi
        shrl    $9, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_rev_t3_end
jt_lfp_rev_t3_end:

        .globl  jt_lfp_rev_t2_begin
jt_lfp_rev_t2_begin:
        movl    %edi, %esi
        shrl    $5, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_rev_t2_end
jt_lfp_rev_t2_end:

        .globl  jt_lfp_rev_t1_begin
jt_lfp_rev_t1_begin:
        movl    %edi, %esi
        shrl    $2, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_rev_t1_end
jt_lfp_rev_t1_end:

        .globl  jt_lfp_rev_t0_begin
jt_lfp_rev_t0_begin:
        movl    %edi, %esi
        addl    $1, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_lfp_rev_t0_end
jt_lfp_rev_t0_end:
        .size   jt_lfp_siblings_reverse, .-jt_lfp_siblings_reverse

        .section .data.rel.ro.jt_lfp_self_callback,"aw",@progbits
        .p2align 3
        .globl  jt_lfp_sized_self_callback_table
        .type   jt_lfp_sized_self_callback_table,@object
jt_lfp_sized_self_callback_table:
        .quad   jt_lfp_sized_self_callback
        .quad   jt_lfp_sized_self_callback
        .size   jt_lfp_sized_self_callback_table, .-jt_lfp_sized_self_callback_table

        .section .rodata.jt_lfp_siblings,"a",@progbits
        .p2align 3
        .globl  jt_lfp_fwd_table
        .type   jt_lfp_fwd_table,@object
jt_lfp_fwd_table:
        .quad   jt_lfp_fwd_t0_begin
        .quad   jt_lfp_fwd_t1_begin
        .quad   jt_lfp_fwd_t2_begin
        .quad   jt_lfp_fwd_t3_begin
        .size   jt_lfp_fwd_table, .-jt_lfp_fwd_table

        .p2align 3
        .globl  jt_lfp_rev_table
        .type   jt_lfp_rev_table,@object
jt_lfp_rev_table:
        .quad   jt_lfp_rev_t0_begin
        .quad   jt_lfp_rev_t1_begin
        .quad   jt_lfp_rev_t2_begin
        .quad   jt_lfp_rev_t3_begin
        .size   jt_lfp_rev_table, .-jt_lfp_rev_table

        .section .note.GNU-stack,"",@progbits

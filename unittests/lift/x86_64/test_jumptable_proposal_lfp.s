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

// These mirrors model an optimized computed-goto loop.  All five direct-memory
// indirect branches consume the same four relocation slots, so no candidate
// may reject a sibling merely because the sibling has not published yet in
// the current traversal order.
        .globl  jt_lfp_siblings_forward
        .type   jt_lfp_siblings_forward,@function
jt_lfp_siblings_forward:
        .globl  jt_lfp_fwd_entry_begin
jt_lfp_fwd_entry_begin:
        // Keep a sizeable harmless lexical prefix so the focused evidence
        // test can exhaust whole-function group inventory only after the
        // current instruction's exact absolute-table model is established.
        .rept   96
        nop
        .endr
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

// Ordinary memory callback control: an indirect jump through caller-provided
// storage has no absolute code-pointer relocation run and must remain eligible
// for tail-call lowering under the same small evidence allowance.
        .globl  jt_lfp_memory_callback
        .type   jt_lfp_memory_callback,@function
jt_lfp_memory_callback:
        movq    (%rdi), %rax
        jmpq    *%rax
        .size   jt_lfp_memory_callback, .-jt_lfp_memory_callback

// Instruction-local callback control for the unresolved-base/VA-zero seam.
// Unlike the decoupled callback above, this instruction lowers to one record
// containing both LOAD and INDIR_BR, so it exercises the exact-group local
// prepass without providing a constant table base.
        .globl  jt_lfp_indexed_memory_callback
        .type   jt_lfp_indexed_memory_callback,@function
jt_lfp_indexed_memory_callback:
        jmpq    *(%rsi,%rcx,8)
        .size   jt_lfp_indexed_memory_callback, .-jt_lfp_indexed_memory_callback

// Same-function negative for exact-group anchoring.  The callback and two
// direct absolute-table consumers share one authoritative function range; the
// callback must not borrow the siblings' relocation model merely because they
// occur later in the same inventory.
        .globl  jt_lfp_mixed_callback_group
        .type   jt_lfp_mixed_callback_group,@function
jt_lfp_mixed_callback_group:
        leaq    jt_lfp_mixed_table(%rip), %rdx
        testl   %edi, %edi
        js      jt_lfp_mixed_callback_begin
        andl    $1, %edi
        jmpq    *(%rdx,%rdi,8)
jt_lfp_mixed_t0:
        addl    $1, %edi
        andl    $1, %edi
        jmpq    *(%rdx,%rdi,8)
jt_lfp_mixed_t1:
        retq
        .globl  jt_lfp_mixed_callback_begin
jt_lfp_mixed_callback_begin:
        jmpq    *(%rsi,%rcx,8)
        .globl  jt_lfp_mixed_callback_end
jt_lfp_mixed_callback_end:
        .size   jt_lfp_mixed_callback_group, .-jt_lfp_mixed_callback_group

// A two-level relative table whose second dispatch is decoded only after the
// first candidate publishes graph-growth targets.  This is the focused
// transaction fixture for recursively discovered resolver invocations.
        .globl  jt_lfp_nested_relative
        .type   jt_lfp_nested_relative,@function
jt_lfp_nested_relative:
        andl    $1, %edi
        leaq    jt_lfp_nested_relative_table0(%rip), %r8
        movslq  (%r8,%rdi,4), %rax
        addq    %r8, %rax
        jmpq    *%rax
jt_lfp_nested_relative_dispatch:
        andl    $1, %esi
        leaq    jt_lfp_nested_relative_table1(%rip), %r8
        jmpq    *(%r8,%rsi,8)
jt_lfp_nested_relative_exit:
        retq
jt_lfp_nested_relative_leaf0:
        movl    $1, %eax
        retq
jt_lfp_nested_relative_leaf1:
        movl    $2, %eax
        retq
        .size   jt_lfp_nested_relative, .-jt_lfp_nested_relative

// Direct constant-base consumers isolate the const-base detector's early
// shape certificate.  The long unrelated prefix is cheap for Rec.Ops-only
// parsing but expensive for the later whole-function group inventory.
        .globl  jt_lfp_constbase_budget
        .type   jt_lfp_constbase_budget,@function
jt_lfp_constbase_budget:
        .rept   96
        nop
        .endr
        andl    $1, %edi
        .globl  jt_lfp_constbase_budget_claimed_branch
jt_lfp_constbase_budget_claimed_branch:
        jmpq    *jt_lfp_constbase_budget_table(,%rdi,8)
jt_lfp_constbase_budget_t0:
        addl    $1, %edi
        andl    $1, %edi
        .globl  jt_lfp_constbase_budget_unclaimed_sibling
jt_lfp_constbase_budget_unclaimed_sibling:
        jmpq    *jt_lfp_constbase_budget_table(,%rdi,8)
jt_lfp_constbase_budget_t1:
        retq
        .size   jt_lfp_constbase_budget, .-jt_lfp_constbase_budget

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

        .p2align 3
        .globl  jt_lfp_mixed_table
        .type   jt_lfp_mixed_table,@object
jt_lfp_mixed_table:
        .quad   jt_lfp_mixed_t0
        .quad   jt_lfp_mixed_t1
        .size   jt_lfp_mixed_table, .-jt_lfp_mixed_table

        .section .rodata.jt_lfp_nested_relative,"a",@progbits
        .p2align 3
        .globl  jt_lfp_nested_relative_table0
        .type   jt_lfp_nested_relative_table0,@object
jt_lfp_nested_relative_table0:
        .long   jt_lfp_nested_relative_dispatch-jt_lfp_nested_relative_table0
        .long   jt_lfp_nested_relative_exit-jt_lfp_nested_relative_table0
        .size   jt_lfp_nested_relative_table0, .-jt_lfp_nested_relative_table0
        .p2align 3
        .globl  jt_lfp_nested_relative_table1
        .type   jt_lfp_nested_relative_table1,@object
jt_lfp_nested_relative_table1:
        .quad   jt_lfp_nested_relative_leaf0
        .quad   jt_lfp_nested_relative_leaf1
        .size   jt_lfp_nested_relative_table1, .-jt_lfp_nested_relative_table1

        .section .rodata.jt_lfp_constbase_budget,"a",@progbits
        .p2align 3
        .globl  jt_lfp_constbase_budget_table
        .type   jt_lfp_constbase_budget_table,@object
jt_lfp_constbase_budget_table:
        .quad   jt_lfp_constbase_budget_t0
        .quad   jt_lfp_constbase_budget_t1
        .size   jt_lfp_constbase_budget_table, .-jt_lfp_constbase_budget_table

        .section .note.GNU-stack,"",@progbits

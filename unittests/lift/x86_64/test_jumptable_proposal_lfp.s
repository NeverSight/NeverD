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

// Both indirect branches consume the same PIC-relative table.  The entry
// branch has an exact scalar selector and is the only initially reachable
// consumer; its first proposal decodes the case blocks without publishing the
// surrounding physical table as its runtime domain.  The loop dispatch becomes
// reachable only through that immutable provisional edge, so its exact LOAD
// and address roles must replay against the lower-rank edge certificate without
// treating the relocation run as an index-domain proof.
        .globl  jt_lfp_relative_occurrence_cycle
        .type   jt_lfp_relative_occurrence_cycle,@function
jt_lfp_relative_occurrence_cycle:
        xorl    %ecx, %ecx
        xorl    %esi, %esi
        leaq    jt_lfp_relative_occurrence_cycle_table(%rip), %rdx
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_occurrence_entry_branch
jt_lfp_relative_occurrence_entry_branch:
        jmpq    *%r8
jt_lfp_relative_occurrence_loop:
        movl    %ecx, %esi
        andl    $3, %esi
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_occurrence_loop_branch
jt_lfp_relative_occurrence_loop_branch:
        jmpq    *%r8
        .globl  jt_lfp_relative_occurrence_t0
jt_lfp_relative_occurrence_t0:
        addl    $1, %eax
        jmp     jt_lfp_relative_occurrence_join
        .globl  jt_lfp_relative_occurrence_t1
jt_lfp_relative_occurrence_t1:
        addl    $2, %eax
        jmp     jt_lfp_relative_occurrence_join
        .globl  jt_lfp_relative_occurrence_t2
jt_lfp_relative_occurrence_t2:
        addl    $3, %eax
        jmp     jt_lfp_relative_occurrence_join
        .globl  jt_lfp_relative_occurrence_t3
jt_lfp_relative_occurrence_t3:
        addl    $4, %eax
jt_lfp_relative_occurrence_join:
        incl    %ecx
        cmpl    $4, %ecx
        jne     jt_lfp_relative_occurrence_loop
        retq
        .size   jt_lfp_relative_occurrence_cycle, .-jt_lfp_relative_occurrence_cycle

// The loop consumer has its own x&3 proof and publishes all four slots.  The
// entry consumer sees only slot zero, whose target executes a direct call
// before reaching the loop.  That call leaves the entry consumer's destination
// graph open: the loop's sibling runtime certificate may qualify singleton
// storage, but it must never be transferred as the entry selector's [0,4)
// runtime domain.  A direct entry-to-loop arm keeps the certified producer in
// the final ordinary CFG after the unsafe singleton is withheld.
        .globl  jt_lfp_relative_open_sibling
        .type   jt_lfp_relative_open_sibling,@function
jt_lfp_relative_open_sibling:
        xorl    %r13d, %r13d
        xorl    %esi, %esi
        leaq    jt_lfp_relative_open_sibling_table(%rip), %r12
        testl   %edi, %edi
        js      jt_lfp_relative_open_sibling_loop
        // A redundant local mask must not turn the exact singleton entry
        // selector into the sibling loop's four-way runtime domain.
        andl    $3, %esi
        movslq  (%r12,%rsi,4), %r8
        addq    %r12, %r8
        .globl  jt_lfp_relative_open_sibling_entry_branch
jt_lfp_relative_open_sibling_entry_branch:
        jmpq    *%r8
jt_lfp_relative_open_sibling_loop:
        movl    %r13d, %esi
        andl    $3, %esi
        movslq  (%r12,%rsi,4), %r8
        addq    %r12, %r8
        .globl  jt_lfp_relative_open_sibling_loop_branch
jt_lfp_relative_open_sibling_loop_branch:
        jmpq    *%r8
        .globl  jt_lfp_relative_open_sibling_t0
jt_lfp_relative_open_sibling_t0:
        callq   jt_lfp_relative_open_sibling_callback
        addl    $1, %eax
        jmp     jt_lfp_relative_open_sibling_join
        .globl  jt_lfp_relative_open_sibling_t1
jt_lfp_relative_open_sibling_t1:
        addl    $2, %eax
        jmp     jt_lfp_relative_open_sibling_join
        .globl  jt_lfp_relative_open_sibling_t2
jt_lfp_relative_open_sibling_t2:
        addl    $3, %eax
        jmp     jt_lfp_relative_open_sibling_join
        .globl  jt_lfp_relative_open_sibling_t3
jt_lfp_relative_open_sibling_t3:
        addl    $4, %eax
jt_lfp_relative_open_sibling_join:
        incl    %r13d
        cmpl    $4, %r13d
        jne     jt_lfp_relative_open_sibling_loop
        retq
jt_lfp_relative_open_sibling_callback:
        retq
        .size   jt_lfp_relative_open_sibling, .-jt_lfp_relative_open_sibling

// The entry dispatch exposes only slot zero.  Its provisional edge is the sole
// path to the second dispatch, whose selector is bounded by a cmp/ja guard
// rather than a mask or modulo.  Recovering the loop table therefore requires
// the precise-guard proof to consume the same lower-rank edge overlay as the
// target and address role proofs.  Once the loop table is published, the entry
// singleton must replay on the ordinary CFG and both provisional edges retire.
        .globl  jt_lfp_relative_guard_cycle
        .type   jt_lfp_relative_guard_cycle,@function
jt_lfp_relative_guard_cycle:
        xorl    %esi, %esi
        leaq    jt_lfp_relative_guard_cycle_table(%rip), %rdx
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_guard_entry_branch
jt_lfp_relative_guard_entry_branch:
        jmpq    *%r8
jt_lfp_relative_guard_loop:
        cmpl    $3, %ecx
        ja      jt_lfp_relative_guard_exit
        movl    %ecx, %esi
        movslq  (%rdx,%rsi,4), %r8
        addq    %rdx, %r8
        .globl  jt_lfp_relative_guard_loop_branch
jt_lfp_relative_guard_loop_branch:
        jmpq    *%r8
        .globl  jt_lfp_relative_guard_t0
jt_lfp_relative_guard_t0:
        movl    %edi, %ecx
        jmp     jt_lfp_relative_guard_loop
        .globl  jt_lfp_relative_guard_t1
jt_lfp_relative_guard_t1:
        movl    $1, %eax
        retq
        .globl  jt_lfp_relative_guard_t2
jt_lfp_relative_guard_t2:
        movl    $2, %eax
        retq
        .globl  jt_lfp_relative_guard_t3
jt_lfp_relative_guard_t3:
        movl    $3, %eax
        retq
jt_lfp_relative_guard_exit:
        retq
        .size   jt_lfp_relative_guard_cycle, .-jt_lfp_relative_guard_cycle

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

        .p2align 2
        .globl  jt_lfp_relative_occurrence_cycle_table
        .type   jt_lfp_relative_occurrence_cycle_table,@object
jt_lfp_relative_occurrence_cycle_table:
        .long   jt_lfp_relative_occurrence_t0-jt_lfp_relative_occurrence_cycle_table
        .long   jt_lfp_relative_occurrence_t1-jt_lfp_relative_occurrence_cycle_table
        .long   jt_lfp_relative_occurrence_t2-jt_lfp_relative_occurrence_cycle_table
        .long   jt_lfp_relative_occurrence_t3-jt_lfp_relative_occurrence_cycle_table
        .size   jt_lfp_relative_occurrence_cycle_table, .-jt_lfp_relative_occurrence_cycle_table

        .p2align 2
        .globl  jt_lfp_relative_open_sibling_table
        .type   jt_lfp_relative_open_sibling_table,@object
jt_lfp_relative_open_sibling_table:
        .long   jt_lfp_relative_open_sibling_t0-jt_lfp_relative_open_sibling_table
        .long   jt_lfp_relative_open_sibling_t1-jt_lfp_relative_open_sibling_table
        .long   jt_lfp_relative_open_sibling_t2-jt_lfp_relative_open_sibling_table
        .long   jt_lfp_relative_open_sibling_t3-jt_lfp_relative_open_sibling_table
        .size   jt_lfp_relative_open_sibling_table, .-jt_lfp_relative_open_sibling_table

        .p2align 2
        .globl  jt_lfp_relative_guard_cycle_table
        .type   jt_lfp_relative_guard_cycle_table,@object
jt_lfp_relative_guard_cycle_table:
        .long   jt_lfp_relative_guard_t0-jt_lfp_relative_guard_cycle_table
        .long   jt_lfp_relative_guard_t1-jt_lfp_relative_guard_cycle_table
        .long   jt_lfp_relative_guard_t2-jt_lfp_relative_guard_cycle_table
        .long   jt_lfp_relative_guard_t3-jt_lfp_relative_guard_cycle_table
        .size   jt_lfp_relative_guard_cycle_table, .-jt_lfp_relative_guard_cycle_table

        .section .rodata.jt_lfp_constbase_budget,"a",@progbits
        .p2align 3
        .globl  jt_lfp_constbase_budget_table
        .type   jt_lfp_constbase_budget_table,@object
jt_lfp_constbase_budget_table:
        .quad   jt_lfp_constbase_budget_t0
        .quad   jt_lfp_constbase_budget_t1
        .size   jt_lfp_constbase_budget_table, .-jt_lfp_constbase_budget_table

        .section .note.GNU-stack,"",@progbits

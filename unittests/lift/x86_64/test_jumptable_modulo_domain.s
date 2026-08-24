// Adversarial modulo-domain candidates.  Both functions contain a readable
// inline target table, but their selectors are not remainders and therefore
// have no finite runtime domain.  Keep this fixture small so both symbols are
// independently lifted even after jump-table recovery correctly rejects them.

        .text
        .globl  jt_modulo_independent_quotient
        .type   jt_modulo_independent_quotient,@function
jt_modulo_independent_quotient:
        imull   $4, %esi, %eax
        movl    %edi, %r10d
        subl    %eax, %r10d
        leaq    .Lmod4_table(%rip), %rax
        movq    (%rax,%r10,8), %rcx
        jmpq    *%rcx
.Lmod4_case0:
        movl    $4300, %eax
        retq
.Lmod4_case1:
        movl    $4301, %eax
        retq
.Lmod4_case2:
        movl    $4302, %eax
        retq
.Lmod4_case3:
        movl    $4303, %eax
        retq
        .size   jt_modulo_independent_quotient, .-jt_modulo_independent_quotient
        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lmod4_table:
        .quad .Lmod4_case0
        .quad .Lmod4_case1
        .quad .Lmod4_case2
        .quad .Lmod4_case3

        .text
        .globl  jt_modulo_mixed_roots
        .type   jt_modulo_mixed_roots,@function
jt_modulo_mixed_roots:
        imull   $2, %esi, %eax
        imull   $3, %edx, %ecx
        addl    %ecx, %eax
        movl    %edi, %r10d
        subl    %eax, %r10d
        leaq    .Lmod5_table(%rip), %rax
        movq    (%rax,%r10,8), %rcx
        jmpq    *%rcx
.Lmod5_case0:
        movl    $4400, %eax
        retq
.Lmod5_case1:
        movl    $4401, %eax
        retq
.Lmod5_case2:
        movl    $4402, %eax
        retq
.Lmod5_case3:
        movl    $4403, %eax
        retq
.Lmod5_case4:
        movl    $4404, %eax
        retq
        .size   jt_modulo_mixed_roots, .-jt_modulo_mixed_roots
        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lmod5_table:
        .quad .Lmod5_case0
        .quad .Lmod5_case1
        .quad .Lmod5_case2
        .quad .Lmod5_case3
        .quad .Lmod5_case4

// A relocation run authenticates four physical slots, not the range of a raw
// attacker-controlled selector.  These two variants lock the capacity/domain
// split for absolute code pointers and PIC-relative offsets respectively.
        .text
        .globl  jt_raw_absolute_capacity
        .type   jt_raw_absolute_capacity,@function
jt_raw_absolute_capacity:
        movl    %edi, %r10d
        leaq    .Lraw_abs_table(%rip), %rax
        movq    (%rax,%r10,8), %rcx
        jmpq    *%rcx
.Lraw_abs_case0:
        movl    $4500, %eax
        retq
.Lraw_abs_case1:
        movl    $4501, %eax
        retq
.Lraw_abs_case2:
        movl    $4502, %eax
        retq
.Lraw_abs_case3:
        movl    $4503, %eax
        retq
        .size   jt_raw_absolute_capacity, .-jt_raw_absolute_capacity

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lraw_abs_table:
        .quad .Lraw_abs_case0
        .quad .Lraw_abs_case1
        .quad .Lraw_abs_case2
        .quad .Lraw_abs_case3

// These two selectors have the surface shape `x - q*5`, but q is not the
// LLVM unsigned-division-by-five quotient.  A relocation-backed five-slot
// table supplies physical capacity only; the wrong reciprocal or post-shift
// must not authenticate the runtime index domain.
        .text
        .globl  jt_modulo_wrong_magic
        .type   jt_modulo_wrong_magic,@function
jt_modulo_wrong_magic:
        movl    %edi, %eax
        movl    $0xcccccccc, %ecx
        imulq   %rax, %rcx
        shrq    $34, %rcx
        leal    (%rcx,%rcx,4), %edx
        subl    %edx, %eax
        leaq    .Lwrong_magic_table(%rip), %rcx
        movq    (%rcx,%rax,8), %rdx
        jmpq    *%rdx
.Lwrong_magic_case0:
        movl    $4600, %eax
        retq
.Lwrong_magic_case1:
        movl    $4601, %eax
        retq
.Lwrong_magic_case2:
        movl    $4602, %eax
        retq
.Lwrong_magic_case3:
        movl    $4603, %eax
        retq
.Lwrong_magic_case4:
        movl    $4604, %eax
        retq
        .size   jt_modulo_wrong_magic, .-jt_modulo_wrong_magic

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lwrong_magic_table:
        .quad .Lwrong_magic_case0
        .quad .Lwrong_magic_case1
        .quad .Lwrong_magic_case2
        .quad .Lwrong_magic_case3
        .quad .Lwrong_magic_case4

        .text
        .globl  jt_modulo_wrong_postshift
        .type   jt_modulo_wrong_postshift,@function
jt_modulo_wrong_postshift:
        movl    %edi, %eax
        movl    $0xcccccccd, %ecx
        imulq   %rax, %rcx
        shrq    $33, %rcx
        leal    (%rcx,%rcx,4), %edx
        subl    %edx, %eax
        leaq    .Lwrong_shift_table(%rip), %rcx
        movq    (%rcx,%rax,8), %rdx
        jmpq    *%rdx
.Lwrong_shift_case0:
        movl    $4700, %eax
        retq
.Lwrong_shift_case1:
        movl    $4701, %eax
        retq
.Lwrong_shift_case2:
        movl    $4702, %eax
        retq
.Lwrong_shift_case3:
        movl    $4703, %eax
        retq
.Lwrong_shift_case4:
        movl    $4704, %eax
        retq
        .size   jt_modulo_wrong_postshift, .-jt_modulo_wrong_postshift

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lwrong_shift_table:
        .quad .Lwrong_shift_case0
        .quad .Lwrong_shift_case1
        .quad .Lwrong_shift_case2
        .quad .Lwrong_shift_case3
        .quad .Lwrong_shift_case4

// A dead unsigned remainder may propose a modulus during the provisional
// whole-function scan, but it has no path to the real selector use.  The live
// arm dispatches with the literal zero.  Five relocation-backed slots are
// physical capacity only; final replay must require the selector to depend on
// the exact INT_REM occurrence before it may publish this table.
        .text
        .globl  jt_modulo_constant_selector_unreachable_rem
        .type   jt_modulo_constant_selector_unreachable_rem,@function
jt_modulo_constant_selector_unreachable_rem:
        xorl    %r10d, %r10d
        testl   %r10d, %r10d
        je      .Lunreachable_rem_dispatch
.Lunreachable_rem_producer:
        movl    %edi, %eax
        xorl    %edx, %edx
        movl    $5, %ecx
        divl    %ecx
        movl    $4799, %eax
        retq
.Lunreachable_rem_dispatch:
        leaq    .Lunreachable_rem_table(%rip), %rax
        movq    (%rax,%r10,8), %rcx
        jmpq    *%rcx
.Lunreachable_rem_case0:
        movl    $4790, %eax
        retq
.Lunreachable_rem_case1:
        movl    $4791, %eax
        retq
.Lunreachable_rem_case2:
        movl    $4792, %eax
        retq
.Lunreachable_rem_case3:
        movl    $4793, %eax
        retq
.Lunreachable_rem_case4:
        movl    $4794, %eax
        retq
        .size   jt_modulo_constant_selector_unreachable_rem, .-jt_modulo_constant_selector_unreachable_rem

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
        .globl  jt_modulo_unreachable_rem_table
        .type   jt_modulo_unreachable_rem_table,@object
jt_modulo_unreachable_rem_table:
.Lunreachable_rem_table:
        .quad .Lunreachable_rem_case0
        .quad .Lunreachable_rem_case1
        .quad .Lunreachable_rem_case2
        .quad .Lunreachable_rem_case3
        .quad .Lunreachable_rem_case4
        .size jt_modulo_unreachable_rem_table, .-jt_modulo_unreachable_rem_table

// The dead proposal chain is a shared doubling DAG: each add reads the same
// reaching value twice.  A recursive coefficient walk without occurrence-key
// memoization expands it exponentially and exhausts the proof budget before
// reaching the later producers.  The unused exact `% 1000` producer also
// precedes the real `% 5`; physical capacity must discard it before it can
// consume the table-probe/replay budget needed by the small real modulus.
// With bounded memoization and ascending per-bound probes the valid table
// remains recoverable.
        .text
        .globl  jt_modulo_shared_dag_budget
        .type   jt_modulo_shared_dag_budget,@function
jt_modulo_shared_dag_budget:
        movq    %rsi, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        addq    %r8, %r8
        movq    %rdi, %r9
        subq    %r8, %r9

        movl    %esi, %eax
        xorl    %edx, %edx
        movl    $1000, %ecx
        divl    %ecx

        movl    %edi, %eax
        xorl    %edx, %edx
        movl    $5, %ecx
        divl    %ecx
        leaq    .Lshared_dag_table(%rip), %rax
        movq    (%rax,%rdx,8), %rdx
        jmpq    *%rdx
.Lshared_dag_case0:
        movl    $4810, %eax
        retq
.Lshared_dag_case1:
        movl    $4811, %eax
        retq
.Lshared_dag_case2:
        movl    $4812, %eax
        retq
.Lshared_dag_case3:
        movl    $4813, %eax
        retq
.Lshared_dag_case4:
        movl    $4814, %eax
        retq
        .size   jt_modulo_shared_dag_budget, .-jt_modulo_shared_dag_budget

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
        .globl  jt_modulo_shared_dag_table
        .type   jt_modulo_shared_dag_table,@object
jt_modulo_shared_dag_table:
.Lshared_dag_table:
        .quad .Lshared_dag_case0
        .quad .Lshared_dag_case1
        .quad .Lshared_dag_case2
        .quad .Lshared_dag_case3
        .quad .Lshared_dag_case4
        .size jt_modulo_shared_dag_table, .-jt_modulo_shared_dag_table

// Both exact producers are legitimate proposals, but only `% 5` reaches the
// dispatch.  The inline relative table has no relocation-derived physical
// capacity and exactly five readable entries.  Probing one synthetic max of
// seven would fail the bounded read and incorrectly discard the valid five;
// each candidate must be probed independently.
        .text
        .globl  jt_modulo_compact_probe_isolated
        .type   jt_modulo_compact_probe_isolated,@function
jt_modulo_compact_probe_isolated:
        movl    %esi, %eax
        xorl    %edx, %edx
        movl    $7, %ecx
        divl    %ecx

        movl    %edi, %eax
        xorl    %edx, %edx
        movl    $5, %ecx
        divl    %ecx
        leaq    .Lcompact_probe_table(%rip), %rax
        movslq  (%rax,%rdx,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lcompact_probe_case0:
        movl    $4820, %eax
        retq
.Lcompact_probe_case1:
        movl    $4821, %eax
        retq
.Lcompact_probe_case2:
        movl    $4822, %eax
        retq
.Lcompact_probe_case3:
        movl    $4823, %eax
        retq
.Lcompact_probe_case4:
        movl    $4824, %eax
        retq
        .size   jt_modulo_compact_probe_isolated, .-jt_modulo_compact_probe_isolated

        .p2align 2
        .globl  jt_modulo_compact_probe_table
        .type   jt_modulo_compact_probe_table,@object
jt_modulo_compact_probe_table:
.Lcompact_probe_table:
        .long .Lcompact_probe_case0-.Lcompact_probe_table
        .long .Lcompact_probe_case1-.Lcompact_probe_table
        .long .Lcompact_probe_case2-.Lcompact_probe_table
        .long .Lcompact_probe_case3-.Lcompact_probe_table
        .long .Lcompact_probe_case4-.Lcompact_probe_table
        .size jt_modulo_compact_probe_table, .-jt_modulo_compact_probe_table

// The third relocation is outside the authenticated guard domain, but points
// back into this function at a path that bypasses the guard.  Provisional
// capacity ownership hides that root during bootstrap; after the exact domain
// shrinks to slots 0..1, restoring slot 2 must force the guard witness to be
// replayed on the final proof graph and reject the static table.
        .text
        .globl  jt_guard_domain_replays_after_root_restore
        .type   jt_guard_domain_replays_after_root_restore,@function
jt_guard_domain_replays_after_root_restore:
        cmpq    $2, %rdi
        jae     .Lguard_replay_default
.Lguard_replay_dispatch:
        leaq    .Lguard_replay_table(%rip), %rax
        movq    (%rax,%rdi,8), %rcx
        jmpq    *%rcx
.Lguard_replay_case0:
        movl    $4800, %eax
        retq
.Lguard_replay_case1:
        movl    $4801, %eax
        retq
.Lguard_replay_default:
        movl    $-1, %eax
        retq
.Lguard_replay_bypass_root:
        movq    %rsi, %rdi
        jmp     .Lguard_replay_dispatch
        .size   jt_guard_domain_replays_after_root_restore, .-jt_guard_domain_replays_after_root_restore

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lguard_replay_table:
        .quad .Lguard_replay_case0
        .quad .Lguard_replay_case1
        .quad .Lguard_replay_bypass_root

// Clang 20 -O0-style unsigned `acc % 140` selector.  The remainder is
// spilled as i32, widened, spilled again as i64, and reloaded for a 140-entry
// rel32 code table.  Every case rejoins a real loop backedge.
        .text
        .globl  jt_modulo_u140_spill_reload_loop
        .type   jt_modulo_u140_spill_reload_loop,@function
jt_modulo_u140_spill_reload_loop:
        pushq   %rbp
        movq    %rsp, %rbp
        movq    %rdi, -8(%rbp)
        movq    -8(%rbp), %rax
        orl     $1, %eax
        movl    %eax, -12(%rbp)
        movl    $0, -16(%rbp)

.Lmod140_loop:
        cmpl    $4, -16(%rbp)
        jge     .Lmod140_done

        // LLVM 20.1.8 unsigned division/modulo-by-140 recipe:
        // q = ((x >> 2) * 0x3a83a83b) >> 35; r = x - q * 140.
        movl    -12(%rbp), %eax
        movl    %eax, %ecx
        shrl    $2, %ecx
        imulq   $0x3a83a83b, %rcx, %rcx
        shrq    $35, %rcx
        imull   $140, %ecx, %ecx
        subl    %ecx, %eax

        movl    %eax, -20(%rbp)
        movl    -20(%rbp), %eax
        movq    %rax, -32(%rbp)
        movq    -32(%rbp), %rax

        leaq    .Lmod140_table(%rip), %rcx
        movslq  (%rcx,%rax,4), %rax
        addq    %rcx, %rax
        jmpq    *%rax

.Lmod140_case_0:
        addl    $5000, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_1:
        addl    $5001, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_2:
        addl    $5002, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_3:
        addl    $5003, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_4:
        addl    $5004, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_5:
        addl    $5005, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_6:
        addl    $5006, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_7:
        addl    $5007, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_8:
        addl    $5008, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_9:
        addl    $5009, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_10:
        addl    $5010, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_11:
        addl    $5011, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_12:
        addl    $5012, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_13:
        addl    $5013, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_14:
        addl    $5014, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_15:
        addl    $5015, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_16:
        addl    $5016, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_17:
        addl    $5017, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_18:
        addl    $5018, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_19:
        addl    $5019, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_20:
        addl    $5020, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_21:
        addl    $5021, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_22:
        addl    $5022, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_23:
        addl    $5023, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_24:
        addl    $5024, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_25:
        addl    $5025, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_26:
        addl    $5026, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_27:
        addl    $5027, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_28:
        addl    $5028, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_29:
        addl    $5029, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_30:
        addl    $5030, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_31:
        addl    $5031, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_32:
        addl    $5032, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_33:
        addl    $5033, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_34:
        addl    $5034, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_35:
        addl    $5035, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_36:
        addl    $5036, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_37:
        addl    $5037, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_38:
        addl    $5038, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_39:
        addl    $5039, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_40:
        addl    $5040, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_41:
        addl    $5041, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_42:
        addl    $5042, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_43:
        addl    $5043, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_44:
        addl    $5044, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_45:
        addl    $5045, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_46:
        addl    $5046, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_47:
        addl    $5047, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_48:
        addl    $5048, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_49:
        addl    $5049, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_50:
        addl    $5050, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_51:
        addl    $5051, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_52:
        addl    $5052, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_53:
        addl    $5053, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_54:
        addl    $5054, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_55:
        addl    $5055, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_56:
        addl    $5056, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_57:
        addl    $5057, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_58:
        addl    $5058, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_59:
        addl    $5059, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_60:
        addl    $5060, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_61:
        addl    $5061, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_62:
        addl    $5062, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_63:
        addl    $5063, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_64:
        addl    $5064, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_65:
        addl    $5065, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_66:
        addl    $5066, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_67:
        addl    $5067, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_68:
        addl    $5068, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_69:
        addl    $5069, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_70:
        addl    $5070, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_71:
        addl    $5071, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_72:
        addl    $5072, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_73:
        addl    $5073, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_74:
        addl    $5074, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_75:
        addl    $5075, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_76:
        addl    $5076, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_77:
        addl    $5077, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_78:
        addl    $5078, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_79:
        addl    $5079, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_80:
        addl    $5080, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_81:
        addl    $5081, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_82:
        addl    $5082, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_83:
        addl    $5083, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_84:
        addl    $5084, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_85:
        addl    $5085, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_86:
        addl    $5086, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_87:
        addl    $5087, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_88:
        addl    $5088, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_89:
        addl    $5089, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_90:
        addl    $5090, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_91:
        addl    $5091, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_92:
        addl    $5092, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_93:
        addl    $5093, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_94:
        addl    $5094, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_95:
        addl    $5095, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_96:
        addl    $5096, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_97:
        addl    $5097, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_98:
        addl    $5098, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_99:
        addl    $5099, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_100:
        addl    $5100, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_101:
        addl    $5101, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_102:
        addl    $5102, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_103:
        addl    $5103, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_104:
        addl    $5104, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_105:
        addl    $5105, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_106:
        addl    $5106, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_107:
        addl    $5107, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_108:
        addl    $5108, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_109:
        addl    $5109, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_110:
        addl    $5110, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_111:
        addl    $5111, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_112:
        addl    $5112, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_113:
        addl    $5113, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_114:
        addl    $5114, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_115:
        addl    $5115, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_116:
        addl    $5116, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_117:
        addl    $5117, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_118:
        addl    $5118, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_119:
        addl    $5119, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_120:
        addl    $5120, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_121:
        addl    $5121, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_122:
        addl    $5122, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_123:
        addl    $5123, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_124:
        addl    $5124, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_125:
        addl    $5125, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_126:
        addl    $5126, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_127:
        addl    $5127, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_128:
        addl    $5128, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_129:
        addl    $5129, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_130:
        addl    $5130, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_131:
        addl    $5131, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_132:
        addl    $5132, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_133:
        addl    $5133, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_134:
        addl    $5134, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_135:
        addl    $5135, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_136:
        addl    $5136, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_137:
        addl    $5137, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_138:
        addl    $5138, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_case_139:
        addl    $5139, -12(%rbp)
        jmp     .Lmod140_continue

.Lmod140_continue:
        movl    -16(%rbp), %eax
        imull   $131, %eax, %eax
        addl    %eax, -12(%rbp)
        addl    $1, -16(%rbp)
        jmp     .Lmod140_loop

.Lmod140_done:
        movl    -12(%rbp), %eax
        popq    %rbp
        retq
        .size   jt_modulo_u140_spill_reload_loop, .-jt_modulo_u140_spill_reload_loop

        .section .rodata,"a",@progbits
        .p2align 2
.Lmod140_table:
        .long   .Lmod140_case_0-.Lmod140_table
        .long   .Lmod140_case_1-.Lmod140_table
        .long   .Lmod140_case_2-.Lmod140_table
        .long   .Lmod140_case_3-.Lmod140_table
        .long   .Lmod140_case_4-.Lmod140_table
        .long   .Lmod140_case_5-.Lmod140_table
        .long   .Lmod140_case_6-.Lmod140_table
        .long   .Lmod140_case_7-.Lmod140_table
        .long   .Lmod140_case_8-.Lmod140_table
        .long   .Lmod140_case_9-.Lmod140_table
        .long   .Lmod140_case_10-.Lmod140_table
        .long   .Lmod140_case_11-.Lmod140_table
        .long   .Lmod140_case_12-.Lmod140_table
        .long   .Lmod140_case_13-.Lmod140_table
        .long   .Lmod140_case_14-.Lmod140_table
        .long   .Lmod140_case_15-.Lmod140_table
        .long   .Lmod140_case_16-.Lmod140_table
        .long   .Lmod140_case_17-.Lmod140_table
        .long   .Lmod140_case_18-.Lmod140_table
        .long   .Lmod140_case_19-.Lmod140_table
        .long   .Lmod140_case_20-.Lmod140_table
        .long   .Lmod140_case_21-.Lmod140_table
        .long   .Lmod140_case_22-.Lmod140_table
        .long   .Lmod140_case_23-.Lmod140_table
        .long   .Lmod140_case_24-.Lmod140_table
        .long   .Lmod140_case_25-.Lmod140_table
        .long   .Lmod140_case_26-.Lmod140_table
        .long   .Lmod140_case_27-.Lmod140_table
        .long   .Lmod140_case_28-.Lmod140_table
        .long   .Lmod140_case_29-.Lmod140_table
        .long   .Lmod140_case_30-.Lmod140_table
        .long   .Lmod140_case_31-.Lmod140_table
        .long   .Lmod140_case_32-.Lmod140_table
        .long   .Lmod140_case_33-.Lmod140_table
        .long   .Lmod140_case_34-.Lmod140_table
        .long   .Lmod140_case_35-.Lmod140_table
        .long   .Lmod140_case_36-.Lmod140_table
        .long   .Lmod140_case_37-.Lmod140_table
        .long   .Lmod140_case_38-.Lmod140_table
        .long   .Lmod140_case_39-.Lmod140_table
        .long   .Lmod140_case_40-.Lmod140_table
        .long   .Lmod140_case_41-.Lmod140_table
        .long   .Lmod140_case_42-.Lmod140_table
        .long   .Lmod140_case_43-.Lmod140_table
        .long   .Lmod140_case_44-.Lmod140_table
        .long   .Lmod140_case_45-.Lmod140_table
        .long   .Lmod140_case_46-.Lmod140_table
        .long   .Lmod140_case_47-.Lmod140_table
        .long   .Lmod140_case_48-.Lmod140_table
        .long   .Lmod140_case_49-.Lmod140_table
        .long   .Lmod140_case_50-.Lmod140_table
        .long   .Lmod140_case_51-.Lmod140_table
        .long   .Lmod140_case_52-.Lmod140_table
        .long   .Lmod140_case_53-.Lmod140_table
        .long   .Lmod140_case_54-.Lmod140_table
        .long   .Lmod140_case_55-.Lmod140_table
        .long   .Lmod140_case_56-.Lmod140_table
        .long   .Lmod140_case_57-.Lmod140_table
        .long   .Lmod140_case_58-.Lmod140_table
        .long   .Lmod140_case_59-.Lmod140_table
        .long   .Lmod140_case_60-.Lmod140_table
        .long   .Lmod140_case_61-.Lmod140_table
        .long   .Lmod140_case_62-.Lmod140_table
        .long   .Lmod140_case_63-.Lmod140_table
        .long   .Lmod140_case_64-.Lmod140_table
        .long   .Lmod140_case_65-.Lmod140_table
        .long   .Lmod140_case_66-.Lmod140_table
        .long   .Lmod140_case_67-.Lmod140_table
        .long   .Lmod140_case_68-.Lmod140_table
        .long   .Lmod140_case_69-.Lmod140_table
        .long   .Lmod140_case_70-.Lmod140_table
        .long   .Lmod140_case_71-.Lmod140_table
        .long   .Lmod140_case_72-.Lmod140_table
        .long   .Lmod140_case_73-.Lmod140_table
        .long   .Lmod140_case_74-.Lmod140_table
        .long   .Lmod140_case_75-.Lmod140_table
        .long   .Lmod140_case_76-.Lmod140_table
        .long   .Lmod140_case_77-.Lmod140_table
        .long   .Lmod140_case_78-.Lmod140_table
        .long   .Lmod140_case_79-.Lmod140_table
        .long   .Lmod140_case_80-.Lmod140_table
        .long   .Lmod140_case_81-.Lmod140_table
        .long   .Lmod140_case_82-.Lmod140_table
        .long   .Lmod140_case_83-.Lmod140_table
        .long   .Lmod140_case_84-.Lmod140_table
        .long   .Lmod140_case_85-.Lmod140_table
        .long   .Lmod140_case_86-.Lmod140_table
        .long   .Lmod140_case_87-.Lmod140_table
        .long   .Lmod140_case_88-.Lmod140_table
        .long   .Lmod140_case_89-.Lmod140_table
        .long   .Lmod140_case_90-.Lmod140_table
        .long   .Lmod140_case_91-.Lmod140_table
        .long   .Lmod140_case_92-.Lmod140_table
        .long   .Lmod140_case_93-.Lmod140_table
        .long   .Lmod140_case_94-.Lmod140_table
        .long   .Lmod140_case_95-.Lmod140_table
        .long   .Lmod140_case_96-.Lmod140_table
        .long   .Lmod140_case_97-.Lmod140_table
        .long   .Lmod140_case_98-.Lmod140_table
        .long   .Lmod140_case_99-.Lmod140_table
        .long   .Lmod140_case_100-.Lmod140_table
        .long   .Lmod140_case_101-.Lmod140_table
        .long   .Lmod140_case_102-.Lmod140_table
        .long   .Lmod140_case_103-.Lmod140_table
        .long   .Lmod140_case_104-.Lmod140_table
        .long   .Lmod140_case_105-.Lmod140_table
        .long   .Lmod140_case_106-.Lmod140_table
        .long   .Lmod140_case_107-.Lmod140_table
        .long   .Lmod140_case_108-.Lmod140_table
        .long   .Lmod140_case_109-.Lmod140_table
        .long   .Lmod140_case_110-.Lmod140_table
        .long   .Lmod140_case_111-.Lmod140_table
        .long   .Lmod140_case_112-.Lmod140_table
        .long   .Lmod140_case_113-.Lmod140_table
        .long   .Lmod140_case_114-.Lmod140_table
        .long   .Lmod140_case_115-.Lmod140_table
        .long   .Lmod140_case_116-.Lmod140_table
        .long   .Lmod140_case_117-.Lmod140_table
        .long   .Lmod140_case_118-.Lmod140_table
        .long   .Lmod140_case_119-.Lmod140_table
        .long   .Lmod140_case_120-.Lmod140_table
        .long   .Lmod140_case_121-.Lmod140_table
        .long   .Lmod140_case_122-.Lmod140_table
        .long   .Lmod140_case_123-.Lmod140_table
        .long   .Lmod140_case_124-.Lmod140_table
        .long   .Lmod140_case_125-.Lmod140_table
        .long   .Lmod140_case_126-.Lmod140_table
        .long   .Lmod140_case_127-.Lmod140_table
        .long   .Lmod140_case_128-.Lmod140_table
        .long   .Lmod140_case_129-.Lmod140_table
        .long   .Lmod140_case_130-.Lmod140_table
        .long   .Lmod140_case_131-.Lmod140_table
        .long   .Lmod140_case_132-.Lmod140_table
        .long   .Lmod140_case_133-.Lmod140_table
        .long   .Lmod140_case_134-.Lmod140_table
        .long   .Lmod140_case_135-.Lmod140_table
        .long   .Lmod140_case_136-.Lmod140_table
        .long   .Lmod140_case_137-.Lmod140_table
        .long   .Lmod140_case_138-.Lmod140_table
        .long   .Lmod140_case_139-.Lmod140_table
        .long   0
// clang's unsigned `% 7` lowering forms the back-multiply as q*8, subtracts
// that from q, and only then adds the original dividend:
//
//   remainder = (q - q*8) + x
//
// This is the same exact x-q*7 identity as `(x+q)-q*8`, but the final producer
// is an ADD.  Candidate discovery may propose seven from this spelling only;
// the exact modulo-recipe relation must still authenticate the reciprocal,
// shift, and shared dividend before the table is published.
        .text
        .globl  jt_modulo_add_after_scaled_difference
        .type   jt_modulo_add_after_scaled_difference,@function
jt_modulo_add_after_scaled_difference:
        movl    %edi, %eax
        movl    %eax, %ecx
        imulq   $0x24924925, %rcx, %rcx
        shrq    $32, %rcx
        movl    %eax, %edx
        subl    %ecx, %edx
        shrl    %edx
        addl    %ecx, %edx
        shrl    $2, %edx
        leal    (,%rdx,8), %ecx
        subl    %ecx, %edx
        addl    %eax, %edx
        leaq    .Ladd_after_scaled_table(%rip), %rax
        movslq  (%rax,%rdx,4), %rdx
        addq    %rax, %rdx
        jmpq    *%rdx
.Ladd_after_scaled_case0:
        movl    $4830, %eax
        retq
.Ladd_after_scaled_case1:
        movl    $4831, %eax
        retq
.Ladd_after_scaled_case2:
        movl    $4832, %eax
        retq
.Ladd_after_scaled_case3:
        movl    $4833, %eax
        retq
.Ladd_after_scaled_case4:
        movl    $4834, %eax
        retq
.Ladd_after_scaled_case5:
        movl    $4835, %eax
        retq
.Ladd_after_scaled_case6:
        movl    $4836, %eax
        retq
        .size   jt_modulo_add_after_scaled_difference, .-jt_modulo_add_after_scaled_difference

        .section .rodata,"a",@progbits
        // Keep the first relocation-backed table away from the section's
        // numeric zero anchor; this fixture exercises modulo occurrence proof,
        // not the separate zero-address table-base boundary.
        .quad 0
        .p2align 3
.Ladd_after_scaled_table:
        .long .Ladd_after_scaled_case0-.Ladd_after_scaled_table
        .long .Ladd_after_scaled_case1-.Ladd_after_scaled_table
        .long .Ladd_after_scaled_case2-.Ladd_after_scaled_table
        .long .Ladd_after_scaled_case3-.Ladd_after_scaled_table
        .long .Ladd_after_scaled_case4-.Ladd_after_scaled_table
        .long .Ladd_after_scaled_case5-.Ladd_after_scaled_table
        .long .Ladd_after_scaled_case6-.Ladd_after_scaled_table

// The commuted final ADD is equally exact: x + (q-q*8).
        .text
        .globl  jt_modulo_add_after_scaled_difference_commuted
        .type   jt_modulo_add_after_scaled_difference_commuted,@function
jt_modulo_add_after_scaled_difference_commuted:
        movl    %edi, %eax
        movl    %eax, %ecx
        imulq   $0x24924925, %rcx, %rcx
        shrq    $32, %rcx
        movl    %eax, %edx
        subl    %ecx, %edx
        shrl    %edx
        addl    %ecx, %edx
        shrl    $2, %edx
        leal    (,%rdx,8), %ecx
        subl    %ecx, %edx
        movl    %eax, %esi
        addl    %edx, %esi
        leaq    .Ladd_after_scaled_commuted_table(%rip), %rax
        movslq  (%rax,%rsi,4), %rsi
        addq    %rax, %rsi
        jmpq    *%rsi
.Ladd_after_scaled_commuted_case0:
        movl    $4840, %eax
        retq
.Ladd_after_scaled_commuted_case1:
        movl    $4841, %eax
        retq
.Ladd_after_scaled_commuted_case2:
        movl    $4842, %eax
        retq
.Ladd_after_scaled_commuted_case3:
        movl    $4843, %eax
        retq
.Ladd_after_scaled_commuted_case4:
        movl    $4844, %eax
        retq
.Ladd_after_scaled_commuted_case5:
        movl    $4845, %eax
        retq
.Ladd_after_scaled_commuted_case6:
        movl    $4846, %eax
        retq
        .size   jt_modulo_add_after_scaled_difference_commuted, .-jt_modulo_add_after_scaled_difference_commuted

        .section .rodata,"a",@progbits
        .p2align 3
.Ladd_after_scaled_commuted_table:
        .long .Ladd_after_scaled_commuted_case0-.Ladd_after_scaled_commuted_table
        .long .Ladd_after_scaled_commuted_case1-.Ladd_after_scaled_commuted_table
        .long .Ladd_after_scaled_commuted_case2-.Ladd_after_scaled_commuted_table
        .long .Ladd_after_scaled_commuted_case3-.Ladd_after_scaled_commuted_table
        .long .Ladd_after_scaled_commuted_case4-.Ladd_after_scaled_commuted_table
        .long .Ladd_after_scaled_commuted_case5-.Ladd_after_scaled_commuted_table
        .long .Ladd_after_scaled_commuted_case6-.Ladd_after_scaled_commuted_table

// A syntactically identical q-q*8 arm derived from a different dividend is a
// proposal only.  It cannot authenticate x+(q(y)-q(y)*8) as x%7.
        .text
        .globl  jt_modulo_add_after_scaled_difference_foreign
        .type   jt_modulo_add_after_scaled_difference_foreign,@function
jt_modulo_add_after_scaled_difference_foreign:
        movl    %esi, %eax
        movl    %eax, %ecx
        imulq   $0x24924925, %rcx, %rcx
        shrq    $32, %rcx
        movl    %eax, %edx
        subl    %ecx, %edx
        shrl    %edx
        addl    %ecx, %edx
        shrl    $2, %edx
        leal    (,%rdx,8), %ecx
        subl    %ecx, %edx
        addl    %edi, %edx
        leaq    .Ladd_after_scaled_foreign_table(%rip), %rax
        movq    (%rax,%rdx,8), %rdx
        jmpq    *%rdx
.Ladd_after_scaled_foreign_case0:
        movl    $4850, %eax
        retq
.Ladd_after_scaled_foreign_case1:
        movl    $4851, %eax
        retq
.Ladd_after_scaled_foreign_case2:
        movl    $4852, %eax
        retq
.Ladd_after_scaled_foreign_case3:
        movl    $4853, %eax
        retq
.Ladd_after_scaled_foreign_case4:
        movl    $4854, %eax
        retq
.Ladd_after_scaled_foreign_case5:
        movl    $4855, %eax
        retq
.Ladd_after_scaled_foreign_case6:
        movl    $4856, %eax
        retq
        .size   jt_modulo_add_after_scaled_difference_foreign, .-jt_modulo_add_after_scaled_difference_foreign

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Ladd_after_scaled_foreign_table:
        .quad .Ladd_after_scaled_foreign_case0
        .quad .Ladd_after_scaled_foreign_case1
        .quad .Ladd_after_scaled_foreign_case2
        .quad .Ladd_after_scaled_foreign_case3
        .quad .Ladd_after_scaled_foreign_case4
        .quad .Ladd_after_scaled_foreign_case5
        .quad .Ladd_after_scaled_foreign_case6

// The exact producer is `% 7`, but the relocation run exposes only five
// physical slots.  Capacity five may be proposed to the structural theorem,
// but it does not match the producer's divisor; the real bound seven is in
// turn larger than the table.  Neither number may publish this malformed
// dispatch.
        .text
        .globl  jt_modulo_add_after_scaled_difference_wrong_capacity
        .type   jt_modulo_add_after_scaled_difference_wrong_capacity,@function
jt_modulo_add_after_scaled_difference_wrong_capacity:
        movl    %edi, %eax
        movl    %eax, %ecx
        imulq   $0x24924925, %rcx, %rcx
        shrq    $32, %rcx
        movl    %eax, %edx
        subl    %ecx, %edx
        shrl    %edx
        addl    %ecx, %edx
        shrl    $2, %edx
        leal    (,%rdx,8), %ecx
        subl    %ecx, %edx
        addl    %eax, %edx
        leaq    .Ladd_after_scaled_wrong_capacity_table(%rip), %rax
        movq    (%rax,%rdx,8), %rdx
        jmpq    *%rdx
.Ladd_after_scaled_wrong_capacity_case0:
        movl    $4860, %eax
        retq
.Ladd_after_scaled_wrong_capacity_case1:
        movl    $4861, %eax
        retq
.Ladd_after_scaled_wrong_capacity_case2:
        movl    $4862, %eax
        retq
.Ladd_after_scaled_wrong_capacity_case3:
        movl    $4863, %eax
        retq
.Ladd_after_scaled_wrong_capacity_case4:
        movl    $4864, %eax
        retq
        .size   jt_modulo_add_after_scaled_difference_wrong_capacity, .-jt_modulo_add_after_scaled_difference_wrong_capacity

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Ladd_after_scaled_wrong_capacity_table:
        .quad .Ladd_after_scaled_wrong_capacity_case0
        .quad .Ladd_after_scaled_wrong_capacity_case1
        .quad .Ladd_after_scaled_wrong_capacity_case2
        .quad .Ladd_after_scaled_wrong_capacity_case3
        .quad .Ladd_after_scaled_wrong_capacity_case4

// clang's x86 `% 6u` lowering keeps the quotient in a 64-bit container, then
// spells q*6 as low32(3*zext32(2*q)).  Low-bit ring normalization must prove
// that exact factorized back-multiply without admitting a wrong factor,
// reciprocal, or post-shift.
        .text
        .globl  jt_modulo_factorized_six
        .type   jt_modulo_factorized_six,@function
jt_modulo_factorized_six:
        movl    %edi, %eax
        movl    $0xaaaaaaab, %ecx
        imulq   %rax, %rcx
        shrq    $34, %rcx
        addl    %ecx, %ecx
        leal    (%rcx,%rcx,2), %edx
        subl    %edx, %eax
        leaq    .Lfactorized_six_table(%rip), %rcx
        movq    (%rcx,%rax,8), %rdx
        jmpq    *%rdx
.Lfactorized_six_case0:
        movl    $4870, %eax
        retq
.Lfactorized_six_case1:
        movl    $4871, %eax
        retq
.Lfactorized_six_case2:
        movl    $4872, %eax
        retq
.Lfactorized_six_case3:
        movl    $4873, %eax
        retq
.Lfactorized_six_case4:
        movl    $4874, %eax
        retq
.Lfactorized_six_case5:
        movl    $4875, %eax
        retq
        .size   jt_modulo_factorized_six, .-jt_modulo_factorized_six

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lfactorized_six_table:
        .quad .Lfactorized_six_case0
        .quad .Lfactorized_six_case1
        .quad .Lfactorized_six_case2
        .quad .Lfactorized_six_case3
        .quad .Lfactorized_six_case4
        .quad .Lfactorized_six_case5

        .text
        .globl  jt_modulo_factorized_six_wrong_factor
        .type   jt_modulo_factorized_six_wrong_factor,@function
jt_modulo_factorized_six_wrong_factor:
        movl    %edi, %eax
        movl    $0xaaaaaaab, %ecx
        imulq   %rax, %rcx
        shrq    $34, %rcx
        addl    %ecx, %ecx
        leal    (%rcx,%rcx,4), %edx
        subl    %edx, %eax
        leaq    .Lfactorized_six_wrong_factor_table(%rip), %rcx
        movq    (%rcx,%rax,8), %rdx
        jmpq    *%rdx
.Lfactorized_six_wrong_factor_case0:
        movl    $4880, %eax
        retq
.Lfactorized_six_wrong_factor_case1:
        movl    $4881, %eax
        retq
.Lfactorized_six_wrong_factor_case2:
        movl    $4882, %eax
        retq
.Lfactorized_six_wrong_factor_case3:
        movl    $4883, %eax
        retq
.Lfactorized_six_wrong_factor_case4:
        movl    $4884, %eax
        retq
.Lfactorized_six_wrong_factor_case5:
        movl    $4885, %eax
        retq
        .size   jt_modulo_factorized_six_wrong_factor, .-jt_modulo_factorized_six_wrong_factor

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lfactorized_six_wrong_factor_table:
        .quad .Lfactorized_six_wrong_factor_case0
        .quad .Lfactorized_six_wrong_factor_case1
        .quad .Lfactorized_six_wrong_factor_case2
        .quad .Lfactorized_six_wrong_factor_case3
        .quad .Lfactorized_six_wrong_factor_case4
        .quad .Lfactorized_six_wrong_factor_case5

        .text
        .globl  jt_modulo_factorized_six_wrong_magic
        .type   jt_modulo_factorized_six_wrong_magic,@function
jt_modulo_factorized_six_wrong_magic:
        movl    %edi, %eax
        movl    $0xaaaaaaac, %ecx
        imulq   %rax, %rcx
        shrq    $34, %rcx
        addl    %ecx, %ecx
        leal    (%rcx,%rcx,2), %edx
        subl    %edx, %eax
        leaq    .Lfactorized_six_wrong_magic_table(%rip), %rcx
        movq    (%rcx,%rax,8), %rdx
        jmpq    *%rdx
.Lfactorized_six_wrong_magic_case0:
        movl    $4890, %eax
        retq
.Lfactorized_six_wrong_magic_case1:
        movl    $4891, %eax
        retq
.Lfactorized_six_wrong_magic_case2:
        movl    $4892, %eax
        retq
.Lfactorized_six_wrong_magic_case3:
        movl    $4893, %eax
        retq
.Lfactorized_six_wrong_magic_case4:
        movl    $4894, %eax
        retq
.Lfactorized_six_wrong_magic_case5:
        movl    $4895, %eax
        retq
        .size   jt_modulo_factorized_six_wrong_magic, .-jt_modulo_factorized_six_wrong_magic

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lfactorized_six_wrong_magic_table:
        .quad .Lfactorized_six_wrong_magic_case0
        .quad .Lfactorized_six_wrong_magic_case1
        .quad .Lfactorized_six_wrong_magic_case2
        .quad .Lfactorized_six_wrong_magic_case3
        .quad .Lfactorized_six_wrong_magic_case4
        .quad .Lfactorized_six_wrong_magic_case5

        .text
        .globl  jt_modulo_factorized_six_wrong_postshift
        .type   jt_modulo_factorized_six_wrong_postshift,@function
jt_modulo_factorized_six_wrong_postshift:
        movl    %edi, %eax
        movl    $0xaaaaaaab, %ecx
        imulq   %rax, %rcx
        shrq    $33, %rcx
        addl    %ecx, %ecx
        leal    (%rcx,%rcx,2), %edx
        subl    %edx, %eax
        leaq    .Lfactorized_six_wrong_postshift_table(%rip), %rcx
        movq    (%rcx,%rax,8), %rdx
        jmpq    *%rdx
.Lfactorized_six_wrong_postshift_case0:
        movl    $4900, %eax
        retq
.Lfactorized_six_wrong_postshift_case1:
        movl    $4901, %eax
        retq
.Lfactorized_six_wrong_postshift_case2:
        movl    $4902, %eax
        retq
.Lfactorized_six_wrong_postshift_case3:
        movl    $4903, %eax
        retq
.Lfactorized_six_wrong_postshift_case4:
        movl    $4904, %eax
        retq
.Lfactorized_six_wrong_postshift_case5:
        movl    $4905, %eax
        retq
        .size   jt_modulo_factorized_six_wrong_postshift, .-jt_modulo_factorized_six_wrong_postshift

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
.Lfactorized_six_wrong_postshift_table:
        .quad .Lfactorized_six_wrong_postshift_case0
        .quad .Lfactorized_six_wrong_postshift_case1
        .quad .Lfactorized_six_wrong_postshift_case2
        .quad .Lfactorized_six_wrong_postshift_case3
        .quad .Lfactorized_six_wrong_postshift_case4
        .quad .Lfactorized_six_wrong_postshift_case5

        .section .note.GNU-stack,"",@progbits

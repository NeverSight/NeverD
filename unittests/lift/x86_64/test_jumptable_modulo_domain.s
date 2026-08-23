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

        .section .note.GNU-stack,"",@progbits

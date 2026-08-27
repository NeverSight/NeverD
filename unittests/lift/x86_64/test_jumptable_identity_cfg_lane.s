        .text

// Both diamond arms define the table-index register from the same guarded
// value.  A CFG-aware identity proof must merge the equal definitions and keep
// the inclusive guard's final case.
        .globl  jt_identity_diamond_agree
        .type   jt_identity_diamond_agree,@function
jt_identity_diamond_agree:
        cmpl    $2, %edi
        ja      .Ldiamond_agree_default
        testl   %esi, %esi
        jne     .Ldiamond_agree_right
        movl    %edi, %r10d
        jmp     .Ldiamond_agree_join
.Ldiamond_agree_right:
        movl    %edi, %r10d
.Ldiamond_agree_join:
        leaq    .Ldiamond_agree_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Ldiamond_agree_case0:
        movl    $500, %eax
        retq
.Ldiamond_agree_case1:
        movl    $501, %eax
        retq
.Ldiamond_agree_case2:
        movl    $502, %eax
        retq
.Ldiamond_agree_default:
        movl    $599, %eax
        retq
        .size   jt_identity_diamond_agree, .-jt_identity_diamond_agree

// The two diamond arms define R10 from different inputs.  Each arm has a real
// exclusive two-entry guard, while only the left arm also contains a redundant
// inclusive comparison.  That sibling-only comparison must not add a third
// slot at the join; both legal cases must remain recoverable.
        .globl  jt_identity_diamond_ambiguous
        .type   jt_identity_diamond_ambiguous,@function
jt_identity_diamond_ambiguous:
        cmpl    $2, %edi
        jae     .Ldiamond_ambiguous_default
        cmpl    $2, %edx
        jae     .Ldiamond_ambiguous_default
        // This redundant inclusive guard dominates the join but constrains
        // only EDX.  A linear backward scan chooses the later right-arm EDX
        // definition of R10 and wrongly applies +1 to both arms; a CFG merge
        // must retain the EDI/EDX ambiguity and reject that association.
        cmpl    $2, %edx
        ja      .Ldiamond_ambiguous_default
        testl   %esi, %esi
        jne     .Ldiamond_ambiguous_right
        movl    %edi, %r10d
        jmp     .Ldiamond_ambiguous_join
.Ldiamond_ambiguous_right:
        movl    %edx, %r10d
.Ldiamond_ambiguous_join:
        // Bound the actual merged value so both legal entries remain
        // recoverable after the ambiguous EDI/EDX provenance rejects the
        // earlier inclusive EDX comparison.
        cmpl    $2, %r10d
        jae     .Ldiamond_ambiguous_default
        leaq    .Ldiamond_ambiguous_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Ldiamond_ambiguous_case0:
        movl    $600, %eax
        retq
.Ldiamond_ambiguous_case1:
        movl    $601, %eax
        retq
.Ldiamond_ambiguous_poison:
        movl    $699, %eax
        retq
.Ldiamond_ambiguous_default:
        movl    $698, %eax
        retq
        .size   jt_identity_diamond_ambiguous, .-jt_identity_diamond_ambiguous

// AH is a non-overlapping lane while AL remains the guarded/indexed value.
// Reaching-value analysis must preserve AL through the AH write.
        .globl  jt_identity_ah_nonoverlap
        .type   jt_identity_ah_nonoverlap,@function
jt_identity_ah_nonoverlap:
        movb    %dil, %al
        cmpb    $2, %al
        ja      .Lah_nonoverlap_default
        movl    %esi, %ecx
        movb    %cl, %ah
        movzbl  %al, %r10d
        leaq    .Lah_nonoverlap_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lah_nonoverlap_case0:
        movl    $700, %eax
        retq
.Lah_nonoverlap_case1:
        movl    $701, %eax
        retq
.Lah_nonoverlap_case2:
        movl    $702, %eax
        retq
.Lah_nonoverlap_default:
        movl    $799, %eax
        retq
        .size   jt_identity_ah_nonoverlap, .-jt_identity_ah_nonoverlap

// Here AH overlaps the guarded EAX value.  A later exclusive comparison of
// the modified EAX is the only sound two-entry bound; the pre-write inclusive
// comparison must not expose the physically adjacent poison slot.
        .globl  jt_identity_ah_overlap
        .type   jt_identity_ah_overlap,@function
jt_identity_ah_overlap:
        movl    %edi, %eax
        cmpl    $2, %eax
        ja      .Lah_overlap_default
        movl    %esi, %ecx
        movb    %cl, %ah
        cmpl    $2, %eax
        jae     .Lah_overlap_default
        movl    %eax, %r10d
        leaq    .Lah_overlap_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lah_overlap_case0:
        movl    $800, %eax
        retq
.Lah_overlap_case1:
        movl    $801, %eax
        retq
.Lah_overlap_poison:
        movl    $899, %eax
        retq
.Lah_overlap_default:
        movl    $898, %eax
        retq
        .size   jt_identity_ah_overlap, .-jt_identity_ah_overlap

// A signed non-negative range and an explicit MOVSLQ describe the same value.
// The signed inclusive guard must therefore retain all three cases.
        .globl  jt_identity_sext_agree
        .type   jt_identity_sext_agree,@function
jt_identity_sext_agree:
        testl   %edi, %edi
        js      .Lsext_agree_default
        cmpl    $2, %edi
        jg      .Lsext_agree_default
        movslq  %edi, %r10
        leaq    .Lsext_agree_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lsext_agree_case0:
        movl    $900, %eax
        retq
.Lsext_agree_case1:
        movl    $901, %eax
        retq
.Lsext_agree_case2:
        movl    $902, %eax
        retq
.Lsext_agree_default:
        movl    $999, %eax
        retq
        .size   jt_identity_sext_agree, .-jt_identity_sext_agree

// The earlier unsigned inclusive comparison constrains a zero-extended view,
// not the sign-extended RAX used by the table.  A later exact 64-bit exclusive
// guard supplies the real two-entry bound; slot 2 remains poison.
        .globl  jt_identity_zext_sext_mismatch
        .type   jt_identity_zext_sext_mismatch,@function
jt_identity_zext_sext_mismatch:
        movl    %edi, %eax
        cmpl    $2, %eax
        ja      .Lext_mismatch_default
        movslq  %eax, %rax
        cmpq    $2, %rax
        jae     .Lext_mismatch_default
        leaq    .Lext_mismatch_table(%rip), %r11
        movslq  (%r11,%rax,4), %rcx
        addq    %r11, %rcx
        jmpq    *%rcx
.Lext_mismatch_case0:
        movl    $1000, %eax
        retq
.Lext_mismatch_case1:
        movl    $1001, %eax
        retq
.Lext_mismatch_poison:
        movl    $1099, %eax
        retq
.Lext_mismatch_default:
        movl    $1098, %eax
        retq
        .size   jt_identity_zext_sext_mismatch, .-jt_identity_zext_sext_mismatch

// -O0-style spill: the guard and table address use distinct reloads of the
// same exact frame slot.  Identity must follow the reaching STORE, not compare
// the two LOAD result registers.
        .globl  jt_identity_spill_reload
        .type   jt_identity_spill_reload,@function
jt_identity_spill_reload:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movl    %edi, -4(%rbp)
        movl    -4(%rbp), %r10d
        cmpl    $2, %r10d
        ja      .Lspill_default
        movl    -4(%rbp), %r10d
        leaq    .Lspill_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lspill_case0:
        movl    $1100, %eax
        leave
        retq
.Lspill_case1:
        movl    $1101, %eax
        leave
        retq
.Lspill_case2:
        movl    $1102, %eax
        leave
        retq
.Lspill_default:
        movl    $1199, %eax
        leave
        retq
        .size   jt_identity_spill_reload, .-jt_identity_spill_reload

// The first complete-CFG pass has not decoded the table targets yet.  Case 0
// adds a backedge that redefines the guarded source before re-entering the
// dispatch.  A proof-dependent table must be revalidated after target
// exploration: the entry-only inclusive guard no longer describes every
// iteration, while the per-iteration exclusive guard still proves two slots.
        .globl  jt_identity_case_backedge
        .type   jt_identity_case_backedge,@function
jt_identity_case_backedge:
        cmpl    $2, %edi
        ja      .Lcase_backedge_default
.Lcase_backedge_dispatch:
        cmpl    $2, %edi
        jae     .Lcase_backedge_default
        movl    %edi, %r10d
        leaq    .Lcase_backedge_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lcase_backedge_case0:
        movl    $1200, %eax
        movl    %esi, %edi
        jmp     .Lcase_backedge_dispatch
.Lcase_backedge_case1:
        movl    $1201, %eax
        retq
.Lcase_backedge_poison:
        movl    $1299, %eax
        retq
.Lcase_backedge_default:
        movl    $1298, %eax
        retq
        .size   jt_identity_case_backedge, .-jt_identity_case_backedge

// An atomic RMW is a reaching memory definition even when its result is not
// otherwise consumed.  Ignoring it makes the reload look equal to the older
// guarded store and wrongly applies that guard's inclusive +1.
        .globl  jt_identity_atomic_overwrite
        .type   jt_identity_atomic_overwrite,@function
jt_identity_atomic_overwrite:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movl    %edi, -4(%rbp)
        movl    -4(%rbp), %eax
        cmpl    $2, %eax
        ja      .Latomic_overwrite_default
        lock xaddl %esi, -4(%rbp)
        movl    -4(%rbp), %r10d
        cmpl    $2, %r10d
        jae     .Latomic_overwrite_default
        leaq    .Latomic_overwrite_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Latomic_overwrite_case0:
        movl    $1300, %eax
        leave
        retq
.Latomic_overwrite_case1:
        movl    $1301, %eax
        leave
        retq
.Latomic_overwrite_poison:
        movl    $1399, %eax
        leave
        retq
.Latomic_overwrite_default:
        movl    $1398, %eax
        leave
        retq
        .size   jt_identity_atomic_overwrite, .-jt_identity_atomic_overwrite

// RSP+12 and RBP-4 name the same frame slot after this prologue, but a
// physical-base-only key treats them as separate domains.  The intervening FP
// store must therefore be a may-alias barrier for an SP-keyed reload proof.
        .globl  jt_identity_cross_frame_overwrite
        .type   jt_identity_cross_frame_overwrite,@function
jt_identity_cross_frame_overwrite:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movl    %edi, 12(%rsp)
        movl    12(%rsp), %eax
        cmpl    $2, %eax
        ja      .Lcross_frame_default
        movl    %esi, -4(%rbp)
        movl    12(%rsp), %r10d
        cmpl    $2, %r10d
        jae     .Lcross_frame_default
        leaq    .Lcross_frame_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lcross_frame_case0:
        movl    $1400, %eax
        leave
        retq
.Lcross_frame_case1:
        movl    $1401, %eax
        leave
        retq
.Lcross_frame_poison:
        movl    $1499, %eax
        leave
        retq
.Lcross_frame_default:
        movl    $1498, %eax
        leave
        retq
        .size   jt_identity_cross_frame_overwrite, .-jt_identity_cross_frame_overwrite

// Once the slot address escapes to an ordinary call, LowIR has no callee
// memory summary that can prove the old spill survived.  The post-call exact
// guard still bounds the two legal entries; the pre-call inclusive guard must
// not expose the adjacent poison entry.
        .globl  jt_identity_call_overwrite
        .type   jt_identity_call_overwrite,@function
jt_identity_call_overwrite:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movl    %edi, -4(%rbp)
        movl    -4(%rbp), %eax
        cmpl    $2, %eax
        ja      .Lcall_overwrite_default
        leaq    -4(%rbp), %rdi
        callq   .Lcall_overwrite_mutate
        movl    -4(%rbp), %r10d
        cmpl    $2, %r10d
        jae     .Lcall_overwrite_default
        leaq    .Lcall_overwrite_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lcall_overwrite_case0:
        movl    $1600, %eax
        leave
        retq
.Lcall_overwrite_case1:
        movl    $1601, %eax
        leave
        retq
.Lcall_overwrite_poison:
        movl    $1699, %eax
        leave
        retq
.Lcall_overwrite_default:
        movl    $1698, %eax
        leave
        retq
.Lcall_overwrite_mutate:
        movl    %esi, (%rdi)
        retq
        .size   jt_identity_call_overwrite, .-jt_identity_call_overwrite

// A conditional address can name either the exact frame slot or a disjoint
// global.  The STORE is therefore a may-alias definition of the slot; treating
// an unsupported SELECT address as non-frame would reuse the stale spill.
        .globl  jt_identity_select_store_alias
        .type   jt_identity_select_store_alias,@function
jt_identity_select_store_alias:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movl    %edi, -4(%rbp)
        movl    -4(%rbp), %eax
        cmpl    $2, %eax
        ja      .Lselect_store_default
        leaq    -4(%rbp), %rax
        leaq    .Lselect_store_global(%rip), %rcx
        testl   %edx, %edx
        cmovneq %rcx, %rax
        movl    %esi, (%rax)
        movl    -4(%rbp), %r10d
        cmpl    $2, %r10d
        jae     .Lselect_store_default
        leaq    .Lselect_store_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lselect_store_case0:
        movl    $1700, %eax
        leave
        retq
.Lselect_store_case1:
        movl    $1701, %eax
        leave
        retq
.Lselect_store_poison:
        movl    $1799, %eax
        leave
        retq
.Lselect_store_default:
        movl    $1798, %eax
        leave
        retq
        .size   jt_identity_select_store_alias, .-jt_identity_select_store_alias

// FLAGS reaching the join are path-dependent: only the right arm compares the
// table source, while the left arm leaves unrelated TEST flags.  A flattened
// address-order scan would pick the later CMP and certify an inclusive third
// slot.  The post-join mask is the only path-global two-slot bound.
        .globl  jt_identity_flags_diamond
        .type   jt_identity_flags_diamond,@function
jt_identity_flags_diamond:
        testl   %esi, %esi
        jne     .Lflags_diamond_right
        testl   %edx, %edx
        jmp     .Lflags_diamond_join
.Lflags_diamond_right:
        cmpl    $2, %edi
.Lflags_diamond_join:
        ja      .Lflags_diamond_default
        andl    $1, %edi
        movl    %edi, %r10d
        leaq    .Lflags_diamond_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lflags_diamond_case0:
        movl    $1800, %eax
        retq
.Lflags_diamond_case1:
        movl    $1801, %eax
        retq
.Lflags_diamond_poison:
        movl    $1899, %eax
        retq
.Lflags_diamond_default:
        movl    $1898, %eax
        retq
        .size   jt_identity_flags_diamond, .-jt_identity_flags_diamond

// The condition contains CF+ZF but its table-reaching polarity is idx>2, not
// idx<=2.  It is a lower filter and cannot turn a separate two-slot mask into
// an inclusive three-entry upper bound.
        .globl  jt_identity_reversed_polarity
        .type   jt_identity_reversed_polarity,@function
jt_identity_reversed_polarity:
        cmpl    $2, %edi
        jbe     .Lreversed_polarity_default
        andl    $1, %edi
        movl    %edi, %r10d
        leaq    .Lreversed_polarity_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lreversed_polarity_case0:
        movl    $1900, %eax
        retq
.Lreversed_polarity_case1:
        movl    $1901, %eax
        retq
.Lreversed_polarity_poison:
        movl    $1999, %eax
        retq
.Lreversed_polarity_default:
        movl    $1998, %eax
        retq
        .size   jt_identity_reversed_polarity, .-jt_identity_reversed_polarity

// Both guards constrain the exact same value and bound.  The first permits
// equality (idx<=2), but the later stronger guard admits only idx<2.  Bounds
// along one path intersect, so the weaker inclusive evidence cannot expose
// the physically adjacent third slot.
        .globl  jt_identity_redundant_weaker_inclusive
        .type   jt_identity_redundant_weaker_inclusive,@function
jt_identity_redundant_weaker_inclusive:
        cmpl    $2, %edi
        ja      .Lredundant_weaker_default
        cmpl    $2, %edi
        jae     .Lredundant_weaker_default
        movl    %edi, %r10d
        leaq    .Lredundant_weaker_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lredundant_weaker_case0:
        movl    $2000, %eax
        retq
.Lredundant_weaker_case1:
        movl    $2001, %eax
        retq
.Lredundant_weaker_poison:
        movl    $2099, %eax
        retq
.Lredundant_weaker_default:
        movl    $2098, %eax
        retq
        .size   jt_identity_redundant_weaker_inclusive, .-jt_identity_redundant_weaker_inclusive

// The table index is the four-value mask produced before the LOAD.  The same
// physical register is reused afterwards for an unrelated two-value mask and
// followed by another LOAD.  A function-prefix/"last LOAD" scan binds the
// later lifetime and silently drops legal cases 2 and 3; occurrence-level
// evidence must stay anchored at the table-address use.
        .globl  jt_identity_postload_mask_lifetime
        .type   jt_identity_postload_mask_lifetime,@function
jt_identity_postload_mask_lifetime:
        movl    %edi, %r10d
        andl    $3, %r10d
        leaq    .Lpostload_mask_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        movl    %esi, %r10d
        andl    $1, %r10d
        movl    (%rsp), %r11d
        jmpq    *%rcx
.Lpostload_mask_case0:
        movl    $2100, %eax
        retq
.Lpostload_mask_case1:
        movl    $2101, %eax
        retq
.Lpostload_mask_case2:
        movl    $2102, %eax
        retq
.Lpostload_mask_case3:
        movl    $2103, %eax
        retq
        .size   jt_identity_postload_mask_lifetime, .-jt_identity_postload_mask_lifetime

// Two distinct stack epochs use the same physical [rsp] spelling.  The old
// epoch stores x&7 at [sp0], while y&1 is stored at [sp0-16] before SP moves;
// after `sp -= 16`, the dispatch reload observes y&1.  A physical
// (SP-register, displacement) key incorrectly forwards x&7 and exposes the
// six adjacent poison slots.
        .globl  jt_identity_frame_epoch_mask
        .type   jt_identity_frame_epoch_mask,@function
jt_identity_frame_epoch_mask:
        subq    $32, %rsp
        movl    %edi, %eax
        andl    $7, %eax
        movl    %eax, (%rsp)
        movl    %esi, %ecx
        andl    $1, %ecx
        movl    %ecx, -16(%rsp)
        subq    $16, %rsp
        movl    (%rsp), %r10d
        leaq    .Lframe_epoch_mask_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        addq    $48, %rsp
        jmpq    *%rcx
.Lframe_epoch_mask_case0:
        movl    $2200, %eax
        retq
.Lframe_epoch_mask_case1:
        movl    $2201, %eax
        retq
.Lframe_epoch_mask_poison2:
        movl    $2292, %eax
        retq
.Lframe_epoch_mask_poison3:
        movl    $2293, %eax
        retq
.Lframe_epoch_mask_poison4:
        movl    $2294, %eax
        retq
.Lframe_epoch_mask_poison5:
        movl    $2295, %eax
        retq
.Lframe_epoch_mask_poison6:
        movl    $2296, %eax
        retq
.Lframe_epoch_mask_poison7:
        movl    $2297, %eax
        retq
        .size   jt_identity_frame_epoch_mask, .-jt_identity_frame_epoch_mask

// The authenticated table coordinate is `(x & 7) + 1`, with physical slots
// 1..8 reachable.  The adjacent tenth relocation is another valid code target
// but not part of this dispatch.  Exact transform evidence must preserve the
// +1 while preventing the relocation run from absorbing that poison slot.
        .globl  jt_identity_mask_offset
        .type   jt_identity_mask_offset,@function
jt_identity_mask_offset:
        movl    %edi, %r10d
        andl    $7, %r10d
        addl    $1, %r10d
        leaq    .Lmask_offset_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_offset_case0:
        movl    $2400, %eax
        retq
.Lmask_offset_case1:
        movl    $2401, %eax
        retq
.Lmask_offset_case2:
        movl    $2402, %eax
        retq
.Lmask_offset_case3:
        movl    $2403, %eax
        retq
.Lmask_offset_case4:
        movl    $2404, %eax
        retq
.Lmask_offset_case5:
        movl    $2405, %eax
        retq
.Lmask_offset_case6:
        movl    $2406, %eax
        retq
.Lmask_offset_case7:
        movl    $2407, %eax
        retq
.Lmask_offset_case8:
        movl    $2408, %eax
        retq
.Lmask_offset_poison:
        movl    $2499, %eax
        retq
        .size   jt_identity_mask_offset, .-jt_identity_mask_offset

// `(x & 7) - 1` has no finite non-negative domain without a lower-bound
// certificate: x&7==0 wraps to UINT32_MAX and indexes the slot immediately
// before the nominal table.  An upper-bound-only mask proof must leave the
// dispatch unresolved rather than publish the seven non-negative slots.
        .globl  jt_identity_mask_negative_offset
        .type   jt_identity_mask_negative_offset,@function
jt_identity_mask_negative_offset:
        movl    %edi, %r10d
        andl    $7, %r10d
        subl    $1, %r10d
        leaq    .Lmask_negative_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_negative_case0:
        movl    $2500, %eax
        retq
.Lmask_negative_case1:
        movl    $2501, %eax
        retq
.Lmask_negative_case2:
        movl    $2502, %eax
        retq
.Lmask_negative_case3:
        movl    $2503, %eax
        retq
.Lmask_negative_case4:
        movl    $2504, %eax
        retq
.Lmask_negative_case5:
        movl    $2505, %eax
        retq
.Lmask_negative_case6:
        movl    $2506, %eax
        retq
.Lmask_negative_poison:
        movl    $2599, %eax
        retq
        .size   jt_identity_mask_negative_offset, .-jt_identity_mask_negative_offset

// Two checked translations remain a complete finite domain.  The proof must
// close transitively rather than recognizing only the first ADD and then
// treating the second as an opaque reason to discard an otherwise valid table.
        .globl  jt_identity_mask_double_offset
        .type   jt_identity_mask_double_offset,@function
jt_identity_mask_double_offset:
        movl    %edi, %r10d
        andl    $3, %r10d
        addl    $1, %r10d
        addl    $1, %r10d
        leaq    .Lmask_double_offset_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_double_offset_case0:
        movl    $2600, %eax
        retq
.Lmask_double_offset_case1:
        movl    $2601, %eax
        retq
.Lmask_double_offset_case2:
        movl    $2602, %eax
        retq
.Lmask_double_offset_case3:
        movl    $2603, %eax
        retq
.Lmask_double_offset_case4:
        movl    $2604, %eax
        retq
.Lmask_double_offset_case5:
        movl    $2605, %eax
        retq
.Lmask_double_offset_poison:
        movl    $2699, %eax
        retq
        .size   jt_identity_mask_double_offset, .-jt_identity_mask_double_offset

// An attacker-controlled second operand destroys the finite mask domain.  The
// adjacent relocation run is deliberately decodable, so losing the dependency
// edge would publish a switch instead of leaving the indirect branch intact.
        .globl  jt_identity_mask_unknown_offset
        .type   jt_identity_mask_unknown_offset,@function
jt_identity_mask_unknown_offset:
        movl    %edi, %r10d
        andl    $3, %r10d
        addl    %esi, %r10d
        leaq    .Lmask_unknown_offset_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_unknown_offset_case0:
        movl    $2700, %eax
        retq
.Lmask_unknown_offset_case1:
        movl    $2701, %eax
        retq
.Lmask_unknown_offset_case2:
        movl    $2702, %eax
        retq
.Lmask_unknown_offset_case3:
        movl    $2703, %eax
        retq
.Lmask_unknown_offset_poison:
        movl    $2799, %eax
        retq
        .size   jt_identity_mask_unknown_offset, .-jt_identity_mask_unknown_offset

// A runtime mask is still a mask producer, but it proves no static finite
// domain.  Candidates.empty() must not make the resolver forget that the real
// table index depends on it and then borrow the adjacent relocation run.
        .globl  jt_identity_runtime_mask
        .type   jt_identity_runtime_mask,@function
jt_identity_runtime_mask:
        movl    %edi, %r10d
        andl    %esi, %r10d
        leaq    .Lruntime_mask_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lruntime_mask_case0:
        movl    $2800, %eax
        retq
.Lruntime_mask_case1:
        movl    $2801, %eax
        retq
.Lruntime_mask_case2:
        movl    $2802, %eax
        retq
.Lruntime_mask_case3:
        movl    $2803, %eax
        retq
.Lruntime_mask_poison:
        movl    $2899, %eax
        retq
        .size   jt_identity_runtime_mask, .-jt_identity_runtime_mask

// Byte arithmetic wraps before the zero-extension consumed by the table
// address.  `(x & 0xff) + 1` therefore still covers all 256 byte values; it is
// not a 257-entry non-wrapping interval and a short relocation run cannot make
// it one.
        .globl  jt_identity_mask_byte_wrap
        .type   jt_identity_mask_byte_wrap,@function
jt_identity_mask_byte_wrap:
        movb    %dil, %r10b
        andb    $0xff, %r10b
        addb    $1, %r10b
        movzbl  %r10b, %r10d
        leaq    .Lmask_byte_wrap_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_byte_wrap_case0:
        movl    $2900, %eax
        retq
.Lmask_byte_wrap_case1:
        movl    $2901, %eax
        retq
.Lmask_byte_wrap_case2:
        movl    $2902, %eax
        retq
.Lmask_byte_wrap_case3:
        movl    $2903, %eax
        retq
.Lmask_byte_wrap_poison:
        movl    $2999, %eax
        retq
        .size   jt_identity_mask_byte_wrap, .-jt_identity_mask_byte_wrap

// Only R10B is masked; the remaining bytes of R10D still come from the
// attacker-controlled input.  A partial-register resolver that replaces the
// composite value with one opaque identity loses the dependency on the narrow
// AND and lets the three-entry relocation run masquerade as the index domain.
        .globl  jt_identity_mask_partial_wide
        .type   jt_identity_mask_partial_wide,@function
jt_identity_mask_partial_wide:
        movl    %edi, %r10d
        andb    $1, %r10b
        leaq    .Lmask_partial_wide_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_partial_wide_case0:
        movl    $3000, %eax
        retq
.Lmask_partial_wide_case1:
        movl    $3001, %eax
        retq
.Lmask_partial_wide_poison:
        movl    $3099, %eax
        retq
        .size   jt_identity_mask_partial_wide, .-jt_identity_mask_partial_wide

// Both masks execute unconditionally on the exact table-index lifetime.  The
// second (outer) AND admits eight bit patterns in isolation, but its input is
// already confined to {0,1}; complete domains intersect, so only the first two
// slots are reachable.  Six adjacent valid targets make an early outer-mask
// return observably wrong rather than naturally terminating the table scan.
        .globl  jt_identity_nested_mask_intersection
        .type   jt_identity_nested_mask_intersection,@function
jt_identity_nested_mask_intersection:
        movl    %edi, %r10d
        andl    $1, %r10d
        andl    $7, %r10d
        leaq    .Lnested_mask_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lnested_mask_case0:
        movl    $3100, %eax
        retq
.Lnested_mask_case1:
        movl    $3101, %eax
        retq
.Lnested_mask_poison2:
        movl    $3192, %eax
        retq
.Lnested_mask_poison3:
        movl    $3193, %eax
        retq
.Lnested_mask_poison4:
        movl    $3194, %eax
        retq
.Lnested_mask_poison5:
        movl    $3195, %eax
        retq
.Lnested_mask_poison6:
        movl    $3196, %eax
        retq
.Lnested_mask_poison7:
        movl    $3197, %eax
        retq
        .size   jt_identity_nested_mask_intersection, .-jt_identity_nested_mask_intersection

// The mask proves that slots 0..7 are feasible, while the occurrence-local
// relocation run owns only slots 0..3.  Taking min(8,4) would publish an
// under-sized switch and silently trap/misroute the real 4..7 executions.
// Physical storage is not an upper-bound substitute for the runtime domain.
        .globl  jt_identity_mask_exceeds_storage
        .type   jt_identity_mask_exceeds_storage,@function
jt_identity_mask_exceeds_storage:
        movl    %edi, %r10d
        andl    $7, %r10d
        leaq    .Lmask_exceeds_storage_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_exceeds_storage_case0:
        movl    $3200, %eax
        retq
.Lmask_exceeds_storage_case1:
        movl    $3201, %eax
        retq
.Lmask_exceeds_storage_case2:
        movl    $3202, %eax
        retq
.Lmask_exceeds_storage_case3:
        movl    $3203, %eax
        retq
        .size   jt_identity_mask_exceeds_storage, .-jt_identity_mask_exceeds_storage

// The authenticated selector still admits eight slots, but only the first four
// contain code-pointer relocations.  Those four happen to be callable entries;
// they cannot authorize CALL lowering because slots 4..7 remain feasible and
// have no authenticated target identity.
        .globl  jt_identity_mask_exceeds_callback_storage
        .type   jt_identity_mask_exceeds_callback_storage,@function
jt_identity_mask_exceeds_callback_storage:
        movl    %edi, %r10d
        andl    $7, %r10d
        leaq    jt_identity_mask_exceeds_callback_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .size   jt_identity_mask_exceeds_callback_storage, .-jt_identity_mask_exceeds_callback_storage

        .irp callback,0,1,2,3
        .globl jt_identity_mask_exceeds_callback_\callback
        .type jt_identity_mask_exceeds_callback_\callback,@function
jt_identity_mask_exceeds_callback_\callback:
        movl $(3210 + \callback), %eax
        retq
        .size jt_identity_mask_exceeds_callback_\callback, .-jt_identity_mask_exceeds_callback_\callback
        .endr

        .section .rodata,"a",@progbits
        .p2align 2
.Ldiamond_agree_table:
        .long .Ldiamond_agree_case0-.Ldiamond_agree_table
        .long .Ldiamond_agree_case1-.Ldiamond_agree_table
        .long .Ldiamond_agree_case2-.Ldiamond_agree_table
.Ldiamond_ambiguous_table:
        .long .Ldiamond_ambiguous_case0-.Ldiamond_ambiguous_table
        .long .Ldiamond_ambiguous_case1-.Ldiamond_ambiguous_table
        .long .Ldiamond_ambiguous_poison-.Ldiamond_ambiguous_table
.Lah_nonoverlap_table:
        .long .Lah_nonoverlap_case0-.Lah_nonoverlap_table
        .long .Lah_nonoverlap_case1-.Lah_nonoverlap_table
        .long .Lah_nonoverlap_case2-.Lah_nonoverlap_table
.Lah_overlap_table:
        .long .Lah_overlap_case0-.Lah_overlap_table
        .long .Lah_overlap_case1-.Lah_overlap_table
        .long .Lah_overlap_poison-.Lah_overlap_table
.Lsext_agree_table:
        .long .Lsext_agree_case0-.Lsext_agree_table
        .long .Lsext_agree_case1-.Lsext_agree_table
        .long .Lsext_agree_case2-.Lsext_agree_table
.Lext_mismatch_table:
        .long .Lext_mismatch_case0-.Lext_mismatch_table
        .long .Lext_mismatch_case1-.Lext_mismatch_table
        .long .Lext_mismatch_poison-.Lext_mismatch_table
.Lspill_table:
        .long .Lspill_case0-.Lspill_table
        .long .Lspill_case1-.Lspill_table
        .long .Lspill_case2-.Lspill_table
.Lcase_backedge_table:
        .long .Lcase_backedge_case0-.Lcase_backedge_table
        .long .Lcase_backedge_case1-.Lcase_backedge_table
        .long .Lcase_backedge_poison-.Lcase_backedge_table
.Latomic_overwrite_table:
        .long .Latomic_overwrite_case0-.Latomic_overwrite_table
        .long .Latomic_overwrite_case1-.Latomic_overwrite_table
        .long .Latomic_overwrite_poison-.Latomic_overwrite_table
.Lcross_frame_table:
        .long .Lcross_frame_case0-.Lcross_frame_table
        .long .Lcross_frame_case1-.Lcross_frame_table
        .long .Lcross_frame_poison-.Lcross_frame_table
.Lcall_overwrite_table:
        .long .Lcall_overwrite_case0-.Lcall_overwrite_table
        .long .Lcall_overwrite_case1-.Lcall_overwrite_table
        .long .Lcall_overwrite_poison-.Lcall_overwrite_table
.Lselect_store_table:
        .long .Lselect_store_case0-.Lselect_store_table
        .long .Lselect_store_case1-.Lselect_store_table
        .long .Lselect_store_poison-.Lselect_store_table
.Lflags_diamond_table:
        .long .Lflags_diamond_case0-.Lflags_diamond_table
        .long .Lflags_diamond_case1-.Lflags_diamond_table
        .long .Lflags_diamond_poison-.Lflags_diamond_table
.Lreversed_polarity_table:
        .long .Lreversed_polarity_case0-.Lreversed_polarity_table
        .long .Lreversed_polarity_case1-.Lreversed_polarity_table
        .long .Lreversed_polarity_poison-.Lreversed_polarity_table
.Lredundant_weaker_table:
        .long .Lredundant_weaker_case0-.Lredundant_weaker_table
        .long .Lredundant_weaker_case1-.Lredundant_weaker_table
        .long .Lredundant_weaker_poison-.Lredundant_weaker_table
.Lpostload_mask_table:
        .long .Lpostload_mask_case0-.Lpostload_mask_table
        .long .Lpostload_mask_case1-.Lpostload_mask_table
        .long .Lpostload_mask_case2-.Lpostload_mask_table
        .long .Lpostload_mask_case3-.Lpostload_mask_table
.Lframe_epoch_mask_table:
        .long .Lframe_epoch_mask_case0-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_case1-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_poison2-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_poison3-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_poison4-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_poison5-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_poison6-.Lframe_epoch_mask_table
        .long .Lframe_epoch_mask_poison7-.Lframe_epoch_mask_table
.Lmask_offset_table:
        .long .Lmask_offset_case0-.Lmask_offset_table
        .long .Lmask_offset_case1-.Lmask_offset_table
        .long .Lmask_offset_case2-.Lmask_offset_table
        .long .Lmask_offset_case3-.Lmask_offset_table
        .long .Lmask_offset_case4-.Lmask_offset_table
        .long .Lmask_offset_case5-.Lmask_offset_table
        .long .Lmask_offset_case6-.Lmask_offset_table
        .long .Lmask_offset_case7-.Lmask_offset_table
        .long .Lmask_offset_case8-.Lmask_offset_table
        .long .Lmask_offset_poison-.Lmask_offset_table
        .long .Lmask_negative_poison-.Lmask_negative_table
.Lmask_negative_table:
        .long .Lmask_negative_case0-.Lmask_negative_table
        .long .Lmask_negative_case1-.Lmask_negative_table
        .long .Lmask_negative_case2-.Lmask_negative_table
        .long .Lmask_negative_case3-.Lmask_negative_table
        .long .Lmask_negative_case4-.Lmask_negative_table
        .long .Lmask_negative_case5-.Lmask_negative_table
        .long .Lmask_negative_case6-.Lmask_negative_table
.Lmask_double_offset_table:
        .long .Lmask_double_offset_case0-.Lmask_double_offset_table
        .long .Lmask_double_offset_case1-.Lmask_double_offset_table
        .long .Lmask_double_offset_case2-.Lmask_double_offset_table
        .long .Lmask_double_offset_case3-.Lmask_double_offset_table
        .long .Lmask_double_offset_case4-.Lmask_double_offset_table
        .long .Lmask_double_offset_case5-.Lmask_double_offset_table
        .long .Lmask_double_offset_poison-.Lmask_double_offset_table
.Lmask_unknown_offset_table:
        .long .Lmask_unknown_offset_case0-.Lmask_unknown_offset_table
        .long .Lmask_unknown_offset_case1-.Lmask_unknown_offset_table
        .long .Lmask_unknown_offset_case2-.Lmask_unknown_offset_table
        .long .Lmask_unknown_offset_case3-.Lmask_unknown_offset_table
        .long .Lmask_unknown_offset_poison-.Lmask_unknown_offset_table
.Lruntime_mask_table:
        .long .Lruntime_mask_case0-.Lruntime_mask_table
        .long .Lruntime_mask_case1-.Lruntime_mask_table
        .long .Lruntime_mask_case2-.Lruntime_mask_table
        .long .Lruntime_mask_case3-.Lruntime_mask_table
        .long .Lruntime_mask_poison-.Lruntime_mask_table
.Lmask_byte_wrap_table:
        .long .Lmask_byte_wrap_case0-.Lmask_byte_wrap_table
        .long .Lmask_byte_wrap_case1-.Lmask_byte_wrap_table
        .long .Lmask_byte_wrap_case2-.Lmask_byte_wrap_table
        .long .Lmask_byte_wrap_case3-.Lmask_byte_wrap_table
        .long .Lmask_byte_wrap_poison-.Lmask_byte_wrap_table
.Lmask_partial_wide_table:
        .long .Lmask_partial_wide_case0-.Lmask_partial_wide_table
        .long .Lmask_partial_wide_case1-.Lmask_partial_wide_table
        .long .Lmask_partial_wide_poison-.Lmask_partial_wide_table
.Lnested_mask_table:
        .long .Lnested_mask_case0-.Lnested_mask_table
        .long .Lnested_mask_case1-.Lnested_mask_table
        .long .Lnested_mask_poison2-.Lnested_mask_table
        .long .Lnested_mask_poison3-.Lnested_mask_table
        .long .Lnested_mask_poison4-.Lnested_mask_table
        .long .Lnested_mask_poison5-.Lnested_mask_table
        .long .Lnested_mask_poison6-.Lnested_mask_table
        .long .Lnested_mask_poison7-.Lnested_mask_table
.Lmask_exceeds_storage_table:
        .long .Lmask_exceeds_storage_case0-.Lmask_exceeds_storage_table
        .long .Lmask_exceeds_storage_case1-.Lmask_exceeds_storage_table
        .long .Lmask_exceeds_storage_case2-.Lmask_exceeds_storage_table
        .long .Lmask_exceeds_storage_case3-.Lmask_exceeds_storage_table
        // These bytes are readable but deliberately carry no code relocation;
        // they are outside the only authenticated physical slot run.
        .long 0
        .long 0
        .long 0
        .long 0
        .section .data.rel.ro.jt_mask_exceeds_callback,"aw",@progbits
        .p2align 3
        .globl jt_identity_mask_exceeds_callback_table
        .type jt_identity_mask_exceeds_callback_table,@object
jt_identity_mask_exceeds_callback_table:
        .quad jt_identity_mask_exceeds_callback_0
        .quad jt_identity_mask_exceeds_callback_1
        .quad jt_identity_mask_exceeds_callback_2
        .quad jt_identity_mask_exceeds_callback_3
        .quad 0
        .quad 0
        .quad 0
        .quad 0
        .size jt_identity_mask_exceeds_callback_table, .-jt_identity_mask_exceeds_callback_table
        .data
        .p2align 2
.Lselect_store_global:
        .long 0


// The guard independently proves [0,5), while the merged selector's complete
// mask domain tightens that to [0,4).  Final replay may preserve or tighten the
// authenticated domain, but a dense fallback may never widen it.
        .text
        .globl  jt_identity_guard5_mask4_dense_merge
        .type   jt_identity_guard5_mask4_dense_merge,@function
jt_identity_guard5_mask4_dense_merge:
        xorl    %r10d, %r10d
        jmp     .Lguard5_mask4_dispatch
.Lguard5_mask4_case0:
        movl    %edi, %r10d
        andl    $3, %r10d
.Lguard5_mask4_dispatch:
        cmpl    $5, %r10d
        jae     .Lguard5_mask4_default
        leaq    .Lguard5_mask4_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard5_mask4_case1:
        movl    $4601, %eax
        retq
.Lguard5_mask4_case2:
        movl    $4602, %eax
        retq
.Lguard5_mask4_case3:
        movl    $4603, %eax
        retq
.Lguard5_mask4_default:
        movl    $4699, %eax
        retq
        .size   jt_identity_guard5_mask4_dense_merge, .-jt_identity_guard5_mask4_dense_merge

        .section .rodata,"a",@progbits
        .p2align 2
.Lguard5_mask4_table:
        .long .Lguard5_mask4_case0-.Lguard5_mask4_table
        .long .Lguard5_mask4_case1-.Lguard5_mask4_table
        .long .Lguard5_mask4_case2-.Lguard5_mask4_table
        .long .Lguard5_mask4_case3-.Lguard5_mask4_table


// The comparison looks like a dense four-entry upper bound only in the
// sampled prefix [0,4096].  Over the real 32-bit selector domain the mask
// repeats: 8192 reaches the table again and indexes far beyond its storage.
// A guard proof must therefore quantify the whole bit domain, not infer a
// bound from a finite prefix of test values.
        .text
        .globl  jt_identity_guard_periodic_alias
        .type   jt_identity_guard_periodic_alias,@function
jt_identity_guard_periodic_alias:
        movl    %edi, %r10d
        movl    %r10d, %eax
        andl    $0x1fff, %eax
        cmpl    $4, %eax
        jae     .Lguard_periodic_default
        leaq    .Lguard_periodic_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_periodic_case0:
        movl    $3400, %eax
        retq
.Lguard_periodic_case1:
        movl    $3401, %eax
        retq
.Lguard_periodic_case2:
        movl    $3402, %eax
        retq
.Lguard_periodic_case3:
        movl    $3403, %eax
        retq
.Lguard_periodic_default:
        movl    $3499, %eax
        retq
        .size   jt_identity_guard_periodic_alias, .-jt_identity_guard_periodic_alias

// Signed `idx < 4` also admits every negative i32.  The address expression
// consumes the zero-extended 32-bit bit pattern, so -1 indexes UINT32_MAX and
// is not part of a dense [0,4) table domain.  Sampling only non-negative
// inputs misses the entire re-entering half-domain.
        .globl  jt_identity_guard_signed_negative_alias
        .type   jt_identity_guard_signed_negative_alias,@function
jt_identity_guard_signed_negative_alias:
        movl    %edi, %r10d
        cmpl    $4, %r10d
        jge     .Lguard_signed_default
        leaq    .Lguard_signed_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_signed_case0:
        movl    $3500, %eax
        retq
.Lguard_signed_case1:
        movl    $3501, %eax
        retq
.Lguard_signed_case2:
        movl    $3502, %eax
        retq
.Lguard_signed_case3:
        movl    $3503, %eax
        retq
.Lguard_signed_default:
        movl    $3599, %eax
        retq
        .size   jt_identity_guard_signed_negative_alias, .-jt_identity_guard_signed_negative_alias

// The same complete signed-negative guard rejection over an exactly sized
// table whose entries are both authenticated callable functions.  This is a
// semantic tail-callback classification, not graph/resource incompleteness;
// no local-frame target exists that would require preserving JMP identity.
        .globl  jt_identity_guard_signed_callback_tailcall
        .type   jt_identity_guard_signed_callback_tailcall,@function
jt_identity_guard_signed_callback_tailcall:
        movl    %edi, %r10d
        cmpl    $2, %r10d
        jge     .Lguard_signed_callback_default
        leaq    jt_identity_guard_signed_callback_table(%rip), %rax
        movq    (%rax,%r10,8), %rax
        jmpq    *%rax
.Lguard_signed_callback_default:
        movl    $3598, %eax
        retq
        .size   jt_identity_guard_signed_callback_tailcall, .-jt_identity_guard_signed_callback_tailcall

        .globl  jt_identity_guard_callback_a
        .type   jt_identity_guard_callback_a,@function
jt_identity_guard_callback_a:
        movl    $3580, %eax
        retq
        .size   jt_identity_guard_callback_a, .-jt_identity_guard_callback_a

        .globl  jt_identity_guard_callback_b
        .type   jt_identity_guard_callback_b,@function
jt_identity_guard_callback_b:
        movl    $3581, %eax
        retq
        .size   jt_identity_guard_callback_b, .-jt_identity_guard_callback_b

        .section .data.rel.ro.jt_identity_guard_signed_callback,"aw",@progbits
        .p2align 3
        .globl  jt_identity_guard_signed_callback_table
        .type   jt_identity_guard_signed_callback_table,@object
jt_identity_guard_signed_callback_table:
        .quad   jt_identity_guard_callback_a
        .quad   jt_identity_guard_callback_b
        .size   jt_identity_guard_signed_callback_table, .-jt_identity_guard_signed_callback_table

        .section .rodata,"a",@progbits
        .p2align 2
.Lguard_periodic_table:
        .long .Lguard_periodic_case0-.Lguard_periodic_table
        .long .Lguard_periodic_case1-.Lguard_periodic_table
        .long .Lguard_periodic_case2-.Lguard_periodic_table
        .long .Lguard_periodic_case3-.Lguard_periodic_table
        // Break the relocation run so physical storage cannot masquerade as
        // an independent index-domain proof.
        .long 0
.Lguard_signed_table:
        .long .Lguard_signed_case0-.Lguard_signed_table
        .long .Lguard_signed_case1-.Lguard_signed_table
        .long .Lguard_signed_case2-.Lguard_signed_table
        .long .Lguard_signed_case3-.Lguard_signed_table
        .long 0


// The low-byte write changes the RAX value tested by the guard while the table
// address still consumes the independent raw index in R10.  For example,
// input 200 becomes guarded RAX=0 and reaches a four-slot table at raw slot
// 200.  An exact-size lexical def scan skips the AL write, recovers the stale
// old RAX<-index definition, and manufactures a false idx<4 certificate.
        .text
        .globl  jt_identity_guard_overlapping_write
        .type   jt_identity_guard_overlapping_write,@function
jt_identity_guard_overlapping_write:
        movl    %edi, %r10d
        movq    %r10, %rax
        movb    $0, %al
        cmpq    $4, %rax
        jae     .Lguard_overlap_default
        leaq    .Lguard_overlap_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_overlap_case0:
        movl    $3600, %eax
        retq
.Lguard_overlap_case1:
        movl    $3601, %eax
        retq
.Lguard_overlap_case2:
        movl    $3602, %eax
        retq
.Lguard_overlap_case3:
        movl    $3603, %eax
        retq
.Lguard_overlap_default:
        movl    $3699, %eax
        retq
        .size   jt_identity_guard_overlapping_write, .-jt_identity_guard_overlapping_write

// The exact guard allows 5000 selector values, while the authenticated
// relocation run contains only four slots.  Failing to find a rejected value
// in the small candidate-discovery window must not let physical storage stand
// in for the missing complete index-domain proof.
        .globl  jt_identity_guard_bound_beyond_sample
        .type   jt_identity_guard_bound_beyond_sample,@function
jt_identity_guard_bound_beyond_sample:
        movl    %edi, %r10d
        cmpl    $5000, %r10d
        jae     .Lguard_large_default
        leaq    .Lguard_large_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_large_case0:
        movl    $3700, %eax
        retq
.Lguard_large_case1:
        movl    $3701, %eax
        retq
.Lguard_large_case2:
        movl    $3702, %eax
        retq
.Lguard_large_case3:
        movl    $3703, %eax
        retq
.Lguard_large_default:
        movl    $3799, %eax
        retq
        .size   jt_identity_guard_bound_beyond_sample, .-jt_identity_guard_bound_beyond_sample

        .section .rodata,"a",@progbits
        .p2align 2
.Lguard_overlap_table:
        .long .Lguard_overlap_case0-.Lguard_overlap_table
        .long .Lguard_overlap_case1-.Lguard_overlap_table
        .long .Lguard_overlap_case2-.Lguard_overlap_table
        .long .Lguard_overlap_case3-.Lguard_overlap_table
        .long 0
.Lguard_large_table:
        .long .Lguard_large_case0-.Lguard_large_table
        .long .Lguard_large_case1-.Lguard_large_table
        .long .Lguard_large_case2-.Lguard_large_table
        .long .Lguard_large_case3-.Lguard_large_table
        .long 0


// The controlling comparison is laid out after the dispatch and jumps
// backward to it.  Instruction virtual-address order is not execution order:
// CFG dominance/polarity must still see this 5000-value domain and reject the
// four-slot physical relocation run.
        .text
        .globl  jt_identity_postlaid_guard_large
        .type   jt_identity_postlaid_guard_large,@function
jt_identity_postlaid_guard_large:
        jmp     .Lpostlaid_large_guard
.Lpostlaid_large_dispatch:
        leaq    .Lpostlaid_large_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lpostlaid_large_case0:
        movl    $3800, %eax
        retq
.Lpostlaid_large_case1:
        movl    $3801, %eax
        retq
.Lpostlaid_large_case2:
        movl    $3802, %eax
        retq
.Lpostlaid_large_case3:
        movl    $3803, %eax
        retq
.Lpostlaid_large_default:
        movl    $3899, %eax
        retq
.Lpostlaid_large_guard:
        movl    %edi, %r10d
        cmpl    $5000, %r10d
        jae     .Lpostlaid_large_default
        jmp     .Lpostlaid_large_dispatch
        .size   jt_identity_postlaid_guard_large, .-jt_identity_postlaid_guard_large

// Positive control for the same non-monotonic layout: a complete [0,4)
// domain must still recover all four cases rather than rejecting every
// backward-to-dispatch CFG.
        .globl  jt_identity_postlaid_guard_four
        .type   jt_identity_postlaid_guard_four,@function
jt_identity_postlaid_guard_four:
        jmp     .Lpostlaid_four_guard
.Lpostlaid_four_dispatch:
        leaq    .Lpostlaid_four_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lpostlaid_four_case0:
        movl    $3900, %eax
        retq
.Lpostlaid_four_case1:
        movl    $3901, %eax
        retq
.Lpostlaid_four_case2:
        movl    $3902, %eax
        retq
.Lpostlaid_four_case3:
        movl    $3903, %eax
        retq
.Lpostlaid_four_default:
        movl    $3999, %eax
        retq
.Lpostlaid_four_guard:
        movl    %edi, %r10d
        cmpl    $4, %r10d
        jae     .Lpostlaid_four_default
        jmp     .Lpostlaid_four_dispatch
        .size   jt_identity_postlaid_guard_four, .-jt_identity_postlaid_guard_four

        .section .rodata,"a",@progbits
        .p2align 2
.Lpostlaid_large_table:
        .long .Lpostlaid_large_case0-.Lpostlaid_large_table
        .long .Lpostlaid_large_case1-.Lpostlaid_large_table
        .long .Lpostlaid_large_case2-.Lpostlaid_large_table
        .long .Lpostlaid_large_case3-.Lpostlaid_large_table
        .long 0
.Lpostlaid_four_table:
        .long .Lpostlaid_four_case0-.Lpostlaid_four_table
        .long .Lpostlaid_four_case1-.Lpostlaid_four_table
        .long .Lpostlaid_four_case2-.Lpostlaid_four_table
        .long .Lpostlaid_four_case3-.Lpostlaid_four_table
        .long 0


// Each AND reads the same prior SSA value twice.  A tree-shaped recursive
// builder expands this as 2^20 subexpressions; an exact-occurrence DAG memo
// visits each definition once and retains the sound four-entry proof.
        .text
        .globl  jt_identity_guard_shared_dag_budget
        .type   jt_identity_guard_shared_dag_budget,@function
jt_identity_guard_shared_dag_budget:
        movl    %edi, %r10d
        movl    %r10d, %eax
        .rept   20
        andl    %eax, %eax
        .endr
        cmpl    $4, %eax
        jae     .Lguard_shared_dag_default
        leaq    .Lguard_shared_dag_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_shared_dag_case0:
        movl    $4000, %eax
        retq
.Lguard_shared_dag_case1:
        movl    $4001, %eax
        retq
.Lguard_shared_dag_case2:
        movl    $4002, %eax
        retq
.Lguard_shared_dag_case3:
        movl    $4003, %eax
        retq
.Lguard_shared_dag_default:
        movl    $4099, %eax
        retq
        .size   jt_identity_guard_shared_dag_budget, .-jt_identity_guard_shared_dag_budget

// A long non-value-preserving syntax chain exceeds the explicit expression
// depth ceiling before SAT construction.  Exhaustion is a failed certificate,
// so the relocation run must not rescue the table and the analysis must not
// recurse until the host stack fails.
        .globl  jt_identity_guard_depth_budget
        .type   jt_identity_guard_depth_budget,@function
jt_identity_guard_depth_budget:
        movl    %edi, %r10d
        movl    %r10d, %eax
        .rept   80
        xorl    $0, %eax
        .endr
        cmpl    $4, %eax
        jae     .Lguard_depth_default
        leaq    .Lguard_depth_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_depth_case0:
        movl    $4100, %eax
        retq
.Lguard_depth_case1:
        movl    $4101, %eax
        retq
.Lguard_depth_case2:
        movl    $4102, %eax
        retq
.Lguard_depth_case3:
        movl    $4103, %eax
        retq
.Lguard_depth_default:
        movl    $4199, %eax
        retq
        .size   jt_identity_guard_depth_budget, .-jt_identity_guard_depth_budget

        .section .rodata,"a",@progbits
        .p2align 2
.Lguard_shared_dag_table:
        .long .Lguard_shared_dag_case0-.Lguard_shared_dag_table
        .long .Lguard_shared_dag_case1-.Lguard_shared_dag_table
        .long .Lguard_shared_dag_case2-.Lguard_shared_dag_table
        .long .Lguard_shared_dag_case3-.Lguard_shared_dag_table
        .long 0
.Lguard_depth_table:
        .long .Lguard_depth_case0-.Lguard_depth_table
        .long .Lguard_depth_case1-.Lguard_depth_table
        .long .Lguard_depth_case2-.Lguard_depth_table
        .long .Lguard_depth_case3-.Lguard_depth_table
        .long 0


// Hundreds of unrelated conditions precede the one real table guard.  Control
// discovery must build one CFG and share a traversal budget; exhaustion is an
// incomplete certificate, never hundreds of independently rebuilt graphs or
// a relocation-run fallback.
        .text
        .globl  jt_identity_guard_control_budget
        .type   jt_identity_guard_control_budget,@function
jt_identity_guard_control_budget:
        movl    %edi, %r10d
        .rept   300
        testl   %edi, %edi
        je      1f
1:
        .endr
        cmpl    $4, %r10d
        jae     .Lguard_control_budget_default
        leaq    .Lguard_control_budget_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lguard_control_budget_case0:
        movl    $4200, %eax
        retq
.Lguard_control_budget_case1:
        movl    $4201, %eax
        retq
.Lguard_control_budget_case2:
        movl    $4202, %eax
        retq
.Lguard_control_budget_case3:
        movl    $4203, %eax
        retq
.Lguard_control_budget_default:
        movl    $4299, %eax
        retq
        .size   jt_identity_guard_control_budget, .-jt_identity_guard_control_budget

        .section .rodata,"a",@progbits
        .p2align 2
.Lguard_control_budget_table:
        .long .Lguard_control_budget_case0-.Lguard_control_budget_table
        .long .Lguard_control_budget_case1-.Lguard_control_budget_table
        .long .Lguard_control_budget_case2-.Lguard_control_budget_table
        .long .Lguard_control_budget_case3-.Lguard_control_budget_table
        .long 0


// Merely seeing `x - y*N` does not prove a modulo domain: y is an independent
// input, not floor(x/N), so the 32-bit result can name any table slot.  Keep the
// table inline in .text so it has four readable targets but no relocation-run
// bound; the obsolete coefficient-only modulo recognizer therefore publishes
// it, while a whole-expression range proof must find an index>=4 model.
        .text
        .globl  jt_identity_fake_modulo_independent_quotient
        .type   jt_identity_fake_modulo_independent_quotient,@function
jt_identity_fake_modulo_independent_quotient:
        imull   $4, %esi, %eax
        movl    %edi, %r10d
        subl    %eax, %r10d
        leaq    .Lfake_mod4_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lfake_mod4_case0:
        movl    $4300, %eax
        retq
.Lfake_mod4_case1:
        movl    $4301, %eax
        retq
.Lfake_mod4_case2:
        movl    $4302, %eax
        retq
.Lfake_mod4_case3:
        movl    $4303, %eax
        retq
        .size   jt_identity_fake_modulo_independent_quotient, .-jt_identity_fake_modulo_independent_quotient
        .p2align 2
.Lfake_mod4_table:
        .long .Lfake_mod4_case0-.Lfake_mod4_table
        .long .Lfake_mod4_case1-.Lfake_mod4_table
        .long .Lfake_mod4_case2-.Lfake_mod4_table
        .long .Lfake_mod4_case3-.Lfake_mod4_table
        .long 0

// Two unrelated quotient roots must not be treated as one conceptual base.
// The legacy linear decomposition assigns each root coefficient one and folds
// q1*2 + q2*3 into a bogus modulus five.  The actual result remains unbounded.
        .globl  jt_identity_fake_modulo_mixed_roots
        .type   jt_identity_fake_modulo_mixed_roots,@function
jt_identity_fake_modulo_mixed_roots:
        imull   $2, %esi, %eax
        imull   $3, %edx, %ecx
        addl    %ecx, %eax
        movl    %edi, %r10d
        subl    %eax, %r10d
        leaq    .Lfake_mod5_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lfake_mod5_case0:
        movl    $4400, %eax
        retq
.Lfake_mod5_case1:
        movl    $4401, %eax
        retq
.Lfake_mod5_case2:
        movl    $4402, %eax
        retq
.Lfake_mod5_case3:
        movl    $4403, %eax
        retq
.Lfake_mod5_case4:
        movl    $4404, %eax
        retq
        .size   jt_identity_fake_modulo_mixed_roots, .-jt_identity_fake_modulo_mixed_roots
        .p2align 2
.Lfake_mod5_table:
        .long .Lfake_mod5_case0-.Lfake_mod5_table
        .long .Lfake_mod5_case1-.Lfake_mod5_table
        .long .Lfake_mod5_case2-.Lfake_mod5_table
        .long .Lfake_mod5_case3-.Lfake_mod5_table
        .long .Lfake_mod5_case4-.Lfake_mod5_table
        .long 0

// The first dispatch reaches the shared table with a literal zero index;
// loop-back paths reach it with x&3.  The merged selector is not equal to the
// mask producer on every predecessor, but its complete bit-vector domain is
// still the same dense [0,4) interval.  This is the shape emitted by clang for
// a threaded computed-goto loop whose initial pc is known to be zero.
        .text
        .globl  jt_identity_mask_dense_bounded_merge
        .type   jt_identity_mask_dense_bounded_merge,@function
jt_identity_mask_dense_bounded_merge:
        xorl    %r10d, %r10d
        jmp     .Lmask_dense_merge_dispatch
.Lmask_dense_merge_case0:
        movl    %edi, %r10d
        andl    $3, %r10d
.Lmask_dense_merge_dispatch:
        leaq    .Lmask_dense_merge_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_dense_merge_case1:
        movl    $4501, %eax
        retq
.Lmask_dense_merge_case2:
        movl    $4502, %eax
        retq
.Lmask_dense_merge_case3:
        movl    $4503, %eax
        retq
        .size   jt_identity_mask_dense_bounded_merge, .-jt_identity_mask_dense_bounded_merge

        .section .rodata,"a",@progbits
        .p2align 2
.Lmask_dense_merge_table:
        .long .Lmask_dense_merge_case0-.Lmask_dense_merge_table
        .long .Lmask_dense_merge_case1-.Lmask_dense_merge_table
        .long .Lmask_dense_merge_case2-.Lmask_dense_merge_table
        .long .Lmask_dense_merge_case3-.Lmask_dense_merge_table

// The real selector is y%5.  The x&31 value is carried through a multiply by
// zero before being added to that selector, so occurrence-level dependency can
// see the mask while machine semantics erase it.  The 32 physical relocation
// slots make a bogus mask-derived domain observable: a sound resolver either
// recovers only the independent five-value modulo domain or fails closed.
        .text
        .globl  jt_identity_dead_mask_dependency_mod5
        .type   jt_identity_dead_mask_dependency_mod5,@function
jt_identity_dead_mask_dependency_mod5:
        movl    %edi, %r10d
        andl    $31, %r10d
        imull   $0, %r10d, %r10d
        movl    %esi, %eax
        xorl    %edx, %edx
        movl    $5, %ecx
        divl    %ecx
        addl    %r10d, %edx
        leaq    jt_identity_dead_mask_dependency_table(%rip), %rax
        movslq  (%rax,%rdx,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Ldead_mask_dependency_case0:
        movl    $4700, %eax
        retq
.Ldead_mask_dependency_case1:
        movl    $4701, %eax
        retq
.Ldead_mask_dependency_case2:
        movl    $4702, %eax
        retq
.Ldead_mask_dependency_case3:
        movl    $4703, %eax
        retq
.Ldead_mask_dependency_case4:
        movl    $4704, %eax
        retq
.Ldead_mask_dependency_case5:
        movl    $4705, %eax
        retq
.Ldead_mask_dependency_case6:
        movl    $4706, %eax
        retq
.Ldead_mask_dependency_case7:
        movl    $4707, %eax
        retq
.Ldead_mask_dependency_case8:
        movl    $4708, %eax
        retq
.Ldead_mask_dependency_case9:
        movl    $4709, %eax
        retq
.Ldead_mask_dependency_case10:
        movl    $4710, %eax
        retq
.Ldead_mask_dependency_case11:
        movl    $4711, %eax
        retq
.Ldead_mask_dependency_case12:
        movl    $4712, %eax
        retq
.Ldead_mask_dependency_case13:
        movl    $4713, %eax
        retq
.Ldead_mask_dependency_case14:
        movl    $4714, %eax
        retq
.Ldead_mask_dependency_case15:
        movl    $4715, %eax
        retq
.Ldead_mask_dependency_case16:
        movl    $4716, %eax
        retq
.Ldead_mask_dependency_case17:
        movl    $4717, %eax
        retq
.Ldead_mask_dependency_case18:
        movl    $4718, %eax
        retq
.Ldead_mask_dependency_case19:
        movl    $4719, %eax
        retq
.Ldead_mask_dependency_case20:
        movl    $4720, %eax
        retq
.Ldead_mask_dependency_case21:
        movl    $4721, %eax
        retq
.Ldead_mask_dependency_case22:
        movl    $4722, %eax
        retq
.Ldead_mask_dependency_case23:
        movl    $4723, %eax
        retq
.Ldead_mask_dependency_case24:
        movl    $4724, %eax
        retq
.Ldead_mask_dependency_case25:
        movl    $4725, %eax
        retq
.Ldead_mask_dependency_case26:
        movl    $4726, %eax
        retq
.Ldead_mask_dependency_case27:
        movl    $4727, %eax
        retq
.Ldead_mask_dependency_case28:
        movl    $4728, %eax
        retq
.Ldead_mask_dependency_case29:
        movl    $4729, %eax
        retq
.Ldead_mask_dependency_case30:
        movl    $4730, %eax
        retq
.Ldead_mask_dependency_case31:
        movl    $4731, %eax
        retq
        .size   jt_identity_dead_mask_dependency_mod5, .-jt_identity_dead_mask_dependency_mod5

        .section .rodata,"a",@progbits
        .p2align 2
        .globl  jt_identity_dead_mask_dependency_table
        .type   jt_identity_dead_mask_dependency_table,@object
jt_identity_dead_mask_dependency_table:
        .long .Ldead_mask_dependency_case0-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case1-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case2-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case3-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case4-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case5-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case6-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case7-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case8-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case9-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case10-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case11-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case12-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case13-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case14-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case15-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case16-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case17-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case18-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case19-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case20-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case21-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case22-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case23-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case24-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case25-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case26-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case27-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case28-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case29-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case30-jt_identity_dead_mask_dependency_table
        .long .Ldead_mask_dependency_case31-jt_identity_dead_mask_dependency_table
        .size jt_identity_dead_mask_dependency_table, .-jt_identity_dead_mask_dependency_table

        .text
// A cyclic/double-producer fast path must not treat producer multiplicity as a
// complete domain certificate.  Both predecessor arms have distinct outer
// `&7` occurrences, but their inputs are the translated inner domain
// `(x&1)+1`, so only physical coordinates 1 and 2 are feasible.
        .globl  jt_identity_nested_mask_offset_merge
        .type   jt_identity_nested_mask_offset_merge,@function
jt_identity_nested_mask_offset_merge:
        testl   %edi, %edi
        js      .Lnested_offset_arm1
        movl    %edi, %r10d
        andl    $1, %r10d
        addl    $1, %r10d
        andl    $7, %r10d
        jmp     .Lnested_offset_dispatch
.Lnested_offset_arm1:
        movl    %edi, %r10d
        andl    $1, %r10d
        addl    $1, %r10d
        andl    $7, %r10d
.Lnested_offset_dispatch:
        leaq    jt_identity_nested_mask_offset_merge_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lnested_offset_poison0:
        movl    $4800, %eax
        retq
.Lnested_offset_case1:
        movl    $4801, %eax
        retq
.Lnested_offset_case2:
        movl    $4802, %eax
        retq
.Lnested_offset_poison3:
        movl    $4803, %eax
        retq
.Lnested_offset_poison4:
        movl    $4804, %eax
        retq
.Lnested_offset_poison5:
        movl    $4805, %eax
        retq
.Lnested_offset_poison6:
        movl    $4806, %eax
        retq
.Lnested_offset_poison7:
        movl    $4807, %eax
        retq
        .size jt_identity_nested_mask_offset_merge, .-jt_identity_nested_mask_offset_merge

        .section .rodata,"a",@progbits
        .p2align 2
        .globl  jt_identity_nested_mask_offset_merge_table
        .type   jt_identity_nested_mask_offset_merge_table,@object
jt_identity_nested_mask_offset_merge_table:
        .long .Lnested_offset_poison0-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_case1-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_case2-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_poison3-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_poison4-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_poison5-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_poison6-jt_identity_nested_mask_offset_merge_table
        .long .Lnested_offset_poison7-jt_identity_nested_mask_offset_merge_table
        .size jt_identity_nested_mask_offset_merge_table, .-jt_identity_nested_mask_offset_merge_table

        .text
// Provisional table edges must not make unreachable case-local masks prove
// their own reachability.  The only real selector is the constant zero loop;
// cases 1..7 are physical table entries but outside the least fixed point.
        .globl  jt_identity_mask_fp_self_bootstrap
        .type   jt_identity_mask_fp_self_bootstrap,@function
jt_identity_mask_fp_self_bootstrap:
        xorl    %r10d, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case0:
        movl    %edi, %r11d
        andl    $7, %r11d
        xorl    %r10d, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case1:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case2:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case3:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case4:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case5:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case6:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_self_dispatch
.Lmask_fp_self_case7:
        movl    %edi, %r10d
        andl    $7, %r10d
.Lmask_fp_self_dispatch:
        leaq    jt_identity_mask_fp_self_bootstrap_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .size jt_identity_mask_fp_self_bootstrap, .-jt_identity_mask_fp_self_bootstrap

// Case zero legitimately expands the seed to all eight coordinates, but that
// makes case seven reachable and exposes its out-of-domain selector 15.  A
// one-shot seed proof would publish the table; fixed-point revalidation must
// reject it after rebuilding with the proposed target set.
        .globl  jt_identity_mask_fp_late_escape
        .type   jt_identity_mask_fp_late_escape,@function
jt_identity_mask_fp_late_escape:
        xorl    %r10d, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case0:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case1:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case2:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case3:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case4:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case5:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case6:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_late_dispatch
.Lmask_fp_late_case7:
        movl    $15, %r10d
.Lmask_fp_late_dispatch:
        leaq    jt_identity_mask_fp_late_escape_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .size jt_identity_mask_fp_late_escape, .-jt_identity_mask_fp_late_escape

// A complete entry-side mask is still only a fixed-point seed when candidate
// targets can flow back to the same dispatch.  Case seven widens the selector
// after the initially valid arg&7 domain, so publishing the empty-edge proof
// without replay would admit an out-of-bounds table read on the second trip.
        .globl  jt_identity_mask_fp_entry_bound_late_escape
        .type   jt_identity_mask_fp_entry_bound_late_escape,@function
jt_identity_mask_fp_entry_bound_late_escape:
        movl    %edi, %r10d
        andl    $7, %r10d
        jmp     .Lmask_fp_entry_bound_dispatch
.Lmask_fp_entry_bound_case0:
        xorl    %eax, %eax
        retq
.Lmask_fp_entry_bound_case1:
        movl    $1, %eax
        retq
.Lmask_fp_entry_bound_case2:
        movl    $2, %eax
        retq
.Lmask_fp_entry_bound_case3:
        movl    $3, %eax
        retq
.Lmask_fp_entry_bound_case4:
        movl    $4, %eax
        retq
.Lmask_fp_entry_bound_case5:
        movl    $5, %eax
        retq
.Lmask_fp_entry_bound_case6:
        movl    $6, %eax
        retq
.Lmask_fp_entry_bound_case7:
        movl    $15, %r10d
.Lmask_fp_entry_bound_dispatch:
        leaq    jt_identity_mask_fp_entry_bound_late_escape_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .size jt_identity_mask_fp_entry_bound_late_escape, .-jt_identity_mask_fp_entry_bound_late_escape

// Same closure hazard through a pre-scaled byte selector.  Its address scale
// intentionally differs from the physical entry stride, exercising the
// candidate-local byte-coordinate-to-slot mapper and its final closure check.
        .globl  jt_identity_mask_fp_prescaled_late_escape
        .type   jt_identity_mask_fp_prescaled_late_escape,@function
jt_identity_mask_fp_prescaled_late_escape:
        movl    %edi, %r10d
        shll    $3, %r10d
        andl    $56, %r10d
        jmp     .Lmask_fp_prescaled_dispatch
.Lmask_fp_prescaled_case0:
        xorl    %eax, %eax
        retq
.Lmask_fp_prescaled_case1:
        movl    $1, %eax
        retq
.Lmask_fp_prescaled_case2:
        movl    $2, %eax
        retq
.Lmask_fp_prescaled_case3:
        movl    $3, %eax
        retq
.Lmask_fp_prescaled_case4:
        movl    $4, %eax
        retq
.Lmask_fp_prescaled_case5:
        movl    $5, %eax
        retq
.Lmask_fp_prescaled_case6:
        movl    $6, %eax
        retq
.Lmask_fp_prescaled_case7:
        movl    $120, %r10d
.Lmask_fp_prescaled_dispatch:
        leaq    jt_identity_mask_fp_prescaled_late_escape_table(%rip), %rax
        movq    (%rax,%r10), %rcx
        jmpq    *%rcx
        .size jt_identity_mask_fp_prescaled_late_escape, .-jt_identity_mask_fp_prescaled_late_escape

        .section .rodata,"a",@progbits
        .p2align 2
        .globl  jt_identity_mask_fp_self_bootstrap_table
        .type   jt_identity_mask_fp_self_bootstrap_table,@object
jt_identity_mask_fp_self_bootstrap_table:
        .quad .Lmask_fp_self_case0
        .quad .Lmask_fp_self_case1
        .quad .Lmask_fp_self_case2
        .quad .Lmask_fp_self_case3
        .quad .Lmask_fp_self_case4
        .quad .Lmask_fp_self_case5
        .quad .Lmask_fp_self_case6
        .quad .Lmask_fp_self_case7
        .size jt_identity_mask_fp_self_bootstrap_table, .-jt_identity_mask_fp_self_bootstrap_table

        .globl  jt_identity_mask_fp_late_escape_table
        .type   jt_identity_mask_fp_late_escape_table,@object
jt_identity_mask_fp_late_escape_table:
        .quad .Lmask_fp_late_case0
        .quad .Lmask_fp_late_case1
        .quad .Lmask_fp_late_case2
        .quad .Lmask_fp_late_case3
        .quad .Lmask_fp_late_case4
        .quad .Lmask_fp_late_case5
        .quad .Lmask_fp_late_case6
        .quad .Lmask_fp_late_case7
        .size jt_identity_mask_fp_late_escape_table, .-jt_identity_mask_fp_late_escape_table

        .globl  jt_identity_mask_fp_entry_bound_late_escape_table
        .type   jt_identity_mask_fp_entry_bound_late_escape_table,@object
jt_identity_mask_fp_entry_bound_late_escape_table:
        .quad .Lmask_fp_entry_bound_case0
        .quad .Lmask_fp_entry_bound_case1
        .quad .Lmask_fp_entry_bound_case2
        .quad .Lmask_fp_entry_bound_case3
        .quad .Lmask_fp_entry_bound_case4
        .quad .Lmask_fp_entry_bound_case5
        .quad .Lmask_fp_entry_bound_case6
        .quad .Lmask_fp_entry_bound_case7
        .size jt_identity_mask_fp_entry_bound_late_escape_table, .-jt_identity_mask_fp_entry_bound_late_escape_table

        .globl  jt_identity_mask_fp_prescaled_late_escape_table
        .type   jt_identity_mask_fp_prescaled_late_escape_table,@object
jt_identity_mask_fp_prescaled_late_escape_table:
        .quad .Lmask_fp_prescaled_case0
        .quad .Lmask_fp_prescaled_case1
        .quad .Lmask_fp_prescaled_case2
        .quad .Lmask_fp_prescaled_case3
        .quad .Lmask_fp_prescaled_case4
        .quad .Lmask_fp_prescaled_case5
        .quad .Lmask_fp_prescaled_case6
        .quad .Lmask_fp_prescaled_case7
        .size jt_identity_mask_fp_prescaled_late_escape_table, .-jt_identity_mask_fp_prescaled_late_escape_table

// The optimized x64 computed-goto shape: one entry LEA defines the shared table
// base and five memory-indirect branches (entry plus four target bodies) each
// contribute their own selector, LOAD and INDIR_BR occurrence.  The four table
// targets close the candidate-reachable consumer cycle.  Publishing the entry
// candidate must not suppress the conditional roots needed to prove every
// sibling from the same stage snapshot.  The two functions differ only in the
// physical order of the four target bodies; no shared-table cleanup may copy a
// sibling's occurrence-bearing JumpTableInfo.
        .text
        .globl  jt_identity_sibling_snapshot_narrow_first
        .type   jt_identity_sibling_snapshot_narrow_first,@function
jt_identity_sibling_snapshot_narrow_first:
        .globl  jt_identity_sibling_nf_entry_begin
jt_identity_sibling_nf_entry_begin:
        leaq    jt_identity_sibling_nf_table(%rip), %rdx
        movl    %edi, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_nf_entry_end
jt_identity_sibling_nf_entry_end:

        .globl  jt_identity_sibling_nf_t0_begin
jt_identity_sibling_nf_t0_begin:
        movl    %edi, %esi
        addl    $1, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_nf_t0_end
jt_identity_sibling_nf_t0_end:

        .globl  jt_identity_sibling_nf_t1_begin
jt_identity_sibling_nf_t1_begin:
        movl    %edi, %esi
        shrl    $2, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_nf_t1_end
jt_identity_sibling_nf_t1_end:

        .globl  jt_identity_sibling_nf_t2_begin
jt_identity_sibling_nf_t2_begin:
        movl    %edi, %esi
        shrl    $5, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_nf_t2_end
jt_identity_sibling_nf_t2_end:

        .globl  jt_identity_sibling_nf_t3_begin
jt_identity_sibling_nf_t3_begin:
        movl    %edi, %esi
        shrl    $9, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_nf_t3_end
jt_identity_sibling_nf_t3_end:
        .size jt_identity_sibling_snapshot_narrow_first, .-jt_identity_sibling_snapshot_narrow_first

        .globl  jt_identity_sibling_snapshot_wide_first
        .type   jt_identity_sibling_snapshot_wide_first,@function
jt_identity_sibling_snapshot_wide_first:
        .globl  jt_identity_sibling_wf_entry_begin
jt_identity_sibling_wf_entry_begin:
        leaq    jt_identity_sibling_wf_table(%rip), %rdx
        movl    %edi, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_wf_entry_end
jt_identity_sibling_wf_entry_end:

        .globl  jt_identity_sibling_wf_t3_begin
jt_identity_sibling_wf_t3_begin:
        movl    %edi, %esi
        shrl    $9, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_wf_t3_end
jt_identity_sibling_wf_t3_end:

        .globl  jt_identity_sibling_wf_t2_begin
jt_identity_sibling_wf_t2_begin:
        movl    %edi, %esi
        shrl    $5, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_wf_t2_end
jt_identity_sibling_wf_t2_end:

        .globl  jt_identity_sibling_wf_t1_begin
jt_identity_sibling_wf_t1_begin:
        movl    %edi, %esi
        shrl    $2, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_wf_t1_end
jt_identity_sibling_wf_t1_end:

        .globl  jt_identity_sibling_wf_t0_begin
jt_identity_sibling_wf_t0_begin:
        movl    %edi, %esi
        addl    $1, %esi
        andl    $3, %esi
        jmpq    *(%rdx,%rsi,8)
        .globl  jt_identity_sibling_wf_t0_end
jt_identity_sibling_wf_t0_end:
        .size jt_identity_sibling_snapshot_wide_first, .-jt_identity_sibling_snapshot_wide_first

        .section .rodata,"a",@progbits
        .p2align 3
        .globl  jt_identity_sibling_nf_table
        .type   jt_identity_sibling_nf_table,@object
jt_identity_sibling_nf_table:
        .quad jt_identity_sibling_nf_t0_begin
        .quad jt_identity_sibling_nf_t1_begin
        .quad jt_identity_sibling_nf_t2_begin
        .quad jt_identity_sibling_nf_t3_begin
        .size jt_identity_sibling_nf_table, .-jt_identity_sibling_nf_table

        .p2align 3
        .globl  jt_identity_sibling_wf_table
        .type   jt_identity_sibling_wf_table,@object
jt_identity_sibling_wf_table:
        .quad jt_identity_sibling_wf_t0_begin
        .quad jt_identity_sibling_wf_t1_begin
        .quad jt_identity_sibling_wf_t2_begin
        .quad jt_identity_sibling_wf_t3_begin
        .size jt_identity_sibling_wf_table, .-jt_identity_sibling_wf_table


// Setting bit zero before masking proves that the subsequent -1 cannot wrap:
// `(x | 1) & 7` is exactly {1,3,5,7}, so the physical table coordinates are
// exactly {0,2,4,6}.  The adjacent odd slots are readable relocation-backed
// entries on purpose; a dense-capacity shortcut would incorrectly publish
// their poison targets.
        .text
        .globl  jt_identity_mask_or_negative_offset
        .type   jt_identity_mask_or_negative_offset,@function
jt_identity_mask_or_negative_offset:
        movl    %edi, %r10d
        orl     $1, %r10d
        andl    $7, %r10d
        subl    $1, %r10d
        leaq    jt_identity_mask_or_negative_offset_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx

        .globl  jt_identity_mask_or_negative_case0
jt_identity_mask_or_negative_case0:
        movl    $6200, %eax
        retq
        .globl  jt_identity_mask_or_negative_poison1
jt_identity_mask_or_negative_poison1:
        movl    $6291, %eax
        retq
        .globl  jt_identity_mask_or_negative_case2
jt_identity_mask_or_negative_case2:
        movl    $6202, %eax
        retq
        .globl  jt_identity_mask_or_negative_poison3
jt_identity_mask_or_negative_poison3:
        movl    $6293, %eax
        retq
        .globl  jt_identity_mask_or_negative_case4
jt_identity_mask_or_negative_case4:
        movl    $6204, %eax
        retq
        .globl  jt_identity_mask_or_negative_poison5
jt_identity_mask_or_negative_poison5:
        movl    $6295, %eax
        retq
        .globl  jt_identity_mask_or_negative_case6
jt_identity_mask_or_negative_case6:
        movl    $6206, %eax
        retq
        .globl  jt_identity_mask_or_negative_poison7
jt_identity_mask_or_negative_poison7:
        movl    $6297, %eax
        retq
        .size jt_identity_mask_or_negative_offset, .-jt_identity_mask_or_negative_offset

        .section .rodata,"a",@progbits
        .p2align 2
        .globl  jt_identity_mask_or_negative_offset_table
        .type   jt_identity_mask_or_negative_offset_table,@object
jt_identity_mask_or_negative_offset_table:
        .long jt_identity_mask_or_negative_case0-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_poison1-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_case2-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_poison3-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_case4-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_poison5-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_case6-jt_identity_mask_or_negative_offset_table
        .long jt_identity_mask_or_negative_poison7-jt_identity_mask_or_negative_offset_table
        .size jt_identity_mask_or_negative_offset_table, .-jt_identity_mask_or_negative_offset_table


// The dispatch selector has its own complete x&7 domain.  A reachable sibling
// path computes `(y|1)&7` and consumes it as control, but that value never
// reaches the table address.  Known-one evidence is occurrence-local: the
// sibling OR must not narrow the dispatch to the odd coordinates or otherwise
// perturb final-root replay.
        .text
        .globl  jt_identity_mask_or_unrelated_sibling
        .type   jt_identity_mask_or_unrelated_sibling,@function
jt_identity_mask_or_unrelated_sibling:
        movl    %edi, %r10d
        andl    $7, %r10d
        movl    %esi, %r11d
        orl     $1, %r11d
        andl    $7, %r11d
        testl   $2, %r11d
        je      .Lmask_or_sibling_dispatch
        xorl    %r11d, %r11d
.Lmask_or_sibling_dispatch:
        leaq    jt_identity_mask_or_unrelated_sibling_table(%rip), %rax
        jmpq    *(%rax,%r10,8)

.Lmask_or_sibling_case0:
        movl    $6300, %eax
        retq
.Lmask_or_sibling_case1:
        movl    $6301, %eax
        retq
.Lmask_or_sibling_case2:
        movl    $6302, %eax
        retq
.Lmask_or_sibling_case3:
        movl    $6303, %eax
        retq
.Lmask_or_sibling_case4:
        movl    $6304, %eax
        retq
.Lmask_or_sibling_case5:
        movl    $6305, %eax
        retq
.Lmask_or_sibling_case6:
        movl    $6306, %eax
        retq
.Lmask_or_sibling_case7:
        movl    $6307, %eax
        retq
        .size jt_identity_mask_or_unrelated_sibling, .-jt_identity_mask_or_unrelated_sibling

        .section .rodata,"a",@progbits
        .p2align 3
        .globl  jt_identity_mask_or_unrelated_sibling_table
        .type   jt_identity_mask_or_unrelated_sibling_table,@object
jt_identity_mask_or_unrelated_sibling_table:
        .quad .Lmask_or_sibling_case0
        .quad .Lmask_or_sibling_case1
        .quad .Lmask_or_sibling_case2
        .quad .Lmask_or_sibling_case3
        .quad .Lmask_or_sibling_case4
        .quad .Lmask_or_sibling_case5
        .quad .Lmask_or_sibling_case6
        .quad .Lmask_or_sibling_case7
        .size jt_identity_mask_or_unrelated_sibling_table, .-jt_identity_mask_or_unrelated_sibling_table

// The only entry-reachable selector is constant zero, and case zero loops with
// that same value.  The real `(x|1)&7; -1` producer lives in case two, which is
// reachable only if the resolver first lends the physical table's provisional
// targets to its own proof.  Case two would then support itself in the apparent
// {0,2,4,6} domain.  Least-fixed-point final replay must remove that circular
// certificate and leave the original indirect branch opaque and unsafe.
        .text
        .globl  jt_identity_mask_or_fp_self_bootstrap
        .type   jt_identity_mask_or_fp_self_bootstrap,@function
jt_identity_mask_or_fp_self_bootstrap:
        xorl    %r10d, %r10d
        jmp     .Lmask_or_fp_dispatch
.Lmask_or_fp_case0:
        xorl    %r10d, %r10d
        jmp     .Lmask_or_fp_dispatch
.Lmask_or_fp_case1:
        movl    $6401, %eax
        retq
.Lmask_or_fp_case2:
        movl    %edi, %r10d
        orl     $1, %r10d
        andl    $7, %r10d
        subl    $1, %r10d
        jmp     .Lmask_or_fp_dispatch
.Lmask_or_fp_case3:
        movl    $6403, %eax
        retq
.Lmask_or_fp_case4:
        movl    $6404, %eax
        retq
.Lmask_or_fp_case5:
        movl    $6405, %eax
        retq
.Lmask_or_fp_case6:
        movl    $6406, %eax
        retq
.Lmask_or_fp_case7:
        movl    $6407, %eax
        retq
.Lmask_or_fp_dispatch:
        leaq    jt_identity_mask_or_fp_self_bootstrap_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .size jt_identity_mask_or_fp_self_bootstrap, .-jt_identity_mask_or_fp_self_bootstrap

        .section .rodata,"a",@progbits
        .p2align 3
        .globl  jt_identity_mask_or_fp_self_bootstrap_table
        .type   jt_identity_mask_or_fp_self_bootstrap_table,@object
jt_identity_mask_or_fp_self_bootstrap_table:
        .quad .Lmask_or_fp_case0
        .quad .Lmask_or_fp_case1
        .quad .Lmask_or_fp_case2
        .quad .Lmask_or_fp_case3
        .quad .Lmask_or_fp_case4
        .quad .Lmask_or_fp_case5
        .quad .Lmask_or_fp_case6
        .quad .Lmask_or_fp_case7
        .size jt_identity_mask_or_fp_self_bootstrap_table, .-jt_identity_mask_or_fp_self_bootstrap_table


// The entry path proves `(x|1)&7; -1` without borrowing a single table edge,
// so its exact physical domain is {0,2,4,6}.  Odd slot one contains another
// syntactically identical OR/mask/sub recipe over an unrelated register.  That
// sibling exists only while the resolver provisionally explores the odd
// relocation targets; final sparse-domain replay removes its root.  The clean
// selector's known-one certificate must therefore contain only its own exact
// OR occurrence, not every same-bound proposal seen in the provisional graph.
        .text
        .globl  jt_identity_mask_or_pruned_sibling_clean
        .type   jt_identity_mask_or_pruned_sibling_clean,@function
jt_identity_mask_or_pruned_sibling_clean:
        movl    %edi, %r10d
        orl     $1, %r10d
        andl    $7, %r10d
        subl    $1, %r10d
        leaq    jt_identity_mask_or_pruned_sibling_clean_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx

        .globl  jt_identity_mask_or_pruned_sibling_case0
jt_identity_mask_or_pruned_sibling_case0:
        movl    $6500, %eax
        retq
        .globl  jt_identity_mask_or_pruned_sibling_poison1
jt_identity_mask_or_pruned_sibling_poison1:
        .globl  jt_identity_mask_or_pruned_sibling_begin
jt_identity_mask_or_pruned_sibling_begin:
        movl    %esi, %r11d
        orl     $1, %r11d
        andl    $7, %r11d
        subl    $1, %r11d
        movl    $6591, %eax
        addl    %r11d, %eax
        .globl  jt_identity_mask_or_pruned_sibling_end
jt_identity_mask_or_pruned_sibling_end:
        retq
        .globl  jt_identity_mask_or_pruned_sibling_case2
jt_identity_mask_or_pruned_sibling_case2:
        movl    $6502, %eax
        retq
        .globl  jt_identity_mask_or_pruned_sibling_poison3
jt_identity_mask_or_pruned_sibling_poison3:
        movl    $6593, %eax
        retq
        .globl  jt_identity_mask_or_pruned_sibling_case4
jt_identity_mask_or_pruned_sibling_case4:
        movl    $6504, %eax
        retq
        .globl  jt_identity_mask_or_pruned_sibling_poison5
jt_identity_mask_or_pruned_sibling_poison5:
        movl    $6595, %eax
        retq
        .globl  jt_identity_mask_or_pruned_sibling_case6
jt_identity_mask_or_pruned_sibling_case6:
        movl    $6506, %eax
        retq
        .globl  jt_identity_mask_or_pruned_sibling_poison7
jt_identity_mask_or_pruned_sibling_poison7:
        movl    $6597, %eax
        retq
        .size jt_identity_mask_or_pruned_sibling_clean, .-jt_identity_mask_or_pruned_sibling_clean

        .section .rodata,"a",@progbits
        .p2align 2
        .globl  jt_identity_mask_or_pruned_sibling_clean_table
        .type   jt_identity_mask_or_pruned_sibling_clean_table,@object
jt_identity_mask_or_pruned_sibling_clean_table:
        .long jt_identity_mask_or_pruned_sibling_case0-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_poison1-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_case2-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_poison3-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_case4-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_poison5-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_case6-jt_identity_mask_or_pruned_sibling_clean_table
        .long jt_identity_mask_or_pruned_sibling_poison7-jt_identity_mask_or_pruned_sibling_clean_table
        .size jt_identity_mask_or_pruned_sibling_clean_table, .-jt_identity_mask_or_pruned_sibling_clean_table

        .section .note.GNU-stack,"",@progbits

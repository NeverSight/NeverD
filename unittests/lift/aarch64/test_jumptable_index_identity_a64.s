.text
.p2align 2

// The guard and load consume the same 32-bit value only because UXTW makes
// that relationship explicit.  A value with high X0 bits set therefore uses
// its low W0 value for both the comparison and the table access.
.globl a64_compact_explicit_zext
.type a64_compact_explicit_zext, %function
a64_compact_explicit_zext:
  cmp w0, #2
  b.hi .La64_zext_default
  adrp x9, .La64_zext_table
  add x9, x9, :lo12:.La64_zext_table
  mov w10, w0
  ldrb w11, [x9, x10]
  adr x12, .La64_zext_anchor
  add x12, x12, x11, lsl #2
  br x12
.La64_zext_table:
  .byte (.La64_zext_case0 - .La64_zext_anchor) / 4
  .byte (.La64_zext_case1 - .La64_zext_anchor) / 4
  .byte (.La64_zext_case2 - .La64_zext_anchor) / 4
  .byte (.La64_zext_poison - .La64_zext_anchor) / 4
  .p2align 2
.La64_zext_anchor:
.La64_zext_case0:
  mov w0, #100
  ret
.La64_zext_case1:
  mov w0, #101
  ret
.La64_zext_case2:
  mov w0, #102
  ret
.La64_zext_poison:
  mov w0, #399
  ret
.La64_zext_default:
  mov w0, #-1
  ret
.size a64_compact_explicit_zext, .-a64_compact_explicit_zext

.p2align 2
// -O0-style spill: the comparison and compact-table address use separate
// reloads of the same 32-bit frame slot.  The inclusive guard must reach the
// index through the exact reaching STORE, not through register-number
// coincidence.
.globl a64_compact_spill_reload
.type a64_compact_spill_reload, %function
a64_compact_spill_reload:
  sub sp, sp, #16
  str w0, [sp, #12]
  ldr w10, [sp, #12]
  cmp w10, #2
  b.hi .La64_spill_default
  adrp x9, .La64_spill_table
  add x9, x9, :lo12:.La64_spill_table
  ldr w11, [sp, #12]
  ldrb w12, [x9, x11]
  adr x13, .La64_spill_anchor
  add x13, x13, x12, lsl #2
  br x13
.La64_spill_table:
  .byte (.La64_spill_case0 - .La64_spill_anchor) / 4
  .byte (.La64_spill_case1 - .La64_spill_anchor) / 4
  .byte (.La64_spill_case2 - .La64_spill_anchor) / 4
  .byte (.La64_spill_poison - .La64_spill_anchor) / 4
  .p2align 2
.La64_spill_anchor:
.La64_spill_case0:
  mov w0, #200
  add sp, sp, #16
  ret
.La64_spill_case1:
  mov w0, #201
  add sp, sp, #16
  ret
.La64_spill_case2:
  mov w0, #202
  add sp, sp, #16
  ret
.La64_spill_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.La64_spill_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_compact_spill_reload, .-a64_compact_spill_reload

.p2align 2
// An actual LowIR ATOMIC_ADD is a reaching memory definition.  The old
// inclusive guard describes the pre-RMW value, while the post-RMW exclusive
// guard bounds the two legal slots.  Ignoring the atomic write exposes the
// adjacent poison entry.
.globl a64_compact_atomic_overwrite
.type a64_compact_atomic_overwrite, %function
a64_compact_atomic_overwrite:
  sub sp, sp, #16
  str w0, [sp]
  ldr w10, [sp]
  cmp w10, #2
  b.hi .La64_atomic_default
  ldadd w1, w2, [sp]
  ldr w11, [sp]
  cmp w11, #2
  b.hs .La64_atomic_default
  adrp x9, .La64_atomic_table
  add x9, x9, :lo12:.La64_atomic_table
  ldrb w12, [x9, x11]
  adr x13, .La64_atomic_anchor
  add x13, x13, x12, lsl #2
  br x13
.La64_atomic_table:
  .byte (.La64_atomic_case0 - .La64_atomic_anchor) / 4
  .byte (.La64_atomic_case1 - .La64_atomic_anchor) / 4
  .byte (.La64_atomic_poison - .La64_atomic_anchor) / 4
  .p2align 2
.La64_atomic_anchor:
.La64_atomic_case0:
  mov w0, #1500
  add sp, sp, #16
  ret
.La64_atomic_case1:
  mov w0, #1501
  add sp, sp, #16
  ret
.La64_atomic_poison:
  mov w0, #1599
  add sp, sp, #16
  ret
.La64_atomic_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_compact_atomic_overwrite, .-a64_compact_atomic_overwrite

.p2align 2
// A W-register write clears the complete X-register view.  The call therefore
// receives zero in X0, not the writable table base that was previously held
// there; module arbitration must not retain stale X0 provenance and reject the
// independent dispatch.
.globl a64_writable_arg_alias_clobber
.type a64_writable_arg_alias_clobber, %function
a64_writable_arg_alias_clobber:
  stp x19, x30, [sp, #-16]!
  mov w19, w0
  adrp x0, .La64_writable_arg_alias_table
  add x0, x0, :lo12:.La64_writable_arg_alias_table
  mov w0, wzr
  bl a64_writable_unknown_callee
  and w10, w19, #1
  adrp x9, .La64_writable_arg_alias_table
  add x9, x9, :lo12:.La64_writable_arg_alias_table
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_writable_arg_alias_case0:
  ldp x19, x30, [sp], #16
  mov w0, #1600
  ret
.La64_writable_arg_alias_case1:
  ldp x19, x30, [sp], #16
  mov w0, #1601
  ret
.size a64_writable_arg_alias_clobber, .-a64_writable_arg_alias_clobber

.p2align 2
// Reading W0 from a live X0 table address and writing it back zero-extends the
// projected low 32 bits.  For this low-address object the value still denotes
// the same writable table, so the opaque callee can mutate it.  The address
// lattice must project the X0 provenance into W0 before applying the write.
.globl a64_writable_w_self_escape
.type a64_writable_w_self_escape, %function
a64_writable_w_self_escape:
  stp x19, x30, [sp, #-16]!
  mov w19, w0
  adrp x0, a64_writable_w_self_table
  add x0, x0, :lo12:a64_writable_w_self_table
  mov w0, w0
  bl a64_writable_unknown_callee
  and w10, w19, #1
  adrp x9, a64_writable_w_self_table
  add x9, x9, :lo12:a64_writable_w_self_table
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_writable_w_self_case0:
  ldp x19, x30, [sp], #16
  mov w0, #1602
  ret
.La64_writable_w_self_case1:
  ldp x19, x30, [sp], #16
  mov w0, #1603
  ret
.size a64_writable_w_self_escape, .-a64_writable_w_self_escape

.p2align 2
// A PAGEOFF relocation authenticates only the low 12-bit fragment.  The base
// is deliberately unrelated to the matching ADRP, so trusting the loader-side
// target without a reaching-definition proof would spuriously expose this
// writable table and turn the independent dispatch into a trap.
.globl a64_pageoff_wrong_base_no_escape
.type a64_pageoff_wrong_base_no_escape, %function
a64_pageoff_wrong_base_no_escape:
  stp x19, x30, [sp, #-16]!
  mov w19, w0
  adrp x2, a64_pageoff_wrong_base_table
  mov x0, xzr
  add x0, x0, :lo12:a64_pageoff_wrong_base_table
  bl a64_writable_unknown_callee
  and w10, w19, #1
  adrp x9, a64_pageoff_wrong_base_table
  add x9, x9, :lo12:a64_pageoff_wrong_base_table
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_wrong_case0:
  ldp x19, x30, [sp], #16
  mov w0, #1610
  ret
.La64_pageoff_wrong_case1:
  ldp x19, x30, [sp], #16
  mov w0, #1611
  ret
.size a64_pageoff_wrong_base_no_escape, .-a64_pageoff_wrong_base_no_escape

.p2align 2
// The matching ADRP is overwritten before PAGEOFF consumes its register.  A
// lexical ADRP/ADD pair is not an exact address certificate.
.globl a64_pageoff_clobbered_base_no_escape
.type a64_pageoff_clobbered_base_no_escape, %function
a64_pageoff_clobbered_base_no_escape:
  stp x19, x30, [sp, #-16]!
  mov w19, w0
  adrp x0, a64_pageoff_clobbered_base_table
  mov x0, xzr
  add x0, x0, :lo12:a64_pageoff_clobbered_base_table
  bl a64_writable_unknown_callee
  and w10, w19, #1
  adrp x9, a64_pageoff_clobbered_base_table
  add x9, x9, :lo12:a64_pageoff_clobbered_base_table
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_clobbered_case0:
  ldp x19, x30, [sp], #16
  mov w0, #1612
  ret
.La64_pageoff_clobbered_case1:
  ldp x19, x30, [sp], #16
  mov w0, #1613
  ret
.size a64_pageoff_clobbered_base_no_escape, .-a64_pageoff_clobbered_base_no_escape

.p2align 2
.globl a64_writable_unknown_callee
.type a64_writable_unknown_callee, %function
a64_writable_unknown_callee:
  ret
.size a64_writable_unknown_callee, .-a64_writable_unknown_callee

.section .data.jt_a64_writable_arg_alias,"aw",%progbits
.p2align 3
.La64_writable_arg_alias_table:
  .xword .La64_writable_arg_alias_case0
  .xword .La64_writable_arg_alias_case1
.size .La64_writable_arg_alias_table, .-.La64_writable_arg_alias_table

.section .data.jt_a64_writable_w_self,"aw",%progbits
.p2align 3
.globl a64_writable_w_self_table
.type a64_writable_w_self_table, %object
a64_writable_w_self_table:
  .xword .La64_writable_w_self_case0
  .xword .La64_writable_w_self_case1
.size a64_writable_w_self_table, .-a64_writable_w_self_table

.section .data.jt_a64_pageoff_wrong_base,"aw",%progbits
.p2align 3
.globl a64_pageoff_wrong_base_table
.type a64_pageoff_wrong_base_table, %object
a64_pageoff_wrong_base_table:
  .xword .La64_pageoff_wrong_case0
  .xword .La64_pageoff_wrong_case1
.size a64_pageoff_wrong_base_table, .-a64_pageoff_wrong_base_table

.section .data.jt_a64_pageoff_clobbered,"aw",%progbits
.p2align 3
.globl a64_pageoff_clobbered_base_table
.type a64_pageoff_clobbered_base_table, %object
a64_pageoff_clobbered_base_table:
  .xword .La64_pageoff_clobbered_case0
  .xword .La64_pageoff_clobbered_case1
.size a64_pageoff_clobbered_base_table, .-a64_pageoff_clobbered_base_table

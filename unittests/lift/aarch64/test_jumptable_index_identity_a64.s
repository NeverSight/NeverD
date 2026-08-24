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
// A literal-zero selector must not borrow a four-entry domain from an x&3
// producer on the architecturally dead fallthrough of `cbz w10`.  CFG
// discovery still decodes that lexical block, but it has no path to the
// table-index use.
.globl a64_constant_selector_unreachable_mask
.type a64_constant_selector_unreachable_mask, %function
a64_constant_selector_unreachable_mask:
  mov w10, wzr
  cbz w10, .La64_constant_selector_dispatch
.La64_constant_selector_dead_mask:
  and w10, w0, #3
  mov w0, #1624
  ret
.La64_constant_selector_dispatch:
  adrp x9, a64_constant_selector_table
  add x9, x9, :lo12:a64_constant_selector_table
  ldr x11, [x9, w10, uxtw #3]
  br x11
.La64_constant_selector_case0:
  mov w0, #1620
  ret
.La64_constant_selector_case1:
  mov w0, #1621
  ret
.La64_constant_selector_case2:
  mov w0, #1622
  ret
.La64_constant_selector_case3:
  mov w0, #1623
  ret
.size a64_constant_selector_unreachable_mask, .-a64_constant_selector_unreachable_mask

.p2align 2
// This is q-q*5, not x-(x/5)*5: UDIV destructively overwrites the register that
// held x before the later SUB reads it.  Physical register-name equality across
// that defining occurrence must not authenticate a five-value modulo domain.
.globl a64_destructive_udiv_is_not_remainder
.type a64_destructive_udiv_is_not_remainder, %function
a64_destructive_udiv_is_not_remainder:
  mov w1, #5
  udiv w0, w0, w1
  mul w2, w0, w1
  sub w0, w0, w2
  adrp x9, a64_destructive_udiv_table
  add x9, x9, :lo12:a64_destructive_udiv_table
  ldr x10, [x9, w0, uxtw #3]
  br x10
.La64_destructive_udiv_case0:
  mov w0, #1630
  ret
.La64_destructive_udiv_case1:
  mov w0, #1631
  ret
.La64_destructive_udiv_case2:
  mov w0, #1632
  ret
.La64_destructive_udiv_case3:
  mov w0, #1633
  ret
.La64_destructive_udiv_case4:
  mov w0, #1634
  ret
.size a64_destructive_udiv_is_not_remainder, .-a64_destructive_udiv_is_not_remainder

.p2align 2
// The literal entry coordinate opens case zero.  Its exact unsigned
// `x-(x/5)*5` producer then opens the complete five-slot table and returns to
// the same dispatch.  The four lexically earlier cases deliberately contain
// more than the flat proposal prefix, so the producer must be retained and
// replayed after those case edges become reachable.
.globl a64_explicit_udiv_modulo_lfp
.type a64_explicit_udiv_modulo_lfp, %function
a64_explicit_udiv_modulo_lfp:
  mov w10, wzr
.La64_explicit_udiv_dispatch:
  adrp x9, a64_explicit_udiv_modulo_lfp_table
  add x9, x9, :lo12:a64_explicit_udiv_modulo_lfp_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_udiv_case1:
  .rept 140
  add x11, x11, #1
  .endr
  mov w0, #1641
  ret
.La64_explicit_udiv_case2:
  .rept 140
  add x11, x11, #1
  .endr
  mov w0, #1642
  ret
.La64_explicit_udiv_case3:
  .rept 140
  add x11, x11, #1
  .endr
  mov w0, #1643
  ret
.La64_explicit_udiv_case4:
  .rept 140
  add x11, x11, #1
  .endr
  mov w0, #1644
  ret
.La64_explicit_udiv_case0:
  mov w1, #5
  udiv w2, w0, w1
  // Keep the scalar first: symbolic multiplication is commutative, so operand
  // order cannot be part of the exact remainder certificate.
  mul w3, w1, w2
  sub w10, w0, w3
  b .La64_explicit_udiv_dispatch
.size a64_explicit_udiv_modulo_lfp, .-a64_explicit_udiv_modulo_lfp

.p2align 2
// Signed division does not bound negative inputs to an unsigned table domain.
.globl a64_explicit_sdiv_not_unsigned_modulo
.type a64_explicit_sdiv_not_unsigned_modulo, %function
a64_explicit_sdiv_not_unsigned_modulo:
  mov w1, #5
  sdiv w2, w0, w1
  mul w3, w2, w1
  sub w10, w0, w3
  adrp x9, a64_explicit_sdiv_not_unsigned_modulo_table
  add x9, x9, :lo12:a64_explicit_sdiv_not_unsigned_modulo_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_sdiv_case0: mov w0, #1650; ret
.La64_explicit_sdiv_case1: mov w0, #1651; ret
.La64_explicit_sdiv_case2: mov w0, #1652; ret
.La64_explicit_sdiv_case3: mov w0, #1653; ret
.La64_explicit_sdiv_case4: mov w0, #1654; ret
.size a64_explicit_sdiv_not_unsigned_modulo, .-a64_explicit_sdiv_not_unsigned_modulo

.p2align 2
// The quotient and subtraction must use the same dividend expression.
.globl a64_explicit_udiv_foreign_dividend
.type a64_explicit_udiv_foreign_dividend, %function
a64_explicit_udiv_foreign_dividend:
  mov w1, #5
  udiv w2, w0, w1
  mul w3, w2, w1
  sub w10, w4, w3
  adrp x9, a64_explicit_udiv_foreign_dividend_table
  add x9, x9, :lo12:a64_explicit_udiv_foreign_dividend_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_foreign_case0: mov w0, #1660; ret
.La64_explicit_foreign_case1: mov w0, #1661; ret
.La64_explicit_foreign_case2: mov w0, #1662; ret
.La64_explicit_foreign_case3: mov w0, #1663; ret
.La64_explicit_foreign_case4: mov w0, #1664; ret
.size a64_explicit_udiv_foreign_dividend, .-a64_explicit_udiv_foreign_dividend

.p2align 2
// A table-sized back multiply cannot repair a quotient divided by another N.
.globl a64_explicit_udiv_wrong_divisor
.type a64_explicit_udiv_wrong_divisor, %function
a64_explicit_udiv_wrong_divisor:
  mov w1, #7
  udiv w2, w0, w1
  mov w5, #5
  mul w3, w2, w5
  sub w10, w0, w3
  adrp x9, a64_explicit_udiv_wrong_divisor_table
  add x9, x9, :lo12:a64_explicit_udiv_wrong_divisor_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_wrong_divisor_case0: mov w0, #1670; ret
.La64_explicit_wrong_divisor_case1: mov w0, #1671; ret
.La64_explicit_wrong_divisor_case2: mov w0, #1672; ret
.La64_explicit_wrong_divisor_case3: mov w0, #1673; ret
.La64_explicit_wrong_divisor_case4: mov w0, #1674; ret
.size a64_explicit_udiv_wrong_divisor, .-a64_explicit_udiv_wrong_divisor

.p2align 2
// Conversely, q=(x/5) followed by q*7 is not x%7 merely because the table has
// seven physical entries.
.globl a64_explicit_udiv_wrong_multiplier
.type a64_explicit_udiv_wrong_multiplier, %function
a64_explicit_udiv_wrong_multiplier:
  mov w1, #5
  udiv w2, w0, w1
  mov w5, #7
  mul w3, w2, w5
  sub w10, w0, w3
  adrp x9, a64_explicit_udiv_wrong_multiplier_table
  add x9, x9, :lo12:a64_explicit_udiv_wrong_multiplier_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_wrong_multiplier_case0: mov w0, #1680; ret
.La64_explicit_wrong_multiplier_case1: mov w0, #1681; ret
.La64_explicit_wrong_multiplier_case2: mov w0, #1682; ret
.La64_explicit_wrong_multiplier_case3: mov w0, #1683; ret
.La64_explicit_wrong_multiplier_case4: mov w0, #1684; ret
.La64_explicit_wrong_multiplier_case5: mov w0, #1685; ret
.La64_explicit_wrong_multiplier_case6: mov w0, #1686; ret
.size a64_explicit_udiv_wrong_multiplier, .-a64_explicit_udiv_wrong_multiplier

.p2align 2
// Case zero initially proves x%6.  Once that opens case five, its edge enters
// the middle of the retained recipe with a foreign quotient.  Full-graph exact
// replay must invalidate the producer before publication.
.globl a64_explicit_udiv_late_interior
.type a64_explicit_udiv_late_interior, %function
a64_explicit_udiv_late_interior:
  mov w10, wzr
.La64_explicit_interior_dispatch:
  adrp x9, a64_explicit_udiv_late_interior_table
  add x9, x9, :lo12:a64_explicit_udiv_late_interior_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_interior_case0:
  mov w1, #6
  udiv w2, w0, w1
.La64_explicit_interior_product:
  mul w3, w2, w1
  sub w10, w0, w3
  b .La64_explicit_interior_dispatch
.La64_explicit_interior_case1: mov w0, #1691; ret
.La64_explicit_interior_case2: mov w0, #1692; ret
.La64_explicit_interior_case3: mov w0, #1693; ret
.La64_explicit_interior_case4: mov w0, #1694; ret
.La64_explicit_interior_case5:
  mov w1, #6
  mov w2, #1
  b .La64_explicit_interior_product
.size a64_explicit_udiv_late_interior, .-a64_explicit_udiv_late_interior

.p2align 2
// The producer remains exact, but the last newly authorized case writes a
// selector outside [0,5) before returning to the same dispatch.  Final domain
// replay must reject this table independently of recipe recognition.
.globl a64_explicit_udiv_late_selector_escape
.type a64_explicit_udiv_late_selector_escape, %function
a64_explicit_udiv_late_selector_escape:
  mov w10, wzr
.La64_explicit_escape_dispatch:
  adrp x9, a64_explicit_udiv_late_selector_escape_table
  add x9, x9, :lo12:a64_explicit_udiv_late_selector_escape_table
  ldr x12, [x9, w10, uxtw #3]
  br x12
.La64_explicit_escape_case0:
  mov w1, #5
  udiv w2, w0, w1
  mul w3, w2, w1
  sub w10, w0, w3
  b .La64_explicit_escape_dispatch
.La64_explicit_escape_case1: mov w0, #1701; ret
.La64_explicit_escape_case2: mov w0, #1702; ret
.La64_explicit_escape_case3: mov w0, #1703; ret
.La64_explicit_escape_case4:
  mov w10, #0x100
  b .La64_explicit_escape_dispatch
.size a64_explicit_udiv_late_selector_escape, .-a64_explicit_udiv_late_selector_escape

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

.section .rodata.jt_a64_constant_selector,"a",%progbits
.p2align 3
.globl a64_constant_selector_table
.type a64_constant_selector_table, %object
a64_constant_selector_table:
  .xword .La64_constant_selector_case0
  .xword .La64_constant_selector_case1
  .xword .La64_constant_selector_case2
  .xword .La64_constant_selector_case3
.size a64_constant_selector_table, .-a64_constant_selector_table

.section .rodata.jt_a64_destructive_udiv,"a",%progbits
.p2align 3
.globl a64_destructive_udiv_table
.type a64_destructive_udiv_table, %object
a64_destructive_udiv_table:
  .xword .La64_destructive_udiv_case0
  .xword .La64_destructive_udiv_case1
  .xword .La64_destructive_udiv_case2
  .xword .La64_destructive_udiv_case3
  .xword .La64_destructive_udiv_case4
.size a64_destructive_udiv_table, .-a64_destructive_udiv_table

.section .rodata.jt_a64_explicit_udiv_lfp,"a",%progbits
.p2align 3
.globl a64_explicit_udiv_modulo_lfp_table
.type a64_explicit_udiv_modulo_lfp_table, %object
a64_explicit_udiv_modulo_lfp_table:
  .xword .La64_explicit_udiv_case0
  .xword .La64_explicit_udiv_case1
  .xword .La64_explicit_udiv_case2
  .xword .La64_explicit_udiv_case3
  .xword .La64_explicit_udiv_case4
.size a64_explicit_udiv_modulo_lfp_table, .-a64_explicit_udiv_modulo_lfp_table

.section .rodata.jt_a64_explicit_sdiv,"a",%progbits
.p2align 3
.globl a64_explicit_sdiv_not_unsigned_modulo_table
.type a64_explicit_sdiv_not_unsigned_modulo_table, %object
a64_explicit_sdiv_not_unsigned_modulo_table:
  .xword .La64_explicit_sdiv_case0
  .xword .La64_explicit_sdiv_case1
  .xword .La64_explicit_sdiv_case2
  .xword .La64_explicit_sdiv_case3
  .xword .La64_explicit_sdiv_case4
.size a64_explicit_sdiv_not_unsigned_modulo_table, .-a64_explicit_sdiv_not_unsigned_modulo_table

.section .rodata.jt_a64_explicit_foreign,"a",%progbits
.p2align 3
.globl a64_explicit_udiv_foreign_dividend_table
.type a64_explicit_udiv_foreign_dividend_table, %object
a64_explicit_udiv_foreign_dividend_table:
  .xword .La64_explicit_foreign_case0
  .xword .La64_explicit_foreign_case1
  .xword .La64_explicit_foreign_case2
  .xword .La64_explicit_foreign_case3
  .xword .La64_explicit_foreign_case4
.size a64_explicit_udiv_foreign_dividend_table, .-a64_explicit_udiv_foreign_dividend_table

.section .rodata.jt_a64_explicit_wrong_divisor,"a",%progbits
.p2align 3
.globl a64_explicit_udiv_wrong_divisor_table
.type a64_explicit_udiv_wrong_divisor_table, %object
a64_explicit_udiv_wrong_divisor_table:
  .xword .La64_explicit_wrong_divisor_case0
  .xword .La64_explicit_wrong_divisor_case1
  .xword .La64_explicit_wrong_divisor_case2
  .xword .La64_explicit_wrong_divisor_case3
  .xword .La64_explicit_wrong_divisor_case4
.size a64_explicit_udiv_wrong_divisor_table, .-a64_explicit_udiv_wrong_divisor_table

.section .rodata.jt_a64_explicit_wrong_multiplier,"a",%progbits
.p2align 3
.globl a64_explicit_udiv_wrong_multiplier_table
.type a64_explicit_udiv_wrong_multiplier_table, %object
a64_explicit_udiv_wrong_multiplier_table:
  .xword .La64_explicit_wrong_multiplier_case0
  .xword .La64_explicit_wrong_multiplier_case1
  .xword .La64_explicit_wrong_multiplier_case2
  .xword .La64_explicit_wrong_multiplier_case3
  .xword .La64_explicit_wrong_multiplier_case4
  .xword .La64_explicit_wrong_multiplier_case5
  .xword .La64_explicit_wrong_multiplier_case6
.size a64_explicit_udiv_wrong_multiplier_table, .-a64_explicit_udiv_wrong_multiplier_table

.section .rodata.jt_a64_explicit_late_interior,"a",%progbits
.p2align 3
.globl a64_explicit_udiv_late_interior_table
.type a64_explicit_udiv_late_interior_table, %object
a64_explicit_udiv_late_interior_table:
  .xword .La64_explicit_interior_case0
  .xword .La64_explicit_interior_case1
  .xword .La64_explicit_interior_case2
  .xword .La64_explicit_interior_case3
  .xword .La64_explicit_interior_case4
  .xword .La64_explicit_interior_case5
.size a64_explicit_udiv_late_interior_table, .-a64_explicit_udiv_late_interior_table

.section .rodata.jt_a64_explicit_late_escape,"a",%progbits
.p2align 3
.globl a64_explicit_udiv_late_selector_escape_table
.type a64_explicit_udiv_late_selector_escape_table, %object
a64_explicit_udiv_late_selector_escape_table:
  .xword .La64_explicit_escape_case0
  .xword .La64_explicit_escape_case1
  .xword .La64_explicit_escape_case2
  .xword .La64_explicit_escape_case3
  .xword .La64_explicit_escape_case4
.size a64_explicit_udiv_late_selector_escape_table, .-a64_explicit_udiv_late_selector_escape_table

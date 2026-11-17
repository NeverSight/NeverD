// A compare of the index lane is the domain. The copies that produced the
// lane are not. Eighty copies sit past the guard-expression depth, so a
// proof that unfolds them fails closed. The cases below the compare are
// real; the following word is not an entry.

.text
.p2align 2

.globl a64_deep_w_sub_x_index
.type a64_deep_w_sub_x_index, %function
a64_deep_w_sub_x_index:
  .rept 80
  mov w21, w21
  .endr
  sub w8, w21, #1
  cmp w8, #3
  b.hi .Lsub_default
  adrp x9, .Lsub_table
  add x9, x9, :lo12:.Lsub_table
  adr x10, .Lsub_anchor
.Lsub_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lsub_c0:
  mov w0, #10
  ret
.Lsub_c1:
  mov w0, #11
  ret
.Lsub_c2:
  mov w0, #12
  ret
.Lsub_c3:
  mov w0, #13
  ret
.Lsub_poison:
  mov w0, #499
  ret
.Lsub_default:
  mov w0, #-1
  ret
.size a64_deep_w_sub_x_index, .-a64_deep_w_sub_x_index

// cmp w24, #4 then and x8, x24, #0xffffffff. The mask keeps the compared
// lane. The copies of w24 are not the domain.
.globl a64_deep_full_lane_and
.type a64_deep_full_lane_and, %function
a64_deep_full_lane_and:
  .rept 80
  mov w24, w24
  .endr
  cmp w24, #4
  b.hi .Land_default
  adrp x11, .Land_table
  and x8, x24, #0xffffffff
  add x11, x11, :lo12:.Land_table
  adr x9, .Land_anchor
.Land_anchor:
  ldrsw x10, [x11, x8, lsl #2]
  add x9, x9, x10
  br x9
.Land_c0:
  mov w0, #20
  ret
.Land_c1:
  mov w0, #21
  ret
.Land_c2:
  mov w0, #22
  ret
.Land_c3:
  mov w0, #23
  ret
.Land_c4:
  mov w0, #24
  ret
.Land_poison:
  mov w0, #499
  ret
.Land_default:
  mov w0, #-1
  ret
.size a64_deep_full_lane_and, .-a64_deep_full_lane_and

// cmp x9 does not read the sub result in w8. The bound is not the index.
.globl a64_lane_compare_other_reg
.type a64_lane_compare_other_reg, %function
a64_lane_compare_other_reg:
  sub w8, w0, #1
  cmp x9, #3
  b.hi .Lother_default
  adrp x12, .Lother_table
  add x12, x12, :lo12:.Lother_table
  adr x10, .Lother_anchor
.Lother_anchor:
  ldrsw x11, [x12, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lother_c0:
  mov w0, #30
  ret
.Lother_c1:
  mov w0, #31
  ret
.Lother_c2:
  mov w0, #32
  ret
.Lother_c3:
  mov w0, #33
  ret
.Lother_poison:
  mov w0, #499
  ret
.Lother_default:
  mov w0, #-1
  ret
.size a64_lane_compare_other_reg, .-a64_lane_compare_other_reg

// mov x8, x1 after the compare replaces the lane. The old compare is not
// the value that reaches the load.
.globl a64_lane_compare_clobber
.type a64_lane_compare_clobber, %function
a64_lane_compare_clobber:
  sub w8, w0, #1
  cmp w8, #3
  b.hi .Lclobber_default
  mov x8, x1
  adrp x9, .Lclobber_table
  add x9, x9, :lo12:.Lclobber_table
  adr x10, .Lclobber_anchor
.Lclobber_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lclobber_c0:
  mov w0, #40
  ret
.Lclobber_c1:
  mov w0, #41
  ret
.Lclobber_c2:
  mov w0, #42
  ret
.Lclobber_c3:
  mov w0, #43
  ret
.Lclobber_poison:
  mov w0, #499
  ret
.Lclobber_default:
  mov w0, #-1
  ret
.size a64_lane_compare_clobber, .-a64_lane_compare_clobber

// Six hundred selects and twenty-nine controlling branches exhaust the
// alias-syntax allowance before the compare. That scan is not the compare.
// Eighty copies of the lane are past the guard depth. The compare is the
// domain. The word after the four slots is not an entry.
.globl a64_alias_budget_keeps_lane_compare
.type a64_alias_budget_keeps_lane_compare, %function
a64_alias_budget_keeps_lane_compare:
  .rept 600
  csel w10, w11, w12, eq
  .endr
  .rept 29
  cbz x13, .Lalias_default
  .endr
  .rept 80
  mov w21, w21
  .endr
  sub w8, w21, #1
  cmp w8, #3
  b.hi .Lalias_default
  adrp x9, .Lalias_table
  add x9, x9, :lo12:.Lalias_table
  adr x10, .Lalias_anchor
.Lalias_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lalias_c0:
  mov w0, #50
  ret
.Lalias_c1:
  mov w0, #51
  ret
.Lalias_c2:
  mov w0, #52
  ret
.Lalias_c3:
  mov w0, #53
  ret
.Lalias_poison:
  mov w0, #499
  ret
.Lalias_default:
  mov w0, #-1
  ret
.size a64_alias_budget_keeps_lane_compare, .-a64_alias_budget_keeps_lane_compare

// Forty adds in each of four blocks, ahead of the compare. Collecting them
// finishes inside the ordinary syntax account and leaves too little to
// materialize every guard. Fewer adds lets every guard finish, so the test
// passes without keeping the compare. More adds spends the account during
// collection and withdraws the compare. The compare of w8 is the domain.
// The word after `cmp #3` is not an entry.
.globl a64_guard_budget_keeps_lane_compare
.type a64_guard_budget_keeps_lane_compare, %function
a64_guard_budget_keeps_lane_compare:
  mov w20, w0
  .rept 40
  add w20, w20, #1
  .endr
  cbz w20, .Lguard_default
  mov w22, w1
  .rept 40
  add w22, w22, #1
  .endr
  cbz w22, .Lguard_default
  mov w23, w2
  .rept 40
  add w23, w23, #1
  .endr
  cbz w23, .Lguard_default
  mov w24, w3
  .rept 40
  add w24, w24, #1
  .endr
  cbz w24, .Lguard_default
  sub w8, w8, #1
  cmp w8, #3
  b.hi .Lguard_default
  adrp x9, .Lguard_table
  add x9, x9, :lo12:.Lguard_table
  adr x10, .Lguard_anchor
.Lguard_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lguard_c0:
  mov w0, #60
  ret
.Lguard_c1:
  mov w0, #61
  ret
.Lguard_c2:
  mov w0, #62
  ret
.Lguard_c3:
  mov w0, #63
  ret
.Lguard_poison:
  mov w0, #499
  ret
.Lguard_default:
  mov w0, #-1
  ret
.size a64_guard_budget_keeps_lane_compare, .-a64_guard_budget_keeps_lane_compare

// Fifty adds leave eight syntax units, which cannot build the compare.
// The compare is walked once more on the ordinary account. The word after
// `cmp #3` is not an entry.
.globl a64_guard_budget_retries_lane_compare
.type a64_guard_budget_retries_lane_compare, %function
a64_guard_budget_retries_lane_compare:
  mov w20, w0
  .rept 50
  add w20, w20, #1
  .endr
  cbz w20, .Lretry_default
  mov w22, w1
  .rept 50
  add w22, w22, #1
  .endr
  cbz w22, .Lretry_default
  mov w23, w2
  .rept 50
  add w23, w23, #1
  .endr
  cbz w23, .Lretry_default
  mov w24, w3
  .rept 50
  add w24, w24, #1
  .endr
  cbz w24, .Lretry_default
  sub w8, w8, #1
  cmp w8, #3
  b.hi .Lretry_default
  adrp x9, .Lretry_table
  add x9, x9, :lo12:.Lretry_table
  adr x10, .Lretry_anchor
.Lretry_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lretry_c0:
  mov w0, #70
  ret
.Lretry_c1:
  mov w0, #71
  ret
.Lretry_c2:
  mov w0, #72
  ret
.Lretry_c3:
  mov w0, #73
  ret
.Lretry_poison:
  mov w0, #499
  ret
.Lretry_default:
  mov w0, #-1
  ret
.size a64_guard_budget_retries_lane_compare, .-a64_guard_budget_retries_lane_compare

// ldrh >> 12 is 0..15. XOR #8 permutes that prefix. cmp #0xf; b.eq drops
// the top value. Slot 15 is not an entry.
.globl a64_half_shift_xor_excludes_top
.type a64_half_shift_xor_excludes_top, %function
a64_half_shift_xor_excludes_top:
  ldrh w1, [x0]
  lsr w8, w1, #12
  eor w8, w8, #8
  cmp w8, #0xf
  b.eq .Lsx_default
  adrp x9, .Lsx_table
  add x9, x9, :lo12:.Lsx_table
  adr x10, .Lsx_anchor
.Lsx_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lsx_c0: mov w0, #10; ret
.Lsx_c1: mov w0, #11; ret
.Lsx_c2: mov w0, #12; ret
.Lsx_c3: mov w0, #13; ret
.Lsx_c4: mov w0, #14; ret
.Lsx_c5: mov w0, #15; ret
.Lsx_c6: mov w0, #16; ret
.Lsx_c7: mov w0, #17; ret
.Lsx_c8: mov w0, #18; ret
.Lsx_c9: mov w0, #19; ret
.Lsx_c10: mov w0, #20; ret
.Lsx_c11: mov w0, #21; ret
.Lsx_c12: mov w0, #22; ret
.Lsx_c13: mov w0, #23; ret
.Lsx_c14: mov w0, #24; ret
.Lsx_poison: mov w0, #499; ret
.Lsx_default: mov w0, #-1; ret
.size a64_half_shift_xor_excludes_top, .-a64_half_shift_xor_excludes_top

// The same prefix with b.hi keeps every value, including 15. The word after
// that prefix is not an entry.
.globl a64_half_shift_xor_keeps_prefix
.type a64_half_shift_xor_keeps_prefix, %function
a64_half_shift_xor_keeps_prefix:
  ldrh w1, [x0]
  lsr w8, w1, #12
  eor w8, w8, #8
  cmp w8, #0xf
  b.hi .Lsh_default
  adrp x9, .Lsh_table
  add x9, x9, :lo12:.Lsh_table
  adr x10, .Lsh_anchor
.Lsh_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lsh_c0: mov w0, #30; ret
.Lsh_c1: mov w0, #31; ret
.Lsh_c2: mov w0, #32; ret
.Lsh_c3: mov w0, #33; ret
.Lsh_c4: mov w0, #34; ret
.Lsh_c5: mov w0, #35; ret
.Lsh_c6: mov w0, #36; ret
.Lsh_c7: mov w0, #37; ret
.Lsh_c8: mov w0, #38; ret
.Lsh_c9: mov w0, #39; ret
.Lsh_c10: mov w0, #40; ret
.Lsh_c11: mov w0, #41; ret
.Lsh_c12: mov w0, #42; ret
.Lsh_c13: mov w0, #43; ret
.Lsh_c14: mov w0, #44; ret
.Lsh_c15: mov w0, #45; ret
.Lsh_poison: mov w0, #499; ret
.Lsh_default: mov w0, #-1; ret
.size a64_half_shift_xor_keeps_prefix, .-a64_half_shift_xor_keeps_prefix

// cmp #0xf; b.eq on a parameter has no prefix. The excluded value is a hole
// in a much wider domain, so the table stays unresolved.
.globl a64_eq_without_prefix
.type a64_eq_without_prefix, %function
a64_eq_without_prefix:
  cmp w8, #0xf
  b.eq .Leq_default
  adrp x9, .Leq_table
  add x9, x9, :lo12:.Leq_table
  adr x10, .Leq_anchor
.Leq_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Leq_c0: mov w0, #46; ret
.Leq_c1: mov w0, #47; ret
.Leq_poison: mov w0, #499; ret
.Leq_default: mov w0, #-1; ret
.size a64_eq_without_prefix, .-a64_eq_without_prefix

// A mask before the shift is not the full-width certificate. The equality
// exit must not invent a 15-entry domain.
.globl a64_mask_before_shift_excludes_top
.type a64_mask_before_shift_excludes_top, %function
a64_mask_before_shift_excludes_top:
  and w1, w1, #0xffff
  lsr w8, w1, #12
  eor w8, w8, #8
  cmp w8, #0xf
  b.eq .Lms_default
  adrp x9, .Lms_table
  add x9, x9, :lo12:.Lms_table
  adr x10, .Lms_anchor
.Lms_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lms_c0: mov w0, #48; ret
.Lms_c1: mov w0, #49; ret
.Lms_poison: mov w0, #499; ret
.Lms_default: mov w0, #-1; ret
.size a64_mask_before_shift_excludes_top, .-a64_mask_before_shift_excludes_top

// Both ANDs are `x0 & 0xff`. The compare of that byte is the domain.
// #0xff is not a full-lane mask. Slot 4 is not an entry.
.globl a64_same_source_byte_and
.type a64_same_source_byte_and, %function
a64_same_source_byte_and:
  and w8, w0, #0xff
  cmp w8, #3
  b.hi .Lss_default
  and x8, x0, #0xff
  adrp x9, .Lss_table
  add x9, x9, :lo12:.Lss_table
  adr x10, .Lss_anchor
.Lss_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lss_c0: mov w0, #50; ret
.Lss_c1: mov w0, #51; ret
.Lss_c2: mov w0, #52; ret
.Lss_c3: mov w0, #53; ret
.Lss_poison: mov w0, #499; ret
.Lss_default: mov w0, #-1; ret
.size a64_same_source_byte_and, .-a64_same_source_byte_and

// A write of the source between the two ANDs replaces the lane.
.globl a64_same_source_byte_and_clobber
.type a64_same_source_byte_and_clobber, %function
a64_same_source_byte_and_clobber:
  and w8, w0, #0xff
  cmp w8, #3
  b.hi .Lsc_default
  mov w0, w1
  and x8, x0, #0xff
  adrp x9, .Lsc_table
  add x9, x9, :lo12:.Lsc_table
  adr x10, .Lsc_anchor
.Lsc_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lsc_c0: mov w0, #54; ret
.Lsc_c1: mov w0, #55; ret
.Lsc_poison: mov w0, #499; ret
.Lsc_default: mov w0, #-1; ret
.size a64_same_source_byte_and_clobber, .-a64_same_source_byte_and_clobber

// The second mask is not the mask that was compared.
.globl a64_same_source_byte_and_mismatch
.type a64_same_source_byte_and_mismatch, %function
a64_same_source_byte_and_mismatch:
  and w8, w0, #0xff
  cmp w8, #3
  b.hi .Lsm_default
  and x8, x0, #0xf
  adrp x9, .Lsm_table
  add x9, x9, :lo12:.Lsm_table
  adr x10, .Lsm_anchor
.Lsm_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lsm_c0: mov w0, #56; ret
.Lsm_c1: mov w0, #57; ret
.Lsm_poison: mov w0, #499; ret
.Lsm_default: mov w0, #-1; ret
.size a64_same_source_byte_and_mismatch, .-a64_same_source_byte_and_mismatch

// `(n ^ 7) & 0xf == 0` excludes index 15 of `(n ^ 8) & 0xf`. The raw mask
// of 16 entries would include that slot.
.globl a64_correlated_mask_excludes_top
.type a64_correlated_mask_excludes_top, %function
a64_correlated_mask_excludes_top:
  eor w11, w0, #7
  tst w11, #0xf
  b.eq .Lcm_default
  eor w0, w0, #8
  and x0, x0, #0xf
  adrp x9, .Lcm_table
  add x9, x9, :lo12:.Lcm_table
  adr x10, .Lcm_anchor
.Lcm_anchor:
  ldrsw x11, [x9, x0, lsl #2]
  add x10, x10, x11
  br x10
.Lcm_c0: mov w0, #60; ret
.Lcm_c1: mov w0, #61; ret
.Lcm_c2: mov w0, #62; ret
.Lcm_c3: mov w0, #63; ret
.Lcm_c4: mov w0, #64; ret
.Lcm_c5: mov w0, #65; ret
.Lcm_c6: mov w0, #66; ret
.Lcm_c7: mov w0, #67; ret
.Lcm_c8: mov w0, #68; ret
.Lcm_c9: mov w0, #69; ret
.Lcm_c10: mov w0, #70; ret
.Lcm_c11: mov w0, #71; ret
.Lcm_c12: mov w0, #72; ret
.Lcm_c13: mov w0, #73; ret
.Lcm_c14: mov w0, #74; ret
.Lcm_poison: mov w0, #499; ret
.Lcm_default: mov w0, #-1; ret
.size a64_correlated_mask_excludes_top, .-a64_correlated_mask_excludes_top

// XOR #1 excludes index (1 ^ 8) & 0xf = 9, an interior hole. The raw mask
// must not fill it in.
.globl a64_correlated_mask_interior
.type a64_correlated_mask_interior, %function
a64_correlated_mask_interior:
  eor w11, w0, #1
  tst w11, #0xf
  b.eq .Lci_default
  eor w0, w0, #8
  and x0, x0, #0xf
  adrp x9, .Lci_table
  add x9, x9, :lo12:.Lci_table
  adr x10, .Lci_anchor
.Lci_anchor:
  ldrsw x11, [x9, x0, lsl #2]
  add x10, x10, x11
  br x10
.Lci_c0: mov w0, #75; ret
.Lci_c1: mov w0, #76; ret
.Lci_poison: mov w0, #499; ret
.Lci_default: mov w0, #-1; ret
.size a64_correlated_mask_interior, .-a64_correlated_mask_interior

// `cmp w9, #100` reads a value that `and x9, x0, #7` replaces. The mask is
// the domain. The ninth word is not an entry.
.globl a64_dead_reg_and_mask
.type a64_dead_reg_and_mask, %function
a64_dead_reg_and_mask:
  cmp w9, #100
  b.hi .Ldead_default
  and x9, x0, #7
  adrp x10, .Ldead_table
  add x10, x10, :lo12:.Ldead_table
  adr x11, .Ldead_anchor
.Ldead_anchor:
  ldrsw x12, [x10, x9, lsl #2]
  add x11, x11, x12
  br x11
.Ldead_c0: mov w0, #80; ret
.Ldead_c1: mov w0, #81; ret
.Ldead_c2: mov w0, #82; ret
.Ldead_c3: mov w0, #83; ret
.Ldead_c4: mov w0, #84; ret
.Ldead_c5: mov w0, #85; ret
.Ldead_c6: mov w0, #86; ret
.Ldead_c7: mov w0, #87; ret
.Ldead_poison: mov w0, #499; ret
.Ldead_default: mov w0, #-1; ret
.size a64_dead_reg_and_mask, .-a64_dead_reg_and_mask

// The compare is after the mask, so it is the domain. Index 4 stays out.
.globl a64_and_then_tighter_compare
.type a64_and_then_tighter_compare, %function
a64_and_then_tighter_compare:
  and x9, x0, #7
  cmp x9, #3
  b.hi .Ltight_default
  adrp x10, .Ltight_table
  add x10, x10, :lo12:.Ltight_table
  adr x11, .Ltight_anchor
.Ltight_anchor:
  ldrsw x12, [x10, x9, lsl #2]
  add x11, x11, x12
  br x11
.Ltight_c0: mov w0, #90; ret
.Ltight_c1: mov w0, #91; ret
.Ltight_c2: mov w0, #92; ret
.Ltight_c3: mov w0, #93; ret
.Ltight_poison: mov w0, #499; ret
.Ltight_default: mov w0, #-1; ret
.size a64_and_then_tighter_compare, .-a64_and_then_tighter_compare

// `str x8` stores the zero-extend of `sub w8`. `cmp w10` reads that lane
// back. The clobber of x8 is not the value in the slot. Index 4 stays out.
.globl a64_w_compare_x_spill
.type a64_w_compare_x_spill, %function
a64_w_compare_x_spill:
  sub sp, sp, #16
  sub w8, w0, #1
  str x8, [sp, #8]
  mov x8, #0
  ldr x10, [sp, #8]
  cmp w10, #3
  b.hi .Lspill_default
  adrp x11, .Lspill_table
  add x11, x11, :lo12:.Lspill_table
  adr x12, .Lspill_anchor
.Lspill_anchor:
  ldrsw x13, [x11, x10, lsl #2]
  add x12, x12, x13
  br x12
.Lspill_c0:
  mov w0, #100
  add sp, sp, #16
  ret
.Lspill_c1:
  mov w0, #101
  add sp, sp, #16
  ret
.Lspill_c2:
  mov w0, #102
  add sp, sp, #16
  ret
.Lspill_c3:
  mov w0, #103
  add sp, sp, #16
  ret
.Lspill_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lspill_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_w_compare_x_spill, .-a64_w_compare_x_spill

// `mov x8, x0` does not zero-extend a W definition. The W compare is not
// the X reload.
.globl a64_x_spill_without_zext
.type a64_x_spill_without_zext, %function
a64_x_spill_without_zext:
  sub sp, sp, #16
  mov x8, x0
  str x8, [sp, #8]
  ldr x10, [sp, #8]
  cmp w10, #3
  b.hi .Lxz_default
  adrp x11, .Lxz_table
  add x11, x11, :lo12:.Lxz_table
  adr x12, .Lxz_anchor
.Lxz_anchor:
  ldrsw x13, [x11, x10, lsl #2]
  add x12, x12, x13
  br x12
.Lxz_c0:
  mov w0, #110
  add sp, sp, #16
  ret
.Lxz_c1:
  mov w0, #111
  add sp, sp, #16
  ret
.Lxz_c2:
  mov w0, #112
  add sp, sp, #16
  ret
.Lxz_c3:
  mov w0, #113
  add sp, sp, #16
  ret
.Lxz_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lxz_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_x_spill_without_zext, .-a64_x_spill_without_zext

// `sub w8` zero-extends into the stored X. The compare and the index are
// two reloads of that slot, in different registers. A call between the
// store and the reloads does not replace the slot. `adr x9` after the
// compare does not replace the index reload. Index 4 stays out.
.globl a64_same_slot_zext_reload
.type a64_same_slot_zext_reload, %function
a64_same_slot_zext_reload:
  sub sp, sp, #16
  sub w8, w0, #1
  str x8, [sp, #8]
  mov x8, #0
  bl a64_pac_roundtrip_leaf
  ldr x9, [sp, #8]
  cmp w9, #3
  b.hi .Lsame_default
  ldr x10, [sp, #8]
  adrp x11, .Lsame_table
  add x11, x11, :lo12:.Lsame_table
  adr x9, .Lsame_anchor
.Lsame_anchor:
  ldrsw x12, [x11, x10, lsl #2]
  add x9, x9, x12
  br x9
.Lsame_c0:
  mov w0, #140
  add sp, sp, #16
  ret
.Lsame_c1:
  mov w0, #141
  add sp, sp, #16
  ret
.Lsame_c2:
  mov w0, #142
  add sp, sp, #16
  ret
.Lsame_c3:
  mov w0, #143
  add sp, sp, #16
  ret
.Lsame_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lsame_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_same_slot_zext_reload, .-a64_same_slot_zext_reload

// `mov x8, x0` does not zero-extend a W definition. Two reloads of that
// slot are not the W compare.
.globl a64_same_slot_plain_x
.type a64_same_slot_plain_x, %function
a64_same_slot_plain_x:
  sub sp, sp, #16
  mov x8, x0
  str x8, [sp, #8]
  mov x8, #0
  bl a64_pac_roundtrip_leaf
  ldr x9, [sp, #8]
  cmp w9, #3
  b.hi .Lsamex_default
  ldr x10, [sp, #8]
  adrp x11, .Lsamex_table
  add x11, x11, :lo12:.Lsamex_table
  adr x9, .Lsamex_anchor
.Lsamex_anchor:
  ldrsw x12, [x11, x10, lsl #2]
  add x9, x9, x12
  br x9
.Lsamex_c0:
  mov w0, #150
  add sp, sp, #16
  ret
.Lsamex_c1:
  mov w0, #151
  add sp, sp, #16
  ret
.Lsamex_c2:
  mov w0, #152
  add sp, sp, #16
  ret
.Lsamex_c3:
  mov w0, #153
  add sp, sp, #16
  ret
.Lsamex_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lsamex_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_same_slot_plain_x, .-a64_same_slot_plain_x

// A later plain X store replaces the zero-extended lane.
.globl a64_same_slot_zext_clobber
.type a64_same_slot_zext_clobber, %function
a64_same_slot_zext_clobber:
  sub sp, sp, #16
  sub w8, w0, #1
  str x8, [sp, #8]
  mov x8, x1
  str x8, [sp, #8]
  ldr x9, [sp, #8]
  cmp w9, #3
  b.hi .Lsameclobber_default
  ldr x10, [sp, #8]
  adrp x11, .Lsameclobber_table
  add x11, x11, :lo12:.Lsameclobber_table
  adr x9, .Lsameclobber_anchor
.Lsameclobber_anchor:
  ldrsw x12, [x11, x10, lsl #2]
  add x9, x9, x12
  br x9
.Lsameclobber_c0:
  mov w0, #160
  add sp, sp, #16
  ret
.Lsameclobber_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lsameclobber_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_same_slot_zext_clobber, .-a64_same_slot_zext_clobber

// `str x8` is the zero-extend of the W load from the compared slot. The
// later reload of that slot is the byte-table index. Index 3 stays out.
.globl a64_copied_slot_byte_table
.type a64_copied_slot_byte_table, %function
a64_copied_slot_byte_table:
  sub sp, sp, #16
  str w0, [sp, #12]
  ldr w8, [sp, #12]
  mov w8, w8
  str x8, [sp]
  mov x8, #0
  ldr w8, [sp, #12]
  cmp w8, #2
  b.hi .Lcopy_default
  ldr x11, [sp]
  adrp x12, .Lcopy_table
  add x12, x12, :lo12:.Lcopy_table
  adr x8, .Lcopy_anchor
.Lcopy_anchor:
  ldrb w9, [x12, x11]
  add x8, x8, x9, lsl #2
  br x8
.Lcopy_c0:
  mov w0, #120
  add sp, sp, #16
  ret
.Lcopy_c1:
  mov w0, #121
  add sp, sp, #16
  ret
.Lcopy_c2:
  mov w0, #122
  add sp, sp, #16
  ret
.Lcopy_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lcopy_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_copied_slot_byte_table, .-a64_copied_slot_byte_table

// A store to the compared slot after the copy replaces the lane.
.globl a64_copied_slot_clobber
.type a64_copied_slot_clobber, %function
a64_copied_slot_clobber:
  sub sp, sp, #16
  str w0, [sp, #12]
  ldr w8, [sp, #12]
  mov w8, w8
  str x8, [sp]
  str w1, [sp, #12]
  ldr w8, [sp, #12]
  cmp w8, #2
  b.hi .Lclobber_slot_default
  ldr x11, [sp]
  adrp x12, .Lclobber_slot_table
  add x12, x12, :lo12:.Lclobber_slot_table
  adr x8, .Lclobber_slot_anchor
.Lclobber_slot_anchor:
  ldrb w9, [x12, x11]
  add x8, x8, x9, lsl #2
  br x8
.Lclobber_slot_c0:
  mov w0, #130
  add sp, sp, #16
  ret
.Lclobber_slot_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lclobber_slot_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_copied_slot_clobber, .-a64_copied_slot_clobber

// The case calls this and branches back to the reload. The leaf itself
// does not receive the frame.
.globl a64_pac_roundtrip_leaf
.type a64_pac_roundtrip_leaf, %function
a64_pac_roundtrip_leaf:
  ret
.size a64_pac_roundtrip_leaf, .-a64_pac_roundtrip_leaf

// paciasp signs x30 with SP, and the shadow store publishes that signed
// link. The modifier is not a pointer to the spilled index. A case call
// that branches back to the reload still reads the same slot. cmp #2
// publishes three entries. The next byte stays out.
.globl a64_copied_slot_pac_roundtrip
.type a64_copied_slot_pac_roundtrip, %function
a64_copied_slot_pac_roundtrip:
  paciasp
  sub sp, sp, #16
  str x30, [x18]
  str w0, [sp, #12]
  ldr w8, [sp, #12]
  mov w8, w8
  str x8, [sp]
  b .Lpac_header
.Lpac_header:
  ldr w8, [sp, #12]
  cmp w8, #2
  b.hi .Lpac_default
  ldr x11, [sp]
  adrp x12, .Lpac_table
  add x12, x12, :lo12:.Lpac_table
  adr x8, .Lpac_anchor
.Lpac_anchor:
  ldrb w9, [x12, x11]
  add x8, x8, x9, lsl #2
  br x8
.Lpac_c0:
  bl a64_pac_roundtrip_leaf
  b .Lpac_header
.Lpac_c1:
  mov w0, #181
  add sp, sp, #16
  ret
.Lpac_c2:
  mov w0, #182
  add sp, sp, #16
  ret
.Lpac_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lpac_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_copied_slot_pac_roundtrip, .-a64_copied_slot_pac_roundtrip

// The same round trip passes SP to the callee. That publishes the frame,
// so the spill is not private across the call and the branch stays a trap.
.globl a64_copied_slot_pac_passes_sp
.type a64_copied_slot_pac_passes_sp, %function
a64_copied_slot_pac_passes_sp:
  paciasp
  sub sp, sp, #16
  str x30, [x18]
  str w0, [sp, #12]
  ldr w8, [sp, #12]
  mov w8, w8
  str x8, [sp]
  b .Lpacesc_header
.Lpacesc_header:
  ldr w8, [sp, #12]
  cmp w8, #2
  b.hi .Lpacesc_default
  ldr x11, [sp]
  adrp x12, .Lpacesc_table
  add x12, x12, :lo12:.Lpacesc_table
  adr x8, .Lpacesc_anchor
.Lpacesc_anchor:
  ldrb w9, [x12, x11]
  add x8, x8, x9, lsl #2
  br x8
.Lpacesc_c0:
  mov x0, sp
  bl a64_pac_roundtrip_leaf
  b .Lpacesc_header
.Lpacesc_c1:
  mov w0, #191
  add sp, sp, #16
  ret
.Lpacesc_poison:
  mov w0, #499
  add sp, sp, #16
  ret
.Lpacesc_default:
  mov w0, #-1
  add sp, sp, #16
  ret
.size a64_copied_slot_pac_passes_sp, .-a64_copied_slot_pac_passes_sp

// mov w8, w0 before the compare. adrp then replaces w0. The byte-table
// index is that copy, including the later same-lane mov w8, w8. cmp #3
// publishes four slots. The following byte is not an entry.
.globl a64_copied_register_byte_table
.type a64_copied_register_byte_table, %function
a64_copied_register_byte_table:
  mov w8, w0
  cmp w0, #3
  adrp x0, .Lregcopy_anchor
  add x0, x0, #0x16a
  b.hi .Lregcopy_default
  adrp x9, .Lregcopy_table
  mov w8, w8
  add x9, x9, :lo12:.Lregcopy_table
  adr x10, .Lregcopy_anchor
.Lregcopy_anchor:
  ldrb w11, [x9, x8]
  add x10, x10, x11, lsl #2
  mov x8, xzr
  br x10
.Lregcopy_c0:
  mov w0, #140
  ret
.Lregcopy_c1:
  mov w0, #141
  ret
.Lregcopy_c2:
  mov w0, #142
  ret
.Lregcopy_c3:
  mov w0, #143
  ret
.Lregcopy_poison:
  mov w0, #499
  ret
.Lregcopy_default:
  mov w0, #-1
  ret
.size a64_copied_register_byte_table, .-a64_copied_register_byte_table

// A write of the copied register after the compare replaces the lane.
.globl a64_copied_register_clobber
.type a64_copied_register_clobber, %function
a64_copied_register_clobber:
  mov w8, w0
  cmp w0, #3
  adrp x0, .Lregclobber_anchor
  add x0, x0, #0x16a
  b.hi .Lregclobber_default
  mov w8, w1
  adrp x9, .Lregclobber_table
  add x9, x9, :lo12:.Lregclobber_table
  adr x10, .Lregclobber_anchor
.Lregclobber_anchor:
  ldrb w11, [x9, x8]
  add x10, x10, x11, lsl #2
  br x10
.Lregclobber_c0:
  mov w0, #150
  ret
.Lregclobber_poison:
  mov w0, #499
  ret
.Lregclobber_default:
  mov w0, #-1
  ret
.size a64_copied_register_clobber, .-a64_copied_register_clobber

// mov w8, w1 does not copy the compared register.
.globl a64_copied_register_other
.type a64_copied_register_other, %function
a64_copied_register_other:
  mov w8, w1
  cmp w0, #3
  adrp x0, .Lregother_anchor
  add x0, x0, #0x16a
  b.hi .Lregother_default
  mov w8, w8
  adrp x9, .Lregother_table
  add x9, x9, :lo12:.Lregother_table
  adr x10, .Lregother_anchor
.Lregother_anchor:
  ldrb w11, [x9, x8]
  add x10, x10, x11, lsl #2
  br x10
.Lregother_c0:
  mov w0, #160
  ret
.Lregother_poison:
  mov w0, #499
  ret
.Lregother_default:
  mov w0, #-1
  ret
.size a64_copied_register_other, .-a64_copied_register_other

// A write of w0 between the copy and the compare is a different value.
.globl a64_copied_register_source_clobber
.type a64_copied_register_source_clobber, %function
a64_copied_register_source_clobber:
  mov w8, w0
  mov w0, w1
  cmp w0, #3
  adrp x0, .Lregsource_anchor
  add x0, x0, #0x16a
  b.hi .Lregsource_default
  mov w8, w8
  adrp x9, .Lregsource_table
  add x9, x9, :lo12:.Lregsource_table
  adr x10, .Lregsource_anchor
.Lregsource_anchor:
  ldrb w11, [x9, x8]
  add x10, x10, x11, lsl #2
  br x10
.Lregsource_c0:
  mov w0, #170
  ret
.Lregsource_poison:
  mov w0, #499
  ret
.Lregsource_default:
  mov w0, #-1
  ret
.size a64_copied_register_source_clobber, .-a64_copied_register_source_clobber

// cmp w0, #3. One in-range slot is the b.hi target, a bti j; ret past the
// next function's paciasp. That instruction is already this function's edge.
// The byte after the compare is not an entry.
.globl a64_shared_tail_byte_table
.type a64_shared_tail_byte_table, %function
a64_shared_tail_byte_table:
  cmp w0, #3
  b.hi .Lshared_ret
  adrp x9, .Lshared_table
  add x9, x9, :lo12:.Lshared_table
  adr x10, .Lshared_anchor
.Lshared_anchor:
  ldrb w11, [x9, w0, uxtw]
  add x10, x10, x11, lsl #2
  br x10
.Lshared_c0:
  mov w0, #140
  ret
.Lshared_c1:
  mov w0, #141
  ret
.Lshared_c3:
  mov w0, #143
  ret
.size a64_shared_tail_byte_table, .-a64_shared_tail_byte_table

.globl a64_shared_tail_owner
.type a64_shared_tail_owner, %function
a64_shared_tail_owner:
  paciasp
  autiasp
.Lshared_ret:
  bti j
  ret
.Lshared_poison:
  mov w0, #499
  ret
.size a64_shared_tail_owner, .-a64_shared_tail_owner

// An in-range slot names an interior of the next function that this
// function never branches to. The table stays unresolved.
.globl a64_foreign_slot_byte_table
.type a64_foreign_slot_byte_table, %function
a64_foreign_slot_byte_table:
  cmp w0, #1
  b.hi .Lforeign_direct
  adrp x9, .Lforeign_table
  add x9, x9, :lo12:.Lforeign_table
  adr x10, .Lforeign_anchor
.Lforeign_anchor:
  ldrb w11, [x9, w0, uxtw]
  add x10, x10, x11, lsl #2
  br x10
.Lforeign_local:
  mov w0, #140
  ret
.size a64_foreign_slot_byte_table, .-a64_foreign_slot_byte_table

.globl a64_foreign_slot_owner
.type a64_foreign_slot_owner, %function
a64_foreign_slot_owner:
  paciasp
.Lforeign_interior:
  mov w0, #499
  ret
.Lforeign_direct:
  bti j
  ret
.size a64_foreign_slot_owner, .-a64_foreign_slot_owner

// Thirty-two compares of the incoming argument are other controlling
// branches. Their value queries are not the compare of the subtracted lane.
// cmp w8, #3 publishes four slots. The following word is not an entry.
.globl a64_guard_budget_keeps_word_compare
.type a64_guard_budget_keeps_word_compare, %function
a64_guard_budget_keeps_word_compare:
  .rept 32
  cmp w0, #1
  b.hi .Lwordq_default
  .endr
  sub w8, w0, #1
  cmp w8, #3
  b.hi .Lwordq_default
  adrp x9, .Lwordq_table
  add x9, x9, :lo12:.Lwordq_table
  adr x10, .Lwordq_anchor
.Lwordq_anchor:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lwordq_c0:
  mov w0, #70
  ret
.Lwordq_c1:
  mov w0, #71
  ret
.Lwordq_c2:
  mov w0, #72
  ret
.Lwordq_c3:
  mov w0, #73
  ret
.Lwordq_poison:
  mov w0, #499
  ret
.Lwordq_default:
  mov w0, #-1
  ret
.size a64_guard_budget_keeps_word_compare, .-a64_guard_budget_keeps_word_compare

.section .rodata.a64_deep_w_sub_x_index, "a", %progbits
.p2align 2
.Lsub_table:
  .word .Lsub_c0 - .Lsub_anchor
  .word .Lsub_c1 - .Lsub_anchor
  .word .Lsub_c2 - .Lsub_anchor
  .word .Lsub_c3 - .Lsub_anchor
  .word .Lsub_poison - .Lsub_anchor

.section .rodata.a64_deep_full_lane_and, "a", %progbits
.p2align 2
.Land_table:
  .word .Land_c0 - .Land_anchor
  .word .Land_c1 - .Land_anchor
  .word .Land_c2 - .Land_anchor
  .word .Land_c3 - .Land_anchor
  .word .Land_c4 - .Land_anchor
  .word .Land_poison - .Land_anchor

.section .rodata.a64_lane_compare_other_reg, "a", %progbits
.p2align 2
.Lother_table:
  .word .Lother_c0 - .Lother_anchor
  .word .Lother_c1 - .Lother_anchor
  .word .Lother_c2 - .Lother_anchor
  .word .Lother_c3 - .Lother_anchor
  .word .Lother_poison - .Lother_anchor

.section .rodata.a64_lane_compare_clobber, "a", %progbits
.p2align 2
.Lclobber_table:
  .word .Lclobber_c0 - .Lclobber_anchor
  .word .Lclobber_c1 - .Lclobber_anchor
  .word .Lclobber_c2 - .Lclobber_anchor
  .word .Lclobber_c3 - .Lclobber_anchor
  .word .Lclobber_poison - .Lclobber_anchor

.section .rodata.a64_alias_budget_keeps_lane_compare, "a", %progbits
.p2align 2
.Lalias_table:
  .word .Lalias_c0 - .Lalias_anchor
  .word .Lalias_c1 - .Lalias_anchor
  .word .Lalias_c2 - .Lalias_anchor
  .word .Lalias_c3 - .Lalias_anchor
  .word .Lalias_poison - .Lalias_anchor

.section .rodata.a64_guard_budget_keeps_lane_compare, "a", %progbits
.p2align 2
.Lguard_table:
  .word .Lguard_c0 - .Lguard_anchor
  .word .Lguard_c1 - .Lguard_anchor
  .word .Lguard_c2 - .Lguard_anchor
  .word .Lguard_c3 - .Lguard_anchor
  .word .Lguard_poison - .Lguard_anchor

.section .rodata.a64_guard_budget_retries_lane_compare, "a", %progbits
.p2align 2
.Lretry_table:
  .word .Lretry_c0 - .Lretry_anchor
  .word .Lretry_c1 - .Lretry_anchor
  .word .Lretry_c2 - .Lretry_anchor
  .word .Lretry_c3 - .Lretry_anchor
  .word .Lretry_poison - .Lretry_anchor

.section .rodata.a64_half_shift_xor_excludes_top, "a", %progbits
.p2align 2
.Lsx_table:
  .word .Lsx_c0 - .Lsx_anchor
  .word .Lsx_c1 - .Lsx_anchor
  .word .Lsx_c2 - .Lsx_anchor
  .word .Lsx_c3 - .Lsx_anchor
  .word .Lsx_c4 - .Lsx_anchor
  .word .Lsx_c5 - .Lsx_anchor
  .word .Lsx_c6 - .Lsx_anchor
  .word .Lsx_c7 - .Lsx_anchor
  .word .Lsx_c8 - .Lsx_anchor
  .word .Lsx_c9 - .Lsx_anchor
  .word .Lsx_c10 - .Lsx_anchor
  .word .Lsx_c11 - .Lsx_anchor
  .word .Lsx_c12 - .Lsx_anchor
  .word .Lsx_c13 - .Lsx_anchor
  .word .Lsx_c14 - .Lsx_anchor
  .word .Lsx_poison - .Lsx_anchor

.section .rodata.a64_half_shift_xor_keeps_prefix, "a", %progbits
.p2align 2
.Lsh_table:
  .word .Lsh_c0 - .Lsh_anchor
  .word .Lsh_c1 - .Lsh_anchor
  .word .Lsh_c2 - .Lsh_anchor
  .word .Lsh_c3 - .Lsh_anchor
  .word .Lsh_c4 - .Lsh_anchor
  .word .Lsh_c5 - .Lsh_anchor
  .word .Lsh_c6 - .Lsh_anchor
  .word .Lsh_c7 - .Lsh_anchor
  .word .Lsh_c8 - .Lsh_anchor
  .word .Lsh_c9 - .Lsh_anchor
  .word .Lsh_c10 - .Lsh_anchor
  .word .Lsh_c11 - .Lsh_anchor
  .word .Lsh_c12 - .Lsh_anchor
  .word .Lsh_c13 - .Lsh_anchor
  .word .Lsh_c14 - .Lsh_anchor
  .word .Lsh_c15 - .Lsh_anchor
  .word .Lsh_poison - .Lsh_anchor

.section .rodata.a64_eq_without_prefix, "a", %progbits
.p2align 2
.Leq_table:
  .word .Leq_c0 - .Leq_anchor
  .word .Leq_c1 - .Leq_anchor
  .word .Leq_poison - .Leq_anchor

.section .rodata.a64_mask_before_shift_excludes_top, "a", %progbits
.p2align 2
.Lms_table:
  .word .Lms_c0 - .Lms_anchor
  .word .Lms_c1 - .Lms_anchor
  .word .Lms_poison - .Lms_anchor

.section .rodata.a64_same_source_byte_and, "a", %progbits
.p2align 2
.Lss_table:
  .word .Lss_c0 - .Lss_anchor
  .word .Lss_c1 - .Lss_anchor
  .word .Lss_c2 - .Lss_anchor
  .word .Lss_c3 - .Lss_anchor
  .word .Lss_poison - .Lss_anchor

.section .rodata.a64_same_source_byte_and_clobber, "a", %progbits
.p2align 2
.Lsc_table:
  .word .Lsc_c0 - .Lsc_anchor
  .word .Lsc_c1 - .Lsc_anchor
  .word .Lsc_poison - .Lsc_anchor

.section .rodata.a64_same_source_byte_and_mismatch, "a", %progbits
.p2align 2
.Lsm_table:
  .word .Lsm_c0 - .Lsm_anchor
  .word .Lsm_c1 - .Lsm_anchor
  .word .Lsm_poison - .Lsm_anchor

.section .rodata.a64_correlated_mask_excludes_top, "a", %progbits
.p2align 2
.Lcm_table:
  .word .Lcm_c0 - .Lcm_anchor
  .word .Lcm_c1 - .Lcm_anchor
  .word .Lcm_c2 - .Lcm_anchor
  .word .Lcm_c3 - .Lcm_anchor
  .word .Lcm_c4 - .Lcm_anchor
  .word .Lcm_c5 - .Lcm_anchor
  .word .Lcm_c6 - .Lcm_anchor
  .word .Lcm_c7 - .Lcm_anchor
  .word .Lcm_c8 - .Lcm_anchor
  .word .Lcm_c9 - .Lcm_anchor
  .word .Lcm_c10 - .Lcm_anchor
  .word .Lcm_c11 - .Lcm_anchor
  .word .Lcm_c12 - .Lcm_anchor
  .word .Lcm_c13 - .Lcm_anchor
  .word .Lcm_c14 - .Lcm_anchor
  .word .Lcm_poison - .Lcm_anchor

.section .rodata.a64_correlated_mask_interior, "a", %progbits
.p2align 2
.Lci_table:
  .word .Lci_c0 - .Lci_anchor
  .word .Lci_c1 - .Lci_anchor
  .word .Lci_poison - .Lci_anchor

.section .rodata.a64_dead_reg_and_mask, "a", %progbits
.p2align 2
.Ldead_table:
  .word .Ldead_c0 - .Ldead_anchor
  .word .Ldead_c1 - .Ldead_anchor
  .word .Ldead_c2 - .Ldead_anchor
  .word .Ldead_c3 - .Ldead_anchor
  .word .Ldead_c4 - .Ldead_anchor
  .word .Ldead_c5 - .Ldead_anchor
  .word .Ldead_c6 - .Ldead_anchor
  .word .Ldead_c7 - .Ldead_anchor
  .word .Ldead_poison - .Ldead_anchor

.section .rodata.a64_and_then_tighter_compare, "a", %progbits
.p2align 2
.Ltight_table:
  .word .Ltight_c0 - .Ltight_anchor
  .word .Ltight_c1 - .Ltight_anchor
  .word .Ltight_c2 - .Ltight_anchor
  .word .Ltight_c3 - .Ltight_anchor
  .word .Ltight_poison - .Ltight_anchor
  .word .Ltight_poison - .Ltight_anchor
  .word .Ltight_poison - .Ltight_anchor
  .word .Ltight_poison - .Ltight_anchor

.section .rodata.a64_w_compare_x_spill, "a", %progbits
.p2align 2
.Lspill_table:
  .word .Lspill_c0 - .Lspill_anchor
  .word .Lspill_c1 - .Lspill_anchor
  .word .Lspill_c2 - .Lspill_anchor
  .word .Lspill_c3 - .Lspill_anchor
  .word .Lspill_poison - .Lspill_anchor

.section .rodata.a64_x_spill_without_zext, "a", %progbits
.p2align 2
.Lxz_table:
  .word .Lxz_c0 - .Lxz_anchor
  .word .Lxz_c1 - .Lxz_anchor
  .word .Lxz_c2 - .Lxz_anchor
  .word .Lxz_c3 - .Lxz_anchor
  .word .Lxz_poison - .Lxz_anchor

.section .rodata.a64_same_slot_zext_reload, "a", %progbits
.p2align 2
.Lsame_table:
  .word .Lsame_c0 - .Lsame_anchor
  .word .Lsame_c1 - .Lsame_anchor
  .word .Lsame_c2 - .Lsame_anchor
  .word .Lsame_c3 - .Lsame_anchor
  .word .Lsame_poison - .Lsame_anchor

.section .rodata.a64_same_slot_plain_x, "a", %progbits
.p2align 2
.Lsamex_table:
  .word .Lsamex_c0 - .Lsamex_anchor
  .word .Lsamex_c1 - .Lsamex_anchor
  .word .Lsamex_c2 - .Lsamex_anchor
  .word .Lsamex_c3 - .Lsamex_anchor
  .word .Lsamex_poison - .Lsamex_anchor

.section .rodata.a64_same_slot_zext_clobber, "a", %progbits
.p2align 2
.Lsameclobber_table:
  .word .Lsameclobber_c0 - .Lsameclobber_anchor
  .word .Lsameclobber_poison - .Lsameclobber_anchor

.section .rodata.a64_copied_slot_byte_table, "a", %progbits
.Lcopy_table:
  .byte (.Lcopy_c0 - .Lcopy_anchor) / 4
  .byte (.Lcopy_c1 - .Lcopy_anchor) / 4
  .byte (.Lcopy_c2 - .Lcopy_anchor) / 4
  .byte (.Lcopy_poison - .Lcopy_anchor) / 4

.section .rodata.a64_copied_slot_clobber, "a", %progbits
.Lclobber_slot_table:
  .byte (.Lclobber_slot_c0 - .Lclobber_slot_anchor) / 4
  .byte (.Lclobber_slot_poison - .Lclobber_slot_anchor) / 4
  .byte (.Lclobber_slot_poison - .Lclobber_slot_anchor) / 4
  .byte (.Lclobber_slot_poison - .Lclobber_slot_anchor) / 4

.section .rodata.a64_copied_slot_pac_roundtrip, "a", %progbits
.Lpac_table:
  .byte (.Lpac_c0 - .Lpac_anchor) / 4
  .byte (.Lpac_c1 - .Lpac_anchor) / 4
  .byte (.Lpac_c2 - .Lpac_anchor) / 4
  .byte (.Lpac_poison - .Lpac_anchor) / 4

.section .rodata.a64_copied_slot_pac_passes_sp, "a", %progbits
.Lpacesc_table:
  .byte (.Lpacesc_c0 - .Lpacesc_anchor) / 4
  .byte (.Lpacesc_c1 - .Lpacesc_anchor) / 4
  .byte (.Lpacesc_poison - .Lpacesc_anchor) / 4
  .byte (.Lpacesc_poison - .Lpacesc_anchor) / 4

.section .rodata.a64_copied_register_byte_table, "a", %progbits
.Lregcopy_table:
  .byte (.Lregcopy_c0 - .Lregcopy_anchor) / 4
  .byte (.Lregcopy_c1 - .Lregcopy_anchor) / 4
  .byte (.Lregcopy_c2 - .Lregcopy_anchor) / 4
  .byte (.Lregcopy_c3 - .Lregcopy_anchor) / 4
  .byte (.Lregcopy_poison - .Lregcopy_anchor) / 4

.section .rodata.a64_copied_register_clobber, "a", %progbits
.Lregclobber_table:
  .byte (.Lregclobber_c0 - .Lregclobber_anchor) / 4
  .byte (.Lregclobber_poison - .Lregclobber_anchor) / 4
  .byte (.Lregclobber_poison - .Lregclobber_anchor) / 4
  .byte (.Lregclobber_poison - .Lregclobber_anchor) / 4
  .byte (.Lregclobber_poison - .Lregclobber_anchor) / 4

.section .rodata.a64_copied_register_other, "a", %progbits
.Lregother_table:
  .byte (.Lregother_c0 - .Lregother_anchor) / 4
  .byte (.Lregother_poison - .Lregother_anchor) / 4
  .byte (.Lregother_poison - .Lregother_anchor) / 4
  .byte (.Lregother_poison - .Lregother_anchor) / 4
  .byte (.Lregother_poison - .Lregother_anchor) / 4

.section .rodata.a64_copied_register_source_clobber, "a", %progbits
.Lregsource_table:
  .byte (.Lregsource_c0 - .Lregsource_anchor) / 4
  .byte (.Lregsource_poison - .Lregsource_anchor) / 4
  .byte (.Lregsource_poison - .Lregsource_anchor) / 4
  .byte (.Lregsource_poison - .Lregsource_anchor) / 4
  .byte (.Lregsource_poison - .Lregsource_anchor) / 4

.section .rodata.a64_shared_tail_byte_table, "a", %progbits
.Lshared_table:
  .byte (.Lshared_c0 - .Lshared_anchor) / 4
  .byte (.Lshared_c1 - .Lshared_anchor) / 4
  .byte (.Lshared_ret - .Lshared_anchor) / 4
  .byte (.Lshared_c3 - .Lshared_anchor) / 4
  .byte (.Lshared_poison - .Lshared_anchor) / 4

.section .rodata.a64_foreign_slot_byte_table, "a", %progbits
.Lforeign_table:
  .byte (.Lforeign_local - .Lforeign_anchor) / 4
  .byte (.Lforeign_interior - .Lforeign_anchor) / 4
  .byte (.Lforeign_interior - .Lforeign_anchor) / 4

.section .rodata.a64_guard_budget_keeps_word_compare, "a", %progbits
.p2align 2
.Lwordq_table:
  .word .Lwordq_c0 - .Lwordq_anchor
  .word .Lwordq_c1 - .Lwordq_anchor
  .word .Lwordq_c2 - .Lwordq_anchor
  .word .Lwordq_c3 - .Lwordq_anchor
  .word .Lwordq_poison - .Lwordq_anchor

// A W compare bounds the X index only when that index is the zero extension
// of the compared lane.  `and x2, x0, #0xffffffff` is that extension; the
// compared register is then overwritten so the table cannot borrow a live W
// alias.  A raw X index and an index masked from a different register stay
// unresolved.
	.text
	.globl	full_lane_zext_dispatch
	.p2align	2
	.type	full_lane_zext_dispatch,@function
full_lane_zext_dispatch:
	cmp	w0, #12
	and	x2, x0, #0xffffffff
	b.hi	.Lfull_default
	mov	w0, #99
	adrp	x9, .Lfull_table
	add	x9, x9, :lo12:.Lfull_table
	ldrsw	x8, [x9, x2, lsl #2]
	add	x8, x8, x9
	br	x8
.Lfull_c0:
	mov	w0, #10
	ret
.Lfull_c1:
	mov	w0, #11
	ret
.Lfull_c2:
	mov	w0, #12
	ret
.Lfull_c3:
	mov	w0, #13
	ret
.Lfull_c4:
	mov	w0, #14
	ret
.Lfull_c5:
	mov	w0, #15
	ret
.Lfull_c6:
	mov	w0, #16
	ret
.Lfull_c7:
	mov	w0, #17
	ret
.Lfull_c8:
	mov	w0, #18
	ret
.Lfull_c9:
	mov	w0, #19
	ret
.Lfull_c10:
	mov	w0, #20
	ret
.Lfull_c11:
	mov	w0, #21
	ret
.Lfull_c12:
	mov	w0, #22
	ret
.Lfull_default:
	mov	w0, wzr
	ret
	.size	full_lane_zext_dispatch, .-full_lane_zext_dispatch

	.globl	raw_x_index_dispatch
	.p2align	2
	.type	raw_x_index_dispatch,@function
raw_x_index_dispatch:
	cmp	w0, #12
	b.hi	.Lraw_default
	adrp	x9, .Lraw_table
	add	x9, x9, :lo12:.Lraw_table
	ldrsw	x8, [x9, x0, lsl #2]
	add	x8, x8, x9
	br	x8
.Lraw_c0:
	mov	w0, #10
	ret
.Lraw_c1:
	mov	w0, #11
	ret
.Lraw_c2:
	mov	w0, #12
	ret
.Lraw_c3:
	mov	w0, #13
	ret
.Lraw_c4:
	mov	w0, #14
	ret
.Lraw_c5:
	mov	w0, #15
	ret
.Lraw_c6:
	mov	w0, #16
	ret
.Lraw_c7:
	mov	w0, #17
	ret
.Lraw_c8:
	mov	w0, #18
	ret
.Lraw_c9:
	mov	w0, #19
	ret
.Lraw_c10:
	mov	w0, #20
	ret
.Lraw_c11:
	mov	w0, #21
	ret
.Lraw_c12:
	mov	w0, #22
	ret
.Lraw_default:
	mov	w0, wzr
	ret
	.size	raw_x_index_dispatch, .-raw_x_index_dispatch

	.globl	other_reg_mask_dispatch
	.p2align	2
	.type	other_reg_mask_dispatch,@function
other_reg_mask_dispatch:
	cmp	w0, #12
	and	x2, x1, #0xffffffff
	b.hi	.Lother_default
	adrp	x9, .Lother_table
	add	x9, x9, :lo12:.Lother_table
	ldrsw	x8, [x9, x2, lsl #2]
	add	x8, x8, x9
	br	x8
.Lother_c0:
	mov	w0, #10
	ret
.Lother_c1:
	mov	w0, #11
	ret
.Lother_c2:
	mov	w0, #12
	ret
.Lother_c3:
	mov	w0, #13
	ret
.Lother_c4:
	mov	w0, #14
	ret
.Lother_c5:
	mov	w0, #15
	ret
.Lother_c6:
	mov	w0, #16
	ret
.Lother_c7:
	mov	w0, #17
	ret
.Lother_c8:
	mov	w0, #18
	ret
.Lother_c9:
	mov	w0, #19
	ret
.Lother_c10:
	mov	w0, #20
	ret
.Lother_c11:
	mov	w0, #21
	ret
.Lother_c12:
	mov	w0, #22
	ret
.Lother_default:
	mov	w0, wzr
	ret
	.size	other_reg_mask_dispatch, .-other_reg_mask_dispatch

// The compare reads `ubfx w8, w0, #16, #8`. The table index is that field
// extracted again into X8, because `ldrsw [base, xIndex, lsl #2]` wants a
// 64-bit index. The field sits inside the compared W lane and the source is
// unchanged, so the second extract is the first. A different field, or a
// write of the source between the extracts, is not.
	.globl	reextract_ubfx_dispatch
	.p2align	2
	.type	reextract_ubfx_dispatch,@function
reextract_ubfx_dispatch:
	ubfx	w8, w0, #16, #8
	cmp	w8, #2
	b.hi	.Lre_default
	adrp	x9, .Lre_table
	add	x9, x9, :lo12:.Lre_table
	ubfx	x8, x0, #16, #8
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	mov	w2, #4
	br	x11
.Lre_c0:
	mov	w0, #30
	ret
.Lre_c1:
	mov	w0, #31
	ret
.Lre_c2:
	mov	w0, #32
	ret
.Lre_default:
	mov	w0, wzr
	ret
	.size	reextract_ubfx_dispatch, .-reextract_ubfx_dispatch

	.globl	reextract_ubfx_other_field
	.p2align	2
	.type	reextract_ubfx_other_field,@function
reextract_ubfx_other_field:
	ubfx	w8, w0, #16, #8
	cmp	w8, #2
	b.hi	.Lre_other_default
	adrp	x9, .Lre_other_table
	add	x9, x9, :lo12:.Lre_other_table
	ubfx	x8, x0, #8, #8
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lre_other_c0:
	mov	w0, #40
	ret
.Lre_other_c1:
	mov	w0, #41
	ret
.Lre_other_c2:
	mov	w0, #42
	ret
.Lre_other_default:
	mov	w0, wzr
	ret
	.size	reextract_ubfx_other_field, .-reextract_ubfx_other_field

	.globl	reextract_ubfx_clobbered_source
	.p2align	2
	.type	reextract_ubfx_clobbered_source,@function
reextract_ubfx_clobbered_source:
	ubfx	w8, w0, #16, #8
	cmp	w8, #2
	b.hi	.Lre_clobber_default
	mov	w0, w1
	adrp	x9, .Lre_clobber_table
	add	x9, x9, :lo12:.Lre_clobber_table
	ubfx	x8, x0, #16, #8
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lre_clobber_c0:
	mov	w0, #50
	ret
.Lre_clobber_c1:
	mov	w0, #51
	ret
.Lre_clobber_c2:
	mov	w0, #52
	ret
.Lre_clobber_default:
	mov	w0, wzr
	ret
	.size	reextract_ubfx_clobbered_source, .-reextract_ubfx_clobbered_source

// `and w19, w0, #0x18` keeps bits [3,4]. After a call that must preserve
// w19, `lsr w8, w19, #3` is exactly 0..3 and indexes four table words.
// The AND is in the predecessor, as in a switch arm that shifts a mask
// computed before the dispatch. A wider mask, an arithmetic shift of a
// mask whose sign bit is live, or a write of the masked register before
// the shift does not publish that table.
	.globl	shifted_mask_leaf
	.p2align	2
	.type	shifted_mask_leaf,@function
shifted_mask_leaf:
	ret
	.size	shifted_mask_leaf, .-shifted_mask_leaf

	.globl	shifted_mask_dispatch
	.p2align	2
	.type	shifted_mask_dispatch,@function
shifted_mask_dispatch:
	and	w19, w0, #0x18
	cbz	w1, .Lsm_use
	mov	w0, #1
	ret
.Lsm_use:
	bl	shifted_mask_leaf
	lsr	w8, w19, #3
	adrp	x9, .Lsm_table
	add	x9, x9, :lo12:.Lsm_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lsm_c0:
	mov	w0, #60
	ret
.Lsm_c1:
	mov	w0, #61
	ret
.Lsm_c2:
	mov	w0, #62
	ret
.Lsm_c3:
	mov	w0, #63
	ret
	.size	shifted_mask_dispatch, .-shifted_mask_dispatch

	.globl	shifted_mask_wide_dispatch
	.p2align	2
	.type	shifted_mask_wide_dispatch,@function
shifted_mask_wide_dispatch:
	and	w8, w0, #0x38
	lsr	w8, w8, #3
	adrp	x9, .Lsm_wide_table
	add	x9, x9, :lo12:.Lsm_wide_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lsm_wide_c0:
	mov	w0, #70
	ret
.Lsm_wide_c1:
	mov	w0, #71
	ret
.Lsm_wide_c2:
	mov	w0, #72
	ret
.Lsm_wide_c3:
	mov	w0, #73
	ret
	.size	shifted_mask_wide_dispatch, .-shifted_mask_wide_dispatch

	.globl	shifted_mask_signed_dispatch
	.p2align	2
	.type	shifted_mask_signed_dispatch,@function
shifted_mask_signed_dispatch:
	and	w8, w0, #0x80000000
	asr	w8, w8, #31
	adrp	x9, .Lsm_signed_table
	add	x9, x9, :lo12:.Lsm_signed_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lsm_signed_c0:
	mov	w0, #80
	ret
.Lsm_signed_c1:
	mov	w0, #81
	ret
	.size	shifted_mask_signed_dispatch, .-shifted_mask_signed_dispatch

	.globl	shifted_mask_clobbered_dispatch
	.p2align	2
	.type	shifted_mask_clobbered_dispatch,@function
shifted_mask_clobbered_dispatch:
	and	w8, w0, #0x18
	mov	w8, w1
	lsr	w8, w8, #3
	adrp	x9, .Lsm_clobber_table
	add	x9, x9, :lo12:.Lsm_clobber_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lsm_clobber_c0:
	mov	w0, #90
	ret
.Lsm_clobber_c1:
	mov	w0, #91
	ret
.Lsm_clobber_c2:
	mov	w0, #92
	ret
.Lsm_clobber_c3:
	mov	w0, #93
	ret
	.size	shifted_mask_clobbered_dispatch, .-shifted_mask_clobbered_dispatch

// `lsr w8, w0, #30` leaves exactly two bits. Those four values are the
// index. The source register is not. A load of a full W word is the same
// width even when a later AND keeps the low bits for a different use.
	.globl	full_width_shift_dispatch
	.p2align	2
	.type	full_width_shift_dispatch,@function
full_width_shift_dispatch:
	lsr	w8, w0, #30
	adrp	x9, .Lfw_table
	add	x9, x9, :lo12:.Lfw_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lfw_c0:
	mov	w0, #110
	ret
.Lfw_c1:
	mov	w0, #111
	ret
.Lfw_c2:
	mov	w0, #112
	ret
.Lfw_c3:
	mov	w0, #113
	ret
	.size	full_width_shift_dispatch, .-full_width_shift_dispatch

	.globl	full_width_shift_load_dispatch
	.p2align	2
	.type	full_width_shift_load_dispatch,@function
full_width_shift_load_dispatch:
	ldr	w9, [x1]
	lsr	w8, w9, #30
	and	w10, w9, #0x3fffffff
	adrp	x9, .Lfload_table
	add	x9, x9, :lo12:.Lfload_table
	adr	x11, .Lfload_anchor
	ldrb	w12, [x9, x8]
	add	x11, x11, x12, lsl #2
	br	x11
.Lfload_anchor:
	mov	w0, #120
	ret
.Lfload_c1:
	mov	w0, #121
	ret
.Lfload_c2:
	mov	w0, #122
	ret
.Lfload_c3:
	mov	w0, w10
	ret
	.size	full_width_shift_load_dispatch, .-full_width_shift_load_dispatch

	.globl	full_width_shift_asr_dispatch
	.p2align	2
	.type	full_width_shift_asr_dispatch,@function
full_width_shift_asr_dispatch:
	asr	w8, w0, #30
	adrp	x9, .Lfwasr_table
	add	x9, x9, :lo12:.Lfwasr_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lfwasr_c0:
	mov	w0, #130
	ret
.Lfwasr_c1:
	mov	w0, #131
	ret
.Lfwasr_c2:
	mov	w0, #132
	ret
.Lfwasr_c3:
	mov	w0, #133
	ret
	.size	full_width_shift_asr_dispatch, .-full_width_shift_asr_dispatch

	.globl	full_width_shift_wide_dispatch
	.p2align	2
	.type	full_width_shift_wide_dispatch,@function
full_width_shift_wide_dispatch:
	lsr	w8, w0, #28
	adrp	x9, .Lfwwide_table
	add	x9, x9, :lo12:.Lfwwide_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lfwwide_c0:
	mov	w0, #140
	ret
.Lfwwide_c1:
	mov	w0, #141
	ret
.Lfwwide_c2:
	mov	w0, #142
	ret
.Lfwwide_c3:
	mov	w0, #143
	ret
	.size	full_width_shift_wide_dispatch, .-full_width_shift_wide_dispatch

	.globl	full_width_shift_hole_dispatch
	.p2align	2
	.type	full_width_shift_hole_dispatch,@function
full_width_shift_hole_dispatch:
	and	w8, w0, #0x80000000
	lsr	w8, w8, #30
	adrp	x9, .Lfwhole_table
	add	x9, x9, :lo12:.Lfwhole_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lfwhole_c0:
	mov	w0, #150
	ret
.Lfwhole_c1:
	mov	w0, #151
	ret
.Lfwhole_c2:
	mov	w0, #152
	ret
.Lfwhole_c3:
	mov	w0, #153
	ret
	.size	full_width_shift_hole_dispatch, .-full_width_shift_hole_dispatch

	.globl	full_width_shift_clobber_dispatch
	.p2align	2
	.type	full_width_shift_clobber_dispatch,@function
full_width_shift_clobber_dispatch:
	lsr	w8, w0, #30
	mov	w8, w1
	adrp	x9, .Lfwclobber_table
	add	x9, x9, :lo12:.Lfwclobber_table
	ldrsw	x11, [x9, x8, lsl #2]
	add	x11, x11, x9
	br	x11
.Lfwclobber_c0:
	mov	w0, #160
	ret
.Lfwclobber_c1:
	mov	w0, #161
	ret
.Lfwclobber_c2:
	mov	w0, #162
	ret
.Lfwclobber_c3:
	mov	w0, #163
	ret
	.size	full_width_shift_clobber_dispatch, .-full_width_shift_clobber_dispatch

	.section	.rodata
	.p2align	2
.Lfull_table:
	.word	.Lfull_c0-.Lfull_table
	.word	.Lfull_c1-.Lfull_table
	.word	.Lfull_c2-.Lfull_table
	.word	.Lfull_c3-.Lfull_table
	.word	.Lfull_c4-.Lfull_table
	.word	.Lfull_c5-.Lfull_table
	.word	.Lfull_c6-.Lfull_table
	.word	.Lfull_c7-.Lfull_table
	.word	.Lfull_c8-.Lfull_table
	.word	.Lfull_c9-.Lfull_table
	.word	.Lfull_c10-.Lfull_table
	.word	.Lfull_c11-.Lfull_table
	.word	.Lfull_c12-.Lfull_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lraw_table:
	.word	.Lraw_c0-.Lraw_table
	.word	.Lraw_c1-.Lraw_table
	.word	.Lraw_c2-.Lraw_table
	.word	.Lraw_c3-.Lraw_table
	.word	.Lraw_c4-.Lraw_table
	.word	.Lraw_c5-.Lraw_table
	.word	.Lraw_c6-.Lraw_table
	.word	.Lraw_c7-.Lraw_table
	.word	.Lraw_c8-.Lraw_table
	.word	.Lraw_c9-.Lraw_table
	.word	.Lraw_c10-.Lraw_table
	.word	.Lraw_c11-.Lraw_table
	.word	.Lraw_c12-.Lraw_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lother_table:
	.word	.Lother_c0-.Lother_table
	.word	.Lother_c1-.Lother_table
	.word	.Lother_c2-.Lother_table
	.word	.Lother_c3-.Lother_table
	.word	.Lother_c4-.Lother_table
	.word	.Lother_c5-.Lother_table
	.word	.Lother_c6-.Lother_table
	.word	.Lother_c7-.Lother_table
	.word	.Lother_c8-.Lother_table
	.word	.Lother_c9-.Lother_table
	.word	.Lother_c10-.Lother_table
	.word	.Lother_c11-.Lother_table
	.word	.Lother_c12-.Lother_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lre_table:
	.word	.Lre_c0-.Lre_table
	.word	.Lre_c1-.Lre_table
	.word	.Lre_c2-.Lre_table
	.word	0x40000000
.Lre_other_table:
	.word	.Lre_other_c0-.Lre_other_table
	.word	.Lre_other_c1-.Lre_other_table
	.word	.Lre_other_c2-.Lre_other_table
	.word	0x40000000
.Lre_clobber_table:
	.word	.Lre_clobber_c0-.Lre_clobber_table
	.word	.Lre_clobber_c1-.Lre_clobber_table
	.word	.Lre_clobber_c2-.Lre_clobber_table
	.word	0x40000000
.Lsm_table:
	.word	.Lsm_c0-.Lsm_table
	.word	.Lsm_c1-.Lsm_table
	.word	.Lsm_c2-.Lsm_table
	.word	.Lsm_c3-.Lsm_table
	.word	0x40000000
.Lsm_wide_table:
	.word	.Lsm_wide_c0-.Lsm_wide_table
	.word	.Lsm_wide_c1-.Lsm_wide_table
	.word	.Lsm_wide_c2-.Lsm_wide_table
	.word	.Lsm_wide_c3-.Lsm_wide_table
	.word	0x40000000
.Lsm_signed_table:
	.word	.Lsm_signed_c0-.Lsm_signed_table
	.word	.Lsm_signed_c1-.Lsm_signed_table
	.word	0x40000000
.Lsm_clobber_table:
	.word	.Lsm_clobber_c0-.Lsm_clobber_table
	.word	.Lsm_clobber_c1-.Lsm_clobber_table
	.word	.Lsm_clobber_c2-.Lsm_clobber_table
	.word	.Lsm_clobber_c3-.Lsm_clobber_table
	.word	0x40000000
.Lfw_table:
	.word	.Lfw_c0-.Lfw_table
	.word	.Lfw_c1-.Lfw_table
	.word	.Lfw_c2-.Lfw_table
	.word	.Lfw_c3-.Lfw_table
	.word	0x40000000
.Lfload_table:
	.byte	(.Lfload_anchor-.Lfload_anchor)/4
	.byte	(.Lfload_c1-.Lfload_anchor)/4
	.byte	(.Lfload_c2-.Lfload_anchor)/4
	.byte	(.Lfload_c3-.Lfload_anchor)/4
	.byte	0xff
.Lfwasr_table:
	.word	.Lfwasr_c0-.Lfwasr_table
	.word	.Lfwasr_c1-.Lfwasr_table
	.word	.Lfwasr_c2-.Lfwasr_table
	.word	.Lfwasr_c3-.Lfwasr_table
	.word	0x40000000
.Lfwwide_table:
	.word	.Lfwwide_c0-.Lfwwide_table
	.word	.Lfwwide_c1-.Lfwwide_table
	.word	.Lfwwide_c2-.Lfwwide_table
	.word	.Lfwwide_c3-.Lfwwide_table
	.word	0x40000000
.Lfwhole_table:
	.word	.Lfwhole_c0-.Lfwhole_table
	.word	.Lfwhole_c1-.Lfwhole_table
	.word	.Lfwhole_c2-.Lfwhole_table
	.word	.Lfwhole_c3-.Lfwhole_table
.Lfwclobber_table:
	.word	.Lfwclobber_c0-.Lfwclobber_table
	.word	.Lfwclobber_c1-.Lfwclobber_table
	.word	.Lfwclobber_c2-.Lfwclobber_table
	.word	.Lfwclobber_c3-.Lfwclobber_table
	.section	.note.GNU-stack,"",@progbits

// Computed gotos through one of two label tables: `tb = (w1 & 1) ? A : B;
// goto *tb[idx]`.  The table read is an AArch64 register-offset load, which
// scales its raw index in the instruction, so the merged switch dispatches on
// that slot index plus the lower table's entry count when the higher table is
// selected.  Each function returns a constant that names the table and slot it
// reached: 1000 + 10 * form + slot for the lower table, 2000 + ... for the
// higher one.
// W index, zero-extended and scaled by the load.
	.text
	.globl	twotable_select_uxtw
	.p2align	2
	.type	twotable_select_uxtw,@function
twotable_select_uxtw:
	adrp	x9, .Lt0a
	add	x9, x9, :lo12:.Lt0a
	adrp	x10, .Lt0b
	add	x10, x10, :lo12:.Lt0b
	tst	w1, #0x1
	and	w8, w0, #0x3
	csel	x11, x9, x10, ne
	ldr	x8, [x11, w8, uxtw #3]
	br	x8
.Lf0a0:
	mov	w0, #1000
	ret
.Lf0a1:
	mov	w0, #1001
	ret
.Lf0a2:
	mov	w0, #1002
	ret
.Lf0a3:
	mov	w0, #1003
	ret
.Lf0b0:
	mov	w0, #2000
	ret
.Lf0b1:
	mov	w0, #2001
	ret
.Lf0b2:
	mov	w0, #2002
	ret
.Lf0b3:
	mov	w0, #2003
	ret
	.size	twotable_select_uxtw, .-twotable_select_uxtw
	.section	.data.rel.ro.twotable_select_uxtw,"aw",@progbits
	.p2align	3
.Lt0a:
	.xword	.Lf0a0
	.xword	.Lf0a1
	.xword	.Lf0a2
	.xword	.Lf0a3
.Lt0b:
	.xword	.Lf0b0
	.xword	.Lf0b1
	.xword	.Lf0b2
	.xword	.Lf0b3

// W index, sign-extended by the load; the mask keeps it non-negative.
	.text
	.globl	twotable_select_sxtw
	.p2align	2
	.type	twotable_select_sxtw,@function
twotable_select_sxtw:
	adrp	x9, .Lt1a
	add	x9, x9, :lo12:.Lt1a
	adrp	x10, .Lt1b
	add	x10, x10, :lo12:.Lt1b
	tst	w1, #0x1
	and	w8, w0, #0x3
	csel	x11, x9, x10, ne
	ldr	x8, [x11, w8, sxtw #3]
	br	x8
.Lf1a0:
	mov	w0, #1010
	ret
.Lf1a1:
	mov	w0, #1011
	ret
.Lf1a2:
	mov	w0, #1012
	ret
.Lf1a3:
	mov	w0, #1013
	ret
.Lf1b0:
	mov	w0, #2010
	ret
.Lf1b1:
	mov	w0, #2011
	ret
.Lf1b2:
	mov	w0, #2012
	ret
.Lf1b3:
	mov	w0, #2013
	ret
	.size	twotable_select_sxtw, .-twotable_select_sxtw
	.section	.data.rel.ro.twotable_select_sxtw,"aw",@progbits
	.p2align	3
.Lt1a:
	.xword	.Lf1a0
	.xword	.Lf1a1
	.xword	.Lf1a2
	.xword	.Lf1a3
.Lt1b:
	.xword	.Lf1b0
	.xword	.Lf1b1
	.xword	.Lf1b2
	.xword	.Lf1b3

// X index masked at 64 bits.
	.text
	.globl	twotable_select_xlsl
	.p2align	2
	.type	twotable_select_xlsl,@function
twotable_select_xlsl:
	adrp	x9, .Lt2a
	add	x9, x9, :lo12:.Lt2a
	adrp	x10, .Lt2b
	add	x10, x10, :lo12:.Lt2b
	tst	w1, #0x1
	and	x8, x0, #0x3
	csel	x11, x9, x10, ne
	ldr	x8, [x11, x8, lsl #3]
	br	x8
.Lf2a0:
	mov	w0, #1020
	ret
.Lf2a1:
	mov	w0, #1021
	ret
.Lf2a2:
	mov	w0, #1022
	ret
.Lf2a3:
	mov	w0, #1023
	ret
.Lf2b0:
	mov	w0, #2020
	ret
.Lf2b1:
	mov	w0, #2021
	ret
.Lf2b2:
	mov	w0, #2022
	ret
.Lf2b3:
	mov	w0, #2023
	ret
	.size	twotable_select_xlsl, .-twotable_select_xlsl
	.section	.data.rel.ro.twotable_select_xlsl,"aw",@progbits
	.p2align	3
.Lt2a:
	.xword	.Lf2a0
	.xword	.Lf2a1
	.xword	.Lf2a2
	.xword	.Lf2a3
.Lt2b:
	.xword	.Lf2b0
	.xword	.Lf2b1
	.xword	.Lf2b2
	.xword	.Lf2b3

// The higher table is the false arm.
	.text
	.globl	twotable_select_swapped
	.p2align	2
	.type	twotable_select_swapped,@function
twotable_select_swapped:
	adrp	x9, .Lt3a
	add	x9, x9, :lo12:.Lt3a
	adrp	x10, .Lt3b
	add	x10, x10, :lo12:.Lt3b
	tst	w1, #0x1
	and	w8, w0, #0x3
	csel	x11, x10, x9, ne
	ldr	x8, [x11, w8, uxtw #3]
	br	x8
.Lf3a0:
	mov	w0, #1030
	ret
.Lf3a1:
	mov	w0, #1031
	ret
.Lf3a2:
	mov	w0, #1032
	ret
.Lf3a3:
	mov	w0, #1033
	ret
.Lf3b0:
	mov	w0, #2030
	ret
.Lf3b1:
	mov	w0, #2031
	ret
.Lf3b2:
	mov	w0, #2032
	ret
.Lf3b3:
	mov	w0, #2033
	ret
	.size	twotable_select_swapped, .-twotable_select_swapped
	.section	.data.rel.ro.twotable_select_swapped,"aw",@progbits
	.p2align	3
.Lt3a:
	.xword	.Lf3a0
	.xword	.Lf3a1
	.xword	.Lf3a2
	.xword	.Lf3a3
.Lt3b:
	.xword	.Lf3b0
	.xword	.Lf3b1
	.xword	.Lf3b2
	.xword	.Lf3b3


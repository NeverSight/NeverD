// The dispatch of test_jumptable_twotable_select_a64.s with one proof missing
// in each function.  None may be recovered, as a merged switch or as a single
// table: the indirect branch stays unresolved.  They live in their own object
// because a function left out of the lifted module leaves its label table
// unresolved for every function that shares the pointer mirror.
// The index is below 4 by a shift alone: no mask proves the domain.
	.text
	.globl	twotable_select_shift_bound
	.p2align	2
	.type	twotable_select_shift_bound,@function
twotable_select_shift_bound:
	adrp	x9, .Lt4a
	add	x9, x9, :lo12:.Lt4a
	adrp	x10, .Lt4b
	add	x10, x10, :lo12:.Lt4b
	tst	w1, #0x1
	lsr	w8, w0, #30
	csel	x11, x9, x10, ne
	ldr	x8, [x11, w8, uxtw #3]
	br	x8
.Lf4a0:
	mov	w0, #1040
	ret
.Lf4a1:
	mov	w0, #1041
	ret
.Lf4a2:
	mov	w0, #1042
	ret
.Lf4a3:
	mov	w0, #1043
	ret
.Lf4b0:
	mov	w0, #2040
	ret
.Lf4b1:
	mov	w0, #2041
	ret
.Lf4b2:
	mov	w0, #2042
	ret
.Lf4b3:
	mov	w0, #2043
	ret
	.size	twotable_select_shift_bound, .-twotable_select_shift_bound
	.section	.data.rel.ro.twotable_select_shift_bound,"aw",@progbits
	.p2align	3
.Lt4a:
	.xword	.Lf4a0
	.xword	.Lf4a1
	.xword	.Lf4a2
	.xword	.Lf4a3
.Lt4b:
	.xword	.Lf4b0
	.xword	.Lf4b1
	.xword	.Lf4b2
	.xword	.Lf4b3

// The mask admits 8 slots; each table has 4.
	.text
	.globl	twotable_select_wide_mask
	.p2align	2
	.type	twotable_select_wide_mask,@function
twotable_select_wide_mask:
	adrp	x9, .Lt5a
	add	x9, x9, :lo12:.Lt5a
	adrp	x10, .Lt5b
	add	x10, x10, :lo12:.Lt5b
	tst	w1, #0x1
	and	w8, w0, #0x7
	csel	x11, x9, x10, ne
	ldr	x8, [x11, w8, uxtw #3]
	br	x8
.Lf5a0:
	mov	w0, #1050
	ret
.Lf5a1:
	mov	w0, #1051
	ret
.Lf5a2:
	mov	w0, #1052
	ret
.Lf5a3:
	mov	w0, #1053
	ret
.Lf5b0:
	mov	w0, #2050
	ret
.Lf5b1:
	mov	w0, #2051
	ret
.Lf5b2:
	mov	w0, #2052
	ret
.Lf5b3:
	mov	w0, #2053
	ret
	.size	twotable_select_wide_mask, .-twotable_select_wide_mask
	.section	.data.rel.ro.twotable_select_wide_mask,"aw",@progbits
	.p2align	3
.Lt5a:
	.xword	.Lf5a0
	.xword	.Lf5a1
	.xword	.Lf5a2
	.xword	.Lf5a3
.Lt5b:
	.xword	.Lf5b0
	.xword	.Lf5b1
	.xword	.Lf5b2
	.xword	.Lf5b3

// One SELECT input is a runtime pointer, not a table.
	.text
	.globl	twotable_select_runtime_arm
	.p2align	2
	.type	twotable_select_runtime_arm,@function
twotable_select_runtime_arm:
	adrp	x9, .Lt6a
	add	x9, x9, :lo12:.Lt6a
	adrp	x10, .Lt6b
	add	x10, x10, :lo12:.Lt6b
	tst	w1, #0x1
	and	w8, w0, #0x3
	csel	x11, x9, x2, ne
	ldr	x8, [x11, w8, uxtw #3]
	br	x8
.Lf6a0:
	mov	w0, #1060
	ret
.Lf6a1:
	mov	w0, #1061
	ret
.Lf6a2:
	mov	w0, #1062
	ret
.Lf6a3:
	mov	w0, #1063
	ret
.Lf6b0:
	mov	w0, #2060
	ret
.Lf6b1:
	mov	w0, #2061
	ret
.Lf6b2:
	mov	w0, #2062
	ret
.Lf6b3:
	mov	w0, #2063
	ret
	.size	twotable_select_runtime_arm, .-twotable_select_runtime_arm
	.section	.data.rel.ro.twotable_select_runtime_arm,"aw",@progbits
	.p2align	3
.Lt6a:
	.xword	.Lf6a0
	.xword	.Lf6a1
	.xword	.Lf6a2
	.xword	.Lf6a3
.Lt6b:
	.xword	.Lf6b0
	.xword	.Lf6b1
	.xword	.Lf6b2
	.xword	.Lf6b3

// The load's base is not the SELECT's result.
	.text
	.globl	twotable_select_moved_base
	.p2align	2
	.type	twotable_select_moved_base,@function
twotable_select_moved_base:
	adrp	x9, .Lt7a
	add	x9, x9, :lo12:.Lt7a
	adrp	x10, .Lt7b
	add	x10, x10, :lo12:.Lt7b
	tst	w1, #0x1
	and	w8, w0, #0x3
	csel	x11, x9, x10, ne
	add	x11, x11, #0
	ldr	x8, [x11, w8, uxtw #3]
	br	x8
.Lf7a0:
	mov	w0, #1070
	ret
.Lf7a1:
	mov	w0, #1071
	ret
.Lf7a2:
	mov	w0, #1072
	ret
.Lf7a3:
	mov	w0, #1073
	ret
.Lf7b0:
	mov	w0, #2070
	ret
.Lf7b1:
	mov	w0, #2071
	ret
.Lf7b2:
	mov	w0, #2072
	ret
.Lf7b3:
	mov	w0, #2073
	ret
	.size	twotable_select_moved_base, .-twotable_select_moved_base
	.section	.data.rel.ro.twotable_select_moved_base,"aw",@progbits
	.p2align	3
.Lt7a:
	.xword	.Lf7a0
	.xword	.Lf7a1
	.xword	.Lf7a2
	.xword	.Lf7a3
.Lt7b:
	.xword	.Lf7b0
	.xword	.Lf7b1
	.xword	.Lf7b2
	.xword	.Lf7b3

// The load does not scale its index by the entry width.
	.text
	.globl	twotable_select_unscaled
	.p2align	2
	.type	twotable_select_unscaled,@function
twotable_select_unscaled:
	adrp	x9, .Lt8a
	add	x9, x9, :lo12:.Lt8a
	adrp	x10, .Lt8b
	add	x10, x10, :lo12:.Lt8b
	tst	w1, #0x1
	and	w8, w0, #0x3
	lsl	w8, w8, #3
	csel	x11, x9, x10, ne
	ldr	x8, [x11, w8, uxtw]
	br	x8
.Lf8a0:
	mov	w0, #1080
	ret
.Lf8a1:
	mov	w0, #1081
	ret
.Lf8a2:
	mov	w0, #1082
	ret
.Lf8a3:
	mov	w0, #1083
	ret
.Lf8b0:
	mov	w0, #2080
	ret
.Lf8b1:
	mov	w0, #2081
	ret
.Lf8b2:
	mov	w0, #2082
	ret
.Lf8b3:
	mov	w0, #2083
	ret
	.size	twotable_select_unscaled, .-twotable_select_unscaled
	.section	.data.rel.ro.twotable_select_unscaled,"aw",@progbits
	.p2align	3
.Lt8a:
	.xword	.Lf8a0
	.xword	.Lf8a1
	.xword	.Lf8a2
	.xword	.Lf8a3
.Lt8b:
	.xword	.Lf8b0
	.xword	.Lf8b1
	.xword	.Lf8b2
	.xword	.Lf8b3


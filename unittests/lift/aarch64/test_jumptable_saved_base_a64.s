// A compact word table whose base lives in x20 across a backedge.
// plain_backedge_dispatch leaves x20 untouched.
// saved_base_dispatch saves x20 in x19, clobbers x20, calls a function
// that returns, restores x20, and branches back.  The restored register is
// the same table address.
// foreign_base_dispatch restores x21 instead.  That value is not the table.
// saved_base_split_dispatch takes the same save through a branch, so the
// restore is in another block.  foreign_split_dispatch does that with x21.
	.text
	.globl	saved_base_helper
	.p2align	2
	.type	saved_base_helper,@function
saved_base_helper:
	ret
	.size	saved_base_helper, .-saved_base_helper

	.globl	plain_backedge_dispatch
	.p2align	2
	.type	plain_backedge_dispatch,@function
plain_backedge_dispatch:
	cmp	w0, #2
	and	x22, x0, #0xffffffff
	b.hi	.Lplain_default
	adrp	x20, .Lplain_table
	add	x20, x20, :lo12:.Lplain_table
.Lplain_loop:
	cmp	w22, #2
	b.hi	.Lplain_default
	ldrsw	x8, [x20, x22, lsl #2]
	add	x8, x8, x20
	br	x8
.Lplain_c0:
	mov	w0, #10
	b	.Lplain_loop
.Lplain_c1:
	mov	w0, #11
	ret
.Lplain_c2:
	mov	w0, #12
	ret
.Lplain_default:
	mov	w0, wzr
	ret
	.size	plain_backedge_dispatch, .-plain_backedge_dispatch

	.globl	saved_base_dispatch
	.p2align	2
	.type	saved_base_dispatch,@function
saved_base_dispatch:
	cmp	w0, #2
	and	x22, x0, #0xffffffff
	b.hi	.Lsaved_default
	adrp	x20, .Lsaved_table
	add	x20, x20, :lo12:.Lsaved_table
.Lsaved_loop:
	cmp	w22, #2
	b.hi	.Lsaved_default
	ldrsw	x8, [x20, x22, lsl #2]
	add	x8, x8, x20
	br	x8
.Lsaved_c0:
	mov	x19, x20
	and	x20, x21, #0xffff
	bl	saved_base_helper
	mov	x20, x19
	b	.Lsaved_loop
.Lsaved_c1:
	mov	w0, #11
	ret
.Lsaved_c2:
	mov	w0, #12
	ret
.Lsaved_default:
	mov	w0, wzr
	ret
	.size	saved_base_dispatch, .-saved_base_dispatch

	.globl	foreign_base_dispatch
	.p2align	2
	.type	foreign_base_dispatch,@function
foreign_base_dispatch:
	mov	x21, #1
	cmp	w0, #2
	and	x22, x0, #0xffffffff
	b.hi	.Lforeign_default
	adrp	x20, .Lforeign_table
	add	x20, x20, :lo12:.Lforeign_table
.Lforeign_loop:
	cmp	w22, #2
	b.hi	.Lforeign_default
	ldrsw	x8, [x20, x22, lsl #2]
	add	x8, x8, x20
	br	x8
.Lforeign_c0:
	mov	x19, x21
	and	x20, x22, #0xffff
	bl	saved_base_helper
	mov	x20, x19
	b	.Lforeign_loop
.Lforeign_c1:
	mov	w0, #11
	ret
.Lforeign_c2:
	mov	w0, #12
	ret
.Lforeign_default:
	mov	w0, wzr
	ret
	.size	foreign_base_dispatch, .-foreign_base_dispatch

	.globl	saved_base_split_dispatch
	.p2align	2
	.type	saved_base_split_dispatch,@function
saved_base_split_dispatch:
	cmp	w0, #2
	and	x22, x0, #0xffffffff
	b.hi	.Lsplitsaved_default
	adrp	x20, .Lsplitsaved_table
	add	x20, x20, :lo12:.Lsplitsaved_table
.Lsplitsaved_loop:
	cmp	w22, #2
	b.hi	.Lsplitsaved_default
	ldrsw	x8, [x20, x22, lsl #2]
	add	x8, x8, x20
	br	x8
.Lsplitsaved_c0:
	mov	x19, x20
	and	x20, x21, #0xffff
	cbz	w22, .Lsplitsaved_restore
	bl	saved_base_helper
.Lsplitsaved_restore:
	mov	x20, x19
	b	.Lsplitsaved_loop
.Lsplitsaved_c1:
	mov	w0, #11
	ret
.Lsplitsaved_c2:
	mov	w0, #12
	ret
.Lsplitsaved_default:
	mov	w0, wzr
	ret
	.size	saved_base_split_dispatch, .-saved_base_split_dispatch

	.globl	foreign_split_dispatch
	.p2align	2
	.type	foreign_split_dispatch,@function
foreign_split_dispatch:
	mov	x21, #1
	cmp	w0, #2
	and	x22, x0, #0xffffffff
	b.hi	.Lsplitforeign_default
	adrp	x20, .Lsplitforeign_table
	add	x20, x20, :lo12:.Lsplitforeign_table
.Lsplitforeign_loop:
	cmp	w22, #2
	b.hi	.Lsplitforeign_default
	ldrsw	x8, [x20, x22, lsl #2]
	add	x8, x8, x20
	br	x8
.Lsplitforeign_c0:
	mov	x19, x21
	and	x20, x22, #0xffff
	cbz	w22, .Lsplitforeign_restore
	bl	saved_base_helper
.Lsplitforeign_restore:
	mov	x20, x19
	b	.Lsplitforeign_loop
.Lsplitforeign_c1:
	mov	w0, #11
	ret
.Lsplitforeign_c2:
	mov	w0, #12
	ret
.Lsplitforeign_default:
	mov	w0, wzr
	ret
	.size	foreign_split_dispatch, .-foreign_split_dispatch

	.section	.rodata
	.p2align	2
.Lplain_table:
	.word	.Lplain_c0-.Lplain_table
	.word	.Lplain_c1-.Lplain_table
	.word	.Lplain_c2-.Lplain_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lsaved_table:
	.word	.Lsaved_c0-.Lsaved_table
	.word	.Lsaved_c1-.Lsaved_table
	.word	.Lsaved_c2-.Lsaved_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lforeign_table:
	.word	.Lforeign_c0-.Lforeign_table
	.word	.Lforeign_c1-.Lforeign_table
	.word	.Lforeign_c2-.Lforeign_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lsplitsaved_table:
	.word	.Lsplitsaved_c0-.Lsplitsaved_table
	.word	.Lsplitsaved_c1-.Lsplitsaved_table
	.word	.Lsplitsaved_c2-.Lsplitsaved_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
.Lsplitforeign_table:
	.word	.Lsplitforeign_c0-.Lsplitforeign_table
	.word	.Lsplitforeign_c1-.Lsplitforeign_table
	.word	.Lsplitforeign_c2-.Lsplitforeign_table
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
	.section	.note.GNU-stack,"",@progbits

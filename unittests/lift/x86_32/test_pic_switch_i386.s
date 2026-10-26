// Switch tables of an i386 shared library.  PIC code finds the GOT address
// with a get-PC sequence and an `add $_GLOBAL_OFFSET_TABLE_`, reads a table
// of `case@GOTOFF` entries relative to it, and adds the entry back to it:
//   gcc_switch        gcc -m32 -O2 -fPIC: thunk, `mov tbl@GOTOFF(...)`, add
//   gcc_fused_switch  gcc's fused `add tbl@GOTOFF(%eax,%edx,4), %eax`
//   clang_switch      clang -m32 -O2 -fPIC: call/pop, fused add
//   saved_got_switch  the GOT register set before a branch around a call
//   spilled_got_switch  an aligned frame, the GOT register spilled and
//                       reloaded around a foreign store and a PLT call;
//                       an explicit restore owns the post-call ESP coordinate
// The linked library keeps no relocation for any of them; the PLT call gives
// it a DT_PLTGOT besides its `_GLOBAL_OFFSET_TABLE_` symbol.  Two dispatches
// whose register is not the GOT address on every path must stay unresolved:
//   offset_anchor_switch     adds GOT+4, so every target is a case + 4
//   clobbered_anchor_switch  one path replaces the GOT register
//   escaped_got_switch       the spill's address reaches the PLT callee

	.text
	.p2align 4
	.globl	gcc_switch
	.type	gcc_switch, @function
gcc_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	movl	8(%esp), %eax
	cmpl	$4, %eax
	ja	.Lgs_default
	movl	.Lgs_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
gcc_switch_dispatch:
	jmp	*%eax
gcc_switch_case0:
	movl	$10, %eax
	popl	%ebx
	ret
gcc_switch_case1:
	movl	$21, %eax
	popl	%ebx
	ret
gcc_switch_case2:
	movl	$32, %eax
	popl	%ebx
	ret
gcc_switch_case3:
	movl	$43, %eax
	popl	%ebx
	ret
gcc_switch_case4:
	movl	$54, %eax
	popl	%ebx
	ret
.Lgs_default:
	call	abort@PLT
	.size	gcc_switch, .-gcc_switch

	.p2align 4
	.globl	gcc_fused_switch
	.type	gcc_fused_switch, @function
gcc_fused_switch:
	call	__x86.get_pc_thunk.ax
	addl	$_GLOBAL_OFFSET_TABLE_, %eax
	movl	4(%esp), %edx
	cmpl	$3, %edx
	ja	.Lgf_default
	addl	.Lgf_table@GOTOFF(%eax,%edx,4), %eax
gcc_fused_switch_dispatch:
	jmp	*%eax
gcc_fused_switch_case0:
	movl	8(%esp), %eax
	addl	$3, %eax
	ret
gcc_fused_switch_case1:
	movl	8(%esp), %eax
	imull	$7, %eax, %eax
	ret
gcc_fused_switch_case2:
	movl	8(%esp), %eax
	subl	$11, %eax
	ret
gcc_fused_switch_case3:
	movl	8(%esp), %eax
	xorl	$85, %eax
	ret
.Lgf_default:
	xorl	%eax, %eax
	ret
	.size	gcc_fused_switch, .-gcc_fused_switch

	.p2align 4
	.globl	clang_switch
	.type	clang_switch, @function
clang_switch:
	calll	.Lcs_pc
.Lcs_pc:
	popl	%ecx
.Lcs_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lcs_got-.Lcs_pc), %ecx
	movl	4(%esp), %edx
	cmpl	$3, %edx
	ja	.Lcs_default
	movl	8(%esp), %eax
	addl	.Lcs_table@GOTOFF(%ecx,%edx,4), %ecx
clang_switch_dispatch:
	jmpl	*%ecx
clang_switch_case0:
	addl	$3, %eax
	retl
clang_switch_case1:
	shll	$2, %eax
	retl
clang_switch_case2:
	negl	%eax
	retl
clang_switch_case3:
	notl	%eax
	retl
.Lcs_default:
	xorl	%eax, %eax
	retl
	.size	clang_switch, .-clang_switch

	.p2align 4
	.globl	saved_got_switch
	.type	saved_got_switch, @function
saved_got_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	testl	$1, 12(%esp)
	je	.Lsg_merge
	call	note_entry
.Lsg_merge:
	movl	8(%esp), %eax
	cmpl	$3, %eax
	ja	.Lsg_default
	movl	.Lsg_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
saved_got_switch_dispatch:
	jmp	*%eax
.irp n,0,1,2,3
saved_got_switch_case\n:
	movl	$\n+100, %eax
	popl	%ebx
	ret
.endr
.Lsg_default:
	xorl	%eax, %eax
	popl	%ebx
	ret
	.size	saved_got_switch, .-saved_got_switch

	.p2align 4
	.type	note_entry, @function
note_entry:
	ret
	.size	note_entry, .-note_entry

	.p2align 4
	.globl	offset_anchor_switch
	.type	offset_anchor_switch, @function
offset_anchor_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_+4, %ebx
	movl	8(%esp), %eax
	cmpl	$3, %eax
	ja	.Loa_default
	movl	.Loa_table@GOTOFF-4(%ebx,%eax,4), %eax
	addl	%ebx, %eax
offset_anchor_switch_dispatch:
	jmp	*%eax
.irp n,0,1,2,3
offset_anchor_switch_case\n:
	nopl	0(%eax)
	movl	$\n, %eax
	popl	%ebx
	ret
.endr
.Loa_default:
	xorl	%eax, %eax
	popl	%ebx
	ret
	.size	offset_anchor_switch, .-offset_anchor_switch

	.p2align 4
	.globl	clobbered_anchor_switch
	.type	clobbered_anchor_switch, @function
clobbered_anchor_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	movl	8(%esp), %eax
	testl	$1, 12(%esp)
	je	.Lca_keep
	movl	16(%esp), %ebx
.Lca_keep:
	cmpl	$3, %eax
	ja	.Lca_default
	movl	.Lca_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
clobbered_anchor_switch_dispatch:
	jmp	*%eax
.irp n,0,1,2,3
clobbered_anchor_switch_case\n:
	movl	$\n, %eax
	popl	%ebx
	ret
.endr
.Lca_default:
	xorl	%eax, %eax
	popl	%ebx
	ret
	.size	clobbered_anchor_switch, .-clobbered_anchor_switch

// The default path passes the spill's own address to a callee and ends in
// abort, so it never runs before the reload.  0x54 is no accident: an
// emulated reload with an unknown %esp reads the second program header there.
	.p2align 4
	.globl	spilled_got_switch
	.type	spilled_got_switch, @function
spilled_got_switch:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx
	andl	$-16, %esp
	subl	$0x90, %esp
	calll	.Lsp_pc
.Lsp_pc:
	popl	%eax
.Lsp_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lsp_got-.Lsp_pc), %eax
	movl	%eax, 0x54(%esp)
.Lsp_loop:
	movl	8(%ebp), %ecx
	movl	(%ecx), %edx
	addl	$4, %ecx
	movl	%ecx, 8(%ebp)
	movl	%edx, 0x38(%esp)
	movl	12(%ebp), %ecx
	movl	$1, (%ecx)
	movl	%edx, (%esp)
	movl	0x54(%esp), %ebx
	calll	note_value@PLT
spilled_got_switch_restore:
	leal	-4(%ebp), %esp
	andl	$-16, %esp
	subl	$0x90, %esp
spilled_got_switch_restored:
	movl	0x38(%esp), %ecx
	cmpl	$3, %ecx
	ja	.Lsp_default
	movl	0x54(%esp), %eax
	movl	.Lsp_table@GOTOFF(%eax,%ecx,4), %edx
	addl	%eax, %edx
spilled_got_switch_dispatch:
	jmpl	*%edx
.irp n,0,1,2
spilled_got_switch_case\n:
	jmp	.Lsp_loop
.endr
spilled_got_switch_case3:
	movl	$103, %eax
	leal	-4(%ebp), %esp
	popl	%ebx
	popl	%ebp
	retl
.Lsp_default:
	leal	0x54(%esp), %eax
	movl	%eax, (%esp)
	movl	0x54(%esp), %ebx
	calll	note_pointer@PLT
	calll	abort@PLT
	.size	spilled_got_switch, .-spilled_got_switch

// The same loop hands the spill's address to its callee before the reload,
// so the callee may have replaced the GOT address the dispatch adds.
	.p2align 4
	.globl	escaped_got_switch
	.type	escaped_got_switch, @function
escaped_got_switch:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx
	andl	$-16, %esp
	subl	$0x90, %esp
	calll	.Leg_pc
.Leg_pc:
	popl	%eax
.Leg_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Leg_got-.Leg_pc), %eax
	movl	%eax, 0x54(%esp)
.Leg_loop:
	movl	8(%ebp), %ecx
	movl	(%ecx), %edx
	addl	$4, %ecx
	movl	%ecx, 8(%ebp)
	movl	%edx, 0x38(%esp)
	leal	0x54(%esp), %eax
	movl	%eax, (%esp)
	movl	0x54(%esp), %ebx
	calll	note_pointer@PLT
	leal	-4(%ebp), %esp
	andl	$-16, %esp
	subl	$0x90, %esp
	movl	0x38(%esp), %ecx
	cmpl	$3, %ecx
	ja	.Leg_default
	movl	0x54(%esp), %eax
	movl	.Leg_table@GOTOFF(%eax,%ecx,4), %edx
	addl	%eax, %edx
escaped_got_switch_dispatch:
	jmpl	*%edx
.irp n,0,1,2
escaped_got_switch_case\n:
	jmp	.Leg_loop
.endr
escaped_got_switch_case3:
	movl	$103, %eax
	leal	-4(%ebp), %esp
	popl	%ebx
	popl	%ebp
	retl
.Leg_default:
	xorl	%eax, %eax
	leal	-4(%ebp), %esp
	popl	%ebx
	popl	%ebp
	retl
	.size	escaped_got_switch, .-escaped_got_switch

	.section	.rodata
	.p2align 2
.Lgs_table:
.irp n,0,1,2,3,4
	.long	gcc_switch_case\n@GOTOFF
.endr
.Lgf_table:
.irp n,0,1,2,3
	.long	gcc_fused_switch_case\n@GOTOFF
.endr
.Lcs_table:
.irp n,0,1,2,3
	.long	clang_switch_case\n@GOTOFF
.endr
.Lsg_table:
.irp n,0,1,2,3
	.long	saved_got_switch_case\n@GOTOFF
.endr
.Loa_table:
.irp n,0,1,2,3
	.long	offset_anchor_switch_case\n@GOTOFF
.endr
.Lca_table:
.irp n,0,1,2,3
	.long	clobbered_anchor_switch_case\n@GOTOFF
.endr
.Lsp_table:
.irp n,0,1,2,3
	.long	spilled_got_switch_case\n@GOTOFF
.endr
.Leg_table:
.irp n,0,1,2,3
	.long	escaped_got_switch_case\n@GOTOFF
.endr

	.section	.text.__x86.get_pc_thunk.bx,"axG",@progbits,__x86.get_pc_thunk.bx,comdat
	.globl	__x86.get_pc_thunk.bx
	.hidden	__x86.get_pc_thunk.bx
	.type	__x86.get_pc_thunk.bx, @function
__x86.get_pc_thunk.bx:
	movl	(%esp), %ebx
	ret

	.section	.text.__x86.get_pc_thunk.ax,"axG",@progbits,__x86.get_pc_thunk.ax,comdat
	.globl	__x86.get_pc_thunk.ax
	.hidden	__x86.get_pc_thunk.ax
	.type	__x86.get_pc_thunk.ax, @function
__x86.get_pc_thunk.ax:
	movl	(%esp), %eax
	ret

	.section	.note.GNU-stack,"",@progbits

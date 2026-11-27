// Entry-like parts a frameless function jumps into, each with an FDE of its
// own that begins with the entry rule, as GCC's isolated trap part does.
// Only bound_trap is that part: called_trap is also called, returning_part
// returns as a function tail-called once does, and pointed_trap's address is
// stored in a relocated pointer.
	.text
	.globl	jumps_to_parts
	.type	jumps_to_parts, @function
jumps_to_parts:
	.cfi_startproc
	cmpl	$1, %edi
	je	bound_trap
	cmpl	$2, %edi
	je	called_trap
	cmpl	$3, %edi
	je	returning_part
	cmpl	$4, %edi
	je	pointed_trap
	xorl	%eax, %eax
	ret
	.cfi_endproc
	.size	jumps_to_parts, .-jumps_to_parts

	.globl	calls_trap
	.type	calls_trap, @function
calls_trap:
	.cfi_startproc
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	call	called_trap
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	calls_trap, .-calls_trap

	.section	.text.unlikely,"ax",@progbits
	.type	bound_trap, @function
bound_trap:
	.cfi_startproc
	movq	%rsi, 0
	ud2
	.cfi_endproc
	.type	called_trap, @function
called_trap:
	.cfi_startproc
	movq	%rdx, 0
	ud2
	.cfi_endproc
	.type	returning_part, @function
returning_part:
	.cfi_startproc
	movl	$7, %eax
	ret
	.cfi_endproc
	.type	pointed_trap, @function
pointed_trap:
	.cfi_startproc
	movq	%rcx, 0
	ud2
	.cfi_endproc

	.section	.data.rel.ro,"aw",@progbits
	.p2align 3
	.globl	trap_pointer
	.type	trap_pointer, @object
trap_pointer:
	.quad	pointed_trap
	.size	trap_pointer, 8
	.section	.note.GNU-stack,"",@progbits

// Optimized code may keep RBP as an ordinary register and load a table
// address into it. The table read through it is a relative table, not one
// materialized in the frame.
	.text
	.globl	rbp_dispatch
	.type	rbp_dispatch,@function
rbp_dispatch:
	pushq	%rbp
	leal	-97(%rdi), %eax
	cmpl	$5, %eax
	ja	.Ldefault
	leaq	.Ltable(%rip), %rbp
	movslq	(%rbp,%rax,4), %rax
	addq	%rbp, %rax
	jmpq	*%rax
.Lc0:	movl	$10, %eax
	popq	%rbp
	retq
.Lc1:	movl	$11, %eax
	popq	%rbp
	retq
.Lc2:	movl	$12, %eax
	popq	%rbp
	retq
.Lc3:	movl	$13, %eax
	popq	%rbp
	retq
.Lc4:	movl	$14, %eax
	popq	%rbp
	retq
.Lc5:	movl	$15, %eax
	popq	%rbp
	retq
.Ldefault:
	xorl	%eax, %eax
	popq	%rbp
	retq
	.size	rbp_dispatch, .-rbp_dispatch
	.section	.rodata
	.p2align	2
.Ltable:
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.section	.note.GNU-stack,"",@progbits

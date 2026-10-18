// clang -O0 lowers a cleanup switch's unreachable default to a jump to an
// empty block at the function's end. There, padding falls into the next
// function, which this one also calls.
	.text
	.globl	cleanup_dispatch
	.type	cleanup_dispatch,@function
cleanup_dispatch:
	pushq	%rbx
	movl	%esi, %ebx
	cmpl	$5, %edi
	ja	.Lfinish
	movl	%edi, %eax
	leaq	.Ltable(%rip), %rcx
	movslq	(%rcx,%rax,4), %rax
	addq	%rcx, %rax
	jmpq	*%rax
.Lc0:	movl	$10, %eax
	popq	%rbx
	retq
.Lc1:	movl	$11, %eax
	popq	%rbx
	retq
.Lc2:	movl	$12, %eax
	popq	%rbx
	retq
.Lc3:	movl	$13, %eax
	popq	%rbx
	retq
.Lc4:	movl	$14, %eax
	popq	%rbx
	retq
.Lc5:	movl	$15, %eax
	popq	%rbx
	retq
.Lfinish:
	movl	%ebx, %edi
	callq	helper
	cmpl	$2, %eax
	je	.Lout
	testl	%eax, %eax
	jne	.Lunreachable
.Lout:
	popq	%rbx
	retq
.Lunreachable:
	.size	cleanup_dispatch, .-cleanup_dispatch
	nopl	(%rax)
	.type	helper,@function
helper:
	leal	1(%rdi), %eax
	retq
	.size	helper, .-helper
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

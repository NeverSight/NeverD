# GCC 15.2 -O2 -fno-pic for calls to external functions no summary,
# prototype or library table describes:
#
#   extern double ext_real(void *p);
#   extern void ext_other(long a, long b, long c);
#   extern int kr_flag;
#   extern long kr_a;
#   void kr_callit(void (*g)(void)) { g(); }
#   double kr_mixed(void *p) {
#     if (kr_flag)
#       ext_other(kr_a, 1, 2);
#     return ext_real(p) + 1.0;
#   }
#   double kr_leftover(void *p, long a) {
#     ext_other(a, 1, 2);
#     return ext_real(p) + 1.0;
#   }
#
# kr_callit's indirect tail jump puts the object's calls through MedIR call
# recovery.  At kr_mixed's call of ext_real, RSI, RCX, R8, R9 and the vector
# registers hold what kr_mixed entered with, where no parameter arrives, on
# one way and what ext_other left on the other; only RDI is set on both.  At
# kr_leftover's call every register past RDI holds only what ext_other left.

	.text
	.p2align 4
	.globl	kr_callit
	.type	kr_callit, @function
kr_callit:
	jmp	*%rdi
	.size	kr_callit, .-kr_callit
	.p2align 4
	.globl	kr_mixed
	.type	kr_mixed, @function
kr_mixed:
	subq	$24, %rsp
	movl	kr_flag(%rip), %edx
	movq	%rdi, %rax
	testl	%edx, %edx
	jne	.L6
.L4:
	movq	%rax, %rdi
	call	ext_real
	addsd	.LC0(%rip), %xmm0
	addq	$24, %rsp
	ret
	.p2align 4,,10
	.p2align 3
.L6:
	movq	%rdi, 8(%rsp)
	movl	$2, %edx
	movl	$1, %esi
	movq	kr_a(%rip), %rdi
	call	ext_other
	movq	8(%rsp), %rax
	jmp	.L4
	.size	kr_mixed, .-kr_mixed
	.p2align 4
	.globl	kr_leftover
	.type	kr_leftover, @function
kr_leftover:
	pushq	%rbx
	movl	$2, %edx
	movq	%rdi, %rbx
	movq	%rsi, %rdi
	movl	$1, %esi
	call	ext_other
	movq	%rbx, %rdi
	call	ext_real
	popq	%rbx
	addsd	.LC0(%rip), %xmm0
	ret
	.size	kr_leftover, .-kr_leftover
	.section	.rodata.cst8,"aM",@progbits,8
	.align 8
.LC0:
	.long	0
	.long	1072693248
	.section	.note.GNU-stack,"",@progbits

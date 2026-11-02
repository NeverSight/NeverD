# GCC 15.2 -O2 -fPIC -S output for four translation units, each with its
# local labels renamed: functions that never return, and callers whose
# compiler knew it and laid out nothing after a call of them but the code
# of another path.
#
# die.c: plt_die ends in a call of exit through the PLT.
#   #include <stdlib.h>
#   __attribute__((visibility("hidden"))) void plt_die(int code) { exit(code); }
#
# pick.c:
#   __attribute__((noreturn, visibility("hidden"))) void plt_die(int code);
#   int plt_pick(int x) {
#     if (x == 1)
#       plt_die(3);
#     if (x == 2)
#       plt_die(4);
#     return x * 5;
#   }
#
# spin.c: spin_forever loops forever with no call or trap; only its
# completely lifted loop proves it never returns.
#   __attribute__((visibility("hidden"))) void spin_forever(int code) {
#     volatile int sink = code;
#     for (;;)
#       sink = sink + 1;
#   }
#
# pickspin.c: spin_pick, pick.c's function calling spin_forever instead.

	.text
	.p2align 4
	.globl	plt_die
	.hidden	plt_die
	.type	plt_die, @function
plt_die:
.LFBdie16:
	.cfi_startproc
	endbr64
	pushq	%rax
	.cfi_def_cfa_offset 16
	popq	%rax
	.cfi_def_cfa_offset 8
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	call	exit@PLT
	.cfi_endproc
.LFEdie16:
	.size	plt_die, .-plt_die

	.text
	.p2align 4
	.globl	plt_pick
	.type	plt_pick, @function
plt_pick:
.LFBpick0:
	.cfi_startproc
	endbr64
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	cmpl	$1, %edi
	je	.Lpick6
	cmpl	$2, %edi
	je	.Lpick7
	leal	(%rdi,%rdi,4), %eax
	addq	$8, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret
.Lpick6:
	.cfi_restore_state
	movl	$3, %edi
	call	plt_die
.Lpick7:
	movl	$4, %edi
	call	plt_die
	.cfi_endproc
.LFEpick0:
	.size	plt_pick, .-plt_pick
	.hidden	plt_die

	.text
	.p2align 4
	.globl	spin_forever
	.hidden	spin_forever
	.type	spin_forever, @function
spin_forever:
.LFBspin0:
	.cfi_startproc
	endbr64
	movl	%edi, -4(%rsp)
	.p2align 4
	.p2align 4
	.p2align 3
.Lspin2:
	movl	-4(%rsp), %eax
	addl	$1, %eax
	movl	%eax, -4(%rsp)
	jmp	.Lspin2
	.cfi_endproc
.LFEspin0:
	.size	spin_forever, .-spin_forever

	.text
	.p2align 4
	.globl	spin_pick
	.type	spin_pick, @function
spin_pick:
.LFBpickspin0:
	.cfi_startproc
	endbr64
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	cmpl	$1, %edi
	je	.Lpickspin6
	cmpl	$2, %edi
	je	.Lpickspin7
	leal	(%rdi,%rdi,4), %eax
	addq	$8, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 8
	ret
.Lpickspin6:
	.cfi_restore_state
	movl	$3, %edi
	call	spin_forever
.Lpickspin7:
	movl	$4, %edi
	call	spin_forever
	.cfi_endproc
.LFEpickspin0:
	.size	spin_pick, .-spin_pick
	.hidden	spin_forever

	.section	.note.GNU-stack,"",@progbits

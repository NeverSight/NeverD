// GCC 15.2 `gcc -O2 -fPIC -S` of the shape of Lua 5.4's reallymarkobject
// (lgc.c), whose getgclist returns NULL for a tag it does not own:
//   struct GCObject { unsigned char tt, marked; struct GCObject *next;
//                     struct GCObject *list[9]; };
//   struct global_State { char pad[0x88]; struct GCObject *gray; };
//   static struct GCObject **getgclist(struct GCObject *o) {
//     switch (o->tt) {
//     case 0x05: return &o->list[0];  case 0x06: return &o->list[1];
//     case 0x08: return &o->list[2];  case 0x0a: return &o->list[3];
//     case 0x16: return &o->list[4];  case 0x26: return &o->list[5];
//     default: return 0;
//     }
//   }
//   static void linkgclist(struct global_State *g, struct GCObject *o) {
//     struct GCObject **p = getgclist(o);
//     *p = g->gray; g->gray = o; o->marked &= 0xc7;
//   }
//   void frameless_cold_mark(struct global_State *g, struct GCObject *o) {
//     switch (o->tt) {
//     case 0x04: case 0x14: o->marked = (o->marked & 0xc7) | 0x20; break;
//     case 0x09: { struct GCObject *v = o->next;
//       if (o->list[0] == o) o->marked = (o->marked & 0xc7) | 0x08;
//       else o->marked = (o->marked & 0xc7) | 0x20;
//       if (v && (v->marked & 0x18)) v->marked |= 0x40;
//       break; }
//     case 0x07:
//       if (o->list[8] == 0) {
//         if (o->next && (o->next->marked & 0x18)) o->next->marked |= 0x40;
//         o->marked = (o->marked & 0xc7) | 0x20;
//         break;
//       }
//       /* fall through */
//     case 0x06: case 0x26: case 0x05: case 0x08: case 0x0a: case 0x16:
//       linkgclist(g, o);
//       break;
//     default: break;
//     }
//   }
// GCC isolates the store through the null pointer (`movq %rax,0; ud2`) and
// hot/cold partitioning moves it to `frameless_cold_mark.cold`, with an FDE
// of its own.  The function has no frame, so that FDE begins with the entry
// rule, as a function's would.  The inlined switch's table and two branches
// enter the part.
	.file	"frameless_cold.c"
	.text
	.section	.text.unlikely,"ax",@progbits
.LCOLDB0:
	.text
.LHOTB0:
	.p2align 4
	.globl	frameless_cold_mark
	.type	frameless_cold_mark, @function
frameless_cold_mark:
.LFB2:
	.cfi_startproc
	endbr64
	movzbl	(%rsi), %eax
	cmpb	$38, %al
	ja	.L1
	cmpb	$7, %al
	jbe	.L36
	leal	-8(%rax), %ecx
	movl	$1, %edx
	salq	%cl, %rdx
	testl	$1073758213, %edx
	jne	.L6
	cmpb	$9, %al
	je	.L7
	cmpb	$20, %al
	je	.L11
.L1:
	ret
	.p2align 4,,10
	.p2align 3
.L36:
	je	.L4
	cmpb	$4, %al
	jne	.L37
.L11:
	movzbl	1(%rsi), %eax
	andl	$-57, %eax
	orl	$32, %eax
	movb	%al, 1(%rsi)
	ret
	.p2align 4,,10
	.p2align 3
.L4:
	cmpq	$0, 80(%rsi)
	jne	.L10
	movq	8(%rsi), %rax
	testq	%rax, %rax
	je	.L11
	movzbl	1(%rax), %edx
	testb	$24, %dl
	je	.L11
	orl	$64, %edx
	movb	%dl, 1(%rax)
	jmp	.L11
	.p2align 4,,10
	.p2align 3
.L7:
	movzbl	1(%rsi), %eax
	movq	8(%rsi), %rdx
	andl	$-57, %eax
	movl	%eax, %ecx
	orl	$32, %eax
	orl	$8, %ecx
	cmpq	%rsi, 16(%rsi)
	cmove	%ecx, %eax
	movb	%al, 1(%rsi)
	testq	%rdx, %rdx
	je	.L1
	movzbl	1(%rdx), %eax
	testb	$24, %al
	je	.L1
	orl	$64, %eax
	movb	%al, 1(%rdx)
	ret
	.p2align 4,,10
	.p2align 3
.L37:
	leal	-5(%rax), %edx
	cmpb	$1, %dl
	ja	.L38
.L6:
	subl	$5, %eax
	cmpb	$33, %al
	ja	.L10
	leaq	.L13(%rip), %rdx
	movzbl	%al, %eax
	movslq	(%rdx,%rax,4), %rax
	addq	%rdx, %rax
	notrack jmp	*%rax
	.section	.rodata
	.align 4
	.align 4
.L13:
	.long	.L18-.L13
	.long	.L17-.L13
	.long	.L10-.L13
	.long	.L16-.L13
	.long	.L10-.L13
	.long	.L15-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L14-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L10-.L13
	.long	.L12-.L13
	.text
	.p2align 4,,10
	.p2align 3
.L38:
	ret
.L12:
	leaq	56(%rsi), %rax
.L19:
	movq	136(%rdi), %rdx
	movq	%rdx, (%rax)
	movq	%rsi, 136(%rdi)
	andb	$-57, 1(%rsi)
	ret
.L14:
	leaq	48(%rsi), %rax
	jmp	.L19
.L15:
	leaq	40(%rsi), %rax
	jmp	.L19
.L16:
	leaq	32(%rsi), %rax
	jmp	.L19
.L17:
	leaq	24(%rsi), %rax
	jmp	.L19
.L18:
	leaq	16(%rsi), %rax
	jmp	.L19
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.type	frameless_cold_mark.cold, @function
frameless_cold_mark.cold:
.LFSB2:
.L10:
	movq	136(%rdi), %rax
	movq	%rax, 0
	ud2
	.cfi_endproc
.LFE2:
	.text
	.size	frameless_cold_mark, .-frameless_cold_mark
	.section	.text.unlikely
	.size	frameless_cold_mark.cold, .-frameless_cold_mark.cold
.LCOLDE0:
	.text
.LHOTE0:
	.ident	"GCC: (Ubuntu 15.2.0-16ubuntu1) 15.2.0"
	.section	.note.GNU-stack,"",@progbits
	.section	.note.gnu.property,"a"
	.align 8
	.long	1f - 0f
	.long	4f - 1f
	.long	5
0:
	.string	"GNU"
1:
	.align 8
	.long	0xc0000002
	.long	3f - 2f
2:
	.long	0x3
3:
	.align 8
4:

// A PIC switch loop reloads its spilled GOT base after a byte ring buffer
// write through a masked index.  The ring occupies 0x10..0x1f(%esp) and the
// GOT base is spilled at 0x20(%esp): with RING_MASK 15 no ring write reaches
// the spill slot, while RING_MASK 31 may overwrite it.
//
// ALIGNED_CLEAR adds the aligned `movaps` a compiler clears the ring with,
// whose alignment check faults or changes nothing.  1 puts it where a peeled
// first iteration has it: after the mask of the first ring index and before
// the spill, so the index of the first ring store reaches that store across
// the check.  2 puts it in the loop, between the spill and each reload.
#ifndef RING_MASK
#define RING_MASK 15
#endif
#ifndef ALIGNED_CLEAR
#define ALIGNED_CLEAR 0
#endif

.text
.globl i386_ring_spill
.type i386_ring_spill, @function
i386_ring_spill:
  pushl %ebx
  pushl %esi
  subl $0x2c, %esp
  call .Lpc
.Lpc:
  popl %esi
  .byte 0x81, 0xc6                 // addl imm32, %esi
.Lgot:
  .long .Lgot - .Lpc
  .reloc .Lgot, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
#if ALIGNED_CLEAR == 1
  movl 0x38(%esp), %ebx
  movl %ebx, %edx
  andl $RING_MASK, %edx
  xorps %xmm0, %xmm0
  movaps %xmm0, 0x10(%esp)
  movl %esi, 0x20(%esp)
  movb %bl, 0x10(%esp,%edx,1)
#else
  movl %esi, 0x20(%esp)
  movl 0x38(%esp), %ebx
#endif
  xorl %eax, %eax
  movl $16, %ecx
.Lloop:
#if ALIGNED_CLEAR == 2
  xorps %xmm0, %xmm0
  movaps %xmm0, 0x10(%esp)
#endif
  movl %ebx, %edx
  andl $RING_MASK, %edx
  movb %bl, 0x10(%esp,%edx,1)
  movl 0x20(%esp), %esi
  movl %ebx, %edx
  andl $3, %edx
  movl .Ltable@GOTOFF(%esi,%edx,4), %edx
  addl %esi, %edx
  jmp *%edx
.Lcase0:
  addl $1, %eax
  jmp .Lnext
.Lcase1:
  addl $3, %eax
  jmp .Lnext
.Lcase2:
  addl $5, %eax
  jmp .Lnext
.Lcase3:
  addl $7, %eax
.Lnext:
  imull $1103515245, %ebx, %ebx
  addl $12345, %ebx
  decl %ecx
  jnz .Lloop
  addl $0x2c, %esp
  popl %esi
  popl %ebx
  ret
.size i386_ring_spill, .-i386_ring_spill

.section .rodata
.p2align 2
.Ltable:
  .long .Lcase0@GOTOFF
  .long .Lcase1@GOTOFF
  .long .Lcase2@GOTOFF
  .long .Lcase3@GOTOFF

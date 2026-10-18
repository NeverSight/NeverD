// A PIC switch loop reloads its spilled GOT base after a byte ring buffer
// write through a masked index.  The ring occupies 0x10..0x1f(%esp) and the
// GOT base is spilled at 0x20(%esp): with RING_MASK 15 no ring write reaches
// the spill slot, while RING_MASK 31 may overwrite it.
#ifndef RING_MASK
#define RING_MASK 15
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
  movl %esi, 0x20(%esp)
  movl 0x38(%esp), %ebx
  xorl %eax, %eax
  movl $16, %ecx
.Lloop:
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

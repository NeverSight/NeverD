/* Run the ordinary user-mode string instruction over a 64 KiB data segment. */
.text
.macro measured_copy name, operation, address16
.globl \name
.type \name,@function
\name:
  pushl %ebp
  movl %esp, %ebp
  pushl %esi
  pushl %edi
  push %ds
  push %es
  movl 12(%ebp), %esi
  movl 16(%ebp), %edi
  movl 20(%ebp), %ecx
  movw 8(%ebp), %ax
  cmpl $0, 24(%ebp)
  je 1f
  std
1:
  movw %ax, %ds
  movw %ax, %es
.if \address16
  addr16 rep \operation
.else
  rep \operation
.endif
  cld
  pop %es
  pop %ds
  popl %edi
  popl %esi
  popl %ebp
  ret
.size \name,.-\name
.endm
measured_copy byte16,movsb,1
measured_copy word16,movsw,1
measured_copy dword16,movsl,1
measured_copy byte32,movsb,0
measured_copy word32,movsw,0
measured_copy dword32,movsl,0
.section .note.GNU-stack,"",@progbits

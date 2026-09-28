// Original stateful decoder. The byte key is mutated after every opcode:
// k' = rol8(k,4) XOR 0x55. Its two-step cycle lets a runtime-counted loop
// revisit immutable bytecode without assuming the business counter constant.
#include "generic_vm_shapes.inc"

.macro rolling_vm name, unknown_key
shape_entry \name, 64
  movq 0(%r11), %rax
  movq %rax, 16(%r11)
  movq 8(%r11), %rax
  andq $7, %rax
  movq %rax, 48(%r11)
  movb $0x2d, 40(%r11)
  .if \unknown_key
    movq 0(%r11), %rax
    movb %al, 40(%r11)
  .endif
  cmpq $0, 48(%r11)
  je .Lk_return\@
  leaq .Lk_program\@(%rip), %r10
.Lk_dispatch\@:
  movzbl (%r10), %eax
  incq %r10
  movzbl 40(%r11), %edx
  xorl %edx, %eax
  rolb $4, 40(%r11)
  xorb $0x55, 40(%r11)
  cmpl $2, %eax
  ja .Lk_invalid\@
  leaq .Lk_handlers\@(%rip), %rcx
  movq (%rcx,%rax,8), %rax
  jmpq *%rax
.Lk_step\@:
  movq 16(%r11), %rax
  addq 8(%r11), %rax
  xorq 32(%r11), %rax
  movq %rax, 16(%r11)
  addq $1, 32(%r11)
  jmp .Lk_dispatch\@
.Lk_loop\@:
  movq 32(%r11), %rax
  cmpq 48(%r11), %rax
  jae .Lk_dispatch\@
  leaq .Lk_program\@(%rip), %r10
  jmp .Lk_dispatch\@
.Lk_return\@:
  shape_return 64
.Lk_invalid\@:
  ud2
#if defined(__ELF__)
.size \name, .-\name
#endif
SHAPE_READONLY
.p2align 3
.Lk_handlers\@:
  .quad .Lk_step\@, .Lk_loop\@, .Lk_return\@
.Lk_program\@:
  .byte 0 ^ 0x2d, 1 ^ 0x87, 2 ^ 0x2d
.endm

rolling_vm generic_vm_rolling_loop, 0
rolling_vm generic_vm_rolling_unknown_key, 1
#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

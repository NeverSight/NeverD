// Original absolute-pointer threaded bytecode with two input-selected streams.
// The independent oracle observes arithmetic and stores, not this encoding.
#include "generic_vm_shapes.inc"

shape_entry generic_vm_pointer_program, 64
  leaq .Lp_program(%rip), %r10
  shape_thread_next
.Lp_choose:
  testq $1, 0(%r11)
  jz .Lp_even
  movq $1, 32(%r11)
  movq 8(%r10), %r10
  shape_thread_next
.Lp_even:
  movq (%r10), %r10
  shape_thread_next
.Lp_add:
  movq (%r10), %rax
  addq %rax, 16(%r11)
  addq $8, %r10
  shape_thread_next
.Lp_jump:
  movq (%r10), %r10
  shape_thread_next
.Lp_xor:
  movq (%r10), %rax
  xorq %rax, 16(%r11)
  addq $8, %r10
  shape_thread_next
.Lp_return:
  shape_return 64
#if defined(__ELF__)
.size generic_vm_pointer_program, .-generic_vm_pointer_program
#endif
SHAPE_READONLY
.p2align 3
.Lp_program:
  .quad .Lp_choose, .Lp_even_program, .Lp_odd_program
.Lp_even_program:
  .quad .Lp_add, 19, .Lp_jump, .Lp_tail
.Lp_odd_program:
  .quad .Lp_add, 43, .Lp_jump, .Lp_tail
.Lp_tail:
  .quad .Lp_xor, 0x2468ace, .Lp_return
#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

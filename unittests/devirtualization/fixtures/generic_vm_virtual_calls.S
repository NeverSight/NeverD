// Original virtual CALL/RET machine. A software cursor stack has depth two.
// The outer program calls the same virtual subroutine twice from different
// continuations; each call nests another virtual call. No native CALL occurs.
#include "generic_vm_shapes.inc"

.macro virtual_calls name, corrupt
shape_entry \name, 128
  leaq 64(%rsp), %r9
  leaq .Lv_main\@(%rip), %r10
  shape_thread_next
.Lv_call\@:
  movq (%r10), %rdx
  addq $8, %r10
  movq %r10, (%r9)
  addq $8, %r9
  movq %rdx, %r10
  addq $1, 32(%r11)
  shape_thread_next
.Lv_ret\@:
  subq $8, %r9
  movq (%r9), %r10
  .if \corrupt
    addq 0(%r11), %r10
  .endif
  shape_thread_next
.Lv_add\@:
  movq (%r10), %rax
  addq %rax, 16(%r11)
  addq $8, %r10
  shape_thread_next
.Lv_triple\@:
  movq 16(%r11), %rax
  leaq (%rax,%rax,2), %rax
  movq %rax, 16(%r11)
  shape_thread_next
.Lv_xor\@:
  movq (%r10), %rax
  xorq %rax, 16(%r11)
  addq $8, %r10
  shape_thread_next
.Lv_return\@:
  shape_return 128
#if defined(__ELF__)
.size \name, .-\name
#endif
SHAPE_READONLY
.p2align 3
.Lv_main\@:
  .quad .Lv_call\@, .Lv_outer\@
  .quad .Lv_xor\@, 7
  .quad .Lv_call\@, .Lv_outer\@
  .quad .Lv_return\@
.Lv_outer\@:
  .quad .Lv_add\@, 9
  .quad .Lv_call\@, .Lv_inner\@
  .quad .Lv_ret\@
.Lv_inner\@:
  .quad .Lv_triple\@, .Lv_add\@, 11, .Lv_ret\@
.endm

virtual_calls generic_vm_virtual_calls, 0
virtual_calls generic_vm_virtual_return_unknown, 1
#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

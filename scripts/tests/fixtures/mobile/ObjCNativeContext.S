#if defined(__aarch64__) || defined(__arm64__)
.text
.p2align 2
.globl _nd_context_bridge
.private_extern _nd_context_bridge
_nd_context_bridge:
  stp x20, x30, [sp, #-16]!
  mov x20, x1
  bl _nd_context_value
  ldp x20, x30, [sp], #16
  ret
.p2align 2
.globl _nd_context_value
.private_extern _nd_context_value
_nd_context_value:
  stp x19, x30, [sp, #-16]!
  mov x19, x0
  bl _nd_context_bias
  mov x9, x0
  ldr x8, [x20]
  add x0, x19, x8
  add x0, x0, x9
  ldp x19, x30, [sp], #16
  ret
.p2align 2
.globl _nd_context_only_bridge
.private_extern _nd_context_only_bridge
_nd_context_only_bridge:
  stp x20, x30, [sp, #-16]!
  mov x20, x0
  bl _nd_context_only_value
  ldp x20, x30, [sp], #16
  ret
.p2align 2
.globl _nd_context_only_value
.private_extern _nd_context_only_value
_nd_context_only_value:
  ldr x0, [x20]
  ret
.p2align 2
.globl _nd_context_bias
.private_extern _nd_context_bias
_nd_context_bias:
  mov x0, #19
  ret
#elif defined(__x86_64__)
.text
.p2align 4
.globl _nd_context_bridge
.private_extern _nd_context_bridge
_nd_context_bridge:
  pushq %r13
  movq %rsi, %r13
  callq _nd_context_value
  popq %r13
  ret
.p2align 4
.globl _nd_context_value
.private_extern _nd_context_value
_nd_context_value:
  pushq %rbx
  movq %rdi, %rbx
  callq _nd_context_bias
  movq %rax, %rcx
  movq %rbx, %rax
  addq (%r13), %rax
  addq %rcx, %rax
  popq %rbx
  ret
.p2align 4
.globl _nd_context_only_bridge
.private_extern _nd_context_only_bridge
_nd_context_only_bridge:
  pushq %r13
  movq %rdi, %r13
  callq _nd_context_only_value
  popq %r13
  ret
.p2align 4
.globl _nd_context_only_value
.private_extern _nd_context_only_value
_nd_context_only_value:
  movq (%r13), %rax
  ret
.p2align 4
.globl _nd_context_bias
.private_extern _nd_context_bias
_nd_context_bias:
  movq $19, %rax
  ret
#else
#error Unsupported fixture architecture
#endif

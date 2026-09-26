.text
.p2align 2
// PAGEOFF authenticates only the low 12-bit fragment.  The base is unrelated
// to the matching ADRP, and the incomplete value reaches the call argument.
.globl a64_pageoff_wrong_base_no_escape
.type a64_pageoff_wrong_base_no_escape, %function
a64_pageoff_wrong_base_no_escape:
  stp x19, x30, [sp, #-16]!
  mov w19, w0
  adrp x2, a64_pageoff_wrong_base_table
  mov x0, xzr
  add x0, x0, :lo12:a64_pageoff_wrong_base_table
  bl a64_writable_unknown_callee
  and w10, w19, #1
  adrp x9, a64_pageoff_wrong_base_table
  add x9, x9, :lo12:a64_pageoff_wrong_base_table
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_wrong_case0:
  ldp x19, x30, [sp], #16
  mov w0, #1610
  ret
.La64_pageoff_wrong_case1:
  ldp x19, x30, [sp], #16
  mov w0, #1611
  ret
.size a64_pageoff_wrong_base_no_escape, .-a64_pageoff_wrong_base_no_escape

.p2align 2
.globl a64_writable_unknown_callee
.type a64_writable_unknown_callee, %function
a64_writable_unknown_callee:
  ret
.size a64_writable_unknown_callee, .-a64_writable_unknown_callee

.section .data.jt_a64_pageoff_wrong_base,"aw",%progbits
.p2align 3
.globl a64_pageoff_wrong_base_table
.type a64_pageoff_wrong_base_table, %object
a64_pageoff_wrong_base_table:
  .xword .La64_pageoff_wrong_case0
  .xword .La64_pageoff_wrong_case1
.size a64_pageoff_wrong_base_table, .-a64_pageoff_wrong_base_table

.text
.p2align 2

// One predecessor reaches PAGEOFF through the matching ADRP; the other jumps
// directly to the ADD with a different live value in X0.  The result is not an
// exact address on all paths, but it may expose the writable table and must
// therefore veto a stale static switch.
.globl a64_pageoff_bypass_may_escape
.type a64_pageoff_bypass_may_escape, %function
a64_pageoff_bypass_may_escape:
  stp x19, x30, [sp, #-16]!
  mov w19, w0
  mov x0, xzr
  cbnz w1, .La64_pageoff_bypass_add
  adrp x0, a64_pageoff_bypass_table
.La64_pageoff_bypass_add:
  add x0, x0, :lo12:a64_pageoff_bypass_table
  bl a64_pageoff_bypass_unknown_callee
  and w10, w19, #1
  adrp x9, a64_pageoff_bypass_table
  add x9, x9, :lo12:a64_pageoff_bypass_table
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_bypass_case0:
  ldp x19, x30, [sp], #16
  mov w0, #1614
  ret
.La64_pageoff_bypass_case1:
  ldp x19, x30, [sp], #16
  mov w0, #1615
  ret
.size a64_pageoff_bypass_may_escape, .-a64_pageoff_bypass_may_escape

.p2align 2
.globl a64_pageoff_bypass_unknown_callee
.type a64_pageoff_bypass_unknown_callee, %function
a64_pageoff_bypass_unknown_callee:
  ret
.size a64_pageoff_bypass_unknown_callee, .-a64_pageoff_bypass_unknown_callee

.section .data.jt_a64_pageoff_bypass,"aw",%progbits
.p2align 3
.globl a64_pageoff_bypass_table
.type a64_pageoff_bypass_table, %object
a64_pageoff_bypass_table:
  .xword .La64_pageoff_bypass_case0
  .xword .La64_pageoff_bypass_case1
.size a64_pageoff_bypass_table, .-a64_pageoff_bypass_table

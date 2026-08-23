.text
.p2align 2

// The PAGEOFF relocation names A, but the only feasible ADRP definition at
// the ADD use point names B on another architectural page.  A and B share the
// same low 12 bits, so the runtime argument is exactly B.  Module arbitration
// must retain this non-nominal authenticated page alternative instead of
// treating the absent nominal-page definition as "no address evidence".
.globl a64_pageoff_nominal_miss_escape
.type a64_pageoff_nominal_miss_escape, %function
a64_pageoff_nominal_miss_escape:
  stp x19, x30, [sp, #-16]!
  adrp x0, a64_pageoff_nominal_miss_table_b
  add x0, x0, :lo12:a64_pageoff_nominal_miss_table_a
  bl a64_pageoff_nominal_miss_unknown
  ldp x19, x30, [sp], #16
  ret
.size a64_pageoff_nominal_miss_escape, .-a64_pageoff_nominal_miss_escape

.p2align 2
.globl a64_pageoff_nominal_miss_dispatch_b
.type a64_pageoff_nominal_miss_dispatch_b, %function
a64_pageoff_nominal_miss_dispatch_b:
  and w10, w0, #1
  adrp x9, a64_pageoff_nominal_miss_table_b
  add x9, x9, :lo12:a64_pageoff_nominal_miss_table_b
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_nominal_miss_case_b0:
  mov w0, #2770
  ret
.La64_pageoff_nominal_miss_case_b1:
  mov w0, #2771
  ret
.size a64_pageoff_nominal_miss_dispatch_b, .-a64_pageoff_nominal_miss_dispatch_b

.p2align 2
.globl a64_pageoff_nominal_miss_unknown
.type a64_pageoff_nominal_miss_unknown, %function
a64_pageoff_nominal_miss_unknown:
  ret
.size a64_pageoff_nominal_miss_unknown, .-a64_pageoff_nominal_miss_unknown

.section .data.a64_pageoff_nominal_miss_a,"aw",%progbits
.p2align 12
.globl a64_pageoff_nominal_miss_table_a
.type a64_pageoff_nominal_miss_table_a, %object
a64_pageoff_nominal_miss_table_a:
  .xword 0
  .xword 0
.size a64_pageoff_nominal_miss_table_a, .-a64_pageoff_nominal_miss_table_a

.section .data.a64_pageoff_nominal_miss_b,"aw",%progbits
.p2align 12
.globl a64_pageoff_nominal_miss_table_b
.type a64_pageoff_nominal_miss_table_b, %object
a64_pageoff_nominal_miss_table_b:
  .xword .La64_pageoff_nominal_miss_case_b0
  .xword .La64_pageoff_nominal_miss_case_b1
.size a64_pageoff_nominal_miss_table_b, .-a64_pageoff_nominal_miss_table_b

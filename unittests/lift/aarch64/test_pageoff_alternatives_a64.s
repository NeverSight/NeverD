.text
.p2align 2

// The ADRP and PAGEOFF ADD name different objects/sections that share one
// architectural 4 KiB page.  Owner identity authenticates each relocation,
// but page equality is the arithmetic pairing rule; this output is exactly A.
.globl a64_pageoff_same_page_cross_owner
.type a64_pageoff_same_page_cross_owner, %function
a64_pageoff_same_page_cross_owner:
  adrp x0, a64_pageoff_same_page_b
  add x0, x0, :lo12:a64_pageoff_same_page_a
  ret
.size a64_pageoff_same_page_cross_owner, .-a64_pageoff_same_page_cross_owner

// The two ADRP predecessors have different pages but the same low-12 object
// offset.  The common ADD therefore produces table A or table B.  A relocation
// on the ADD names A, but it must not erase the B alternative from module
// escape analysis.
.p2align 2
.globl a64_pageoff_two_page_escape
.type a64_pageoff_two_page_escape, %function
a64_pageoff_two_page_escape:
  stp x19, x30, [sp, #-16]!
  cbz w1, .La64_pageoff_two_page_a
  adrp x0, a64_pageoff_two_page_table_b
  b .La64_pageoff_two_page_add
.La64_pageoff_two_page_a:
  adrp x0, a64_pageoff_two_page_table_a
.La64_pageoff_two_page_add:
  add x0, x0, :lo12:a64_pageoff_two_page_table_a
  bl a64_pageoff_two_page_unknown
  ldp x19, x30, [sp], #16
  ret
.size a64_pageoff_two_page_escape, .-a64_pageoff_two_page_escape

.p2align 2
.globl a64_pageoff_two_page_dispatch_a
.type a64_pageoff_two_page_dispatch_a, %function
a64_pageoff_two_page_dispatch_a:
  and w10, w0, #1
  adrp x9, a64_pageoff_two_page_table_a
  add x9, x9, :lo12:a64_pageoff_two_page_table_a
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_two_page_case_a0:
  mov w0, #2750
  ret
.La64_pageoff_two_page_case_a1:
  mov w0, #2751
  ret
.size a64_pageoff_two_page_dispatch_a, .-a64_pageoff_two_page_dispatch_a

.p2align 2
.globl a64_pageoff_two_page_dispatch_b
.type a64_pageoff_two_page_dispatch_b, %function
a64_pageoff_two_page_dispatch_b:
  and w10, w0, #1
  adrp x9, a64_pageoff_two_page_table_b
  add x9, x9, :lo12:a64_pageoff_two_page_table_b
  ldr x11, [x9, x10, lsl #3]
  br x11
.La64_pageoff_two_page_case_b0:
  mov w0, #2760
  ret
.La64_pageoff_two_page_case_b1:
  mov w0, #2761
  ret
.size a64_pageoff_two_page_dispatch_b, .-a64_pageoff_two_page_dispatch_b

.p2align 2
.globl a64_pageoff_two_page_unknown
.type a64_pageoff_two_page_unknown, %function
a64_pageoff_two_page_unknown:
  ret
.size a64_pageoff_two_page_unknown, .-a64_pageoff_two_page_unknown

.section .data.a64_pageoff_same_page_a,"aw",%progbits
.p2align 3
.globl a64_pageoff_same_page_a
.type a64_pageoff_same_page_a, %object
a64_pageoff_same_page_a:
  .xword 0
.size a64_pageoff_same_page_a, .-a64_pageoff_same_page_a

.section .data.a64_pageoff_same_page_b,"aw",%progbits
.p2align 3
.globl a64_pageoff_same_page_b
.type a64_pageoff_same_page_b, %object
a64_pageoff_same_page_b:
  .xword 0
.size a64_pageoff_same_page_b, .-a64_pageoff_same_page_b

.section .data.a64_pageoff_two_page_a,"aw",%progbits
.p2align 12
.globl a64_pageoff_two_page_table_a
.type a64_pageoff_two_page_table_a, %object
a64_pageoff_two_page_table_a:
  .xword .La64_pageoff_two_page_case_a0
  .xword .La64_pageoff_two_page_case_a1
.size a64_pageoff_two_page_table_a, .-a64_pageoff_two_page_table_a

.section .data.a64_pageoff_two_page_b,"aw",%progbits
.p2align 12
.globl a64_pageoff_two_page_table_b
.type a64_pageoff_two_page_table_b, %object
a64_pageoff_two_page_table_b:
  .xword .La64_pageoff_two_page_case_b0
  .xword .La64_pageoff_two_page_case_b1
.size a64_pageoff_two_page_table_b, .-a64_pageoff_two_page_table_b

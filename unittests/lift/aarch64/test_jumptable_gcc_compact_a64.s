// GCC's compact switch: the entry is loaded back into the index register, the
// table base register becomes the anchor, and the entry is a signed offset
// (`sxtb #2`/`sxth #2`), so a case may precede the anchor.
.text
.macro A64_GCC_COMPACT name, load, extend, entry, guard=w0
.p2align 2
.globl \name
.type \name,@function
\name:
  cmp \guard, #3
  b.hi .La64_gcc_default\@
  adrp x1, .La64_gcc_table\@
  add x1, x1, :lo12:.La64_gcc_table\@
  \load
  adr x1, .La64_gcc_anchor\@
  add x0, x1, w0, \extend #2
  br x0
.La64_gcc_before\@:
  mov w0, #700
  ret
.La64_gcc_anchor\@:
  mov w0, #701
  ret
.La64_gcc_after\@:
  mov w0, #702
  ret
.La64_gcc_default\@:
  mov w0, #799
  ret
.size \name, .-\name
.pushsection .rodata,"a",%progbits
.p2align 2
.La64_gcc_table\@:
  \entry (.La64_gcc_anchor\@-.La64_gcc_anchor\@)/4
  \entry (.La64_gcc_before\@-.La64_gcc_anchor\@)/4
  \entry (.La64_gcc_after\@-.La64_gcc_anchor\@)/4
  \entry (.La64_gcc_before\@-.La64_gcc_anchor\@)/4
.popsection
.endm
A64_GCC_COMPACT a64_gcc_compact_byte, "ldrb w0, [x1, w0, uxtw]", sxtb, .byte
A64_GCC_COMPACT a64_gcc_compact_half, "ldrh w0, [x1, w0, uxtw #1]", sxth, .hword
// A full-width selector needs both halves of CMP/B.HI. Its encoded immediate
// is narrower than X0, while the raw load must retain all 64 index bits.
A64_GCC_COMPACT a64_gcc_compact_x_index, "ldrb w0, [x1, x0]", sxtb, .byte, x0

// A halfword entry read through its low byte: whatever the table holds, the
// dispatch reads another value from each entry, so no table is recovered.
.p2align 2
.globl a64_gcc_compact_truncated
.type a64_gcc_compact_truncated,@function
a64_gcc_compact_truncated:
  cmp w0, #3
  b.hi .La64_trunc_default
  adrp x1, .La64_trunc_table
  add x1, x1, :lo12:.La64_trunc_table
  ldrh w0, [x1, w0, uxtw #1]
  adr x1, .La64_trunc_anchor
  add x0, x1, w0, sxtb #2
  br x0
.La64_trunc_anchor:
  mov w0, #710
  ret
.La64_trunc_after:
  mov w0, #711
  ret
.La64_trunc_last:
  mov w0, #712
  ret
.La64_trunc_default:
  mov w0, #719
  ret
.size a64_gcc_compact_truncated, .-a64_gcc_compact_truncated
.pushsection .rodata,"a",%progbits
.p2align 2
.La64_trunc_table:
  .hword 0
  .hword (.La64_trunc_after-.La64_trunc_anchor)/4
  .hword (.La64_trunc_last-.La64_trunc_anchor)/4
  .hword (.La64_trunc_after-.La64_trunc_anchor)/4
.popsection

// libgcc's size_of_encoded_value: the guard bounds the index below the bytes
// that follow the table in its data owner, and one case calls abort.
.p2align 2
.globl a64_gcc_compact_guarded_call
.type a64_gcc_compact_guarded_call,@function
a64_gcc_compact_guarded_call:
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  and w0, w0, #0x7
  cmp w0, #4
  b.hi .La64_gcall_abort
  adrp x1, .La64_gcall_table
  add x1, x1, :lo12:.La64_gcall_table
  ldrb w0, [x1, w0, uxtw]
  adr x1, .La64_gcall_anchor
  add x0, x1, w0, sxtb #2
  br x0
.La64_gcall_anchor:
  mov w0, #2
  b .La64_gcall_done
.La64_gcall_four:
  mov w0, #4
  b .La64_gcall_done
.La64_gcall_abort:
  bl abort
.La64_gcall_eight:
  mov w0, #8
.La64_gcall_done:
  ldp x29, x30, [sp], #16
  ret
.size a64_gcc_compact_guarded_call, .-a64_gcc_compact_guarded_call
.pushsection .rodata,"a",%progbits
.La64_gcall_table:
  .byte (.La64_gcall_eight-.La64_gcall_anchor)/4
  .byte (.La64_gcall_abort-.La64_gcall_anchor)/4
  .byte 0
  .byte (.La64_gcall_four-.La64_gcall_anchor)/4
  .byte (.La64_gcall_eight-.La64_gcall_anchor)/4
  .byte 1, 2, 3, 4, 5, 6, 7, 8
.popsection

.text
.p2align 2

// Deliberately unsafe width mismatch: CMP constrains W0, but the table address
// consumes the unmodified X0.  A sound resolver must not borrow the W0 bound
// for X0; e.g. X0=0x1_00000001 passes the guard but indexes far beyond table.
.globl a64_compact_w_guard_x_index
.type a64_compact_w_guard_x_index, %function
a64_compact_w_guard_x_index:
  cmp w0, #1
  b.hi .La64_mismatch_default
  adrp x9, .La64_mismatch_table
  add x9, x9, :lo12:.La64_mismatch_table
  ldrb w11, [x9, x0]
  adr x12, .La64_mismatch_anchor
  add x12, x12, x11, lsl #2
  br x12
.La64_mismatch_table:
  .byte (.La64_mismatch_case0 - .La64_mismatch_anchor) / 4
  .byte (.La64_mismatch_case1 - .La64_mismatch_anchor) / 4
  .byte (.La64_mismatch_poison - .La64_mismatch_anchor) / 4
  .p2align 2
.La64_mismatch_anchor:
.La64_mismatch_case0:
  mov w0, #200
  ret
.La64_mismatch_case1:
  mov w0, #201
  ret
.La64_mismatch_poison:
  mov w0, #499
  ret
.La64_mismatch_default:
  mov w0, #-1
  ret
.size a64_compact_w_guard_x_index, .-a64_compact_w_guard_x_index

.syntax unified
.arm
.text
.p2align 2

// Both paths materialize the same writable table through different applied
// R_ARM_REL32 literal fields.  The common ADD output is exact and escapes to
// the opaque callee; module arbitration must merge the two field witnesses as
// one output value rather than treating them as conflicting certificates.
.globl arm_rel32_two_source_escape
.type arm_rel32_two_source_escape, %function
arm_rel32_two_source_escape:
  push {lr}
  cmp r1, #0
  beq .Larm_rel32_two_source_load_a
  ldr r0, .Larm_rel32_two_source_literal_b
  b .Larm_rel32_two_source_add
.Larm_rel32_two_source_load_a:
  ldr r0, .Larm_rel32_two_source_literal_a
.Larm_rel32_two_source_add:
  add r0, pc, r0
  bl arm_rel32_two_source_unknown
  pop {pc}
.p2align 2
.Larm_rel32_two_source_literal_a:
  .word arm_rel32_two_source_table_a - .Larm_rel32_two_source_add - 8
.Larm_rel32_two_source_literal_b:
  .word arm_rel32_two_source_table_a - .Larm_rel32_two_source_add - 8
.size arm_rel32_two_source_escape, .-arm_rel32_two_source_escape

.p2align 2
.globl arm_rel32_two_source_dispatch_a
.type arm_rel32_two_source_dispatch_a, %function
arm_rel32_two_source_dispatch_a:
  and r0, r0, #1
  ldr r3, .Larm_rel32_two_source_dispatch_a_literal
.Larm_rel32_two_source_dispatch_a_add:
  add r3, pc, r3
  ldr r3, [r3, r0, lsl #2]
  bx r3
.Larm_rel32_two_source_case_a0:
  movw r0, #2730
  bx lr
.Larm_rel32_two_source_case_a1:
  movw r0, #2731
  bx lr
.p2align 2
.Larm_rel32_two_source_dispatch_a_literal:
  .word arm_rel32_two_source_table_a - .Larm_rel32_two_source_dispatch_a_add - 8
.size arm_rel32_two_source_dispatch_a, .-arm_rel32_two_source_dispatch_a

.p2align 2
.globl arm_rel32_two_source_dispatch_b
.type arm_rel32_two_source_dispatch_b, %function
arm_rel32_two_source_dispatch_b:
  and r0, r0, #1
  ldr r3, .Larm_rel32_two_source_dispatch_b_literal
.Larm_rel32_two_source_dispatch_b_add:
  add r3, pc, r3
  ldr r3, [r3, r0, lsl #2]
  bx r3
.Larm_rel32_two_source_case_b0:
  movw r0, #2740
  bx lr
.Larm_rel32_two_source_case_b1:
  movw r0, #2741
  bx lr
.p2align 2
.Larm_rel32_two_source_dispatch_b_literal:
  .word arm_rel32_two_source_table_b - .Larm_rel32_two_source_dispatch_b_add - 8
.size arm_rel32_two_source_dispatch_b, .-arm_rel32_two_source_dispatch_b

.p2align 2
.globl arm_rel32_two_source_unknown
.type arm_rel32_two_source_unknown, %function
arm_rel32_two_source_unknown:
  bx lr
.size arm_rel32_two_source_unknown, .-arm_rel32_two_source_unknown

.section .data.arm_rel32_two_source_tables,"aw",%progbits
.p2align 2
.globl arm_rel32_two_source_table_a
.type arm_rel32_two_source_table_a, %object
arm_rel32_two_source_table_a:
  .word .Larm_rel32_two_source_case_a0
  .word .Larm_rel32_two_source_case_a1
.size arm_rel32_two_source_table_a, .-arm_rel32_two_source_table_a

.p2align 2
.globl arm_rel32_two_source_table_b
.type arm_rel32_two_source_table_b, %object
arm_rel32_two_source_table_b:
  .word .Larm_rel32_two_source_case_b0
  .word .Larm_rel32_two_source_case_b1
.size arm_rel32_two_source_table_b, .-arm_rel32_two_source_table_b

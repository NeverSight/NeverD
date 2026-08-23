.syntax unified
.arm

.text
.p2align 2

.globl arm_predicated_load_may_escape_table
.type arm_predicated_load_may_escape_table, %function
arm_predicated_load_may_escape_table:
  push {r4, lr}
  mov r4, r0
  ldr r0, .Larm_predicated_escape_table_rel
.Larm_predicated_escape_base_add:
  add r0, pc, r0
  cmp r1, #0
  ldrne r0, [r2]
  bl arm_predicated_escape_unknown
  and r4, r4, #1
  ldr r3, .Larm_predicated_escape_dispatch_rel
.Larm_predicated_escape_dispatch_add:
  add r3, pc, r3
  ldr r3, [r3, r4, lsl #2]
  bx r3
.Larm_predicated_escape_case0:
  movw r0, #2710
  pop {r4, pc}
.Larm_predicated_escape_case1:
  movw r0, #2711
  pop {r4, pc}
.p2align 2
.Larm_predicated_escape_table_rel:
  .word arm_predicated_escape_table - .Larm_predicated_escape_base_add - 8
.Larm_predicated_escape_dispatch_rel:
  .word arm_predicated_escape_table - .Larm_predicated_escape_dispatch_add - 8
.size arm_predicated_load_may_escape_table, .-arm_predicated_load_may_escape_table

.p2align 2
.globl arm_predicated_unrelated_load_keeps_table
.type arm_predicated_unrelated_load_keeps_table, %function
arm_predicated_unrelated_load_keeps_table:
  push {r4, lr}
  mov r4, r0
  mov r0, #0
  cmp r1, #0
  ldrne r2, [r3]
  bl arm_predicated_escape_unknown
  and r4, r4, #1
  ldr r3, .Larm_predicated_safe_dispatch_rel
.Larm_predicated_safe_dispatch_add:
  add r3, pc, r3
  ldr r3, [r3, r4, lsl #2]
  bx r3
.Larm_predicated_safe_case0:
  movw r0, #2720
  pop {r4, pc}
.Larm_predicated_safe_case1:
  movw r0, #2721
  pop {r4, pc}
.p2align 2
.Larm_predicated_safe_dispatch_rel:
  .word arm_predicated_safe_table - .Larm_predicated_safe_dispatch_add - 8
.size arm_predicated_unrelated_load_keeps_table, .-arm_predicated_unrelated_load_keeps_table

.text
.p2align 2
.globl arm_predicated_escape_unknown
.type arm_predicated_escape_unknown, %function
arm_predicated_escape_unknown:
  bx lr
.size arm_predicated_escape_unknown, .-arm_predicated_escape_unknown

.section .data.arm_predicated_jt,"aw",%progbits
.p2align 2
.globl arm_predicated_escape_table
.type arm_predicated_escape_table, %object
arm_predicated_escape_table:
  .word .Larm_predicated_escape_case0
  .word .Larm_predicated_escape_case1
.size arm_predicated_escape_table, .-arm_predicated_escape_table

.p2align 2
.globl arm_predicated_safe_table
.type arm_predicated_safe_table, %object
arm_predicated_safe_table:
  .word .Larm_predicated_safe_case0
  .word .Larm_predicated_safe_case1
.size arm_predicated_safe_table, .-arm_predicated_safe_table

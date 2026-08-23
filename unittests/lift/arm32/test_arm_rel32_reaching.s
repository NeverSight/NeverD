.syntax unified
.arm
.text
.p2align 2

// One path loads the authenticated R_ARM_REL32 fragment; the other reaches the
// PC ADD with an opaque live-in R0.  The output may expose the target object but
// is not an all-path exact address certificate.
.globl arm_rel32_bypass_may_depend
.type arm_rel32_bypass_may_depend, %function
arm_rel32_bypass_may_depend:
  cmp r1, #0
  bne .Larm_rel32_bypass_add
  ldr r0, .Larm_rel32_bypass_literal
.Larm_rel32_bypass_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_rel32_bypass_literal:
  .word arm_rel32_bypass_target - .Larm_rel32_bypass_add - 8
.size arm_rel32_bypass_may_depend, .-arm_rel32_bypass_may_depend

// Two section-local relocation fields intentionally have the same raw
// r_offset.  Their mapped SlotVAs and targets are distinct; a CFG consumer
// must use the loader's applied-slot identity rather than searching the raw
// relocation vector by Address.
.section .text.arm_rel32_offset_a,"ax",%progbits
.p2align 2
.globl arm_rel32_same_offset_a
.type arm_rel32_same_offset_a, %function
arm_rel32_same_offset_a:
  ldr r0, .Larm_rel32_offset_a_literal
.Larm_rel32_offset_a_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_rel32_offset_a_literal:
  .word arm_rel32_offset_target_a - .Larm_rel32_offset_a_add - 8
.size arm_rel32_same_offset_a, .-arm_rel32_same_offset_a

.section .text.arm_rel32_offset_b,"ax",%progbits
.p2align 2
.globl arm_rel32_same_offset_b
.type arm_rel32_same_offset_b, %function
arm_rel32_same_offset_b:
  ldr r0, .Larm_rel32_offset_b_literal
.Larm_rel32_offset_b_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_rel32_offset_b_literal:
  .word arm_rel32_offset_target_b - .Larm_rel32_offset_b_add - 8
.size arm_rel32_same_offset_b, .-arm_rel32_same_offset_b

// Both predecessors load a different R_ARM_REL32 field for the same target.
// The common PC ADD has one exact value but two authenticated source fields;
// the public occurrence set must retain both rather than choosing the first.
.section .text.arm_rel32_two_literals,"ax",%progbits
.p2align 2
.globl arm_rel32_same_target_two_literals
.type arm_rel32_same_target_two_literals, %function
arm_rel32_same_target_two_literals:
  cmp r1, #0
  beq .Larm_rel32_two_load_a
  ldr r0, .Larm_rel32_two_literal_b
  b .Larm_rel32_two_add
.Larm_rel32_two_load_a:
  ldr r0, .Larm_rel32_two_literal_a
.Larm_rel32_two_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_rel32_two_literal_a:
  .word arm_rel32_two_literal_target - .Larm_rel32_two_add - 8
.Larm_rel32_two_literal_b:
  .word arm_rel32_two_literal_target - .Larm_rel32_two_add - 8
.size arm_rel32_same_target_two_literals, .-arm_rel32_same_target_two_literals

.section .data.arm_rel32_reaching,"aw",%progbits
.p2align 2
.globl arm_rel32_bypass_target
.type arm_rel32_bypass_target, %object
arm_rel32_bypass_target:
  .word 0
.size arm_rel32_bypass_target, .-arm_rel32_bypass_target

.globl arm_rel32_offset_target_a
.type arm_rel32_offset_target_a, %object
arm_rel32_offset_target_a:
  .word 0
.size arm_rel32_offset_target_a, .-arm_rel32_offset_target_a

.globl arm_rel32_offset_target_b
.type arm_rel32_offset_target_b, %object
arm_rel32_offset_target_b:
  .word 0
.size arm_rel32_offset_target_b, .-arm_rel32_offset_target_b

.globl arm_rel32_two_literal_target
.type arm_rel32_two_literal_target, %object
arm_rel32_two_literal_target:
  .word 0
.size arm_rel32_two_literal_target, .-arm_rel32_two_literal_target

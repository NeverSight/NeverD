.syntax unified
.thumb
.text
.p2align 1

.def arm_tbb_inclusive; .scl 2; .type 32; .endef
.globl arm_tbb_inclusive
.thumb_func
arm_tbb_inclusive:
  cmp r0, #2
  bhi .Larm_tbb_inclusive_default
  // Keep TBB at address 2 modulo 4.  Unlike ordinary Thumb PC-relative
  // operands, TBB/TBH use the raw Thumb PC (instruction address + 4) as both
  // table and target base; aligning it would point into this instruction.
  nop
  tbb [pc, r0]
.Larm_tbb_inclusive_table:
  .byte (.Larm_tbb_inclusive_case0 - .Larm_tbb_inclusive_table) / 2
  .byte (.Larm_tbb_inclusive_case1 - .Larm_tbb_inclusive_table) / 2
  .byte (.Larm_tbb_inclusive_case2 - .Larm_tbb_inclusive_table) / 2
  .byte (.Larm_tbb_inclusive_poison - .Larm_tbb_inclusive_table) / 2
.p2align 1
.Larm_tbb_inclusive_case0:
  movs r0, #100
  bx lr
.Larm_tbb_inclusive_case1:
  movs r0, #101
  bx lr
.Larm_tbb_inclusive_case2:
  movs r0, #102
  bx lr
.Larm_tbb_inclusive_poison:
  movw r0, #399
  bx lr
.Larm_tbb_inclusive_default:
  movs r0, #0
  mvns r0, r0
  bx lr

.p2align 1
.def arm_tbh_exclusive; .scl 2; .type 32; .endef
.globl arm_tbh_exclusive
.thumb_func
arm_tbh_exclusive:
  cmp r0, #2
  bhs .Larm_tbh_exclusive_default
  tbh [pc, r0, lsl #1]
.Larm_tbh_exclusive_table:
  .hword (.Larm_tbh_exclusive_case0 - .Larm_tbh_exclusive_table) / 2
  .hword (.Larm_tbh_exclusive_case1 - .Larm_tbh_exclusive_table) / 2
  .hword (.Larm_tbh_exclusive_poison - .Larm_tbh_exclusive_table) / 2
.p2align 1
.Larm_tbh_exclusive_case0:
  movs r0, #200
  bx lr
.Larm_tbh_exclusive_case1:
  movs r0, #201
  bx lr
.Larm_tbh_exclusive_poison:
  movw r0, #499
  bx lr
.Larm_tbh_exclusive_default:
  movs r0, #0
  mvns r0, r0
  bx lr

.p2align 1
.def arm_tbb_spill_inclusive; .scl 2; .type 32; .endef
.globl arm_tbb_spill_inclusive
.thumb_func
arm_tbb_spill_inclusive:
  sub sp, #4
  str r0, [sp]
  ldr r1, [sp]
  cmp r1, #2
  bhi .Larm_tbb_spill_default
  ldr r2, [sp]
  tbb [pc, r2]
.Larm_tbb_spill_table:
  .byte (.Larm_tbb_spill_case0 - .Larm_tbb_spill_table) / 2
  .byte (.Larm_tbb_spill_case1 - .Larm_tbb_spill_table) / 2
  .byte (.Larm_tbb_spill_case2 - .Larm_tbb_spill_table) / 2
  .byte (.Larm_tbb_spill_poison - .Larm_tbb_spill_table) / 2
.p2align 1
.Larm_tbb_spill_case0:
  movw r0, #300
  add sp, #4
  bx lr
.Larm_tbb_spill_case1:
  movw r0, #301
  add sp, #4
  bx lr
.Larm_tbb_spill_case2:
  movw r0, #302
  add sp, #4
  bx lr
.Larm_tbb_spill_poison:
  movw r0, #599
  add sp, #4
  bx lr
.Larm_tbb_spill_default:
  movs r0, #0
  mvns r0, r0
  add sp, #4
  bx lr

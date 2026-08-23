.syntax unified
.thumb
.text
.p2align 1

// The AND is instruction-predicated, so only the NE path narrows the value to
// two entries; EQ retains an unbounded incoming index.  The adjacent decodable
// slots make the old byte-scan fallback publish a four-way table, proving this
// is not a naturally unresolved negative.
.def arm_tbb_predicated_mask; .scl 2; .type 32; .endef
.globl arm_tbb_predicated_mask
.thumb_func
arm_tbb_predicated_mask:
  cmp r1, #0
  it ne
  andne r0, r0, #1
  tbb [pc, r0]
.Larm_tbb_predicated_table:
  .byte (.Larm_tbb_predicated_case0 - .Larm_tbb_predicated_table) / 2
  .byte (.Larm_tbb_predicated_case1 - .Larm_tbb_predicated_table) / 2
  .byte (.Larm_tbb_predicated_case2 - .Larm_tbb_predicated_table) / 2
  .byte (.Larm_tbb_predicated_case3 - .Larm_tbb_predicated_table) / 2
.p2align 1
.Larm_tbb_predicated_case0:
  movw r0, #2300
  bx lr
.Larm_tbb_predicated_case1:
  movw r0, #2301
  bx lr
.Larm_tbb_predicated_case2:
  movw r0, #2302
  bx lr
.Larm_tbb_predicated_case3:
  movw r0, #2303
  bx lr

// The mask itself is unconditional, but the following offset is predicated.
// Its false arm stays in [0,1] while the true arm is in [1,2]; neither the
// two-slot mask bound nor a four-entry byte scan is a complete runtime domain.
.p2align 1
.def arm_tbb_predicated_offset; .scl 2; .type 32; .endef
.globl arm_tbb_predicated_offset
.thumb_func
arm_tbb_predicated_offset:
  and r0, r0, #1
  cmp r1, #0
  it ne
  addne r0, r0, #1
  tbb [pc, r0]
.Larm_tbb_predicated_offset_table:
  .byte (.Larm_tbb_predicated_offset_case0 - .Larm_tbb_predicated_offset_table) / 2
  .byte (.Larm_tbb_predicated_offset_case1 - .Larm_tbb_predicated_offset_table) / 2
  .byte (.Larm_tbb_predicated_offset_case2 - .Larm_tbb_predicated_offset_table) / 2
  .byte (.Larm_tbb_predicated_offset_poison - .Larm_tbb_predicated_offset_table) / 2
.p2align 1
.Larm_tbb_predicated_offset_case0:
  movw r0, #2310
  bx lr
.Larm_tbb_predicated_offset_case1:
  movw r0, #2311
  bx lr
.Larm_tbb_predicated_offset_case2:
  movw r0, #2312
  bx lr
.Larm_tbb_predicated_offset_poison:
  movw r0, #2399
  bx lr

// The low-byte extraction itself is condition-executed.  Its false arm keeps
// the unbounded incoming word while its true arm is in [0,255], so unsafe-domain
// taint must follow the speculative low-lane producer through the architectural
// SELECT rather than requiring the whole merged output to equal that producer.
.p2align 1
.def arm_tbb_predicated_mask_low8; .scl 2; .type 32; .endef
.globl arm_tbb_predicated_mask_low8
.thumb_func
arm_tbb_predicated_mask_low8:
  cmp r1, #0
  it ne
  uxtbne r0, r0
  tbb [pc, r0]
.Larm_tbb_predicated_low8_table:
  .byte (.Larm_tbb_predicated_low8_case0 - .Larm_tbb_predicated_low8_table) / 2
  .byte (.Larm_tbb_predicated_low8_case1 - .Larm_tbb_predicated_low8_table) / 2
  .byte (.Larm_tbb_predicated_low8_case2 - .Larm_tbb_predicated_low8_table) / 2
  .byte (.Larm_tbb_predicated_low8_poison - .Larm_tbb_predicated_low8_table) / 2
.p2align 1
.Larm_tbb_predicated_low8_case0:
  movw r0, #2320
  bx lr
.Larm_tbb_predicated_low8_case1:
  movw r0, #2321
  bx lr
.Larm_tbb_predicated_low8_case2:
  movw r0, #2322
  bx lr
.Larm_tbb_predicated_low8_poison:
  movw r0, #2398
  bx lr

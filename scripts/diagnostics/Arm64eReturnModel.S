//===- Arm64eReturnModel.S - Arm model entry and EL0 call --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Arm64eReturnModel.def"
#ifdef RETURN_MODEL_ORDERED
#include "Arm64eOrderedModel.def"
#endif

.text
.globl _start
_start:
  ldr x0, =RETURN_MODEL_STACK_EL1
  mov sp, x0
  adr x0, Lvectors
  msr VBAR_EL1, x0
  ldr x0, =RETURN_MODEL_TCR_48BIT
  msr TCR_EL1, x0
  mrs x0, CPACR_EL1
  orr x0, x0, #RETURN_MODEL_FPEN
  msr CPACR_EL1, x0
  ldr x0, =RETURN_MODEL_KEY_A_LO
  msr APIAKeyLo_EL1, x0
  ldr x0, =RETURN_MODEL_KEY_A_HI
  msr APIAKeyHi_EL1, x0
  ldr x0, =RETURN_MODEL_KEY_B_LO
  msr APIBKeyLo_EL1, x0
  ldr x0, =RETURN_MODEL_KEY_B_HI
  msr APIBKeyHi_EL1, x0
#ifdef RETURN_MODEL_NORMAL_MEMORY
  adrp x0, LcursorL0@PAGE
  add x0, x0, LcursorL0@PAGEOFF
  msr TTBR0_EL1, x0
  mov x0, #RETURN_MODEL_NORMAL_WB
  msr MAIR_EL1, x0
  dsb sy
  isb
  tlbi vmalle1
  dsb sy
  isb
#endif
  mrs x0, SCTLR_EL1
  orr x0, x0, #RETURN_MODEL_ENIA_IB
#ifdef RETURN_MODEL_NORMAL_MEMORY
  orr x0, x0, #RETURN_MODEL_MMU
#endif
  msr SCTLR_EL1, x0
  isb
  b _runReturnModel

.globl _runAtEL0
_runAtEL0:
  // The projected arm64e program may overwrite LR and use its own stack.
  stp x29, x30, [sp, #-RETURN_MODEL_STACK_ALIGNMENT]!
  ldr x9, =RETURN_MODEL_STACK_EL0
  msr SP_EL0, x9
  mov x9, #RETURN_MODEL_DAIF_EL0T
  msr SPSR_EL1, x9
  adr x9, Lapplication
  msr ELR_EL1, x9
  eret
Lapplication:
  // x0 and x1 carry the expected enable control and EL0 stack top.
  bl _runReturnOracle
  svc #RETURN_MODEL_SVC
LafterSVC:
  b Lunexpected
Lresume:
  ldp x29, x30, [sp], #RETURN_MODEL_STACK_ALIGNMENT
  ret

.p2align RETURN_MODEL_VECTOR_SHIFT
Lvectors:
.rept RETURN_MODEL_VECTOR_COUNT
  b Lexception
  .space RETURN_MODEL_VECTOR_SLOT - RETURN_MODEL_INSN_BYTES
.endr
Lexception:
#ifdef RETURN_MODEL_ORDERED
  // Keep all interrupted registers intact when a data abort resumes the guest.
  stp x9, x10, [sp, #-RETURN_MODEL_STACK_ALIGNMENT]!
  mrs x9, ESR_EL1
  lsr x10, x9, #ORDERED_EC_SHIFT
  cmp x10, #ORDERED_ABORT_EL0
  b.eq LorderedAbort
  cmp x10, #ORDERED_ABORT_EL1
  b.eq LorderedAbort
  ldp x9, x10, [sp], #RETURN_MODEL_STACK_ALIGNMENT
#endif
  mrs x9, ESR_EL1
  ldr x10, =RETURN_MODEL_SVC_ESR
  cmp x9, x10
  b.ne Lunexpected
  mrs x9, SPSR_EL1
  tst x9, #RETURN_MODEL_PSTATE_MODE_MASK
  b.ne Lunexpected
  mrs x9, ELR_EL1
  adr x10, LafterSVC
  cmp x9, x10
  b.ne Lunexpected
  mov x9, #RETURN_MODEL_DAIF_EL1H
  msr SPSR_EL1, x9
  adr x9, Lresume
  msr ELR_EL1, x9
  eret
#ifdef RETURN_MODEL_ORDERED
LorderedAbort:
  ldr x10, =RETURN_MODEL_EL0_DATA
  str x9, [x10, #ORDERED_ESR_SLOT]
  ldr x9, [x10, #ORDERED_ARMED_SLOT]
  cbz x9, Lunexpected
  str xzr, [x10, #ORDERED_ARMED_SLOT]
  mrs x9, FAR_EL1
  str x9, [x10, #ORDERED_FAR_SLOT]
  mrs x9, ELR_EL1
  ldr w9, [x9]
  str x9, [x10, #ORDERED_WORD_SLOT]
  mrs x9, ELR_EL1
  add x9, x9, #RETURN_MODEL_INSN_BYTES
  msr ELR_EL1, x9
  ldp x9, x10, [sp], #RETURN_MODEL_STACK_ALIGNMENT
  eret
#endif
Lunexpected:
  b _unexpectedReturnException

#ifdef RETURN_MODEL_NORMAL_MEMORY
// Identity-map the image and the two stacks as Normal memory. Apple Clang can
// legally access stack aggregates with unaligned loads and stores.
.const
.p2align RETURN_MODEL_PAGE_SHIFT, 0
LcursorL0:
  .quad LcursorL1 + RETURN_MODEL_TABLE
  .space (RETURN_MODEL_TABLE_ENTRIES - 1) * RETURN_MODEL_ENTRY_BYTES
LcursorL1:
  .space (RETURN_MODEL_TEXT_BASE >> RETURN_MODEL_L1_SHIFT) * RETURN_MODEL_ENTRY_BYTES
  .quad LcursorL2 + RETURN_MODEL_TABLE
  .p2align RETURN_MODEL_PAGE_SHIFT, 0
LcursorL2:
  .set LtextBlock, RETURN_MODEL_TEXT_BASE
  .rept RETURN_MODEL_TEXT_BLOCKS
    .quad LtextBlock | RETURN_MODEL_BLOCK | RETURN_MODEL_EL0_ACCESS | RETURN_MODEL_READ_ONLY
    .set LtextBlock, LtextBlock + RETURN_MODEL_BLOCK_BYTES
  .endr
  .quad RETURN_MODEL_EL0_DATA | RETURN_MODEL_BLOCK | RETURN_MODEL_EL0_ACCESS | RETURN_MODEL_DATA_XN
  .quad RETURN_MODEL_EL1_DATA | RETURN_MODEL_BLOCK | RETURN_MODEL_DATA_XN
  .p2align RETURN_MODEL_PAGE_SHIFT, 0
  .globl RETURN_MODEL_FIRST_STACK_SYMBOL
  .set RETURN_MODEL_FIRST_STACK_SYMBOL, RETURN_MODEL_EL0_DATA
#endif

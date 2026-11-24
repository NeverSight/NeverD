//===- Arm64eReturnModel.S - Arm model entry and EL0 call --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Arm64eReturnModel.def"

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
  mrs x0, SCTLR_EL1
  orr x0, x0, #RETURN_MODEL_ENIA_IB
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
Lunexpected:
  b _unexpectedReturnException

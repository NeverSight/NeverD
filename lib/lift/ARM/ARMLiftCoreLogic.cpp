//===- ARMLiftCoreLogic.cpp - ARM32 core bitwise logic lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 bitwise data processing: AND, ORR, ORN, EOR and BIC.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARM/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftCoreLogic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  NdOp Operation;
  bool InvertRight;
  switch (Insn->id) {
#define NEVERD_ARM_LOGIC(Name, Op, Invert)                                     \
  case ARM_INS_##Name:                                                         \
    Operation = NdOp::Op;                                                      \
    InvertRight = Invert;                                                      \
    break;
#include "ARMIntegerForms.def"
  default:
    return false;
  }
  if (ARM.op_count < 2)
    return true;
  NdVar Dst = L.operandWrite(ARM.operands[0]);
  NdVar Left = ARM.op_count >= 3 ? L.operandRead(S, ARM.operands[1])
                                 : NdVar::reg(Dst.Offset, 4);
  const auto &RightOperand = ARM.operands[ARM.op_count >= 3 ? 2 : 1];
  NdVar Right = L.operandRead(S, RightOperand);
  if (ARM.update_flags)
    L.emitLogicalOpCarry(S, Insn, RightOperand);
  if (InvertRight) {
    NdVar Inverted = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, Inverted, {Right});
    Right = Inverted;
  }
  S.emit(Operation, Dst, {Left, Right});
  if (ARM.update_flags)
    L.emitNZ(S, Dst);
  return true;
}

} // namespace neverd

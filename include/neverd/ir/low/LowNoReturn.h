//===- LowNoReturn.h - LowIR terminating-path proof --------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWNORETURN_H
#define NEVERD_IR_LOW_LOWNORETURN_H

#include "neverd/ir/low/LowIR.h"

namespace neverd {

/// Ends the ordinary path; exceptional successors remain reachable.
inline bool isUnconditionalNoReturn(const LowInstructionBoundary &Insn) {
  return hasLowInstructionControlFlag(Insn.ControlFlags,
                                      LowInstructionControlFlag::NoReturn) &&
         !hasLowInstructionControlFlag(Insn.ControlFlags,
                                       LowInstructionControlFlag::Conditional);
}

bool isArchitecturalNoReturn(const LowOp &Op);
inline bool isArchitecturalNoReturn(const LowOp &Op, Arch) {
  return isArchitecturalNoReturn(Op);
}

/// Follow ordinary and exceptional paths using exact instruction boundaries.
/// Returning or unknown exceptional destinations prevent a no-return proof.
bool lowFunctionNeverReturns(const LowFunc &Func, Arch TheArch);

} // namespace neverd

#endif

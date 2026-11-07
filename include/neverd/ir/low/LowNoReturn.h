//===- LowNoReturn.h - LowIR terminating-path proof --------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWNORETURN_H
#define NEVERD_IR_LOW_LOWNORETURN_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd {

/// Ends the ordinary path; exceptional successors remain reachable.
inline bool isUnconditionalNoReturn(const LowInstructionBoundary &Insn) {
  return hasLowInstructionControlFlag(Insn.ControlFlags,
                                      LowInstructionControlFlag::NoReturn) &&
         !hasLowInstructionControlFlag(Insn.ControlFlags,
                                       LowInstructionControlFlag::Conditional);
}

/// Whether a handler may resume a function where its graph has no edge to show
/// it. A registration frame without a complete proof has none: display recovery
/// defers the proof and those edges together. A frame with no scope or try
/// block only runs cleanups, which continue unwinding.
inline bool hasUnmodeledRegistrationResumption(
    const std::optional<ExceptionFunction> &Metadata,
    const std::optional<RegistrationStateAnalysis> &States) {
  if (!States || States->Complete || !Metadata || !Metadata->Registration)
    return false;
  return !Metadata->Registration->Scopes.empty() ||
         (Metadata->Cxx && !Metadata->Cxx->TryBlocks.empty());
}

bool isArchitecturalNoReturn(const LowOp &Op);
inline bool isArchitecturalNoReturn(const LowOp &Op, Arch) {
  return isArchitecturalNoReturn(Op);
}

/// Follow ordinary and exceptional paths using exact instruction boundaries.
/// Returning or unknown exceptional destinations prevent a no-return proof.
/// A direct tail call ends its path where \p TailCalleeNeverReturns answers
/// that its target never returns; without that answer it returns.
bool lowFunctionNeverReturns(
    const LowFunc &Func, Arch TheArch,
    llvm::function_ref<bool(va_t)> TailCalleeNeverReturns = nullptr);

} // namespace neverd

#endif

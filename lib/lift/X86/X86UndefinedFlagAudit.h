//===- X86UndefinedFlagAudit.h - Scalar flag-effect audit -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check unconditional undefined status bits after family-owned form admission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86UNDEFINEDFLAGAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86UNDEFINEDFLAGAUDIT_H

#include "neverd/ir/low/LowUndefinedEffects.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::undefinedflags {

inline bool matches(llvm::ArrayRef<unsigned> Flags, llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  if (Effects.Effects.size() != Flags.size())
    return false;
  for (size_t I = 0; I != Flags.size(); ++I) {
    const auto &E = Effects.Effects[I];
    if (E.Output != NdVar::reg(Flags[I], 1) || E.BitOffset || E.BitCount != 1 ||
        E.When || !E.AfterOp || E.AfterOp > Ops.size())
      return false;
    for (const auto &Later : Ops.drop_front(E.AfterOp)) {
      if (Later.Output == E.Output)
        return false;
      for (unsigned J = 0; J != Later.NumInputs; ++J)
        if (Later.Inputs[J] == E.Output)
          return false;
    }
  }
  return true;
}

} // namespace neverd::undefinedflags

#endif // NEVERD_LIB_LIFT_X86_X86UNDEFINEDFLAGAUDIT_H

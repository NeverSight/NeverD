//===- X64CallRCXProfile.h - Bounded native call source profile -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The Win64 native call bridge uses the baseline SSE2 register file. A wider
/// vector or another target-specific intrinsic needs its own clobber contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X64CALLRCXPROFILE_H
#define NEVERD_IR_MED_X86_X64CALLRCXPROFILE_H

#include "neverd/ir/intrinsics/X86/X64CallRCX.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {
inline bool x64CallRCXProfileValue(const MedVar &V) { return V.Size <= 16; }
inline bool x64CallRCXProfileOperation(const MedOp &Op) {
  return Op.NumInputs <= Op.Inputs.size() &&
         (Op.Opcode != NdOp::INTRINSIC ||
          (Op.NumInputs && Op.Inputs[0].isConst() &&
           Op.Inputs[0].ConstVal == uint64_t(Intrinsic::X64CallRCX)));
}
inline bool x64CallRCXCallerProfile(const MedFunc &F) {
  for (const auto *Values : {&F.Params, &F.Locals})
    for (const auto &V : *Values)
      if (!x64CallRCXProfileValue(V))
        return false;
  for (const auto &B : F.Blocks) {
    for (const auto &Phi : B.Phis) {
      if (!x64CallRCXProfileValue(Phi.Output))
        return false;
      for (const auto &[Pred, V] : Phi.Args)
        if (!x64CallRCXProfileValue(V))
          return false;
    }
    for (const auto &Op : B.Ops) {
      if (!x64CallRCXProfileOperation(Op) || !x64CallRCXProfileValue(Op.Output))
        return false;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!x64CallRCXProfileValue(Op.Inputs[I]))
          return false;
      for (const auto &V : Op.IntrinsicOutputs)
        if (!x64CallRCXProfileValue(V))
          return false;
    }
  }
  return true;
}
} // namespace neverd

#endif // NEVERD_IR_MED_X86_X64CALLRCXPROFILE_H

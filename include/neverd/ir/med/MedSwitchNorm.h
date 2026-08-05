//===- MedSwitchNorm.h - Switch-variable normalization peeling ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared helper for recovering the *unnormalized* switch variable behind a
/// zero-based table index, used by both the HighIR switch structurer and the
/// LLVM-C switch emitter so they present identical case labels.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSWITCHNORM_H
#define NEVERD_IR_MED_MEDSWITCHNORM_H

#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <map>
#include <utility>

namespace neverd {

/// Recover the source switch variable behind a normalized table index.
///
/// A compiler lowers `switch (x)` whose lowest case label is not 0 by
/// normalizing the variable to a zero-based table index — `idx = x - lo`, or
/// `idx = x + k` for a negative-based switch — and dispatching on `idx`.  A
/// switch presented on `idx` therefore carries cases `0..N-1` instead of the
/// real (possibly negative) labels.  Adding/subtracting a constant is a
/// bijection, so peeling that step off the dispatch variable and shifting every
/// case label by the inverse constant selects the same block for every input
/// while restoring the original variable and labels.
///
/// \p Index is traced through value-preserving reshapes (copy / width change /
/// low-half subpiece) and at most \p MaxAffine constant add/subtract steps.
/// \p LabelDelta receives the amount to add to each case label so it matches the
/// returned variable.  When no affine normalization is found, \p Index is
/// returned unchanged with \p LabelDelta == 0.
inline MedVar peelAffineSwitchVar(const MedFunc &Fn, MedVar Index,
                                  int64_t &LabelDelta, int MaxAffine = 2) {
  LabelDelta = 0;
  if (Index.isConst())
    return Index;

  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &B : Fn.Blocks)
    for (auto &Op : B.Ops)
      if (!Op.Output.isConst() && Op.Output.Size > 0)
        Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Defs.find({V.Id, V.SSAVer});
    return It == Defs.end() ? nullptr : It->second;
  };

  MedVar Cur = Index;
  int64_t Delta = 0;
  int Affine = 0;
  for (int Hop = 0; Hop < 64; ++Hop) {
    const MedOp *D = defOf(Cur);
    if (!D)
      break;
    // Value-preserving reshapes: follow without changing the label mapping.
    if ((D->Opcode == NdOp::COPY || D->Opcode == NdOp::INT_ZEXT ||
         D->Opcode == NdOp::INT_SEXT) &&
        D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
      Cur = D->Inputs[0];
      continue;
    }
    if (D->Opcode == NdOp::SUBBYTES && D->NumInputs >= 2 &&
        D->Inputs[1].isConst() && D->Inputs[1].ConstVal == 0 &&
        !D->Inputs[0].isConst()) {
      Cur = D->Inputs[0];
      continue;
    }
    // Affine normalization: idx = v + c  (v = idx - c, so label += -c)
    //                       idx = v - c  (v = idx + c, so label += +c)
    // Only a constant addend on one side keeps the mapping a bijection; a
    // `c - v` reverse-subtract negates the index and is intentionally not
    // peeled (its labels are not a simple shift).
    if ((D->Opcode == NdOp::INT_ADD || D->Opcode == NdOp::INT_SUB) &&
        D->NumInputs >= 2 && Affine < MaxAffine) {
      int CW = D->Inputs[1].isConst() ? 1 : (D->Inputs[0].isConst() ? 0 : -1);
      if (D->Opcode == NdOp::INT_SUB && CW != 1)
        break; // reverse subtract (const - v): not a simple shift
      if (CW < 0 || D->Inputs[1 - CW].isConst())
        break;
      int64_t C = static_cast<int64_t>(D->Inputs[CW].ConstVal);
      // Sign-extend a sub-64-bit constant so a 32-bit `+6` is not read as a
      // multi-billion offset.
      uint16_t CSz = D->Inputs[CW].Size;
      if (CSz > 0 && CSz < 8) {
        uint64_t M = (1ULL << (CSz * 8)) - 1;
        uint64_t U = static_cast<uint64_t>(C) & M;
        if (U & (1ULL << (CSz * 8 - 1)))
          C = static_cast<int64_t>(U | ~M);
      }
      Delta += (D->Opcode == NdOp::INT_ADD) ? -C : C;
      Cur = D->Inputs[1 - CW];
      Affine += 1;
      continue;
    }
    break;
  }

  if (Affine == 0)
    return Index;
  LabelDelta = Delta;
  return Cur;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSWITCHNORM_H

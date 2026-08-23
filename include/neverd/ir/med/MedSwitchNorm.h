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

#include "llvm/ADT/APInt.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

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
/// \p Index is traced through same-width value-preserving reshapes and at most
/// \p MaxAffine constant add/subtract steps.  Width-changing ZEXT/SEXT and
/// SUBBYTES are deliberately not peeled: presenting the switch on their source
/// is equivalent only after proving the complete case domain is a bijection in
/// the conversion's image.  Stopping at the widened/narrowed result preserves
/// that proof obligation without losing the recovered switch.
///
/// \p LabelDelta is an unsigned modular bit-pattern at the selector width.  It
/// is added to each case label to obtain the case value for the returned
/// variable.  Constants follow the emitter's Coerce semantics: their own bit
/// pattern is zero-extended or truncated to the arithmetic output width.  No
/// host signed negation or addition is used, including for INT64_MIN.
inline MedVar
peelAffineSwitchVar(const MedFunc &Fn, MedVar Index, uint64_t &LabelDelta,
                    int MaxAffine = 2,
                    const std::vector<int64_t> *CaseLabels = nullptr) {
  LabelDelta = 0;
  if (Index.isConst() || Index.Size == 0 || Index.Size > 8)
    return Index;

  using DefKey = std::tuple<MedVar::VarKind, int, int, uint16_t>;
  std::map<DefKey, const MedOp *> Defs;
  for (auto &B : Fn.Blocks)
    for (auto &Op : B.Ops) {
      if (Op.Output.isConst() || Op.Output.Size == 0)
        continue;
      DefKey Key{Op.Output.Kind, Op.Output.Id, Op.Output.SSAVer,
                 Op.Output.Size};
      auto [It, Inserted] = Defs.emplace(Key, &Op);
      if (!Inserted)
        It->second = nullptr; // duplicate SSA identity: fail closed at lookup
    }
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Defs.find({V.Kind, V.Id, V.SSAVer, V.Size});
    return It == Defs.end() ? nullptr : It->second;
  };

  MedVar Cur = Index;
  unsigned Width = Index.Size * 8u;
  llvm::APInt Delta(Width, 0);
  int Affine = 0;
  for (int Hop = 0; Hop < 64; ++Hop) {
    const MedOp *D = defOf(Cur);
    if (!D)
      break;
    // Only same-width reshapes are unconditionally value preserving.  A
    // width-changing conversion can have missing or multiple preimages and
    // therefore requires a separate complete-domain proof.
    if (D->Opcode == NdOp::COPY && D->NumInputs >= 1 &&
        !D->Inputs[0].isConst() && D->Output.Size == Cur.Size &&
        D->Inputs[0].Size == Cur.Size) {
      Cur = D->Inputs[0];
      continue;
    }
    if ((D->Opcode == NdOp::INT_ZEXT || D->Opcode == NdOp::INT_SEXT) &&
        D->NumInputs >= 1 && !D->Inputs[0].isConst() &&
        D->Output.Size == Cur.Size && D->Inputs[0].Size > 0 &&
        D->Inputs[0].Size < Cur.Size && CaseLabels) {
      const unsigned NarrowWidth = D->Inputs[0].Size * 8u;
      std::set<uint64_t> NarrowCases;
      bool InjectiveOnCases = true;
      for (int64_t Label : *CaseLabels) {
        llvm::APInt Bits(Width, static_cast<uint64_t>(Label),
                         /*isSigned=*/false, /*implicitTrunc=*/true);
        Bits += Delta;
        const llvm::APInt Narrow = Bits.trunc(NarrowWidth);
        const llvm::APInt Reextended = D->Opcode == NdOp::INT_ZEXT
                                           ? Narrow.zext(Width)
                                           : Narrow.sext(Width);
        if (Reextended != Bits ||
            !NarrowCases.insert(Narrow.getZExtValue()).second) {
          InjectiveOnCases = false;
          break;
        }
      }
      if (!InjectiveOnCases)
        break;
      Width = NarrowWidth;
      Delta = Delta.trunc(Width);
      Cur = D->Inputs[0];
      continue;
    }
    if (D->Opcode == NdOp::SUBBYTES && D->NumInputs >= 2 &&
        D->Inputs[1].isConst() && D->Inputs[1].ConstVal == 0 &&
        !D->Inputs[0].isConst() && D->Output.Size == Cur.Size) {
      if (D->Inputs[0].Size == Cur.Size) {
        Cur = D->Inputs[0];
        continue;
      }
      // x86 flag materialization often leaves the exact selector behind the
      // canonical round trip `low(zext(x))`.  Peeling the SUBBYTES alone would
      // be unsound when the wider value has non-zero high bits, but the paired
      // ZEXT definition proves the round trip is exactly x at this width.
      const MedOp *WideDef = defOf(D->Inputs[0]);
      if (WideDef && WideDef->Opcode == NdOp::INT_ZEXT &&
          WideDef->NumInputs >= 1 && !WideDef->Inputs[0].isConst() &&
          WideDef->Output.Size == D->Inputs[0].Size &&
          WideDef->Inputs[0].Size == Cur.Size) {
        Cur = WideDef->Inputs[0];
        continue;
      }
    }
    // Affine normalization: idx = v + c  (v = idx - c, so label += -c)
    //                       idx = v - c  (v = idx + c, so label += +c)
    // Only a constant addend on one side keeps the mapping a bijection; a
    // `c - v` reverse-subtract negates the index and is intentionally not
    // peeled (its labels are not a simple shift).
    if ((D->Opcode == NdOp::INT_ADD || D->Opcode == NdOp::INT_SUB) &&
        D->NumInputs >= 2 && Affine < MaxAffine && D->Output.Size == Cur.Size) {
      int CW = D->Inputs[1].isConst() ? 1 : (D->Inputs[0].isConst() ? 0 : -1);
      if (D->Opcode == NdOp::INT_SUB && CW != 1)
        break; // reverse subtract (const - v): not a simple shift
      if (CW < 0 || D->Inputs[1 - CW].isConst() ||
          D->Inputs[1 - CW].Size != Cur.Size || D->Inputs[CW].Size == 0 ||
          D->Inputs[CW].Size > 8)
        break;
      llvm::APInt C(D->Inputs[CW].Size * 8u, D->Inputs[CW].ConstVal,
                    /*isSigned=*/false, /*implicitTrunc=*/true);
      C = C.zextOrTrunc(Width);
      Delta = D->Opcode == NdOp::INT_ADD ? Delta - C : Delta + C;
      Cur = D->Inputs[1 - CW];
      Affine += 1;
      continue;
    }
    break;
  }

  if (Affine == 0)
    return Index;
  LabelDelta = Delta.getZExtValue();
  return Cur;
}

/// Convert signed-storage case labels plus a modular affine delta into unique
/// selector-width bit-patterns.  Both LLVM and HighIR use this helper so a
/// truncation collision fails closed before either backend publishes a switch.
inline std::optional<std::vector<uint64_t>>
uniqueSwitchCaseBitPatterns(const std::vector<int64_t> &Labels,
                            uint64_t LabelDelta, unsigned SelectorBits) {
  if (SelectorBits == 0 || SelectorBits > 64)
    return std::nullopt;

  llvm::APInt Delta(SelectorBits, LabelDelta, /*isSigned=*/false,
                    /*implicitTrunc=*/true);
  std::set<uint64_t> Seen;
  std::vector<uint64_t> Result;
  Result.reserve(Labels.size());
  for (int64_t Label : Labels) {
    llvm::APInt Bits(SelectorBits, static_cast<uint64_t>(Label),
                     /*isSigned=*/false, /*implicitTrunc=*/true);
    Bits += Delta;
    uint64_t Value = Bits.getZExtValue();
    if (!Seen.insert(Value).second)
      return std::nullopt;
    Result.push_back(Value);
  }
  return Result;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSWITCHNORM_H

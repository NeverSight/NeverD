//===- MedScalarSelection.h - Exact-width masked selections ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared recognition of scalar selections lowered to complementary masks.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSCALARSELECTION_H
#define NEVERD_IR_MED_MEDSCALARSELECTION_H

#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd {

struct MedScalarSelection {
  MedVar Condition;
  MedVar TrueValue;
  MedVar FalseValue;
};

/// Match (T & -zext(C)) | (F & ~-zext(C)) at one exact width. Each consumer
/// supplies its occurrence-bound lookup and Boolean proof, including their
/// work limits. A narrow mask must never become a selection of a wider value.
template <typename Lookup, typename Same, typename Boolean>
std::optional<MedScalarSelection>
matchMaskedScalarSelection(const MedOp &Or, Lookup &&Find, Same &&Equal,
                           Boolean &&IsBoolean) {
  if (Or.Opcode != NdOp::INT_OR || Or.NumInputs != 2 || !Or.Output.Size)
    return std::nullopt;
  const uint16_t Width = Or.Output.Size;
  if (Or.Inputs[0].Size != Width || Or.Inputs[1].Size != Width)
    return std::nullopt;
  const MedOp *A = Find(Or.Inputs[0]);
  const MedOp *B = Find(Or.Inputs[1]);
  if (!A || !B || A->Opcode != NdOp::INT_AND || B->Opcode != NdOp::INT_AND ||
      A->NumInputs != 2 || B->NumInputs != 2 || A->Output.Size != Width ||
      B->Output.Size != Width)
    return std::nullopt;
  const auto Mask =
      [&](const MedVar &Value) -> std::optional<std::pair<MedVar, bool>> {
    if (Value.Size != Width)
      return std::nullopt;
    const MedOp *Op = Find(Value);
    bool Positive = true;
    if (Op && Op->Opcode == NdOp::INT_NOT && Op->NumInputs == 1) {
      if (Op->Output.Size != Width || Op->Inputs[0].Size != Width)
        return std::nullopt;
      Positive = false;
      Op = Find(Op->Inputs[0]);
    }
    if (!Op || Op->Opcode != NdOp::INT_NEG2 || Op->NumInputs != 1 ||
        Op->Output.Size != Width || Op->Inputs[0].Size != Width)
      return std::nullopt;
    Op = Find(Op->Inputs[0]);
    if (!Op || Op->Opcode != NdOp::INT_ZEXT || Op->NumInputs != 1 ||
        Op->Output.Size != Width || !Op->Inputs[0].Size ||
        Op->Inputs[0].Size > Width)
      return std::nullopt;
    return std::pair{Op->Inputs[0], Positive};
  };
  for (unsigned I = 0; I != 2; ++I)
    for (unsigned J = 0; J != 2; ++J) {
      if (A->Inputs[1 - I].Size != Width || B->Inputs[1 - J].Size != Width)
        continue;
      const auto First = Mask(A->Inputs[I]);
      const auto Second = Mask(B->Inputs[J]);
      if (First && Second && First->second != Second->second &&
          Equal(First->first, Second->first) && IsBoolean(First->first))
        return MedScalarSelection{
            First->first, First->second ? A->Inputs[1 - I] : B->Inputs[1 - J],
            First->second ? B->Inputs[1 - J] : A->Inputs[1 - I]};
    }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSCALARSELECTION_H

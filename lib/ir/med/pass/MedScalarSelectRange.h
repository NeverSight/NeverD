//===- MedScalarSelectRange.h - Conditional scalar bounds -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Relate a scalar selection to the comparison that selects its arm.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_PASS_MEDSCALARSELECTRANGE_H
#define NEVERD_IR_MED_PASS_MEDSCALARSELECTRANGE_H

#include "neverd/ir/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedScalarSelection.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/InstrTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::detail {

struct ScalarConstraint {
  MedVar Root;
  llvm::ConstantRange Values;
};

/// Lookup distinguishes an unavailable/ambiguous occurrence (nullopt) from a
/// valid scalar root without a defining operation (a null pointer). The caller
/// owns its immutable SSA index and the budget used by the surrounding proof.
template <typename Lookup, typename Same, typename Stable>
class ScalarSelectRange {
public:
  ScalarSelectRange(Lookup Find, Same Equal, Stable Scalar, size_t &Steps)
      : Find(std::move(Find)), Equal(std::move(Equal)),
        Scalar(std::move(Scalar)), Steps(Steps) {}

  std::optional<MedScalarSelection> selection(const MedOp &Op, unsigned Depth) {
    if (!spend(Depth) || !Op.Output.Size || Op.Output.Size > sizeof(uint64_t))
      return std::nullopt;
    const auto Width = Op.Output.Size;
    if (Op.Opcode == NdOp::SELECT && Op.NumInputs == 3 &&
        Op.Inputs[1].Size == Width && Op.Inputs[2].Size == Width)
      return MedScalarSelection{Op.Inputs[0], Op.Inputs[1], Op.Inputs[2]};
    return matchMaskedScalarSelection(
        Op, [&](const MedVar &V) { return operation(V, Depth + 1); }, Equal,
        [&](const MedVar &V) {
          return condition(V, true, Depth + 1).has_value();
        });
  }

  /// Only predicates with Boolean results can establish a full-width mask.
  /// A Boolean union/intersection is useful when both operands constrain the
  /// same occurrence. Otherwise it supplies no relational fact.
  std::optional<ScalarConstraint> condition(const MedVar &Value, bool True,
                                            unsigned Depth) {
    const auto Op = operation(Value, Depth);
    if (!Op)
      return std::nullopt;
    if ((Op->Opcode == NdOp::COPY || Op->Opcode == NdOp::INT_ZEXT) &&
        Op->NumInputs == 1 && Op->Inputs[0].Size &&
        Op->Inputs[0].Size <= Value.Size &&
        (Op->Opcode != NdOp::COPY || Op->Inputs[0].Size == Value.Size))
      return condition(Op->Inputs[0], True, Depth + 1);
    if (Op->Opcode == NdOp::BOOL_NOT && Op->NumInputs == 1)
      return condition(Op->Inputs[0], !True, Depth + 1);
    if ((Op->Opcode == NdOp::BOOL_OR || Op->Opcode == NdOp::BOOL_AND) &&
        Op->NumInputs == 2) {
      auto A = condition(Op->Inputs[0], True, Depth + 1);
      const auto B = condition(Op->Inputs[1], True, Depth + 1);
      if (!A || !B || !Equal(A->Root, B->Root))
        return std::nullopt;
      const bool Union = (Op->Opcode == NdOp::BOOL_OR) == True;
      A->Values = Union ? A->Values.unionWith(B->Values)
                        : A->Values.intersectWith(B->Values);
      return A;
    }
    llvm::CmpInst::Predicate Predicate;
    switch (Op->Opcode) {
    case NdOp::INT_EQUAL:
      Predicate = llvm::CmpInst::ICMP_EQ;
      break;
    case NdOp::INT_NOTEQUAL:
      Predicate = llvm::CmpInst::ICMP_NE;
      break;
    case NdOp::INT_LESS:
      Predicate = llvm::CmpInst::ICMP_ULT;
      break;
    case NdOp::INT_LESSEQUAL:
      Predicate = llvm::CmpInst::ICMP_ULE;
      break;
    case NdOp::INT_SLESS:
      Predicate = llvm::CmpInst::ICMP_SLT;
      break;
    case NdOp::INT_SLESSEQUAL:
      Predicate = llvm::CmpInst::ICMP_SLE;
      break;
    default:
      return std::nullopt;
    }
    if (Op->NumInputs != 2 || !Op->Inputs[0].Size ||
        Op->Inputs[0].Size > sizeof(uint64_t) ||
        Op->Inputs[0].Size != Op->Inputs[1].Size)
      return std::nullopt;
    unsigned Variable = 0;
    if (Op->Inputs[0].isConst()) {
      Variable = 1;
      Predicate = llvm::CmpInst::getSwappedPredicate(Predicate);
    }
    const auto &Constant = Op->Inputs[1 - Variable];
    if (!scalarConstant(Constant))
      return std::nullopt;
    if (!True)
      Predicate = llvm::CmpInst::getInversePredicate(Predicate);
    const unsigned Bits = Constant.Size * 8;
    const auto Expression = affine(Op->Inputs[Variable], Bits, Depth + 1);
    if (!Expression)
      return std::nullopt;
    auto Allowed = llvm::ConstantRange::makeExactICmpRegion(
        Predicate, llvm::APInt(Bits, Constant.ConstVal));
    Allowed = Allowed.sub(llvm::ConstantRange(Expression->Offset));
    return ScalarConstraint{Expression->Root, std::move(Allowed)};
  }

private:
  struct Affine {
    MedVar Root;
    llvm::APInt Offset;
  };

  Lookup Find;
  Same Equal;
  Stable Scalar;
  size_t &Steps;

  bool spend(unsigned Depth) {
    return ++Steps <= limits::ir::kMedStackAlignmentMaxProofSteps &&
           Depth <= limits::ir::kMedStackAlignmentResolveDepthLimit;
  }

  bool scalarConstant(const MedVar &Value) { return Scalar(Value); }

  const MedOp *operation(const MedVar &Value, unsigned Depth) {
    if (!spend(Depth))
      return nullptr;
    const auto Found = Find(Value);
    return Found ? *Found : nullptr;
  }

  /// Normalize only the low Bits. Wider additions wrap identically in those
  /// bits; a narrowing extension or a nonzero slice cannot be inverted here.
  /// The final root must have exactly Bits, so no omitted high-bit constraint
  /// can accidentally become a narrow full-value range.
  std::optional<Affine> affine(const MedVar &Value, unsigned Bits,
                               unsigned Depth) {
    if (!spend(Depth) || Value.isConst() || Value.Size * 8 < Bits)
      return std::nullopt;
    const auto Found = Find(Value);
    if (!Found)
      return std::nullopt;
    const auto *Op = *Found;
    if (Op) {
      if ((Op->Opcode == NdOp::COPY || Op->Opcode == NdOp::INT_ZEXT ||
           Op->Opcode == NdOp::INT_SEXT) &&
          Op->NumInputs == 1 && Op->Inputs[0].Size * 8 >= Bits &&
          Op->Inputs[0].Size <= Value.Size &&
          (Op->Opcode != NdOp::COPY || Op->Inputs[0].Size == Value.Size))
        return affine(Op->Inputs[0], Bits, Depth + 1);
      if (Op->Opcode == NdOp::SUBBYTES && Op->NumInputs == 2 &&
          Op->Inputs[0].Size >= Value.Size && scalarConstant(Op->Inputs[1]) &&
          !Op->Inputs[1].ConstVal)
        return affine(Op->Inputs[0], Bits, Depth + 1);
      if ((Op->Opcode == NdOp::INT_ADD || Op->Opcode == NdOp::INT_SUB) &&
          Op->NumInputs == 2 && Op->Inputs[0].Size == Value.Size &&
          Op->Inputs[1].Size == Value.Size) {
        unsigned Variable = 0;
        if (Op->Opcode == NdOp::INT_ADD && Op->Inputs[0].isConst())
          Variable = 1;
        const auto &Constant = Op->Inputs[1 - Variable];
        if (scalarConstant(Constant)) {
          auto Input = affine(Op->Inputs[Variable], Bits, Depth + 1);
          if (!Input)
            return std::nullopt;
          const llvm::APInt Delta(Bits, Constant.ConstVal);
          Input->Offset += Op->Opcode == NdOp::INT_ADD ? Delta : -Delta;
          return Input;
        }
      }
    }
    if (Value.Size * 8 != Bits)
      return std::nullopt;
    return Affine{Value, llvm::APInt(Bits, 0)};
  }
};

} // namespace neverd::detail

#endif // NEVERD_IR_MED_PASS_MEDSCALARSELECTRANGE_H

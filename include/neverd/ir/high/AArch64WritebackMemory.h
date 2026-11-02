//===- AArch64WritebackMemory.h - HighIR writeback contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate writeback before source inference, filtering or emission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_AARCH64WRITEBACKMEMORY_H
#define NEVERD_IR_HIGH_AARCH64WRITEBACKMEMORY_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"

namespace neverd {

inline bool aarch64WritebackMemoryHighShapeIsValid(const HighExpr &Expression) {
  if (Expression.Kind != ExprKind::Call ||
      Expression.Operands.size() !=
          aarch64WritebackMemoryOperandCount(Expression.IntrinsicId) ||
      !aarch64WritebackMemoryOutputsAreValid(
          Expression.IntrinsicId, Expression.IntrinsicOutputs,
          [](const MedVar &V) { return V.Size; }) ||
      Expression.IsIndirectCall || Expression.IndirectTarget ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Integer = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Unknown);
  };
  const bool Store = isAArch64WritebackStoreIntrinsic(Expression.IntrinsicId);
  for (const auto &Output : Expression.IntrinsicOutputs)
    if ((Output.Kind != MedVar::Reg && Output.Kind != MedVar::Temp) ||
        (Output.TheArch != Arch::Unknown && Output.TheArch != Arch::AArch64))
      return false;
  for (size_t I = 0; I != Expression.IntrinsicOutputs.size(); ++I)
    for (size_t J = 0; J != I; ++J)
      if (Expression.IntrinsicOutputs[I] == Expression.IntrinsicOutputs[J])
        return false;
  if (isAArch64WritebackAuxiliaryLoad(Expression.IntrinsicId)
          ? (!Expression.Type || Expression.Type->Kind != NdTypeKind::Void)
          : (!Integer(Expression.Type) &&
             !(Store && Expression.Type &&
               Expression.Type->Kind == NdTypeKind::Ptr)))
    return false;
  for (const auto &Operand : Expression.Operands)
    if (!Operand || !Operand->Type ||
        (!Integer(Operand->Type) && Operand->Type->Kind != NdTypeKind::Ptr))
      return false;
  const auto &Word = *Expression.Operands[kWritebackWordOperand];
  if (!Integer(Word.Type) || Word.Kind != ExprKind::Const)
    return false;
  if (aarch64StructureOperation(Expression.IntrinsicId)) {
    for (unsigned I = kStructureDataOperand; I < Expression.Operands.size();
         ++I)
      if (!Integer(Expression.Operands[I]->Type))
        return false;
    return aarch64StructureShapeIsValid(
        Expression.IntrinsicId, Expression.Operands.size(),
        Expression.Type->Size,
        [&](unsigned I) { return Expression.Operands[I]->Type->Size; },
        [&](unsigned I) -> std::optional<uint64_t> {
          return Expression.Operands[I]->Kind == ExprKind::Const
                     ? std::optional<uint64_t>(Expression.Operands[I]->ConstVal)
                     : std::nullopt;
        });
  }
  const bool PairStore =
      Store && isAArch64PairMemoryIntrinsic(Expression.IntrinsicId);
  const auto &Data =
      Expression
          .Operands[Store ? kWritebackDataOperand : kWritebackWordOperand];
  if ((Expression.IntrinsicId == Intrinsic::A64_VectorStoreWriteback ||
       Expression.IntrinsicId == Intrinsic::A64_VectorPairStore) &&
      (!Integer(Data->Type) ||
       (PairStore &&
        !Integer(Expression.Operands[kPairSecondDataOperand]->Type))))
    return false;
  return aarch64WritebackMemoryShapeIsValid(
      Expression.IntrinsicId, Expression.Operands.size() + 1,
      Expression.Type->Size,
      Expression.Operands[kWritebackBaseOperand]->Type->Size, Word.Type->Size,
      Word.ConstVal, Store ? Data->Type->Size : 0,
      Store && Data->Kind == ExprKind::Const
          ? std::optional<uint64_t>(Data->ConstVal)
          : std::nullopt,
      PairStore ? Expression.Operands[kPairSecondDataOperand]->Type->Size : 0,
      PairStore && Expression.Operands[kPairSecondDataOperand]->Kind ==
                       ExprKind::Const
          ? std::optional<uint64_t>(
                Expression.Operands[kPairSecondDataOperand]->ConstVal)
          : std::nullopt);
}

} // namespace neverd

#endif // NEVERD_IR_HIGH_AARCH64WRITEBACKMEMORY_H

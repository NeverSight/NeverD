//===- AArch64PAuthLoad.h - HighIR authenticated load contract --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate the combined authenticated load before source admission or
/// emission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_AARCH64PAUTHLOAD_H
#define NEVERD_IR_HIGH_AARCH64PAUTHLOAD_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"

namespace neverd {

inline bool aarch64PAuthLoadHighShapeIsValid(const HighExpr &Expression) {
  if (Expression.Kind != ExprKind::Call ||
      Expression.Operands.size() != kPAuthLoadOperandCount ||
      !Expression.IntrinsicOutputs.empty() || Expression.IsIndirectCall ||
      Expression.IndirectTarget || !Expression.Operands[0] ||
      !Expression.Operands[1] ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Integer = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Unknown);
  };
  const auto &Base = *Expression.Operands[0];
  const auto &Word = *Expression.Operands[1];
  if (!Integer(Expression.Type) || !Base.Type ||
      (!Integer(Base.Type) && Base.Type->Kind != NdTypeKind::Ptr) ||
      !Integer(Word.Type) || Word.Kind != ExprKind::Const)
    return false;
  return aarch64PAuthLoadShapeIsValid(
      Expression.IntrinsicId, Expression.Operands.size() + 1,
      Expression.Type->Size, Base.Type->Size, Word.Type->Size, Word.ConstVal);
}

} // namespace neverd

#endif // NEVERD_IR_HIGH_AARCH64PAUTHLOAD_H

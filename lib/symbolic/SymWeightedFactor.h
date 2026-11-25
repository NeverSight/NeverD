//===- SymWeightedFactor.h - Weighted fields in product factors -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H
#define NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H

#include "neverd/symbolic/SymExpr.h"

namespace neverd::symbolic::detail {

struct WeightedFactor {
  SymRef Offset;
  llvm::ArrayRef<SymRef> Terms;
  SymRef FieldMask{};
  bool SubtractField = false;
};

inline WeightedFactor readWeightedFactor(const SymContext &Ctx, SymRef F) {
  const auto Op = Ctx.op(F);
  if (Op != SymOp::Add && Op != SymOp::Or && Op != SymOp::Xor &&
      Op != SymOp::And)
    return {};
  const auto Args = Ctx.operands(F);
  if (!Ctx.isConst(Args.front()))
    return {};
  if (Args.size() != 2)
    return Op == SymOp::Add ? WeightedFactor{Args.front(), Args.drop_front()}
                            : WeightedFactor{};
  auto Sum = Args.back();
  if (Op == SymOp::And) {
    if (Ctx.op(Sum) != SymOp::Not)
      return {};
    Sum = Ctx.operand(Sum, 0);
  }
  // A mask on the weighted delta precedes its addition or bitwise join
  // with the scalar offset. Keep the mask until its live bits are known.
  // Subtraction is represented as addition of a product by all ones.
  const bool Scaled = Op == SymOp::Add && Ctx.op(Sum) == SymOp::Mul &&
                      Ctx.numOperands(Sum) == 2;
  const SymRef Delta = Scaled ? Ctx.operand(Sum, 1) : Sum;
  if (Ctx.op(Delta) == SymOp::And && Ctx.numOperands(Delta) == 2 &&
      Ctx.isConst(Ctx.operand(Delta, 0)) &&
      Ctx.op(Ctx.operand(Delta, 1)) == SymOp::Add &&
      (!Scaled || Ctx.isConstOnes(Ctx.operand(Sum, 0))))
    return {Args.front(), Ctx.operands(Ctx.operand(Delta, 1)),
            Ctx.operand(Delta, 0), Scaled};
  if (Op == SymOp::Add)
    return {Args.front(), Args.drop_front()};
  if (Ctx.op(Sum) == SymOp::Add)
    return {Args.front(), Ctx.operands(Sum)};
  return {};
}

} // namespace neverd::symbolic::detail

#endif // NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H

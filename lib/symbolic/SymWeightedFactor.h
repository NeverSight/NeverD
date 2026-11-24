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
  SymRef ToggleMask{};
};

inline WeightedFactor readWeightedFactor(const SymContext &Ctx, SymRef F) {
  const auto Op = Ctx.op(F);
  if (Op != SymOp::Add && Op != SymOp::Or && Op != SymOp::Xor &&
      Op != SymOp::And)
    return {};
  const auto Args = Ctx.operands(F);
  if (!Ctx.isConst(Args.front()))
    return {};
  if (Op == SymOp::Add)
    return {Args.front(), Args.drop_front()};
  if (Args.size() != 2)
    return {};
  auto Sum = Args.back();
  if (Op == SymOp::And) {
    if (Ctx.op(Sum) != SymOp::Not)
      return {};
    Sum = Ctx.operand(Sum, 0);
  }
  // An explicit toggle mask also occurs when the compiler spells C & ~D
  // as C ^ (M & D) and clears bits of M already known zero in D.
  if (Op == SymOp::Xor && Ctx.op(Sum) == SymOp::And &&
      Ctx.numOperands(Sum) == 2 && Ctx.isConst(Ctx.operand(Sum, 0)) &&
      Ctx.op(Ctx.operand(Sum, 1)) == SymOp::Add)
    return {Args.front(), Ctx.operands(Ctx.operand(Sum, 1)),
            Ctx.operand(Sum, 0)};
  if (Ctx.op(Sum) == SymOp::Add)
    return {Args.front(), Ctx.operands(Sum)};
  return {};
}

} // namespace neverd::symbolic::detail

#endif // NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H

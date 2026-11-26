//===- SymWeightedFactor.h - Weighted fields in product factors -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H
#define NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H

#include "neverd/symbolic/SymExpr.h"

#include <bit>

namespace neverd::symbolic::detail {

// On sorted factors, one equal pair this far apart certifies a run longer
// than a binary search. Three probes avoid walking a large product to find it.
inline bool hasLongFactorRun(llvm::ArrayRef<SymRef> Factors) {
  const size_t Probe = std::bit_width(Factors.size()),
               Middle = Factors.size() / 2;
  return Probe < Middle &&
         (Factors.front() == Factors[Probe] ||
          Factors[Middle] == Factors[Middle + Probe] ||
          Factors.back() == Factors[Factors.size() - 1 - Probe]);
}

// A stage selects bits with a literal mask, then multiplies by a literal.
struct WeightedFieldStage {
  SymRef Mask{};
  SymRef Scale{};
};

struct WeightedFactor {
  SymRef Offset;
  llvm::ArrayRef<SymRef> Terms;
  WeightedFieldStage Outer{};
  WeightedFieldStage Inner{};
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
  auto Delta = Sum;
  const auto takeLiteral = [&](SymOp Wanted) -> SymRef {
    if (Ctx.op(Delta) != Wanted || Ctx.numOperands(Delta) != 2 ||
        !Ctx.isConst(Ctx.operand(Delta, 0)))
      return {};
    const auto Literal = Ctx.operand(Delta, 0);
    Delta = Ctx.operand(Delta, 1);
    return Literal;
  };
  const auto readStage = [&] {
    const auto Scale = takeLiteral(SymOp::Mul);
    return WeightedFieldStage{takeLiteral(SymOp::And), Scale};
  };
  const auto Outer = readStage();
  if (Outer.Mask) {
    // A multiplier inside the result mask can itself consume a masked sum.
    const auto Inner =
        Ctx.op(Delta) == SymOp::Mul ? readStage() : WeightedFieldStage{};
    if (Ctx.op(Delta) == SymOp::Add)
      return {Args.front(), Ctx.operands(Delta), Outer, Inner};
  }
  if (Op == SymOp::Add)
    return {Args.front(), Args.drop_front()};
  if (Ctx.op(Sum) == SymOp::Add)
    return {Args.front(), Ctx.operands(Sum)};
  return {};
}

} // namespace neverd::symbolic::detail

#endif // NEVERD_LIB_SYMBOLIC_SYMWEIGHTEDFACTOR_H

//===- HighCHiLoCollapse.cpp - Hi/Lo pattern collapse -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pattern matching to collapse hi/lo 32-bit pairs back into 64-bit values
/// in the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"

namespace neverd {

HiLoCollapseResult
tryCollapseHiLo(const HighExpr &Expr, const std::vector<HiLoPair> &Pairs,
                std::function<std::string(const HighExpr &)> UnwrapFn) {
  if (Expr.Kind != ExprKind::BinOp || Expr.Op != NdOp::INT_OR ||
      Expr.Operands.size() != 2)
    return {};

  auto ExtractShifted = [&](const HighExpr &E) -> std::string {
    if (E.Kind != ExprKind::BinOp || E.Op != NdOp::INT_LEFT ||
        E.Operands.size() != 2)
      return {};
    if (E.Operands[1]->Kind != ExprKind::Const || E.Operands[1]->ConstVal != 32)
      return {};
    return UnwrapFn(*E.Operands[0]);
  };

  std::string Hi = ExtractShifted(*Expr.Operands[0]);
  std::string Lo = UnwrapFn(*Expr.Operands[1]);
  if (Hi.empty()) {
    Hi = ExtractShifted(*Expr.Operands[1]);
    Lo = UnwrapFn(*Expr.Operands[0]);
  }
  if (Hi.empty() || Lo.empty())
    return {};

  for (auto &P : Pairs) {
    if (P.Lo == Lo && P.Hi == Hi)
      return {P.CollapseExpr, Lo, Hi, P.Stmt};
  }
  return {};
}

} // namespace neverd

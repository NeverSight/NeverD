//===- HighExprSimplify.cpp - Expression simplification for HighIR --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Algebraic and boolean expression simplification:
///   - Double negation elimination (!(!x) → x)
///   - Comparison negation folding (!(a < b) → b <= a)
///   - Identity/zero elimination  (x + 0 → x, x * 1 → x)
///   - Subtraction-to-comparison folding  ((a - b) == 0 → a == b)
///   - Negative constant folding  (x + (-N) → x - N)
///   - Sub-piece identity elimination  (SUBBYTES((u64)x, 0) → x)
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/ir/high/MedToHigh.h"

#include <unordered_set>

namespace neverd {

static void simplifyExprRecursive(ExprPtr &E,
                                  std::unordered_set<const HighExpr *> &Seen) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  for (auto &Op : E->Operands)
    simplifyExprRecursive(Op, Seen);

  // Atomic accesses are observable even when the surrounding value appears
  // algebraically redundant.  Keep the containing expression intact so a
  // simplification cannot discard, duplicate, or move the access.
  if (E->hasOrderedMemoryAccess())
    return;

  if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
      !E->Operands.empty() && E->Operands[0]->Kind == ExprKind::UnaryOp &&
      E->Operands[0]->Op == NdOp::BOOL_NOT &&
      !E->Operands[0]->Operands.empty()) {
    E = E->Operands[0]->Operands[0];
    return;
  }

  if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
      !E->Operands.empty() && E->Operands[0]->Kind == ExprKind::BinOp &&
      E->Operands[0]->Operands.size() == 2) {
    const ExprPtr &InnerExpr = E->Operands[0];
    NdOp NegOp = NdOp::NOP;
    bool SwapOperands = false;
    switch (InnerExpr->Op) {
    case NdOp::INT_EQUAL:
      NegOp = NdOp::INT_NOTEQUAL;
      break;
    case NdOp::INT_NOTEQUAL:
      NegOp = NdOp::INT_EQUAL;
      break;
    case NdOp::INT_LESS:
      NegOp = NdOp::INT_LESSEQUAL;
      SwapOperands = true;
      break;
    case NdOp::INT_SLESS:
      NegOp = NdOp::INT_SLESSEQUAL;
      SwapOperands = true;
      break;
    case NdOp::INT_LESSEQUAL:
      NegOp = NdOp::INT_LESS;
      SwapOperands = true;
      break;
    case NdOp::INT_SLESSEQUAL:
      NegOp = NdOp::INT_SLESS;
      SwapOperands = true;
      break;
    default:
      break;
    }
    if (NegOp != NdOp::NOP) {
      // The negated comparison replaces `!(cmp)` and only `!(cmp)`.  Every
      // other rewrite in this file holds wherever its node appears, so writing
      // through the pointer is safe for them; this one holds only under this
      // parent.  HighIR is a graph, so the comparison may well have another
      // parent that has no negation to cancel it, and turning `<` into `<=` in
      // place would silently flip that other reading of the same test.
      auto Negated = std::make_shared<HighExpr>(*InnerExpr);
      Negated->Op = NegOp;
      if (SwapOperands)
        std::swap(Negated->Operands[0], Negated->Operands[1]);
      E = std::move(Negated);
    }
  }

  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
    return;

  // Sub-piece identity elimination.  SUBBYTES(x, 0) that extracts x's full
  // width, or the low bytes of a zero/sign-extended value back to its
  // original width, is a no-op.  Such chains arise from modelling x86-64
  // sub-register writes (an EAX write zero-extends into RAX, then the low
  // half is re-read); leaving them in place would block store-forwarding and
  // the RDTSC hi/lo collapse.
  if (E->Op == NdOp::SUBBYTES && E->Operands[1]->Kind == ExprKind::Const &&
      E->Operands[1]->ConstVal == 0 && E->Type) {
    const auto &Val = E->Operands[0];
    if (Val->Type && Val->Type->Size == E->Type->Size) {
      E = Val;
      return;
    }
    if (Val->Kind == ExprKind::UnaryOp &&
        (Val->Op == NdOp::INT_ZEXT || Val->Op == NdOp::INT_SEXT) &&
        !Val->Operands.empty() && Val->Operands[0]->Type &&
        Val->Operands[0]->Type->Size == E->Type->Size) {
      E = Val->Operands[0];
      return;
    }
  }

  if (E->Operands[1]->Kind == ExprKind::Const &&
      E->Operands[1]->ConstVal == 0) {
    if (E->Op == NdOp::INT_LESS) {
      E = HighExpr::makeConst(0, 1);
      return;
    }
  }

  if ((E->Op == NdOp::INT_EQUAL || E->Op == NdOp::INT_NOTEQUAL) &&
      E->Operands[0]->Kind == ExprKind::BinOp &&
      E->Operands[0]->Op == NdOp::INT_SUB &&
      E->Operands[0]->Operands.size() == 2 &&
      E->Operands[1]->Kind == ExprKind::Const &&
      E->Operands[1]->ConstVal == 0) {
    auto SubExpr = E->Operands[0];
    E->Operands[0] = SubExpr->Operands[0];
    E->Operands[1] = SubExpr->Operands[1];
  }

  if (E->Op == NdOp::INT_ADD && E->Operands[1]->Kind == ExprKind::Const) {
    int64_t SignedVal = static_cast<int64_t>(E->Operands[1]->ConstVal);
    if (SignedVal < 0) {
      E->Op = NdOp::INT_SUB;
      E->Operands[1] = HighExpr::makeConst(
          static_cast<uint64_t>(-SignedVal),
          E->Operands[1]->Type ? E->Operands[1]->Type->Size : 8);
    }
  }

  if (E->Op == NdOp::INT_SUB && E->Operands[1]->Kind == ExprKind::Const &&
      E->Operands[1]->ConstVal == 0)
    E = E->Operands[0];
  else if (E->Op == NdOp::INT_ADD && E->Operands[0]->Kind == ExprKind::Const &&
           E->Operands[0]->ConstVal == 0)
    E = E->Operands[1];
  else if (E->Op == NdOp::INT_MULT && E->Operands[1]->Kind == ExprKind::Const &&
           E->Operands[1]->ConstVal == 1)
    E = E->Operands[0];
}

void simplifyAllExprs(std::vector<HighStmt> &Stmts) {
  std::unordered_set<const HighExpr *> Seen;
  walkStmts(Stmts, [&](HighStmt &S) {
    forEachRhsExpr(S, [&](ExprPtr &EP) { simplifyExprRecursive(EP, Seen); });
  });
}

} // namespace neverd

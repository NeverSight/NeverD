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

// Register reconstruction often concatenates adjacent byte slices of the
// same scalar. Fold only direct local reads: two identical-looking loads or
// calls need not observe the same value. Keep the result's integer type when
// replacing a full-width reconstruction, including its unsigned interpretation.
static ExprPtr joinLocalSlices(const ExprPtr &E) {
  if (E->Kind != ExprKind::BinOp || E->Op != NdOp::CONCAT ||
      E->Operands.size() != 2 || !E->Type || E->Type->Kind != NdTypeKind::Int ||
      !E->Type->Size || E->Type->Size > 8 || !E->IntrinsicOutputs.empty() ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return nullptr;
  auto IsSlice = [](const ExprPtr &S) {
    if (!S || S->Kind != ExprKind::BinOp || S->Op != NdOp::SUBBYTES ||
        S->Operands.size() != 2 || !S->Type ||
        S->Type->Kind != NdTypeKind::Int || !S->Type->Size ||
        !S->IntrinsicOutputs.empty() ||
        S->MemoryOrdering != NdMemoryOrdering::None ||
        S->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    const auto &Base = S->Operands[0];
    const auto &Offset = S->Operands[1];
    return Base && Base->Kind == ExprKind::Var && Base->Operands.empty() &&
           Base->IntrinsicOutputs.empty() && Base->Type &&
           Base->Type->Kind == NdTypeKind::Int &&
           Base->Type->Size == Base->Var.Size && Base->Type->Size <= 8 &&
           Base->MemoryOrdering == NdMemoryOrdering::None &&
           Base->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           Offset && Offset->Kind == ExprKind::Const &&
           Offset->Operands.empty() && Offset->Type &&
           Offset->Type->Kind == NdTypeKind::Int &&
           Offset->ConstVal <= Base->Type->Size &&
           S->Type->Size <= Base->Type->Size - Offset->ConstVal;
  };
  const auto &High = E->Operands[0], &Low = E->Operands[1];
  if (!IsSlice(High) || !IsSlice(Low) ||
      !High->Operands[0]->structuralEq(*Low->Operands[0]) ||
      High->Operands[0]->Type->IsSigned != Low->Operands[0]->Type->IsSigned ||
      High->Operands[1]->ConstVal !=
          Low->Operands[1]->ConstVal + Low->Type->Size ||
      E->Type->Size != High->Type->Size + Low->Type->Size)
    return nullptr;
  const auto &Base = Low->Operands[0];
  auto Result = std::make_shared<HighExpr>();
  Result->Type = E->Type;
  if (Low->Operands[1]->ConstVal == 0 && E->Type->Size == Base->Type->Size) {
    if (E->Type->IsSigned == Base->Type->IsSigned)
      return Base;
    Result->Kind = ExprKind::Cast;
    Result->CastTo = E->Type;
    Result->Operands = {Base};
  } else {
    Result->Kind = ExprKind::BinOp;
    Result->Op = NdOp::SUBBYTES;
    Result->Operands = {Base, Low->Operands[1]};
  }
  return Result;
}

/// Nothing in \p E can be observed except its value: no call, memory access
/// or atomic.  Only such a subexpression may be dropped as irrelevant.
static bool isEffectFree(const HighExpr &E, unsigned Depth = 0) {
  if (Depth > 64)
    return false;
  switch (E.Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Undef:
  case ExprKind::Phi:
    return true;
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
    if (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
        E.Op == NdOp::ATOMIC_CMPXCHG)
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !isEffectFree(*Op, Depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

static bool isZeroConst(const ExprPtr &E) {
  return E && E->Kind == ExprKind::Const && E->ConstVal == 0;
}

/// Rewrite \p X knowing only its low \p Bytes bytes are read (the operand
/// of a low SUBBYTES).  A partial x86 register write keeps the stale upper
/// bytes as `(old & ~0xFF) | new`; a byte read of that never sees `old`.
/// Operand widths are unchanged; only subexpressions that cannot affect the
/// demanded bytes and have no effect are removed.
static ExprPtr demandLowBytes(const ExprPtr &X, unsigned Bytes) {
  if (!X || !X->Type || Bytes == 0 || Bytes >= 8 || X->Type->Size > 8 ||
      X->Kind != ExprKind::BinOp || X->Operands.size() != 2)
    return X;
  const uint64_t Mask = (uint64_t(1) << (Bytes * 8)) - 1;
  const ExprPtr &L = X->Operands[0];
  const ExprPtr &R = X->Operands[1];
  auto Zero = [&] { return HighExpr::makeConst(0, X->Type->Size); };
  switch (X->Op) {
  case NdOp::INT_AND:
    for (const auto &[C, Other] : {std::pair{R, L}, std::pair{L, R}})
      if (C && C->Kind == ExprKind::Const) {
        if ((C->ConstVal & Mask) == 0 && Other && isEffectFree(*Other))
          return Zero();
        if ((C->ConstVal & Mask) == Mask)
          return demandLowBytes(Other, Bytes);
      }
    return X;
  case NdOp::INT_OR:
  case NdOp::INT_XOR: {
    ExprPtr NL = demandLowBytes(L, Bytes);
    ExprPtr NR = demandLowBytes(R, Bytes);
    if (isZeroConst(NL))
      return NR;
    if (isZeroConst(NR))
      return NL;
    if (NL == L && NR == R)
      return X;
    auto Copy = std::make_shared<HighExpr>(*X);
    Copy->Operands = {NL, NR};
    return Copy;
  }
  default:
    return X;
  }
}

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

  if (auto Joined = joinLocalSlices(E)) {
    E = std::move(Joined);
    return;
  }

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

  if (E->Kind == ExprKind::Cast && E->Type &&
      E->Type->Kind == NdTypeKind::Int && E->Operands.size() == 1 &&
      E->Operands[0] && E->Operands[0]->Type &&
      E->Operands[0]->Type->Size > E->Type->Size) {
    if (ExprPtr Demanded = demandLowBytes(E->Operands[0], E->Type->Size);
        Demanded != E->Operands[0]) {
      auto Copy = std::make_shared<HighExpr>(*E);
      Copy->Operands[0] = std::move(Demanded);
      E = std::move(Copy);
    }
    return;
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
    if (ExprPtr Demanded = demandLowBytes(E->Operands[0], E->Type->Size);
        Demanded != E->Operands[0]) {
      // Rewrite a copy: HighIR shares nodes, and another parent may read
      // the whole register value.
      auto Copy = std::make_shared<HighExpr>(*E);
      Copy->Operands[0] = std::move(Demanded);
      E = std::move(Copy);
    }
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
          uint64_t(0) - static_cast<uint64_t>(SignedVal),
          E->Operands[1]->Type ? E->Operands[1]->Type->Size : 8);
    }
  }

  // `and [mem], 0` lifts as a read of the old value masked to zero.
  if ((E->Op == NdOp::INT_AND || E->Op == NdOp::INT_MULT) && E->Type &&
      E->Type->Size <= 8)
    for (unsigned I = 0; I < 2; ++I)
      if (isZeroConst(E->Operands[I]) && E->Operands[1 - I] &&
          isEffectFree(*E->Operands[1 - I])) {
        E = HighExpr::makeConst(0, E->Type->Size);
        return;
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

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

// A narrow native result may leave the rest of its register undefined.  If a
// later mask observes only bits supplied by CONCAT's low operand, replace that
// partial-register reconstruction with an explicit zero extension.  This does
// not invent values for the unknown upper bytes: those bytes remain outside
// the mask.  Keep any high expression whose evaluation could be observable.
static bool discardMaskedConcatHigh(const ExprPtr &E) {
  if (!E || E->Kind != ExprKind::BinOp || E->Op != NdOp::INT_AND ||
      E->Operands.size() != 2 || !E->Type ||
      E->Type->Kind != NdTypeKind::Int || !E->Type->Size ||
      E->Type->Size > 8 || E->IntrinsicId != Intrinsic::None ||
      !E->IntrinsicOutputs.empty() ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;

  const auto &Mask = E->Operands[1];
  if (!Mask || Mask->Kind != ExprKind::Const || !Mask->Operands.empty() ||
      !Mask->Type || Mask->Type->Kind != NdTypeKind::Int ||
      !Mask->Type->Size || Mask->Type->Size > 8)
    return false;

  auto Joined = E->Operands[0];
  if (Joined && Joined->Kind == ExprKind::Cast &&
      Joined->Operands.size() == 1 && Joined->Type && Joined->CastTo &&
      Joined->Type->Kind == NdTypeKind::Int &&
      Joined->CastTo->Kind == NdTypeKind::Int &&
      Joined->Type->Size == E->Type->Size &&
      Joined->CastTo->Size == E->Type->Size &&
      Joined->IntrinsicId == Intrinsic::None &&
      Joined->IntrinsicOutputs.empty() &&
      Joined->MemoryOrdering == NdMemoryOrdering::None &&
      Joined->MemoryAddressSpace == NdMemoryAddressSpace::Default)
    Joined = Joined->Operands[0];
  else if (Joined && Joined->Kind == ExprKind::BinOp &&
           Joined->Op == NdOp::SUBBYTES && Joined->Operands.size() == 2 &&
           Joined->Type && Joined->Type->Kind == NdTypeKind::Int &&
           Joined->Type->Size == E->Type->Size && Joined->Operands[1] &&
           Joined->Operands[1]->Kind == ExprKind::Const &&
           Joined->Operands[1]->ConstVal == 0 &&
           Joined->IntrinsicId == Intrinsic::None &&
           Joined->IntrinsicOutputs.empty() &&
           Joined->MemoryOrdering == NdMemoryOrdering::None &&
           Joined->MemoryAddressSpace == NdMemoryAddressSpace::Default)
    Joined = Joined->Operands[0];

  if (!Joined || Joined->Kind != ExprKind::BinOp ||
      Joined->Op != NdOp::CONCAT || Joined->Operands.size() != 2 ||
      !Joined->Type || Joined->Type->Kind != NdTypeKind::Int ||
      !Joined->Type->Size || Joined->Type->Size > 8 ||
      Joined->Type->Size < E->Type->Size ||
      Joined->IntrinsicId != Intrinsic::None ||
      !Joined->IntrinsicOutputs.empty() ||
      Joined->MemoryOrdering != NdMemoryOrdering::None ||
      Joined->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;

  const auto &High = Joined->Operands[0], &Low = Joined->Operands[1];
  if (!High || !Low || !High->Type || !Low->Type ||
      High->Type->Kind != NdTypeKind::Int ||
      Low->Type->Kind != NdTypeKind::Int || !High->Type->Size ||
      !Low->Type->Size || Low->Type->Size >= E->Type->Size ||
      High->Type->Size + Low->Type->Size != Joined->Type->Size)
    return false;

  const unsigned LowBits = Low->Type->Size * 8;
  const uint64_t LowMask = LowBits == 64
                               ? UINT64_MAX
                               : (uint64_t{1} << LowBits) - uint64_t{1};
  size_t Budget = 128;
  if ((Mask->ConstVal & ~LowMask) != 0 ||
      !discardableIntegerValue(High, Budget))
    return false;

  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Low);
  Extended->Type = E->Type;
  E->Operands[0] = std::move(Extended);
  return true;
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

  // Re-reading the low part of a partially defined register does not read
  // its upper padding. Keep the original CONCAT for any other, wider uses,
  // and never discard a call, memory access, or potentially trapping value.
  const bool IntegerCast = E->Kind == ExprKind::Cast &&
                           E->Operands.size() == 1 && E->CastTo &&
                           E->CastTo->Kind == NdTypeKind::Int && E->Type &&
                           E->CastTo->Size == E->Type->Size &&
                           E->CastTo->IsSigned == E->Type->IsSigned;
  const bool LowSlice = E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
                        E->Operands.size() == 2 && E->Operands[1] &&
                        E->Operands[1]->Kind == ExprKind::Const &&
                        E->Operands[1]->ConstVal == 0;
  if ((IntegerCast || LowSlice) && E->Type &&
      E->Type->Kind == NdTypeKind::Int && E->Type->Size &&
      E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
      E->MemoryOrdering == NdMemoryOrdering::None &&
      E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
    const auto &Joined = E->Operands[0];
    if (Joined && Joined->Kind == ExprKind::BinOp &&
        Joined->Op == NdOp::CONCAT && Joined->Operands.size() == 2 &&
        Joined->Type && Joined->Type->Kind == NdTypeKind::Int &&
        Joined->Type->Size <= 16 && Joined->IntrinsicId == Intrinsic::None &&
        Joined->IntrinsicOutputs.empty() &&
        Joined->MemoryOrdering == NdMemoryOrdering::None &&
        Joined->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto &High = Joined->Operands[0], &Low = Joined->Operands[1];
      size_t Budget = 128;
      if (High && Low && High->Type && Low->Type &&
          High->Type->Kind == NdTypeKind::Int &&
          Low->Type->Kind == NdTypeKind::Int && High->Type->Size &&
          Low->Type->Size == E->Type->Size &&
          High->Type->Size + Low->Type->Size == Joined->Type->Size &&
          discardableIntegerValue(High, Budget)) {
        if (Low->Type->IsSigned == E->Type->IsSigned) {
          E = Low;
        } else {
          auto Cast = std::make_shared<HighExpr>();
          Cast->Kind = ExprKind::Cast;
          Cast->Type = Cast->CastTo = E->Type;
          Cast->Operands = {Low};
          E = std::move(Cast);
        }
        return;
      }
    }
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

  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
    return;

  discardMaskedConcatHigh(E);

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
          uint64_t(0) - static_cast<uint64_t>(SignedVal),
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

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
///   - Order/equality merging  (c <= x && x != c → c < x)
///   - Negative constant folding  (x + (-N) → x - N)
///   - Sub-piece identity elimination  (SUBBYTES((u64)x, 0) → x)
///   - Truncation narrowing  ((u32)((u64)a + (u64)b) → a + b)
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/ir/high/MedToHigh.h"

#include <optional>
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
static bool isEffectFree(const ExprPtr &E) {
  size_t Budget = 128;
  return discardableIntegerValue(E, Budget);
}

static bool isZeroConst(const ExprPtr &E) {
  return E && E->Kind == ExprKind::Const && E->ConstVal == 0;
}

/// A constant mask observes only these bits. In particular, a masked status
/// word must not evaluate unknown incoming bits that the machine never reads.
/// This is local bit projection, independent of optional symbolic rewriting.
/// Copy changed paths because another use of a shared node may read more bits.
static ExprPtr demandMaskedBits(const ExprPtr &E, uint64_t Demand,
                                size_t &Budget) {
  if (!E || !Budget || !E->Type || E->Type->Kind != NdTypeKind::Int ||
      !E->Type->Size || E->Type->Size > 8 ||
      E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return E;
  --Budget;
  const auto Mask = [](unsigned Bytes) {
    return Bytes == 8 ? UINT64_MAX : (uint64_t{1} << (Bytes * 8)) - 1;
  };
  Demand &= Mask(E->Type->Size);
  if (!Demand) {
    size_t DropBudget = 128;
    if (!discardableIntegerValue(E, DropBudget))
      return E;
    auto Zero = HighExpr::makeConst(0, E->Type->Size,
                                    ConstantAddressProvenance::Scalar);
    Zero->Type = E->Type;
    return isZeroConst(E) ? E : Zero;
  }
  if (E->Kind == ExprKind::Const) {
    if (!E->Operands.empty())
      return E;
    if ((E->ConstVal & Demand) == E->ConstVal)
      return E;
    auto Copy = std::make_shared<HighExpr>(*E);
    Copy->ConstVal &= Demand;
    return Copy;
  }
  const bool Cast = E->Kind == ExprKind::Cast && E->CastTo &&
                    E->CastTo->Kind == NdTypeKind::Int &&
                    E->CastTo->Size == E->Type->Size &&
                    E->CastTo->IsSigned == E->Type->IsSigned;
  const bool Extend = E->Kind == ExprKind::UnaryOp &&
                      (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT);
  if ((Cast || Extend) && E->Operands.size() == 1) {
    const auto &In = E->Operands[0];
    if (!In || !In->Type || In->Type->Kind != NdTypeKind::Int ||
        !In->Type->Size || In->Type->Size > 8 ||
        (Extend && In->Type->Size > E->Type->Size))
      return E;
    const uint64_t InputMask = Mask(In->Type->Size);
    uint64_t InputDemand = Demand & InputMask;
    const bool Signed = Cast ? In->Type->IsSigned : E->Op == NdOp::INT_SEXT;
    if (Signed && (Demand & ~InputMask))
      InputDemand |= uint64_t{1} << (In->Type->Size * 8 - 1);
    auto Projected = demandMaskedBits(In, InputDemand, Budget);
    if (Projected == In)
      return E;
    auto Copy = std::make_shared<HighExpr>(*E);
    Copy->Operands = {std::move(Projected)};
    return Copy;
  }
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::INT_AND && E->Op != NdOp::INT_OR &&
       E->Op != NdOp::INT_XOR))
    return E;
  const auto &L = E->Operands[0], &R = E->Operands[1];
  if (!L || !R || !L->Type || !R->Type || L->Type->Kind != NdTypeKind::Int ||
      R->Type->Kind != NdTypeKind::Int || L->Type->Size != E->Type->Size ||
      R->Type->Size != E->Type->Size)
    return E;
  auto ChildDemand = [&](const ExprPtr &Other) {
    // Two constants must not each give the other responsibility for a set
    // OR bit and then both erase it.
    if (Other->Kind != ExprKind::Const ||
        (L->Kind == ExprKind::Const && R->Kind == ExprKind::Const))
      return Demand;
    if (E->Op == NdOp::INT_AND)
      return Demand & Other->ConstVal;
    if (E->Op == NdOp::INT_OR)
      return Demand & ~Other->ConstVal;
    return Demand;
  };
  auto Left = demandMaskedBits(L, ChildDemand(R), Budget);
  auto Right = demandMaskedBits(R, ChildDemand(L), Budget);
  if (Left == L && Right == R)
    return E;
  auto Copy = std::make_shared<HighExpr>(*E);
  Copy->Operands = {std::move(Left), std::move(Right)};
  return Copy;
}

/// Rewrite \p X knowing only its low \p Bytes bytes are read (the operand
/// of a low SUBBYTES).  A partial x86 register write keeps the stale upper
/// bytes as `(old & ~0xFF) | new`; a byte read of that never sees `old`, and
/// the low bytes of a sum, difference, product or left shift never see the
/// operands' upper bytes.  Operand widths are unchanged; only subexpressions
/// that cannot affect the demanded bytes and have no effect are removed.
static ExprPtr demandLowBytes(const ExprPtr &X, unsigned Bytes) {
  if (!X || !X->Type || Bytes == 0 || Bytes >= 8 || X->Type->Size > 8)
    return X;
  auto WithOperands = [&](std::vector<ExprPtr> Operands) {
    auto Copy = std::make_shared<HighExpr>(*X);
    Copy->Operands = std::move(Operands);
    return Copy;
  };
  const bool Integer = X->Type->Kind == NdTypeKind::Int;
  // An extension's low bytes are its operand's.
  if (Integer && X->Kind == ExprKind::UnaryOp &&
      (X->Op == NdOp::INT_ZEXT || X->Op == NdOp::INT_SEXT) &&
      X->Operands.size() == 1 && X->Operands[0] && X->Operands[0]->Type &&
      X->Operands[0]->Type->Size >= Bytes) {
    ExprPtr In = demandLowBytes(X->Operands[0], Bytes);
    return In == X->Operands[0] ? X : WithOperands({In});
  }
  if (X->Kind != ExprKind::BinOp || X->Operands.size() != 2)
    return X;
  const uint64_t Mask = (uint64_t(1) << (Bytes * 8)) - 1;
  const ExprPtr &L = X->Operands[0];
  const ExprPtr &R = X->Operands[1];
  auto Zero = [&] { return HighExpr::makeConst(0, X->Type->Size); };
  switch (X->Op) {
  case NdOp::INT_AND: {
    for (const auto &[C, Other] : {std::pair{R, L}, std::pair{L, R}})
      if (C && C->Kind == ExprKind::Const) {
        if ((C->ConstVal & Mask) == 0 && Other && isEffectFree(Other))
          return Zero();
        if ((C->ConstVal & Mask) == Mask)
          return demandLowBytes(Other, Bytes);
      }
    ExprPtr NL = demandLowBytes(L, Bytes);
    ExprPtr NR = demandLowBytes(R, Bytes);
    if (NL == L && NR == R)
      return X;
    return WithOperands({NL, NR});
  }
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
    return WithOperands({NL, NR});
  }
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT: {
    if (!Integer)
      return X;
    ExprPtr NL = demandLowBytes(L, Bytes);
    ExprPtr NR = demandLowBytes(R, Bytes);
    if (NL == L && NR == R)
      return X;
    return WithOperands({NL, NR});
  }
  case NdOp::INT_LEFT: {
    if (!Integer || !R || R->Kind != ExprKind::Const)
      return X;
    // Every demanded byte comes from below the shift amount: all zero.
    if (R->ConstVal >= uint64_t{Bytes} * 8 && L && isEffectFree(L))
      return Zero();
    ExprPtr NL = demandLowBytes(L, Bytes);
    return NL == L ? X : WithOperands({NL, R});
  }
  case NdOp::CONCAT: {
    // Only the low operand supplies the demanded bytes.  A high operand that
    // cannot be observed is not read at all, even an undefined one.
    if (!Integer || !L || !R || !R->Type || R->Type->Size < Bytes)
      return X;
    ExprPtr NR = demandLowBytes(R, Bytes);
    size_t Budget = 128;
    if (discardableIntegerValue(L, Budget)) {
      auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, NR);
      Extended->Type = X->Type;
      return Extended;
    }
    return NR == R ? X : WithOperands({L, NR});
  }
  default:
    return X;
  }
}

/// `x OP y` truncated to \p Ty, computed at that width instead.  The low
/// bytes of a sum, difference, product or bitwise combination read only the
/// low bytes of the operands, so an extension from exactly that width, or a
/// constant, supplies them directly -- the same operation a native narrow
/// instruction lifts to.
static ExprPtr narrowTruncatedOp(const ExprPtr &Val, const TypeRef &Ty) {
  if (!Val || !Ty || Ty->Kind != NdTypeKind::Int || !Ty->Size ||
      Val->Kind != ExprKind::BinOp || Val->Operands.size() != 2 || !Val->Type ||
      Val->Type->Kind != NdTypeKind::Int || Val->Type->Size <= Ty->Size ||
      Val->Type->Size > 8 || Val->IntrinsicId != Intrinsic::None ||
      !Val->IntrinsicOutputs.empty() ||
      Val->MemoryOrdering != NdMemoryOrdering::None ||
      Val->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return nullptr;
  switch (Val->Op) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
    break;
  default:
    return nullptr;
  }
  const uint64_t Mask = (uint64_t(1) << (Ty->Size * 8)) - 1;
  auto Narrow = [&](const ExprPtr &Operand) -> ExprPtr {
    if (!Operand)
      return nullptr;
    if (Operand->Kind == ExprKind::Const && Operand->Operands.empty() &&
        (Operand->ConstProvenance == ConstantAddressProvenance::Unknown ||
         Operand->ConstProvenance == ConstantAddressProvenance::Scalar))
      return HighExpr::makeConst(Operand->ConstVal & Mask, Ty->Size,
                                 Operand->ConstProvenance);
    if (Operand->Kind == ExprKind::UnaryOp &&
        (Operand->Op == NdOp::INT_ZEXT || Operand->Op == NdOp::INT_SEXT) &&
        Operand->Operands.size() == 1 && Operand->Operands[0] &&
        Operand->Operands[0]->Type &&
        Operand->Operands[0]->Type->Kind == NdTypeKind::Int &&
        Operand->Operands[0]->Type->Size == Ty->Size)
      return Operand->Operands[0];
    return nullptr;
  };
  ExprPtr NL = Narrow(Val->Operands[0]);
  ExprPtr NR = Narrow(Val->Operands[1]);
  if (!NL || !NR)
    return nullptr;
  auto Result = std::make_shared<HighExpr>(*Val);
  Result->Operands = {std::move(NL), std::move(NR)};
  Result->Type = Ty;
  return Result;
}

// A narrow native result may leave the rest of its register undefined.  If a
// later mask observes only bits supplied by CONCAT's low operand, replace that
// partial-register reconstruction with an explicit zero extension.  This does
// not invent values for the unknown upper bytes: those bytes remain outside
// the mask.  Keep any high expression whose evaluation could be observable.
static bool discardMaskedConcatHigh(const ExprPtr &E) {
  if (!E || E->Kind != ExprKind::BinOp || E->Op != NdOp::INT_AND ||
      E->Operands.size() != 2 || !E->Type || E->Type->Kind != NdTypeKind::Int ||
      !E->Type->Size || E->Type->Size > 8 ||
      E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;

  const auto &Mask = E->Operands[1];
  if (!Mask || Mask->Kind != ExprKind::Const || !Mask->Operands.empty() ||
      !Mask->Type || Mask->Type->Kind != NdTypeKind::Int || !Mask->Type->Size ||
      Mask->Type->Size > 8)
    return false;

  auto Joined = E->Operands[0];
  // A zero-bit logical shift does not change the bits observed by the mask.
  // AArch64 TBZ lowering can place this shift between a narrow call result
  // and its bit-zero test, even when the register's upper bytes are unknown.
  if (Joined && Joined->Kind == ExprKind::BinOp &&
      Joined->Op == NdOp::INT_RIGHT && Joined->Operands.size() == 2 &&
      Joined->Type && Joined->Type->Kind == NdTypeKind::Int &&
      Joined->Operands[0] && Joined->Operands[0]->Type &&
      Joined->Type->Size == Joined->Operands[0]->Type->Size &&
      Joined->Operands[1] && Joined->Operands[1]->Kind == ExprKind::Const &&
      Joined->Operands[1]->ConstVal == 0 &&
      Joined->IntrinsicId == Intrinsic::None &&
      Joined->IntrinsicOutputs.empty() &&
      Joined->MemoryOrdering == NdMemoryOrdering::None &&
      Joined->MemoryAddressSpace == NdMemoryAddressSpace::Default)
    Joined = Joined->Operands[0];
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
      !Low->Type->Size || Low->Type->Size > E->Type->Size ||
      High->Type->Size + Low->Type->Size != Joined->Type->Size)
    return false;

  const unsigned LowBits = Low->Type->Size * 8;
  const uint64_t LowMask =
      LowBits == 64 ? UINT64_MAX : (uint64_t{1} << LowBits) - uint64_t{1};
  size_t Budget = 128;
  if ((Mask->ConstVal & ~LowMask) != 0 ||
      !discardableIntegerValue(High, Budget))
    return false;

  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Low);
  Extended->Type = E->Type;
  E->Operands[0] = std::move(Extended);
  return true;
}

/// Whether \p A and \p B are one integer value: the same expression,
/// possibly behind same-width integer views (a signedness cast or the full
/// low slice), as flag tests of one compare read it.
static bool sameIntegerValue(const ExprPtr &A, const ExprPtr &B) {
  auto Peel = [](ExprPtr E) {
    for (unsigned Depth = 0; E && Depth < 4; ++Depth) {
      const bool View =
          E->Operands.size() >= 1 && E->Operands[0] && E->Type &&
          E->Operands[0]->Type &&
          E->Operands[0]->Type->Kind == NdTypeKind::Int &&
          E->Type->Kind == NdTypeKind::Int &&
          E->Operands[0]->Type->Size == E->Type->Size &&
          ((E->Kind == ExprKind::Cast && E->Operands.size() == 1) ||
           (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
            E->Operands.size() == 2 && E->Operands[1] &&
            E->Operands[1]->Kind == ExprKind::Const &&
            E->Operands[1]->ConstVal == 0));
      if (!View)
        break;
      E = E->Operands[0];
    }
    return E;
  };
  if (!A || !B || !A->Type || !B->Type || A->Type->Size != B->Type->Size)
    return false;
  const ExprPtr PA = Peel(A), PB = Peel(B);
  return PA && PB && PA->structuralEq(*PB);
}

/// `c <= x && x != c` is `c < x`, and `x < c || x == c` is `x <= c`: the
/// unsigned or signed above / below-or-equal tests of one compare, which x86
/// spells as two flag tests (`ja` reads !CF && !ZF).
static ExprPtr mergeOrderAndEquality(const ExprPtr &E) {
  if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::BOOL_AND && E->Op != NdOp::BOOL_OR))
    return nullptr;
  const bool And = E->Op == NdOp::BOOL_AND;
  for (unsigned I = 0; I < 2; ++I) {
    const ExprPtr &Order = E->Operands[I], &Equality = E->Operands[1 - I];
    if (!Order || !Equality || Order->Kind != ExprKind::BinOp ||
        Equality->Kind != ExprKind::BinOp || Order->Operands.size() != 2 ||
        Equality->Operands.size() != 2 ||
        Equality->Op != (And ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL))
      continue;
    NdOp Merged = NdOp::NOP;
    if (And && Order->Op == NdOp::INT_LESSEQUAL)
      Merged = NdOp::INT_LESS;
    else if (And && Order->Op == NdOp::INT_SLESSEQUAL)
      Merged = NdOp::INT_SLESS;
    else if (!And && Order->Op == NdOp::INT_LESS)
      Merged = NdOp::INT_LESSEQUAL;
    else if (!And && Order->Op == NdOp::INT_SLESS)
      Merged = NdOp::INT_SLESSEQUAL;
    if (Merged == NdOp::NOP)
      continue;
    const ExprPtr &L = Order->Operands[0], &R = Order->Operands[1];
    const ExprPtr &P = Equality->Operands[0], &Q = Equality->Operands[1];
    if (!(sameIntegerValue(L, P) && sameIntegerValue(R, Q)) &&
        !(sameIntegerValue(L, Q) && sameIntegerValue(R, P)))
      continue;
    auto Result = std::make_shared<HighExpr>(*Order);
    Result->Op = Merged;
    return Result;
  }
  return nullptr;
}

/// `(a - b < 0) != sborrow(a, b)` is `a < b`, and `==` is `b <= a`: the
/// sign and signed-overflow flags of one subtraction, as AArch64's `b.lt`
/// (N != V) and x86's `jl` (SF != OF) test them.
static ExprPtr signedOrderFromFlags(const ExprPtr &E) {
  if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::INT_NOTEQUAL && E->Op != NdOp::INT_EQUAL))
    return nullptr;
  for (unsigned I = 0; I < 2; ++I) {
    const ExprPtr &Sign = E->Operands[I], &Borrow = E->Operands[1 - I];
    if (!Sign || !Borrow || Sign->Kind != ExprKind::BinOp ||
        Sign->Op != NdOp::INT_SLESS || Sign->Operands.size() != 2 ||
        !isZeroConst(Sign->Operands[1]) || Borrow->Kind != ExprKind::BinOp ||
        Borrow->Op != NdOp::INT_SBOR || Borrow->Operands.size() != 2)
      continue;
    const ExprPtr &Difference = Sign->Operands[0];
    if (!Difference || Difference->Kind != ExprKind::BinOp ||
        Difference->Op != NdOp::INT_SUB || Difference->Operands.size() != 2 ||
        !Difference->Type || !Borrow->Operands[0] ||
        !Borrow->Operands[0]->Type ||
        Difference->Type->Size != Borrow->Operands[0]->Type->Size ||
        !sameIntegerValue(Difference->Operands[0], Borrow->Operands[0]) ||
        !sameIntegerValue(Difference->Operands[1], Borrow->Operands[1]))
      continue;
    const bool Less = E->Op == NdOp::INT_NOTEQUAL;
    auto Order =
        HighExpr::makeBinop(Less ? NdOp::INT_SLESS : NdOp::INT_SLESSEQUAL,
                            Less ? Borrow->Operands[0] : Borrow->Operands[1],
                            Less ? Borrow->Operands[1] : Borrow->Operands[0]);
    Order->Type = E->Type;
    return Order;
  }
  return nullptr;
}

static void simplifyExprRecursive(ExprPtr &E, HighExprSet &Seen,
                                  const HighExprSet &OrderedMemory,
                                  bool &Changed) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  for (auto &Op : E->Operands)
    simplifyExprRecursive(Op, Seen, OrderedMemory, Changed);

  // Atomic accesses are observable even when the surrounding value appears
  // algebraically redundant.  Keep the containing expression intact so a
  // simplification cannot discard, duplicate, or move the access.
  if (OrderedMemory.count(E.get()))
    return;

  auto Replace = [&](ExprPtr Value) {
    Changed |= E != Value;
    E = std::move(Value);
  };

  if (auto Joined = joinLocalSlices(E)) {
    Replace(std::move(Joined));
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
          Replace(Low);
        } else {
          auto Cast = std::make_shared<HighExpr>();
          Cast->Kind = ExprKind::Cast;
          Cast->Type = Cast->CastTo = E->Type;
          Cast->Operands = {Low};
          Replace(std::move(Cast));
        }
        return;
      }
    }
  }

  // A slice within one part of a join reads that part: a lane of a vector
  // the expression has just assembled.  Printed whole, the join repeats at
  // every lane read, and again at every level that slices it.  The other
  // part is not evaluated, so it must be a value nothing would miss.
  for (bool Narrowed = true; Narrowed;) {
    Narrowed = false;
    if (E->Kind != ExprKind::BinOp || E->Op != NdOp::SUBBYTES ||
        E->Operands.size() != 2 || !E->Operands[0] || !E->Operands[1] ||
        E->Operands[1]->Kind != ExprKind::Const || !E->Type ||
        E->Type->Kind != NdTypeKind::Int || !E->Type->Size ||
        E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      break;
    const auto &Joined = E->Operands[0];
    if (Joined->Kind != ExprKind::BinOp || Joined->Op != NdOp::CONCAT ||
        Joined->Operands.size() != 2 || !Joined->Type ||
        Joined->Type->Kind != NdTypeKind::Int ||
        Joined->IntrinsicId != Intrinsic::None ||
        !Joined->IntrinsicOutputs.empty() ||
        Joined->MemoryOrdering != NdMemoryOrdering::None ||
        Joined->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      break;
    const ExprPtr High = Joined->Operands[0], Low = Joined->Operands[1];
    if (!High || !Low || !High->Type || !Low->Type ||
        High->Type->Kind != NdTypeKind::Int ||
        Low->Type->Kind != NdTypeKind::Int ||
        High->Type->Size + Low->Type->Size != Joined->Type->Size)
      break;
    const uint64_t Offset = E->Operands[1]->ConstVal;
    const uint64_t Bytes = E->Type->Size, LowBytes = Low->Type->Size;
    size_t Budget = 4096;
    ExprPtr Part;
    uint64_t PartOffset = 0;
    if (Offset + Bytes <= LowBytes && harmlessIntegerValue(High, Budget)) {
      Part = Low;
      PartOffset = Offset;
    } else if (Offset >= LowBytes &&
               Offset + Bytes <= LowBytes + High->Type->Size &&
               harmlessIntegerValue(Low, Budget)) {
      Part = High;
      PartOffset = Offset - LowBytes;
    }
    if (!Part)
      break;
    if (PartOffset == 0 && Part->Type->Size == Bytes) {
      if (Part->Type->IsSigned == E->Type->IsSigned) {
        Replace(Part);
      } else {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = Cast->CastTo = E->Type;
        Cast->Operands = {Part};
        Replace(std::move(Cast));
      }
      return;
    }
    // Rewrite a copy: another parent may read the join itself.
    auto Slice = std::make_shared<HighExpr>(*E);
    Slice->Operands = {
        Part, HighExpr::makeConst(PartOffset, E->Operands[1]->Type
                                                  ? E->Operands[1]->Type->Size
                                                  : 4)};
    Replace(std::move(Slice));
    Narrowed = true;
  }

  if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
      !E->Operands.empty() && E->Operands[0]->Kind == ExprKind::UnaryOp &&
      E->Operands[0]->Op == NdOp::BOOL_NOT &&
      !E->Operands[0]->Operands.empty()) {
    Replace(E->Operands[0]->Operands[0]);
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
      Replace(std::move(Negated));
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
      Replace(std::move(Copy));
    }
    if (ExprPtr Narrowed = narrowTruncatedOp(E->Operands[0], E->Type))
      Replace(std::move(Narrowed));
    return;
  }

  // A selection on a constant reads one arm.
  if (E->Kind == ExprKind::BinOp && E->Op == NdOp::SELECT &&
      E->Operands.size() == 3 && E->Operands[0] &&
      E->Operands[0]->Kind == ExprKind::Const && E->Operands[1] &&
      E->Operands[2] && E->Type && E->Type->Kind == NdTypeKind::Int) {
    const bool Taken = E->Operands[0]->ConstVal != 0;
    const ExprPtr Kept = E->Operands[Taken ? 1 : 2];
    size_t Budget = 4096;
    if (Kept->Type && Kept->Type->Kind == NdTypeKind::Int &&
        Kept->Type->Size == E->Type->Size &&
        harmlessIntegerValue(E->Operands[Taken ? 2 : 1], Budget)) {
      if (Kept->Type->IsSigned == E->Type->IsSigned) {
        Replace(Kept);
      } else {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = Cast->CastTo = E->Type;
        Cast->Operands = {Kept};
        Replace(std::move(Cast));
      }
      return;
    }
  }

  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
    return;

  // Two constants compare to a constant, at their width.
  if (E->Operands[0] && E->Operands[1] &&
      E->Operands[0]->Kind == ExprKind::Const &&
      E->Operands[1]->Kind == ExprKind::Const && E->Operands[0]->Type &&
      E->Operands[1]->Type &&
      E->Operands[0]->Type->Size == E->Operands[1]->Type->Size &&
      E->Operands[0]->Type->Size >= 1 && E->Operands[0]->Type->Size <= 8 &&
      E->Type && E->Type->Kind == NdTypeKind::Int) {
    const unsigned Bits = E->Operands[0]->Type->Size * 8;
    const uint64_t Mask = Bits == 64 ? ~uint64_t{0} : (uint64_t{1} << Bits) - 1;
    const uint64_t A = E->Operands[0]->ConstVal & Mask;
    const uint64_t B = E->Operands[1]->ConstVal & Mask;
    auto Signed = [&](uint64_t V) {
      return Bits == 64 ? static_cast<int64_t>(V)
                        : static_cast<int64_t>(V << (64 - Bits)) >> (64 - Bits);
    };
    std::optional<bool> Result;
    switch (E->Op) {
    case NdOp::INT_EQUAL:
      Result = A == B;
      break;
    case NdOp::INT_NOTEQUAL:
      Result = A != B;
      break;
    case NdOp::INT_LESS:
      Result = A < B;
      break;
    case NdOp::INT_LESSEQUAL:
      Result = A <= B;
      break;
    case NdOp::INT_SLESS:
      Result = Signed(A) < Signed(B);
      break;
    case NdOp::INT_SLESSEQUAL:
      Result = Signed(A) <= Signed(B);
      break;
    default:
      break;
    }
    if (Result) {
      Replace(HighExpr::makeConst(*Result ? 1 : 0, E->Type->Size));
      return;
    }
  }

  if (ExprPtr Merged = mergeOrderAndEquality(E)) {
    Replace(std::move(Merged));
    return;
  }
  if (ExprPtr Order = signedOrderFromFlags(E)) {
    Replace(std::move(Order));
    return;
  }

  Changed |= discardMaskedConcatHigh(E);

  if (E->Op == NdOp::INT_AND && E->Type && E->Type->Kind == NdTypeKind::Int &&
      E->Type->Size && E->Type->Size <= 8 &&
      E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
      E->MemoryOrdering == NdMemoryOrdering::None &&
      E->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      E->Operands[0] && E->Operands[1])
    for (unsigned I = 0; I < 2; ++I)
      if (E->Operands[I]->Kind == ExprKind::Const && E->Operands[1 - I]->Type &&
          E->Operands[1 - I]->Type->Size == E->Type->Size) {
        size_t Budget = 64;
        auto Projected = demandMaskedBits(E->Operands[1 - I],
                                          E->Operands[I]->ConstVal, Budget);
        if (Projected != E->Operands[1 - I]) {
          auto Copy = std::make_shared<HighExpr>(*E);
          Copy->Operands[1 - I] = std::move(Projected);
          Replace(std::move(Copy));
        }
        break;
      }

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
      Replace(std::move(Copy));
    }
    if (ExprPtr Narrowed = narrowTruncatedOp(E->Operands[0], E->Type)) {
      Replace(std::move(Narrowed));
      return;
    }
    const auto &Val = E->Operands[0];
    if (Val->Type && Val->Type->Size == E->Type->Size) {
      Replace(Val);
      return;
    }
    if (Val->Kind == ExprKind::UnaryOp &&
        (Val->Op == NdOp::INT_ZEXT || Val->Op == NdOp::INT_SEXT) &&
        !Val->Operands.empty() && Val->Operands[0]->Type &&
        Val->Operands[0]->Type->Size == E->Type->Size) {
      Replace(Val->Operands[0]);
      return;
    }
  }

  if (E->Operands[1]->Kind == ExprKind::Const &&
      E->Operands[1]->ConstVal == 0) {
    if (E->Op == NdOp::INT_LESS) {
      Replace(HighExpr::makeConst(0, 1));
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
    Changed = true;
  }

  if (E->Op == NdOp::INT_ADD && E->Operands[1]->Kind == ExprKind::Const) {
    int64_t SignedVal = static_cast<int64_t>(E->Operands[1]->ConstVal);
    if (SignedVal < 0) {
      E->Op = NdOp::INT_SUB;
      Changed = true;
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
          isEffectFree(E->Operands[1 - I])) {
        Replace(HighExpr::makeConst(0, E->Type->Size));
        return;
      }

  if (E->Op == NdOp::INT_SUB && E->Operands[1]->Kind == ExprKind::Const &&
      E->Operands[1]->ConstVal == 0)
    Replace(E->Operands[0]);
  else if (E->Op == NdOp::INT_ADD && E->Operands[0]->Kind == ExprKind::Const &&
           E->Operands[0]->ConstVal == 0)
    Replace(E->Operands[1]);
  else if (E->Op == NdOp::INT_MULT && E->Operands[1]->Kind == ExprKind::Const &&
           E->Operands[1]->ConstVal == 1)
    Replace(E->Operands[0]);
  // A shift by zero, as AArch64's `tbz x, #0` tests bit 0 as `(x >> 0) & 1`.
  else if ((E->Op == NdOp::INT_LEFT || E->Op == NdOp::INT_RIGHT ||
            E->Op == NdOp::INT_ASHR) &&
           isZeroConst(E->Operands[1]) && E->Type && E->Operands[0]->Type &&
           E->Operands[0]->Type->Size == E->Type->Size)
    Replace(E->Operands[0]);
}

bool simplifyAllExprs(std::vector<HighStmt> &Stmts) {
  std::vector<ExprPtr> Roots;
  walkStmts(Stmts, [&](const HighStmt &S) {
    forEachRhsExpr(S, [&](const ExprPtr &EP) { Roots.push_back(EP); });
  });
  // These rewrites only replace pure expressions. No ordered node or its
  // ancestors can be removed, and new nodes contain only pure operands, so
  // one graph-wide effect analysis remains valid throughout this pass.
  const auto OrderedMemory = findOrderedMemoryAncestors(Roots);
  HighExprSet Seen;
  bool Changed = false;
  walkStmts(Stmts, [&](HighStmt &S) {
    forEachRhsExpr(S, [&](ExprPtr &EP) {
      simplifyExprRecursive(EP, Seen, OrderedMemory, Changed);
    });
  });
  return Changed;
}

} // namespace neverd

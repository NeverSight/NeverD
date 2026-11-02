//===- SymExprArith.cpp - Canonicalizing arithmetic builders --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the sum, product, division and remainder builders.
///
/// Division and remainder behaviour follows SMT-LIB QF_BV rather than any one
/// machine's: `bvudiv x 0` is all-ones, `bvsdiv x 0` is -1 or 1 by the sign of
/// x, and the remainders by zero return x.
///
//===----------------------------------------------------------------------===//

#include "SymExprMask.h"

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cassert>

namespace neverd::symbolic {

void SymContext::splitCoefficient(SymRef R, llvm::APInt &Coeff,
                                  SymRef &Base) const {
  uint32_t W = width(R);
  if (op(R) == SymOp::Mul) {
    llvm::ArrayRef<SymRef> Ops = operands(R);
    if (!Ops.empty() && isConst(Ops[0])) {
      Coeff = constValue(Ops[0]);
      if (Ops.size() == 2) {
        Base = Ops[1];
        return;
      }
      // A product of three or more factors keeps its non-constant tail as the
      // base.  Rebuilding it here would intern, which the caller is not
      // prepared for, so the tail is reconstructed by the caller instead.
      Base = SymRef();
      return;
    }
  }
  Coeff = llvm::APInt(W, 1);
  Base = R;
}

SymRef SymContext::mkAdd(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty() && "mkAdd needs at least one operand");
  uint32_t W = width(Ops[0]);

  llvm::APInt ConstTerm(W, 0);
  // Small sums need no separately allocated tree nodes. Sort the completed
  // entries by base index before rebuilding, preserving canonical order.
  struct Addend {
    llvm::APInt Coefficient;
    SymRef Original;
  };
  llvm::SmallMapVector<uint32_t, Addend, 8> Terms;

  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  llvm::SmallVector<SymRef, 8> Masked;
  bool CollectedMasks = false;
  for (;;) {
    while (!Work.empty()) {
      SymRef R = Work.pop_back_val();
      assert(width(R) == W && "mkAdd operands must share a width");

      SymOp Op = op(R);
      if (Op == SymOp::Add) {
        llvm::ArrayRef<SymRef> Sub = operands(R);
        Work.append(Sub.begin(), Sub.end());
        continue;
      }
      if (Op == SymOp::Const) {
        ConstTerm += constValue(R);
        continue;
      }
      if (!CollectedMasks && Op == SymOp::And && isConst(operand(R, 0))) {
        Masked.push_back(R);
        continue;
      }

      llvm::APInt Coeff(W, 1);
      SymRef Base;
      splitCoefficient(R, Coeff, Base);
      if (!Base.isValid()) {
        // Product with three or more factors: rebuild the non-constant tail.
        llvm::SmallVector<SymRef, 4> Tail(operands(R).begin() + 1,
                                          operands(R).end());
        Base = mkMul(Tail);
      }

      auto It = Terms.find(Base.index());
      if (It == Terms.end()) {
        Terms.try_emplace(Base.index(), Addend{Coeff, R});
      } else {
        It->second.Coefficient += Coeff;
        It->second.Original = {};
      }
    }
    if (Masked.empty())
      break;
    mergeMaskedOperands(Masked, /*RequireDisjoint=*/true);
    Work = std::move(Masked);
    Masked.clear();
    CollectedMasks = true;
  }

  llvm::SmallVector<SymRef, 8> Final;
  bool Recollect = false;
  if (!ConstTerm.isZero())
    Final.push_back(mkConst(ConstTerm));
  auto OrderedTerms = Terms.takeVector();
  llvm::sort(OrderedTerms, llvm::less_first());
  for (const auto &[BaseIdx, Entry] : OrderedTerms) {
    const auto &Coeff = Entry.Coefficient;
    if (Coeff.isZero())
      continue;
    SymRef Base(BaseIdx);
    // A unique addend already has exactly the collected coefficient. Reuse
    // it instead of rebuilding its unchanged, potentially very long product.
    SymRef Term = Entry.Original.isValid() ? Entry.Original
                  : Coeff.isOne()          ? Base
                                           : mkMul(mkConst(Coeff), Base);
    Recollect |= !Coeff.isOne() && op(Term) != SymOp::Mul;
    Final.push_back(Term);
  }

  if (Recollect) {
    // Materializing a coefficient can recover a masked shift slice, exposing
    // a constant or a base already present in the sum. Collect those once.
    // A recovered nonconstant slice retains its mask, so the new base cannot
    // be a bare LShr and this collection cannot expose another such slice.
    OrderedTerms.clear();
    ConstTerm = llvm::APInt(W, 0);
    for (SymRef R : Final) {
      if (isConst(R)) {
        ConstTerm += constValue(R);
        continue;
      }
      llvm::APInt Coeff(W, 1);
      SymRef Base;
      splitCoefficient(R, Coeff, Base);
      if (!Base.isValid()) {
        llvm::SmallVector<SymRef, 4> Tail(operands(R).begin() + 1,
                                          operands(R).end());
        Base = mkMul(Tail);
      }
      auto [It, Inserted] = Terms.try_emplace(Base.index(), Addend{Coeff, R});
      if (!Inserted) {
        It->second.Coefficient += Coeff;
        It->second.Original = {};
      }
    }
    Final.clear();
    if (!ConstTerm.isZero())
      Final.push_back(mkConst(ConstTerm));
    OrderedTerms = Terms.takeVector();
    llvm::sort(OrderedTerms, llvm::less_first());
    for (const auto &[BaseIdx, Entry] : OrderedTerms) {
      const auto &Coeff = Entry.Coefficient;
      if (!Coeff.isZero()) {
        SymRef Base(BaseIdx);
        Final.push_back(Entry.Original.isValid() ? Entry.Original
                        : Coeff.isOne()          ? Base
                                                 : mkMul(mkConst(Coeff), Base));
      }
    }
  }

  if (Final.empty())
    return mkZero(W);
  if (Final.size() == 1)
    return Final[0];
  return intern(SymOp::Add, W, Final, 0);
}

SymRef SymContext::mkSub(SymRef A, SymRef B) {
  assert(width(A) == width(B) && "subtraction operands must share a width");
  if (op(A) == SymOp::Add || op(B) == SymOp::Add) {
    // Cancel complete additive terms before constructing a negated sum.
    // Operands are owned copies because any builder may relocate the pool.
    llvm::SmallVector<SymRef, 8> Left, Right;
    if (op(A) == SymOp::Add)
      Left.append(operands(A).begin(), operands(A).end());
    else
      Left.push_back(A);
    if (op(B) == SymOp::Add)
      Right.append(operands(B).begin(), operands(B).end());
    else
      Right.push_back(B);
    std::sort(Left.begin(), Left.end());
    std::sort(Right.begin(), Right.end());
    llvm::SmallVector<SymRef, 8> KeptLeft, KeptRight;
    size_t L = 0, R = 0;
    bool Cancelled = false;
    while (L != Left.size() && R != Right.size()) {
      if (Left[L] == Right[R]) {
        ++L;
        ++R;
        Cancelled = true;
      } else if (Left[L] < Right[R])
        KeptLeft.push_back(Left[L++]);
      else
        KeptRight.push_back(Right[R++]);
    }
    if (Cancelled) {
      KeptLeft.append(Left.begin() + L, Left.end());
      KeptRight.append(Right.begin() + R, Right.end());
      for (SymRef Term : KeptRight)
        KeptLeft.push_back(mkNeg(Term));
      return KeptLeft.empty() ? mkZero(width(A)) : mkAdd(KeptLeft);
    }
  }
  return mkAdd(A, mkNeg(B));
}

SymRef SymContext::mkNeg(SymRef A) {
  uint32_t W = width(A);
  return mkMul(mkConst(llvm::APInt::getAllOnes(W)), A);
}

SymRef SymContext::mkMul(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty() && "mkMul needs at least one operand");
  uint32_t W = width(Ops[0]);

  // Incrementally built products already have sorted nonconstant operands
  // and at most one leading coefficient. Insert the new atom directly instead
  // of flattening, multiplying the same coefficient and sorting that prefix
  // again. Keep an owned snapshot before interning can relocate the arena.
  if (Ops.size() == 2) {
    SymRef Product = Ops[0], Atom = Ops[1];
    if (op(Product) != SymOp::Mul)
      std::swap(Product, Atom);
    if (op(Product) == SymOp::Mul && op(Atom) != SymOp::Mul && !isConst(Atom)) {
      assert(width(Atom) == W && width(Product) == W &&
             "mkMul operands must share a width");
      auto Existing = operands(Product);
      llvm::SmallVector<SymRef, 16> Factors;
      Factors.reserve(Existing.size() + 1);
      Factors.append(Existing.begin(), Existing.end());
      auto Begin = Factors.begin() + unsigned(isConst(Factors.front()));
      Factors.insert(std::lower_bound(Begin, Factors.end(), Atom), Atom);
      return intern(SymOp::Mul, W, Factors, 0);
    }
    if (op(Product) == SymOp::Mul && isConst(Atom)) {
      auto Existing = operands(Product);
      const bool Scaled = isConst(Existing.front());
      auto Tail = Existing.drop_front(unsigned(Scaled));
      // A single logical shift still needs the matching-power fold below.
      if (Tail.size() >= 2) {
        assert(width(Atom) == W && width(Product) == W &&
               "mkMul operands must share a width");
        llvm::APInt Coefficient = constValue(Atom);
        if (Scaled)
          Coefficient *= constValue(Existing.front());
        if (Coefficient.isZero())
          return mkZero(W);
        llvm::SmallVector<SymRef, 16> Factors;
        Factors.reserve(Tail.size() + 1);
        Factors.append(Tail.begin(), Tail.end());
        if (!Coefficient.isOne())
          Factors.insert(Factors.begin(), mkConst(Coefficient));
        return intern(SymOp::Mul, W, Factors, 0);
      }
    }
  }
  // Arithmetic normalization and sum coefficient collection often hand back
  // an already canonical flat product. Verify that order before reusing it;
  // zero/unit coefficients and unary shift products retain ordinary folding.
  if (Ops.size() >= 3 && !isConst(Ops.back())) {
    const bool Scaled = isConst(Ops.front());
    bool Canonical = !Scaled || (!isConstZero(Ops.front()) &&
                                 !constValue(Ops.front()).isOne());
    SymRef Previous;
    for (SymRef Factor : Ops.drop_front(unsigned(Scaled))) {
      assert(width(Factor) == W && "mkMul operands must share a width");
      if (isConst(Factor) || op(Factor) == SymOp::Mul ||
          (Previous.isValid() && Factor < Previous)) {
        Canonical = false;
        break;
      }
      Previous = Factor;
    }
    if (Canonical) {
      llvm::SmallVector<SymRef, 16> Factors(Ops);
      return intern(SymOp::Mul, W, Factors, 0);
    }
  }

  llvm::APInt ConstFactor(W, 1);
  llvm::SmallVector<SymRef, 8> Rest;

  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkMul operands must share a width");
    if (op(R) == SymOp::Mul) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      ConstFactor *= constValue(R);
      continue;
    }
    Rest.push_back(R);
  }

  if (ConstFactor.isZero())
    return mkZero(W);
  if (Rest.empty())
    return mkConst(ConstFactor);

  std::sort(Rest.begin(), Rest.end());

  if (Rest.size() == 1 && op(Rest[0]) == SymOp::LShr) {
    SymRef Source = operand(Rest[0], 0);
    SymRef Count = operand(Rest[0], 1);
    if (auto K = detail::matchingShiftPower(*this, Count, ConstFactor))
      return mkAnd(Source, mkConst(~llvm::APInt::getLowBitsSet(W, *K)));
  }

  if (ConstFactor.isOne()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::Mul, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(ConstFactor));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Mul, W, Final, 0);
}

SymRef SymContext::mkUDiv(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt D = constValue(B);
    return mkConst(D.isZero() ? llvm::APInt::getAllOnes(W)
                              : constValue(A).udiv(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return A;
  return intern(SymOp::UDiv, W, {A, B}, 0);
}

SymRef SymContext::mkSDiv(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt N = constValue(A), D = constValue(B);
    if (D.isZero())
      return mkConst(N.isNonNegative() ? llvm::APInt::getAllOnes(W)
                                       : llvm::APInt(W, 1));
    // The single overflowing case, INT_MIN / -1, wraps back to INT_MIN.
    if (N.isMinSignedValue() && D.isAllOnes())
      return mkConst(N);
    return mkConst(N.sdiv(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return A;
  return intern(SymOp::SDiv, W, {A, B}, 0);
}

SymRef SymContext::mkURem(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt D = constValue(B);
    return mkConst(D.isZero() ? constValue(A) : constValue(A).urem(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return mkZero(W);
  if (op(A) == SymOp::ZExt && op(B) == SymOp::ZExt) {
    const auto X = operand(A, 0), Y = operand(B, 0);
    const auto Narrow = std::max(width(X), width(Y));
    // Both unsigned inputs fit this width. The remainder does too, including
    // the total bitvector case x % 0 == x. Division by zero instead produces
    // all ones at its original width and cannot use this extension rule.
    return mkZExt(mkURem(mkZExt(X, Narrow), mkZExt(Y, Narrow)), W);
  }
  return intern(SymOp::URem, W, {A, B}, 0);
}

SymRef SymContext::mkSRem(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt N = constValue(A), D = constValue(B);
    if (D.isZero())
      return mkConst(N);
    if (N.isMinSignedValue() && D.isAllOnes())
      return mkZero(W);
    return mkConst(N.srem(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return mkZero(W);
  return intern(SymOp::SRem, W, {A, B}, 0);
}

} // namespace neverd::symbolic

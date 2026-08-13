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

#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <cassert>
#include <map>

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
  // Ordered by base node index so the rebuilt operand list is canonical.
  std::map<uint32_t, llvm::APInt> Terms;

  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkAdd operands must share a width");

    if (op(R) == SymOp::Add) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      ConstTerm += constValue(R);
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
    if (It == Terms.end())
      Terms.emplace(Base.index(), Coeff);
    else
      It->second += Coeff;
  }

  llvm::SmallVector<SymRef, 8> Final;
  if (!ConstTerm.isZero())
    Final.push_back(mkConst(ConstTerm));
  for (const auto &[BaseIdx, Coeff] : Terms) {
    if (Coeff.isZero())
      continue;
    SymRef Base(BaseIdx);
    Final.push_back(Coeff.isOne() ? Base : mkMul(mkConst(Coeff), Base));
  }

  if (Final.empty())
    return mkZero(W);
  if (Final.size() == 1)
    return Final[0];
  return intern(SymOp::Add, W, Final, 0);
}

SymRef SymContext::mkSub(SymRef A, SymRef B) { return mkAdd(A, mkNeg(B)); }

SymRef SymContext::mkNeg(SymRef A) {
  uint32_t W = width(A);
  return mkMul(mkConst(llvm::APInt::getAllOnes(W)), A);
}

SymRef SymContext::mkMul(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty() && "mkMul needs at least one operand");
  uint32_t W = width(Ops[0]);

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

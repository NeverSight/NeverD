//===- SymExprLogic.cpp - Canonicalizing bitwise and shift builders -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the bitwise, shift and rotate builders, together with the
/// absorbing, idempotent and complement laws they apply on construction.
///
/// Shift behaviour follows SMT-LIB QF_BV rather than any one machine's: a
/// shift by at least the operand width yields zero, or the sign for an
/// arithmetic right shift.
///
//===----------------------------------------------------------------------===//

#include "SymExprCompare.h"
#include "SymExprMask.h"

#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace neverd::symbolic {
namespace {

// Keep the zero-filled high bits outside the bitwise operation. Only AND may
// discard high constant bits; OR and XOR must retain every set constant bit.
SymRef contractZeroExtensions(SymContext &C, SymOp Op,
                              llvm::ArrayRef<SymRef> Rest,
                              const llvm::APInt &Constant) {
  if (C.op(Rest.front()) != SymOp::ZExt)
    return {};
  const unsigned Width = C.width(C.operand(Rest.front(), 0));
  if (Op != SymOp::And && Constant.getActiveBits() > Width)
    return {};
  for (auto R : Rest)
    if (C.op(R) != SymOp::ZExt || C.width(C.operand(R, 0)) != Width)
      return {};
  llvm::SmallVector<SymRef, 8> Narrow;
  for (auto R : Rest)
    Narrow.push_back(C.operand(R, 0));
  Narrow.push_back(C.mkConst(Constant.trunc(Width)));
  SymRef Value;
  if (Op == SymOp::And)
    Value = C.mkAnd(Narrow);
  else if (Op == SymOp::Or)
    Value = C.mkOr(Narrow);
  else
    Value = C.mkXor(Narrow);
  return C.mkZExt(Value, Constant.getBitWidth());
}

} // namespace

/// Shared shape for And/Or/Xor: flatten the nested same-operator operands,
/// fold the constants into one, then let the caller apply the operator's own
/// absorbing, idempotent and complement laws.
SymRef SymContext::mkAnd(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  uint32_t W = width(Ops[0]);

  llvm::APInt Acc = llvm::APInt::getAllOnes(W);
  llvm::SmallVector<SymRef, 8> Rest;
  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkAnd operands must share a width");
    if (op(R) == SymOp::And) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      Acc &= constValue(R);
      continue;
    }
    Rest.push_back(R);
  }

  if (Rest.size() == 1 && Acc.isMask() && !Acc.isAllOnes()) {
    SymRef Reduced = simplifyLowMaskedAdd(Rest[0], Acc);
    if (Reduced != Rest[0]) {
      Rest.clear();
      // Normalize the new immediate AND/constant without recursively
      // applying demand to another layer of a potentially deep source DAG.
      if (op(Reduced) == SymOp::And)
        Rest.append(operands(Reduced).begin(), operands(Reduced).end());
      else
        Rest.push_back(Reduced);
      llvm::erase_if(Rest, [&](SymRef R) {
        if (!isConst(R))
          return false;
        Acc &= constValue(R);
        return true;
      });
    }
  }

  if (Acc.isZero())
    return mkZero(W);

  std::sort(Rest.begin(), Rest.end());
  Rest.erase(std::unique(Rest.begin(), Rest.end()), Rest.end());

  // Absorption is independent of what computes an operand: x & (x | y)
  // equals x even when x is arithmetic or otherwise opaque to MBA measurement.
  llvm::SmallVector<SymRef, 8> Unabsorbed;
  for (SymRef R : Rest) {
    bool Absorbed =
        op(R) == SymOp::Or && llvm::any_of(operands(R), [&](SymRef C) {
          return std::binary_search(Rest.begin(), Rest.end(), C);
        });
    if (!Absorbed)
      Unabsorbed.push_back(R);
  }
  Rest = std::move(Unabsorbed);

  // x & ~x == 0.  Testing the Not operands against the set avoids interning a
  // complement just to look it up.
  for (SymRef R : Rest) {
    if (op(R) != SymOp::Not)
      continue;
    if (std::binary_search(Rest.begin(), Rest.end(), operand(R, 0)))
      return mkZero(W);
  }

  if (Rest.empty())
    return mkConst(Acc);

  if (auto Narrow = contractZeroExtensions(*this, SymOp::And, Rest, Acc))
    return Narrow;

  if (W == 1)
    if (SymRef Predicate =
            detail::recoverObservedComparison(*this, SymOp::And, Rest, Acc))
      return Predicate;

  // A small masked OR can expose known bits without expanding the general
  // expression. In particular, a status bit remains constant when every
  // unknown OR arm is masked away from that bit.
  if (!Acc.isAllOnes() && Rest.size() == 1 && op(Rest[0]) == SymOp::Or) {
    llvm::ArrayRef<SymRef> OrTerms = operands(Rest[0]);
    llvm::SmallVector<SymRef, 8> Terms(OrTerms.begin(), OrTerms.end());
    const auto IsDisjoint = [&](SymRef Term) {
      if (isConst(Term))
        return (constValue(Term) & Acc).isZero();
      if (op(Term) != SymOp::And)
        return false;
      for (SymRef Factor : operands(Term))
        if (isConst(Factor) && (constValue(Factor) & Acc).isZero())
          return true;
      return false;
    };
    if (Terms.size() <= 8 &&
        std::any_of(Terms.begin(), Terms.end(), IsDisjoint)) {
      llvm::SmallVector<SymRef, 8> Masked;
      const SymRef Mask = mkConst(Acc);
      for (SymRef Term : Terms)
        Masked.push_back(mkAnd(Mask, Term));
      return mkOr(Masked);
    }
  }

  if (Acc.isAllOnes()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::And, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::And, W, Final, 0);
}

SymRef SymContext::mkOr(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  uint32_t W = width(Ops[0]);

  llvm::APInt Acc(W, 0);
  llvm::SmallVector<SymRef, 8> Rest;
  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkOr operands must share a width");
    if (op(R) == SymOp::Or) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      Acc |= constValue(R);
      continue;
    }
    Rest.push_back(R);
  }

  if (mergeMaskedOperands(Rest, /*RequireDisjoint=*/false)) {
    // A fully covered source can itself be an OR. Flatten it iteratively;
    // mask reasoning stays local instead of re-entering this builder.
    Work = std::move(Rest);
    Rest.clear();
    while (!Work.empty()) {
      SymRef R = Work.pop_back_val();
      if (op(R) == SymOp::Or) {
        auto Sub = operands(R);
        Work.append(Sub.begin(), Sub.end());
      } else if (isConst(R)) {
        Acc |= constValue(R);
      } else {
        Rest.push_back(R);
      }
    }
  }

  if (Acc.isAllOnes())
    return mkConst(Acc);

  std::sort(Rest.begin(), Rest.end());
  Rest.erase(std::unique(Rest.begin(), Rest.end()), Rest.end());

  // Dual absorption: x | (x & y) == x, including compound x.
  llvm::SmallVector<SymRef, 8> Unabsorbed;
  for (SymRef R : Rest) {
    bool Absorbed =
        op(R) == SymOp::And && llvm::any_of(operands(R), [&](SymRef C) {
          return std::binary_search(Rest.begin(), Rest.end(), C);
        });
    if (!Absorbed)
      Unabsorbed.push_back(R);
  }
  Rest = std::move(Unabsorbed);

  // x | ~x == -1.
  for (SymRef R : Rest) {
    if (op(R) != SymOp::Not)
      continue;
    if (std::binary_search(Rest.begin(), Rest.end(), operand(R, 0)))
      return mkOnes(W);
  }

  if (Rest.empty())
    return mkConst(Acc);

  if (auto Narrow = contractZeroExtensions(*this, SymOp::Or, Rest, Acc))
    return Narrow;

  if (W == 1)
    if (SymRef Predicate =
            detail::recoverObservedComparison(*this, SymOp::Or, Rest, Acc))
      return Predicate;

  if (Acc.isZero()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::Or, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Or, W, Final, 0);
}

SymRef SymContext::mkXor(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  uint32_t W = width(Ops[0]);

  llvm::APInt Acc(W, 0);
  llvm::SmallVector<SymRef, 8> Flat;
  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkXor operands must share a width");
    if (op(R) == SymOp::Xor) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      Acc ^= constValue(R);
      continue;
    }
    Flat.push_back(R);
  }

  // x ^ x == 0, so only the parity of each operand's multiplicity survives.
  std::sort(Flat.begin(), Flat.end());
  llvm::SmallVector<SymRef, 8> Rest;
  for (size_t I = 0; I < Flat.size();) {
    size_t J = I;
    while (J < Flat.size() && Flat[J] == Flat[I])
      ++J;
    if ((J - I) & 1)
      Rest.push_back(Flat[I]);
    I = J;
  }

  // x ^ ~x == -1.  Each such pair contributes all-ones to the constant and
  // drops both operands.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t I = 0; I < Rest.size(); ++I) {
      if (op(Rest[I]) != SymOp::Not)
        continue;
      SymRef Inner = operand(Rest[I], 0);
      auto It = std::find(Rest.begin(), Rest.end(), Inner);
      if (It == Rest.end())
        continue;
      Acc ^= llvm::APInt::getAllOnes(W);
      Rest.erase(Rest.begin() + I);
      Rest.erase(std::find(Rest.begin(), Rest.end(), Inner));
      Changed = true;
      break;
    }
  }

  if (Rest.empty())
    return mkConst(Acc);

  if (auto Narrow = contractZeroExtensions(*this, SymOp::Xor, Rest, Acc))
    return Narrow;

  if (W == 1 || !isVar(Rest[0]))
    if (SymRef Predicate =
            detail::recoverObservedComparison(*this, SymOp::Xor, Rest, Acc))
      return Predicate;

  // Bits forced by OR disappear when XOR uses the same constant mask.
  if (!Acc.isZero() && Rest.size() == 1 && op(Rest[0]) == SymOp::Or) {
    auto Factors = operands(Rest[0]);
    if (isConst(Factors[0]) && constValue(Factors[0]) == Acc) {
      llvm::SmallVector<SymRef, 8> Source(Factors.begin() + 1, Factors.end());
      SymRef Value = Source.size() == 1 ? Source[0] : mkOr(Source);
      return mkAnd(Value, mkConst(~Acc));
    }
  }

  // x ^ -1 == ~x: prefer the complement, which the bitwise laws above and the
  // MBA solver's boolean domain both recognise.
  if (Acc.isAllOnes()) {
    SymRef Inner = Rest.size() == 1 ? Rest[0] : intern(SymOp::Xor, W, Rest, 0);
    return mkNot(Inner);
  }

  if (Acc.isZero()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::Xor, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Xor, W, Final, 0);
}

SymRef SymContext::mkNot(SymRef A) {
  uint32_t W = width(A);
  if (isConst(A))
    return mkConst(~constValue(A));
  if (op(A) == SymOp::Not)
    return operand(A, 0);
  // A negated free input needs one fewer operation as x - 1. Keep compound
  // sources opaque, since their complement can identify a shared factor.
  if (W > 1 && op(A) == SymOp::Mul && numOperands(A) == 2 &&
      isConstOnes(operand(A, 0)) && isVar(operand(A, 1)))
    return mkAdd(operand(A, 1), mkOnes(W));
  // One-bit flag networks retain their Boolean spelling for comparison
  // recovery; affine word normalization belongs to wider arithmetic.
  if (W > 1 && op(A) == SymOp::Add) {
    // ~(c + sum(k*x)) = ~c + sum(-k*x), at the original word width.
    // Estimate only the immediate spelling change before allocating anything:
    // negating a bare term adds a product, while -1*x loses its product.
    // Other products keep their operator. Do not expand their factors.
    llvm::ArrayRef<SymRef> Terms = operands(A);
    const bool HasConstant = isConst(Terms.front());
    const llvm::APInt Offset =
        HasConstant ? ~constValue(Terms.front()) : llvm::APInt::getAllOnes(W);
    size_t Added = 0;
    size_t Removed = 1; // The outer complement disappears.
    for (SymRef Term : Terms.drop_front(HasConstant ? 1 : 0)) {
      if (isVar(Term)) {
        ++Added;
      } else if (op(Term) == SymOp::Mul && numOperands(Term) == 2 &&
                 isConst(operand(Term, 0)) && isVar(operand(Term, 1))) {
        if (isConstOnes(operand(Term, 0)))
          ++Removed;
      } else {
        // Compound inputs need their complement boundary intact: rewriting
        // one use can hide its relation to the same source in a product or
        // bitwise identity, even when this local spelling becomes shorter.
        return intern(SymOp::Not, W, {A}, 0);
      }
    }
    if (Offset.isZero() && Terms.size() == 2 && HasConstant)
      ++Removed; // One surviving term needs no sum.
    if (Removed > Added) {
      // Builders may intern and reallocate the operand pool. Copy immediate
      // operands, keeping deeper/shared sources opaque throughout this step.
      llvm::SmallVector<SymRef, 8> Negated(Terms.begin(), Terms.end());
      if (HasConstant)
        Negated.erase(Negated.begin());
      for (SymRef &Term : Negated)
        Term = mkNeg(Term);
      if (!Offset.isZero())
        Negated.push_back(mkConst(Offset));
      SymRef Reduced = mkAdd(Negated);
      const SymReadability Cost = readability(A);
      if (Cost.Nodes < std::numeric_limits<size_t>::max() - 1 &&
          readability(Reduced) <
              SymReadability{Cost.Nodes + 1, Cost.Operations + 1})
        return Reduced;
    }
  }
  return intern(SymOp::Not, W, {A}, 0);
}

SymRef SymContext::mkShl(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    llvm::APInt Amt = constValue(B);
    if (Amt.uge(W))
      return mkZero(W);
    if (Amt.isZero())
      return A;
    if (isConst(A))
      return mkConst(constValue(A).shl(Amt.getZExtValue()));
    // A left shift is a multiply by a power of two.  Normalising to Mul lets
    // the sum canonicalisation collect `x + (x << 1)` into `3*x`, which is a
    // very common obfuscation shape.
    return mkMul(mkConst(llvm::APInt(W, 1).shl(Amt.getZExtValue())), A);
  }
  if (isConstZero(A))
    return mkZero(W);
  return intern(SymOp::Shl, W, {A, B}, 0);
}

SymRef SymContext::mkLShr(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    llvm::APInt Amt = constValue(B);
    if (Amt.uge(W))
      return mkZero(W);
    if (Amt.isZero())
      return A;
    if (isConst(A))
      return mkConst(constValue(A).lshr(Amt.getZExtValue()));
    if (op(A) == SymOp::Mul && operands(A).size() == 2 &&
        isConst(operand(A, 0))) {
      llvm::APInt Factor = constValue(operand(A, 0));
      SymRef Source = operand(A, 1);
      if (auto K = detail::matchingShiftPower(*this, B, Factor))
        return mkAnd(Source, mkConst(llvm::APInt::getLowBitsSet(W, W - *K)));
    }
    if (Amt == W - 1)
      if (SymRef Predicate = detail::recoverSignComparison(*this, A))
        return mkZExt(Predicate, W);
  }
  // Retain the entire count: truncating it to the narrow value width would
  // turn a large, zero-producing shift into a small one.
  if (op(A) == SymOp::ZExt)
    return mkZExt(mkLShr(operand(A, 0), B), W);
  if (isConstZero(A))
    return mkZero(W);
  return intern(SymOp::LShr, W, {A, B}, 0);
}

SymRef SymContext::mkAShr(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (W == 1)
    return A;
  if (isConst(B)) {
    llvm::APInt Amt = constValue(B);
    if (isConst(A)) {
      llvm::APInt V = constValue(A);
      return mkConst(Amt.uge(W) ? (V.isNegative() ? llvm::APInt::getAllOnes(W)
                                                  : llvm::APInt(W, 0))
                                : V.ashr(Amt.getZExtValue()));
    }
    if (Amt.isZero())
      return A;
    if (Amt.uge(W - 1))
      if (SymRef Predicate = detail::recoverSignComparison(*this, A))
        return mkSExt(Predicate, W);
  }
  if (isConstZero(A))
    return mkZero(W);
  return intern(SymOp::AShr, W, {A, B}, 0);
}

SymRef SymContext::mkRol(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    uint64_t Amt = constValue(B).urem(W);
    if (Amt == 0)
      return A;
    if (isConst(A))
      return mkConst(constValue(A).rotl(Amt));
  }
  return intern(SymOp::Rol, W, {A, B}, 0);
}

SymRef SymContext::mkRor(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    uint64_t Amt = constValue(B).urem(W);
    if (Amt == 0)
      return A;
    if (isConst(A))
      return mkConst(constValue(A).rotr(Amt));
  }
  return intern(SymOp::Ror, W, {A, B}, 0);
}

} // namespace neverd::symbolic

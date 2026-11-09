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

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymKnownBits.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace neverd::symbolic {
namespace {

SymRef joinPackedByteSlices(SymContext &C, llvm::ArrayRef<SymRef> Terms,
                            const llvm::APInt &Constant) {
  if (Terms.size() < 2)
    return {};
  struct Part {
    SymRef Value;
    unsigned Low, Width;
  };
  const unsigned Width = Constant.getBitWidth();
  llvm::SmallVector<Part, 8> Parts;
  llvm::APInt Occupied(Width, 0);
  for (auto Term : Terms) {
    unsigned Low = 0;
    if (C.op(Term) == SymOp::Shl && C.isConst(C.operand(Term, 1))) {
      const auto &Shift = C.constValue(C.operand(Term, 1));
      if (Shift.uge(Width))
        return {};
      Low = Shift.getZExtValue();
      Term = C.operand(Term, 0);
    } else if (C.op(Term) == SymOp::Mul && C.numOperands(Term) == 2 &&
               C.isConst(C.operand(Term, 0)) &&
               C.constValue(C.operand(Term, 0)).isPowerOf2()) {
      Low = C.constValue(C.operand(Term, 0)).logBase2();
      Term = C.operand(Term, 1);
    }
    if (C.op(Term) != SymOp::ZExt || Low % 8)
      return {};
    const auto Value = C.operand(Term, 0);
    const unsigned Bits = C.width(Value);
    if (Bits % 8 || Bits > Width - Low)
      return {};
    const auto Mask = llvm::APInt::getBitsSet(Width, Low, Low + Bits);
    if (!(Occupied & Mask).isZero())
      return {};
    Occupied |= Mask;
    Parts.push_back({Value, Low, Bits});
  }
  if (!(Constant & Occupied).isZero())
    return {};
  std::sort(Parts.begin(), Parts.end(),
            [](const Part &A, const Part &B) { return A.Low > B.Low; });
  llvm::SmallVector<SymRef, 8> Slices;
  unsigned End = Width;
  for (auto P : Parts) {
    if (End > P.Low + P.Width)
      Slices.push_back(C.mkConst(
          Constant.extractBits(End - P.Low - P.Width, P.Low + P.Width)));
    Slices.push_back(P.Value);
    End = P.Low;
  }
  if (End)
    Slices.push_back(C.mkConst(Constant.extractBits(End, 0)));
  const auto Joined = C.mkConcat(Slices);
  auto Body = C.op(Joined) == SymOp::ZExt ? C.operand(Joined, 0) : Joined;
  const auto Remaining = C.op(Body) == SymOp::Concat ? C.numOperands(Body) : 1;
  // Merely spelling an arbitrary OR as a concatenation is no improvement.
  // At least two original data parts must have collapsed into one value.
  return Remaining < Parts.size() ? Joined : SymRef{};
}

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

SymRef foldSelectedLiterals(
    SymContext &C, SymRef Value,
    llvm::function_ref<llvm::APInt(const llvm::APInt &)> Fold) {
  const auto Op = C.op(Value);
  const auto Selection =
      Op == SymOp::SExt || Op == SymOp::ZExt ? C.operand(Value, 0) : Value;
  if (C.op(Selection) != SymOp::Ite || !C.isConst(C.operand(Selection, 1)) ||
      !C.isConst(C.operand(Selection, 2)))
    return {};
  const auto Condition = C.operand(Selection, 0);
  const unsigned Width = C.width(Value), Narrow = C.width(Selection);
  auto Apply = [&](unsigned Arm) {
    const auto &Literal = C.constValue(C.operand(Selection, Arm));
    return Fold(Op == SymOp::SExt ? Literal.sext(Width) : Literal.zext(Width));
  };
  const llvm::APInt Yes = Apply(1), No = Apply(2);
  // Keep narrow carriers visible to cheap range/known-bit queries. Folding
  // literals must not replace a zero extension with a wide opaque selection.
  if (Narrow < Width && Yes.getActiveBits() <= Narrow &&
      No.getActiveBits() <= Narrow)
    return C.mkZExt(C.mkIte(Condition, C.mkConst(Yes.trunc(Narrow)),
                            C.mkConst(No.trunc(Narrow))),
                    Width);
  if (Narrow < Width && Yes.isSignedIntN(Narrow) && No.isSignedIntN(Narrow))
    return C.mkSExt(C.mkIte(Condition, C.mkConst(Yes.trunc(Narrow)),
                            C.mkConst(No.trunc(Narrow))),
                    Width);
  return C.mkIte(Condition, C.mkConst(Yes), C.mkConst(No));
}

// A logical right shift observes only [Amount, Width). Remove bitwise terms
// that are neutral throughout that window, without distributing the shift or
// expanding the DAG. A failed or exhausted fact query keeps its operand.
SymRef removeDiscardedShiftTerms(SymContext &C, SymRef A, unsigned Amount) {
  const auto Op = C.op(A);
  const unsigned Width = C.width(A);
  if ((Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor) ||
      Width > ::neverd::limits::symbolic::kMaxDiscardedShiftWidth ||
      C.numOperands(A) >
          ::neverd::limits::symbolic::
              kSymExprLogicRemoveDiscardedShiftTermsCNumOperandsALimit)
    return A;
  unsigned Work =
      ::neverd::limits::symbolic::kSymExprLogicRemoveDiscardedShiftTermsWork;
  llvm::SmallVector<SymRef, 8> Kept;
  for (auto Child : C.operands(A)) {
    bool Neutral = true;
    for (unsigned Low = Amount; Low < Width;) {
      const unsigned Bits = std::min(Width - Low, 64U);
      auto Value = C.constantWindow(Child, Low, Bits, Work);
      if (!Value ||
          (Op == SymOp::And ? !Value->isAllOnes() : !Value->isZero())) {
        Neutral = false;
        break;
      }
      Low += Bits;
    }
    if (!Neutral)
      Kept.push_back(Child);
  }
  if (Kept.size() == C.numOperands(A))
    return A;
  if (Kept.empty())
    return Op == SymOp::And ? C.mkOnes(Width) : C.mkZero(Width);
  if (Op == SymOp::And)
    return C.mkAnd(Kept);
  if (Op == SymOp::Or)
    return C.mkOr(Kept);
  return C.mkXor(Kept);
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
    // The constant is folded separately from Rest. An OR that sets every
    // bit retained by this AND cannot constrain the remaining result.
    Absorbed |= op(R) == SymOp::Or && isConst(operand(R, 0)) &&
                (constValue(operand(R, 0)) & Acc) == Acc;
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

  // Packed fields are commonly written as a shifted or scaled narrow value.
  // Resolve a constant observed slice before it can become another operand
  // of a growing status word. The shared fact transfer includes overflow and
  // shift semantics; an exhausted query leaves the original expression.
  if (W <= SymKnownBits::MaxWidth && !Acc.isAllOnes() && Rest.size() == 1 &&
      (op(Rest[0]) == SymOp::Mul || op(Rest[0]) == SymOp::Shl)) {
    unsigned Work = ::neverd::limits::symbolic::kSymExprLogicSubsetWork;
    SymKnownBits Known(*this);
    if (auto Bits = Known.query(Rest[0], Work))
      if (((Bits->Zero | Bits->One) & Acc) == Acc)
        return mkConst(Bits->One & Acc);
  }

  // OR/XOR cannot set a bit that is zero in every operand. Explicit masks
  // provide this fact without distributing AND or recursively walking the
  // data expression. Bound both fan-in and word size for builder-time work.
  if (W <= 128 && !Acc.isAllOnes() && Rest.size() == 1 &&
      (op(Rest[0]) == SymOp::Or || op(Rest[0]) == SymOp::Xor)) {
    auto Terms = operands(Rest[0]);
    const auto Outside = ~Acc;
    if (Terms.size() <= 8 && llvm::all_of(Terms, [&](SymRef Term) {
          if (isConst(Term))
            return (constValue(Term) & Outside).isZero();
          return op(Term) == SymOp::And && isConst(operand(Term, 0)) &&
                 (constValue(operand(Term, 0)) & Outside).isZero();
        }))
      return Rest[0];
  }

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
    unsigned MaskWork = ::neverd::limits::symbolic::kSymExprLogicSubsetWork;
    SymKnownBits Known(*this);
    const auto IsDisjoint = [&](SymRef Term) {
      if (isConst(Term))
        return (constValue(Term) & Acc).isZero();
      if (op(Term) == SymOp::And)
        for (SymRef Factor : operands(Term))
          if (isConst(Factor) && (constValue(Factor) & Acc).isZero())
            return true;
      // Scaled narrow fields can be disjoint from any observed mask. Keeping
      // them until a later MBA pass makes repeated packed-word updates grow
      // a new dead arithmetic cone at every instruction. The bounded known-
      // bits query proves all observed bits zero without distributing AND.
      if (auto Bits = Known.query(Term, MaskWork))
        return (Bits->Zero & Acc) == Acc;
      return false;
    };
    llvm::SmallVector<bool, 8> Disjoint;
    if (Terms.size() <= 8)
      for (auto Term : Terms)
        Disjoint.push_back(IsDisjoint(Term));
    if (llvm::is_contained(Disjoint, true)) {
      llvm::SmallVector<SymRef, 8> Masked;
      const SymRef Mask = mkConst(Acc);
      for (size_t I = 0; I != Terms.size(); ++I)
        if (!Disjoint[I])
          Masked.push_back(mkAnd(Mask, Terms[I]));
      return Masked.empty() ? mkZero(W) : mkOr(Masked);
    }
  }

  if (Acc.isAllOnes()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::And, W, Rest, 0);
  }

  if (Rest.size() == 1)
    if (auto Folded = foldSelectedLiterals(
            *this, Rest[0], [&](const llvm::APInt &V) { return V & Acc; }))
      return Folded;

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

  // The XOR of values contained in X cannot set any bit outside X. This
  // includes X ^ (X & Y), without reasoning about the arithmetic inside X.
  unsigned SubsetWork = ::neverd::limits::symbolic::kSymExprLogicSubsetWork;
  const auto CoveredByConstant = [&](SymRef Value) {
    if (!SubsetWork)
      return false;
    --SubsetWork;
    SymRef Mask;
    bool Complement = false;
    if (isConst(Value)) {
      Mask = Value;
    } else if (op(Value) == SymOp::And && isConst(operand(Value, 0))) {
      Mask = operand(Value, 0);
    } else if (op(Value) == SymOp::Not) {
      const auto Inner = operand(Value, 0);
      if (op(Inner) == SymOp::Or && isConst(operand(Inner, 0))) {
        Mask = operand(Inner, 0);
        Complement = true;
      }
    }
    if (!Mask)
      return false;
    // Charge full-width mask inspection as well as each immediate term.
    // No recursive source walk or expression construction is needed.
    const unsigned Words = (W - 1) / 64 + 1;
    if (Words > SubsetWork) {
      SubsetWork = 0;
      return false;
    }
    SubsetWork -= Words;
    auto Possible = constValue(Mask);
    if (Complement)
      Possible.flipAllBits();
    return (Possible & ~Acc).isZero();
  };
  const auto CoveredXor = [&](SymRef Value) {
    if (op(Value) != SymOp::Xor)
      return false;
    // Constants have already moved out of Rest. Their set bits can cover
    // every XOR arm too, including ~(K | X), whose possible bits lie in ~K.
    if (!Acc.isZero() && llvm::all_of(operands(Value), CoveredByConstant))
      return true;
    for (SymRef Cover : Rest) {
      if (!SubsetWork)
        return false;
      --SubsetWork;
      if (Cover == Value)
        continue;
      bool Covered = true;
      for (SymRef Term : operands(Value)) {
        if (!SubsetWork)
          return false;
        --SubsetWork;
        if (Term == Cover)
          continue;
        if (op(Term) != SymOp::And) {
          Covered = false;
          break;
        }
        bool Found = false;
        for (SymRef Factor : operands(Term)) {
          if (!SubsetWork)
            return false;
          --SubsetWork;
          if (Factor == Cover) {
            Found = true;
            break;
          }
        }
        if (!Found) {
          Covered = false;
          break;
        }
      }
      if (Covered)
        return true;
    }
    return false;
  };
  // Dual absorption: x | (x & y) == x, including compound x.
  llvm::SmallVector<SymRef, 8> Unabsorbed;
  for (SymRef R : Rest) {
    bool Absorbed =
        op(R) == SymOp::And && llvm::any_of(operands(R), [&](SymRef Factor) {
          if (std::binary_search(Rest.begin(), Rest.end(), Factor))
            return true;
          // The outer OR has flattened a compound cover: (a | b) |
          // (mask & (a | b)) still absorbs the masked copy. Every inner
          // term must be covered; a missing term or work refusal keeps R.
          if (op(Factor) != SymOp::Or)
            return false;
          return llvm::all_of(operands(Factor), [&](SymRef Term) {
            if (!SubsetWork)
              return false;
            --SubsetWork;
            if (isConst(Term))
              return (constValue(Term) & ~Acc).isZero();
            return std::binary_search(Rest.begin(), Rest.end(), Term);
          });
        });
    Absorbed |= CoveredXor(R);
    // A masked value cannot add any bit already set by the folded constant.
    Absorbed |= op(R) == SymOp::And && isConst(operand(R, 0)) &&
                (constValue(operand(R, 0)) & ~Acc).isZero();
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

  if (auto Joined = joinPackedByteSlices(*this, Rest, Acc))
    return Joined;

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

  if (Rest.size() == 1)
    if (auto Folded = foldSelectedLiterals(
            *this, Rest[0], [&](const llvm::APInt &V) { return V | Acc; }))
      return Folded;

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Or, W, Final, 0);
}

// A masked word cannot carry into a disjoint constant, so that sum holds the
// bits of an OR of its terms. Only comparisons read it as one: the sum stays a
// sum everywhere else, since that is how an address names a base and an offset.
static SymRef packedWordView(SymContext &C, SymRef Value) {
  if (C.op(Value) != SymOp::Add || C.numOperands(Value) != 2 ||
      !C.isConst(C.operand(Value, 0)))
    return Value;
  const SymRef Offset = C.operand(Value, 0), Word = C.operand(Value, 1);
  if (C.op(Word) != SymOp::And || !C.isConst(C.operand(Word, 0)) ||
      !(C.constValue(Offset) & C.constValue(C.operand(Word, 0))).isZero())
    return Value;
  return C.mkOr(Offset, Word);
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

  // x ^ (x & mask) == x & ~mask. The captured x can be a complete word;
  // its unobserved fields need not be expanded to prove this identity.
  if (Rest.size() == 2) {
    for (unsigned I = 0; I != 2; ++I) {
      SymRef Value = Rest[I], Masked = Rest[1 - I];
      if (op(Masked) != SymOp::And)
        continue;
      llvm::SmallVector<SymRef, 8> Mask;
      bool ContainsValue = false;
      for (SymRef Factor : operands(Masked)) {
        if (Factor == Value)
          ContainsValue = true;
        else
          Mask.push_back(Factor);
      }
      if (ContainsValue) {
        SymRef Bits = Mask.size() == 1 ? Mask.front() : mkAnd(Mask);
        SymRef Difference = mkAnd(Value, mkNot(Bits));
        return Acc.isZero() ? Difference : mkXor(Difference, mkConst(Acc));
      }
    }
  }

  // (x | a) ^ (x | b) == ~x & (a ^ b), for constant masks a and b.
  // Canonical ORs keep their optional constant first and all other factors
  // sorted. Compare those factors directly; do not expand the shared word
  // or enumerate the atoms that compute it.
  SymRef LeftWord =
      Rest.size() == 2 ? packedWordView(*this, Rest[0]) : SymRef();
  SymRef RightWord =
      Rest.size() == 2 ? packedWordView(*this, Rest[1]) : SymRef();
  if (Rest.size() == 2 && op(LeftWord) == SymOp::Or &&
      op(RightWord) == SymOp::Or) {
    auto Left = operands(LeftWord), Right = operands(RightWord);
    llvm::APInt ChangedMask(W, 0);
    llvm::APInt LeftMask(W, 0), RightMask(W, 0);
    if (isConst(Left.front())) {
      LeftMask = constValue(Left.front());
      ChangedMask ^= LeftMask;
      Left = Left.drop_front();
    }
    if (isConst(Right.front())) {
      RightMask = constValue(Right.front());
      ChangedMask ^= RightMask;
      Right = Right.drop_front();
    }
    if (Left == Right) {
      SymRef Shared = Left.size() == 1 ? Left.front() : mkOr(Left);
      SymRef Difference = mkAnd(mkNot(Shared), mkConst(ChangedMask));
      return Acc.isZero() ? Difference : mkXor(Difference, mkConst(Acc));
    }
    // (k | x) ^ (k | y) == ~k & (x ^ y). Copy both remaining factor
    // lists before rebuilding either word, since builders may move the pool.
    if (!LeftMask.isZero() && LeftMask == RightMask) {
      llvm::SmallVector<SymRef, 8> LeftFactors(Left.begin(), Left.end());
      llvm::SmallVector<SymRef, 8> RightFactors(Right.begin(), Right.end());
      SymRef A =
          LeftFactors.size() == 1 ? LeftFactors.front() : mkOr(LeftFactors);
      SymRef B =
          RightFactors.size() == 1 ? RightFactors.front() : mkOr(RightFactors);
      SymRef Difference = mkAnd(mkConst(~LeftMask), mkXor(A, B));
      return Acc.isZero() ? Difference : mkXor(Difference, mkConst(Acc));
    }
  }

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

  if (Rest.size() == 1)
    if (auto Folded = foldSelectedLiterals(
            *this, Rest[0], [&](const llvm::APInt &V) { return V ^ Acc; }))
      return Folded;

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
  if (auto Folded = foldSelectedLiterals(
          *this, A, [](const llvm::APInt &V) { return ~V; }))
    return Folded;
  // ~(k | ~x) == ~k & x, and its AND/OR dual. Keep the captured word
  // intact while exposing the constant observation mask before deep work.
  if ((op(A) == SymOp::Or || op(A) == SymOp::And) && numOperands(A) == 2 &&
      isConst(operand(A, 0)) && op(operand(A, 1)) == SymOp::Not) {
    const bool IsOr = op(A) == SymOp::Or;
    const llvm::APInt Mask = ~constValue(operand(A, 0));
    const SymRef Value = operand(operand(A, 1), 0);
    return IsOr ? mkAnd(Value, mkConst(Mask)) : mkOr(Value, mkConst(Mask));
  }
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
    // Compare the full unsigned count before canonicalizing its storage
    // width. Equal in-range counts must denote the same shift expression.
    B = mkConst(W, Amt.getZExtValue());
    A = removeDiscardedShiftTerms(*this, A, Amt.getZExtValue());
    if (isConst(A))
      return mkConst(constValue(A).lshr(Amt.getZExtValue()));
    if (op(A) == SymOp::And && operands(A).size() == 2 &&
        isConst(operand(A, 0)) &&
        constValue(operand(A, 0)).countLeadingOnes() >=
            W - Amt.getZExtValue()) {
      // The mask changes only bits discarded by this logical right shift.
      // Keep the original count and source; this says nothing about signed
      // shifts, other masks, or LLVM's separate definedness obligations.
      return mkLShr(operand(A, 1), B);
    }
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
    // Arithmetic overshifts saturate at the sign bit. Inspect the complete
    // count before narrowing so large counts cannot wrap to a smaller shift.
    B = mkConst(W, Amt.uge(W - 1) ? W - 1 : Amt.getZExtValue());
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

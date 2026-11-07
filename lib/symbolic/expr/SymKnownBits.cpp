//===- SymKnownBits.cpp - Bounded facts about the symbolic DAG ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymKnownBits.h"

#include <algorithm>

namespace neverd::symbolic {
namespace {
using Bits = llvm::KnownBits;

bool spend(unsigned &Remaining, unsigned Amount = 1) {
  if (Amount > Remaining) {
    Remaining = 0;
    return false;
  }
  Remaining -= Amount;
  return true;
}

unsigned words(unsigned Width) { return (Width - 1) / 64 + 1; }

SymRef lowBitSource(const SymContext &C, SymRef R, unsigned Width) {
  const auto Op = C.op(R);
  if ((Op == SymOp::ZExt || Op == SymOp::SExt) &&
      C.width(C.operand(R, 0)) >= Width)
    return C.operand(R, 0);
  if (Op == SymOp::Extract && C.node(R).Aux == 0)
    return C.operand(R, 0);
  return R;
}

// Recognize projections that preserve this exact source. Facts come from the
// total symbolic expression, never from an LLVM no-wrap annotation.
std::optional<bool> losslessProjection(const SymContext &C, SymRef Back,
                                       SymRef Source, const Bits &Facts,
                                       unsigned &Remaining) {
  const unsigned W = C.width(Source);
  if (C.op(Back) == SymOp::And) {
    auto BackTerms = C.operands(Back);
    auto SourceTerms = C.op(Source) == SymOp::And
                           ? C.operands(Source)
                           : llvm::ArrayRef<SymRef>(Source);
    if (!spend(Remaining, BackTerms.size() + SourceTerms.size() + words(W)))
      return std::nullopt;
    auto BackMask = llvm::APInt::getAllOnes(W);
    auto SourceMask = BackMask;
    if (C.isConst(BackTerms.front())) {
      BackMask = C.constValue(BackTerms.front());
      BackTerms = BackTerms.drop_front();
    }
    if (C.isConst(SourceTerms.front())) {
      SourceMask = C.constValue(SourceTerms.front());
      SourceTerms = SourceTerms.drop_front();
    }
    // AND canonicalization flattens a masked compound source. Compare all
    // remaining operands exactly, including multiplicity and existing mask.
    // A wider replacement mask could expose bits already removed by Source.
    if (BackTerms == SourceTerms && (BackMask & SourceMask) == BackMask &&
        (Facts.Zero | BackMask).isAllOnes())
      return true;
  }
  if (C.op(Back) != SymOp::AShr || !C.isConst(C.operand(Back, 1)))
    return false;
  if (!spend(Remaining, 3 + words(W)))
    return std::nullopt;
  // Arithmetic overshifts saturate at the sign bit in this total domain.
  // The full count must be inspected before limiting it to the word width.
  const unsigned Shift =
      C.constValue(C.operand(Back, 1)).getLimitedValue(W - 1);
  auto Product = C.operand(Back, 0);
  if (!Shift || C.op(Product) != SymOp::Mul || C.operands(Product).size() != 2)
    return false;
  for (unsigned I = 0; I != 2; ++I) {
    auto Factor = C.operand(Product, I);
    if (C.operand(Product, 1 - I) != Source || !C.isConst(Factor) ||
        C.constValue(Factor) != llvm::APInt::getOneBitSet(W, Shift))
      continue;
    // Left shift is canonicalized as multiplication by a power of two.
    // Shifting it back is lossless only if all discarded bits and the new
    // sign bit agree with the original sign, including negative sources.
    const auto High = llvm::APInt::getHighBitsSet(W, Shift + 1);
    return (Facts.Zero & High) == High || (Facts.One & High) == High;
  }
  return false;
}
} // namespace

struct SymKnownBits::Query {
  unsigned Remaining;
  explicit Query(unsigned Work) : Remaining(Work) {}
  uint64_t Truncations = 0;
  struct Entry {
    Bits Facts;
    unsigned Available, Depth;
    bool Complete;
  };
  llvm::DenseMap<uint32_t, Entry> Local;
};

std::optional<Bits> SymKnownBits::query(SymRef R, unsigned &WorkBudget) {
  if (!R || R.index() >= C.numNodes() || C.width(R) > MaxWidth)
    return std::nullopt;
  const unsigned Available = std::min(WorkBudget, MaxQueryWork);
  Query Q{Available};
  auto Result = infer(R, Q, 0);
  WorkBudget -= Available - Q.Remaining;
  return Result;
}

std::optional<Bits> SymKnownBits::infer(SymRef R, Query &Q, unsigned Depth) {
  auto &Remaining = Q.Remaining;
  if (!spend(Remaining)) {
    ++Q.Truncations;
    return std::nullopt;
  }
  const unsigned W = C.width(R);
  if (W > MaxWidth || !spend(Remaining, words(W))) {
    ++Q.Truncations;
    return std::nullopt;
  }
  if (Depth >= MaxDepth) {
    ++Q.Truncations;
    return Bits(W);
  }
  if (auto I = Cache.find(R.index()); I != Cache.end())
    return I->second;
  const unsigned Offered = Remaining;
  std::optional<Bits> Previous;
  if (auto I = Q.Local.find(R.index()); I != Q.Local.end()) {
    Previous = I->second.Facts;
    if (I->second.Complete ||
        (Offered <= I->second.Available && Depth >= I->second.Depth)) {
      if (!I->second.Complete)
        ++Q.Truncations;
      return I->second.Facts;
    }
  }
  const auto Before = Q.Truncations;
  auto Result = compute(R, Q, Depth);
  if (!Remaining)
    ++Q.Truncations;
  bool Complete = Before == Q.Truncations;
  if (Previous) {
    if (!Result)
      Result = *Previous;
    else {
      Result->Zero |= Previous->Zero;
      Result->One |= Previous->One;
    }
  }
  if (Result) {
    Complete |= Result->isConstant();
    if (Complete && !Result->isUnknown() && Cache.size() < MaxCachedFacts)
      Cache.try_emplace(R.index(), *Result);
    Q.Local.insert_or_assign(R.index(),
                             Query::Entry{*Result, Offered, Depth, Complete});
  }
  return Result;
}

std::optional<Bits> SymKnownBits::compute(SymRef R, Query &Q, unsigned Depth) {
  auto &Remaining = Q.Remaining;
  const unsigned W = C.width(R);
  if (auto K = C.asConst(R))
    return Bits::makeConstant(*K);
  const auto Op = C.op(R);
  switch (Op) {
  case SymOp::Not:
  case SymOp::And:
  case SymOp::Or:
  case SymOp::Xor:
  case SymOp::Add:
  case SymOp::Mul:
  case SymOp::UDiv:
  case SymOp::URem:
  case SymOp::Extract:
  case SymOp::Concat:
  case SymOp::ZExt:
  case SymOp::SExt:
  case SymOp::Shl:
  case SymOp::LShr:
  case SymOp::AShr:
  case SymOp::Ite:
  case SymOp::Eq:
  case SymOp::Ult:
  case SymOp::Ule:
  case SymOp::Slt:
  case SymOp::Sle:
    break;
  default:
    return Bits(W);
  }
  const auto Operands = C.operands(R);
  if (Op == SymOp::ZExt) {
    // The extension's high zeros do not depend on proving its input value.
    // In particular, a wide Boolean carrier remains in {0,1} even when the
    // flag expression producing the predicate exhausts optional query work.
    auto Input = infer(Operands[0], Q, Depth + 1);
    return Input.value_or(Bits(C.width(Operands[0]))).zext(W);
  }
  if (Op == SymOp::Ite) {
    // Branch-common facts need no condition proof. Analyze the arms first so
    // a large predicate cannot erase an already established carrier range.
    auto Taken = infer(Operands[1], Q, Depth + 1);
    if (!Taken)
      return std::nullopt;
    auto Otherwise = infer(Operands[2], Q, Depth + 1);
    if (!Otherwise)
      return std::nullopt;
    auto Common = Taken->intersectWith(*Otherwise);
    auto Condition = infer(Operands[0], Q, Depth + 1);
    if (Condition && Condition->isConstant())
      return Condition->One.isZero() ? *Otherwise : *Taken;
    if (!Condition && Common.isUnknown())
      return std::nullopt;
    return Common;
  }
  if (Op == SymOp::URem) {
    if (!spend(Remaining, words(W)))
      return std::nullopt;
    const auto Divisor = C.operand(R, 1);
    auto Right = infer(Divisor, Q, Depth + 1);
    if (!Right)
      return std::nullopt;
    bool Nonzero = Right->isNonZero();
    if (!Nonzero && C.op(Divisor) == SymOp::Add)
      for (auto Term : C.operands(Divisor))
        if (auto Literal = C.asConst(Term); Literal && !Literal->isZero()) {
          auto NoWrap = nonwrappingAtLeast(Divisor, Term, Q, Depth + 1);
          if (!NoWrap)
            return std::nullopt;
          Nonzero = *NoWrap;
          break;
        }
    auto Left = infer(C.operand(R, 0), Q, Depth + 1);
    const auto Dividend = Left.value_or(Bits(W));
    if (Left && Right->isConstant() && !Right->getConstant().isZero() &&
        Left->getMaxValue().ult(Right->getConstant()))
      return *Left;
    auto Result = Bits::urem(Dividend, *Right);
    // LLVM's transfer excludes division by zero. In the total bitvector
    // domain x urem 0 is x, so retain that case unless nonzero is proved.
    if (!Nonzero)
      Result = Result.intersectWith(Dividend);
    if (!Left && Result.isUnknown())
      return std::nullopt;
    return Result;
  }
  // Reserve the parent transfer before optional child inference. A child's
  // budget exhaustion must not erase bits it already established (e.g. the
  // seven zero bits of a Boolean byte beside unspecified concatenated bytes).
  // The transfer still pays exactly its previous per-word, per-operand cost.
  if (Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
      Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::Concat)
    for (unsigned I = 1; I < Operands.size(); ++I)
      if (!spend(Remaining, words(W)))
        return std::nullopt;
  llvm::SmallVector<Bits, 4> Children;
  for (auto O : C.operands(R)) {
    auto K = infer(O, Q, Depth + 1);
    if (!K && Op != SymOp::Concat)
      return std::nullopt;
    // A refused concatenated lane cannot erase facts in another lane. The
    // parent transfer is already reserved; retain only that child's unknown
    // width and keep the query incomplete so a later request can retry it.
    Children.push_back(K.value_or(Bits(C.width(O))));
  }
  Bits Result = Children.front();
  if (Op == SymOp::Not) {
    std::swap(Result.One, Result.Zero);
    return Result;
  }
  if (Op == SymOp::Extract)
    return Result.extractBits(W, C.node(R).Aux);
  if (Op == SymOp::SExt)
    return Result.sext(W);
  if (Op == SymOp::Shl || Op == SymOp::LShr || Op == SymOp::AShr) {
    if (!Children[1].isConstant())
      return Bits(W);
    const auto &Count = Children[1].One;
    // SymContext shifts are total, and the count may have a different width.
    // LLVM's poison-based shift assumptions must not enter this domain.
    if (Count.uge(W)) {
      if (Op != SymOp::AShr)
        return Bits::makeConstant(llvm::APInt(W, 0));
      return Result.extractBits(1, W - 1).sext(W);
    }
    const unsigned N = Count.getZExtValue();
    if (Op == SymOp::Shl) {
      Result.Zero = Result.Zero.shl(N) | llvm::APInt::getLowBitsSet(W, N);
      Result.One <<= N;
    } else if (Op == SymOp::LShr) {
      Result.Zero = Result.Zero.lshr(N) | llvm::APInt::getHighBitsSet(W, N);
      Result.One = Result.One.lshr(N);
    } else {
      Result.Zero = Result.Zero.ashr(N);
      Result.One = Result.One.ashr(N);
    }
    return Result;
  }
  if (Op == SymOp::UDiv || Op == SymOp::URem) {
    // SymContext division is total: x/0 is all ones and x%0 is x. Only a
    // nonzero literal divisor has the ordinary unsigned meaning LLVM's rules
    // assume. A dividend below the divisor is its own remainder.
    if (Children.size() != 2 || !Children[1].isConstant() ||
        Children[1].getConstant().isZero() ||
        Children[0].getBitWidth() != Children[1].getBitWidth())
      return Bits(W);
    if (Op == SymOp::UDiv)
      return Bits::udiv(Children[0], Children[1]);
    if (Children[0].getMaxValue().ult(Children[1].getConstant()))
      return Children[0];
    return Bits::urem(Children[0], Children[1]);
  }
  if (isPredicate(Op))
    return compare(R, Children, Q, Depth);
  for (unsigned I = 1; I < Children.size(); ++I) {
    switch (Op) {
    case SymOp::And:
      Result &= Children[I];
      break;
    case SymOp::Or:
      Result |= Children[I];
      break;
    case SymOp::Xor:
      Result ^= Children[I];
      break;
    case SymOp::Add:
      Result = Bits::add(Result, Children[I]);
      break;
    case SymOp::Mul:
      Result = Bits::mul(Result, Children[I]);
      break;
    case SymOp::Concat:
      Result = Result.concat(Children[I]);
      break;
    default:
      llvm_unreachable("handled symbolic known-bit operation");
    }
  }
  return Result;
}

std::optional<Bits> SymKnownBits::compare(SymRef R,
                                          llvm::ArrayRef<Bits> Children,
                                          Query &Q, unsigned Depth) {
  auto &Remaining = Q.Remaining;
  const auto Op = C.op(R);
  const auto Ops = C.operands(R);
  const auto &Left = Children[0];
  const auto &Right = Children[1];
  std::optional<bool> Value;
  switch (Op) {
  case SymOp::Eq:
    Value = Bits::eq(Left, Right);
    break;
  case SymOp::Ult:
    Value = Bits::ult(Left, Right);
    break;
  case SymOp::Ule:
    Value = Bits::ule(Left, Right);
    break;
  case SymOp::Slt:
    Value = Bits::slt(Left, Right);
    break;
  case SymOp::Sle:
    Value = Bits::sle(Left, Right);
    break;
  default:
    llvm_unreachable("symbolic comparison");
  }
  if (!Value && Op == SymOp::Eq) {
    for (unsigned Side = 0; Side != 2; ++Side) {
      auto Back = Ops[Side], Source = Ops[1 - Side];
      auto Lossless =
          losslessProjection(C, Back, Source, Children[1 - Side], Remaining);
      if (!Lossless)
        return std::nullopt;
      if (*Lossless) {
        Value = true;
        break;
      }
      const auto Kind = C.op(Back);
      if (Kind != SymOp::ZExt && Kind != SymOp::SExt)
        continue;
      auto Slice = C.operand(Back, 0);
      const unsigned Low = C.width(Slice) - (Kind == SymOp::SExt ? 1 : 0);
      const auto Mask =
          llvm::APInt::getHighBitsSet(C.width(Source), C.width(Source) - Low);
      const auto &Facts = Children[1 - Side];
      if ((Facts.Zero & Mask) == Mask ||
          (Kind == SymOp::SExt && (Facts.One & Mask) == Mask)) {
        // Low extraction distributes through modular arithmetic in the DAG.
        // Check that relation too, without building a second expression.
        auto Same = sameLowBits(Slice, Source, C.width(Slice), Q, Depth + 1);
        if (!Same)
          return std::nullopt;
        if (*Same)
          Value = true;
      }
    }
    for (unsigned Side = 0; !Value && Side != 2; ++Side) {
      auto Signed = Ops[Side], Unsigned = Ops[1 - Side];
      if (C.op(Signed) != SymOp::SExt || C.op(Unsigned) != SymOp::ZExt ||
          C.operand(Signed, 0) != C.operand(Unsigned, 0))
        continue;
      // Only the replicated sign differs. Different roots cannot use this.
      if (Children[Side].Zero.isSignBitSet())
        Value = true;
      if (Children[Side].One.isSignBitSet())
        Value = false;
    }
    if (!Value) {
      auto Same = sameLowBits(Ops[0], Ops[1], C.width(Ops[0]), Q, Depth + 1);
      if (!Same)
        return std::nullopt;
      if (*Same)
        Value = true;
    }
  }
  if (!Value && (Op == SymOp::Ult || Op == SymOp::Ule)) {
    // Known bits cannot retain every interval endpoint (for example, a sum
    // of four independent Boolean words is at most four, not seven). Sum
    // the bounds of immediate terms only when their unsigned maximum cannot
    // wrap. This is a total-operation fact, independent of LLVM annotations.
    auto Bounds = [&](unsigned Side)
        -> std::optional<std::pair<llvm::APInt, llvm::APInt>> {
      auto Coarse =
          std::pair{Children[Side].getMinValue(), Children[Side].getMaxValue()};
      if (C.op(Ops[Side]) != SymOp::Add)
        return Coarse;
      const unsigned W = C.width(Ops[Side]);
      llvm::APInt Minimum(W, 0), Maximum(W, 0);
      for (auto Term : C.operands(Ops[Side])) {
        if (!spend(Remaining, 2 * words(W)))
          return std::nullopt;
        auto K = infer(Term, Q, Depth + 2);
        if (!K)
          return std::nullopt;
        bool Overflow = false;
        Maximum = Maximum.uadd_ov(K->getMaxValue(), Overflow);
        if (Overflow)
          return Coarse;
        Minimum += K->getMinValue();
      }
      return std::pair{Minimum, Maximum};
    };
    auto A = Bounds(0), B = Bounds(1);
    if (!A || !B)
      return std::nullopt;
    if (Op == SymOp::Ult) {
      if (A->second.ult(B->first))
        Value = true;
      else if (A->first.uge(B->second))
        Value = false;
    } else {
      if (A->second.ule(B->first))
        Value = true;
      else if (A->first.ugt(B->second))
        Value = false;
    }
  }
  if (!Value && (Op == SymOp::Ult || Op == SymOp::Ule)) {
    auto Contains =
        nonwrappingAtLeast(Ops[Op == SymOp::Ult ? 0 : 1],
                           Ops[Op == SymOp::Ult ? 1 : 0], Q, Depth + 2);
    if (!Contains)
      return std::nullopt;
    if (*Contains)
      Value = Op == SymOp::Ule;
  }
  return Value ? Bits::makeConstant(llvm::APInt(1, *Value)) : Bits(1);
}

std::optional<bool> SymKnownBits::sameLowBits(SymRef A, SymRef B,
                                              unsigned Width, Query &Q,
                                              unsigned Depth) {
  auto &Remaining = Q.Remaining;
  if (!spend(Remaining, 1 + words(Width)))
    return std::nullopt;
  if (A == B)
    return true;
  if (Depth >= MaxDepth) {
    ++Q.Truncations;
    return false;
  }
  auto Left = lowBitSource(C, A, Width), Right = lowBitSource(C, B, Width);
  if (Left != A || Right != B)
    return sameLowBits(Left, Right, Width, Q, Depth + 1);
  auto IsExtension = [](SymOp Op) {
    return Op == SymOp::ZExt || Op == SymOp::SExt;
  };
  if (IsExtension(C.op(A)) && IsExtension(C.op(B))) {
    auto AInner = C.operand(A, 0), BInner = C.operand(B, 0);
    const unsigned InnerWidth = C.width(AInner);
    if (InnerWidth != C.width(BInner))
      return false;
    auto Same = sameLowBits(AInner, BInner, InnerWidth, Q, Depth + 1);
    if (!Same || !*Same || C.op(A) == C.op(B))
      return Same;
    // Above the shared source width, sign and zero extension agree only when
    // the source sign is proved zero. No LLVM nsw premise is assumed here.
    auto K = infer(AInner, Q, Depth + 1);
    if (!K)
      return std::nullopt;
    return K->Zero.isSignBitSet();
  }
  if (C.isConst(A) && C.isConst(B))
    return C.constValue(A).trunc(Width) == C.constValue(B).trunc(Width);
  if (C.op(A) == SymOp::Mul || C.op(B) == SymOp::Mul)
    return sameLowProduct(A, B, Width, Q, Depth);
  if (C.op(A) != C.op(B))
    return false;
  // These operations commute with reduction modulo 2^Width. Shifts, division,
  // comparisons and high extracts do not, so they cannot use this relation.
  const auto Op = C.op(A);
  if (Op != SymOp::Add && !isBitwise(Op))
    return false;
  const auto As = C.operands(A), Bs = C.operands(B);
  if (As.size() != Bs.size())
    return false;
  if (!spend(Remaining, Bs.size()))
    return std::nullopt;
  llvm::SmallVector<bool, 8> Used(Bs.size(), false);
  for (auto Term : As) {
    bool Found = false;
    for (unsigned I = 0; I < Bs.size(); ++I) {
      if (!spend(Remaining))
        return std::nullopt;
      if (Used[I])
        continue;
      auto Same = sameLowBits(Term, Bs[I], Width, Q, Depth + 1);
      if (!Same)
        return std::nullopt;
      if (*Same) {
        Used[I] = true;
        Found = true;
        break;
      }
    }
    if (!Found)
      return false;
  }
  return true;
}

std::optional<bool> SymKnownBits::sameLowProduct(SymRef A, SymRef B,
                                                 unsigned Width, Query &Q,
                                                 unsigned Depth) {
  auto &Remaining = Q.Remaining;
  llvm::APInt CA(Width, 1), CB(Width, 1);
  using Factor = std::pair<SymRef, unsigned>;
  llvm::SmallVector<Factor, 8> LA, LB;
  // Extensions can hide an associative product from ordinary interning.
  // Flatten only within the requested modular width. A narrower operation
  // stays behind its extension: its overflow cannot be moved to a wider word.
  auto Collect =
      [&](SymRef Root, llvm::APInt &Coefficient,
          llvm::SmallVectorImpl<Factor> &Leaves) -> std::optional<bool> {
    llvm::SmallVector<std::pair<SymRef, unsigned>, 16> Pending{{Root, Depth}};
    while (!Pending.empty()) {
      if (!spend(Remaining, 1 + words(Width)))
        return std::nullopt;
      auto [R, D] = Pending.pop_back_val();
      if (D >= MaxDepth) {
        ++Q.Truncations;
        return false;
      }
      auto P = lowBitSource(C, R, Width);
      if (P != R) {
        Pending.push_back({P, D + 1});
      } else if (C.isConst(R)) {
        Coefficient *= C.constValue(R).zextOrTrunc(Width);
      } else if (C.op(R) == SymOp::Mul) {
        if (!spend(Remaining, C.operands(R).size()))
          return std::nullopt;
        // Keep multiplicity, even for shared terms, and charge before pushing.
        for (auto O : C.operands(R))
          Pending.push_back({O, D + 1});
      } else {
        Leaves.push_back({R, D});
      }
    }
    return true;
  };
  auto GotA = Collect(A, CA, LA);
  if (!GotA || !*GotA)
    return GotA;
  auto GotB = Collect(B, CB, LB);
  if (!GotB || !*GotB)
    return GotB;
  if (CA != CB)
    return false;
  if (CA.isZero())
    return true;
  if (LA.size() != LB.size())
    return false;
  if (!spend(Remaining, LB.size()))
    return std::nullopt;
  llvm::SmallVector<bool, 8> Used(LB.size(), false);
  for (auto [Term, TermDepth] : LA) {
    bool Found = false;
    for (unsigned I = 0; I != LB.size(); ++I) {
      if (!spend(Remaining))
        return std::nullopt;
      if (Used[I])
        continue;
      auto Same = sameLowBits(Term, LB[I].first, Width, Q,
                              std::max(TermDepth, LB[I].second) + 1);
      if (!Same)
        return std::nullopt;
      if (*Same) {
        Used[I] = true;
        Found = true;
        break;
      }
    }
    if (!Found)
      return false;
  }
  return true;
}

std::optional<bool> SymKnownBits::nonwrappingAtLeast(SymRef Sum, SymRef Term,
                                                     Query &Q, unsigned Depth) {
  auto &Remaining = Q.Remaining;
  // Repeated addition is interned as a scalar multiple. Compare unsigned
  // coefficients only for the same base and a proved nonwrapping maximum.
  if (C.op(Sum) == SymOp::Mul || C.op(Term) == SymOp::Mul) {
    const unsigned W = C.width(Sum);
    if (!spend(Remaining, 4 + 2 * words(W)))
      return std::nullopt;
    auto Scale = [&](SymRef R) {
      if (C.op(R) == SymOp::Mul && C.operands(R).size() == 2)
        for (unsigned I = 0; I != 2; ++I)
          if (C.isConst(C.operand(R, I)))
            return std::pair{C.operand(R, 1 - I),
                             C.constValue(C.operand(R, I))};
      return std::pair{R, llvm::APInt(W, 1)};
    };
    auto [A, KA] = Scale(Sum);
    auto [B, KB] = Scale(Term);
    if (A == B && KA.uge(KB)) {
      auto K = infer(A, Q, Depth);
      if (!K)
        return std::nullopt;
      bool Overflow = false;
      (void)KA.umul_ov(K->getMaxValue(), Overflow);
      return !Overflow;
    }
  }
  if (C.op(Sum) != SymOp::Add)
    return false;
  llvm::APInt Maximum(C.width(Sum), 0);
  bool Includes = false;
  const auto SumOps = C.operands(Sum);
  for (auto O : SumOps) {
    auto K = infer(O, Q, Depth);
    if (!K)
      return std::nullopt;
    bool Overflow = false;
    Maximum = Maximum.uadd_ov(K->getMaxValue(), Overflow);
    if (Overflow)
      return false;
    Includes |= O == Term;
  }
  if (Includes || C.op(Term) != SymOp::Add)
    return Includes;
  if (!spend(Remaining, SumOps.size()))
    return std::nullopt;
  llvm::SmallVector<bool, 8> Used(SumOps.size(), false);
  for (auto T : C.operands(Term)) {
    bool Found = false;
    for (unsigned I = 0; I < SumOps.size(); ++I) {
      if (!spend(Remaining))
        return std::nullopt;
      if (!Used[I] && SumOps[I] == T) {
        Used[I] = true;
        Found = true;
        break;
      }
    }
    if (!Found)
      return false;
  }
  return true;
}

} // namespace neverd::symbolic

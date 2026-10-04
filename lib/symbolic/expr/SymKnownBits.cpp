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
} // namespace

std::optional<Bits> SymKnownBits::query(SymRef R, unsigned &WorkBudget) {
  if (!R || R.index() >= C.numNodes() || C.width(R) > MaxWidth)
    return std::nullopt;
  const unsigned Available = std::min(WorkBudget, MaxQueryWork);
  unsigned Remaining = Available;
  auto Result = infer(R, Remaining, 0);
  WorkBudget -= Available - Remaining;
  return Result;
}

std::optional<Bits> SymKnownBits::infer(SymRef R, unsigned &Remaining,
                                        unsigned Depth) {
  if (!spend(Remaining))
    return std::nullopt;
  const unsigned W = C.width(R);
  if (W > MaxWidth || !spend(Remaining, words(W)))
    return std::nullopt;
  if (Depth >= MaxDepth)
    return Bits(W);
  if (auto I = Cache.find(R.index()); I != Cache.end())
    return I->second;
  auto Result = compute(R, Remaining, Depth);
  if (Result && !Result->isUnknown() && Cache.size() < MaxCachedFacts)
    Cache.try_emplace(R.index(), *Result);
  return Result;
}

std::optional<Bits> SymKnownBits::compute(SymRef R, unsigned &Remaining,
                                          unsigned Depth) {
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
  llvm::SmallVector<Bits, 4> Children;
  for (auto O : C.operands(R)) {
    auto K = infer(O, Remaining, Depth + 1);
    if (!K)
      return std::nullopt;
    Children.push_back(*K);
  }
  Bits Result = Children.front();
  if (Op == SymOp::Not) {
    std::swap(Result.One, Result.Zero);
    return Result;
  }
  if (Op == SymOp::Extract)
    return Result.extractBits(W, C.node(R).Aux);
  if (Op == SymOp::ZExt)
    return Result.zext(W);
  if (Op == SymOp::SExt)
    return Result.sext(W);
  if (Op == SymOp::Ite) {
    if (Result.isConstant())
      return Children[Result.One.isZero() ? 2 : 1];
    return Children[1].intersectWith(Children[2]);
  }
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
  if (isPredicate(Op))
    return compare(R, Children, Remaining, Depth);
  for (unsigned I = 1; I < Children.size(); ++I) {
    if (!spend(Remaining, words(W)))
      return std::nullopt;
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
                                          unsigned &Remaining, unsigned Depth) {
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
        auto Same =
            sameLowBits(Slice, Source, C.width(Slice), Remaining, Depth + 1);
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
      auto Same =
          sameLowBits(Ops[0], Ops[1], C.width(Ops[0]), Remaining, Depth + 1);
      if (!Same)
        return std::nullopt;
      if (*Same)
        Value = true;
    }
  }
  if (!Value && (Op == SymOp::Ult || Op == SymOp::Ule)) {
    auto Contains = containsNonwrappingSum(Ops[Op == SymOp::Ult ? 0 : 1],
                                           Ops[Op == SymOp::Ult ? 1 : 0],
                                           Remaining, Depth + 2);
    if (!Contains)
      return std::nullopt;
    if (*Contains)
      Value = Op == SymOp::Ule;
  }
  return Value ? Bits::makeConstant(llvm::APInt(1, *Value)) : Bits(1);
}

std::optional<bool> SymKnownBits::sameLowBits(SymRef A, SymRef B,
                                              unsigned Width,
                                              unsigned &Remaining,
                                              unsigned Depth) {
  if (!spend(Remaining, 1 + words(Width)))
    return std::nullopt;
  if (A == B)
    return true;
  if (Depth >= MaxDepth)
    return false;
  auto Peel = [&](SymRef R) {
    const auto Op = C.op(R);
    if ((Op == SymOp::ZExt || Op == SymOp::SExt) &&
        C.width(C.operand(R, 0)) >= Width)
      return C.operand(R, 0);
    if (Op == SymOp::Extract && C.node(R).Aux == 0)
      return C.operand(R, 0);
    return R;
  };
  auto Left = Peel(A), Right = Peel(B);
  if (Left != A || Right != B)
    return sameLowBits(Left, Right, Width, Remaining, Depth + 1);
  auto IsExtension = [](SymOp Op) {
    return Op == SymOp::ZExt || Op == SymOp::SExt;
  };
  if (IsExtension(C.op(A)) && IsExtension(C.op(B))) {
    auto AInner = C.operand(A, 0), BInner = C.operand(B, 0);
    const unsigned InnerWidth = C.width(AInner);
    if (InnerWidth != C.width(BInner))
      return false;
    auto Same = sameLowBits(AInner, BInner, InnerWidth, Remaining, Depth + 1);
    if (!Same || !*Same || C.op(A) == C.op(B))
      return Same;
    // Above the shared source width, sign and zero extension agree only when
    // the source sign is proved zero. No LLVM nsw premise is assumed here.
    auto K = infer(AInner, Remaining, Depth + 1);
    if (!K)
      return std::nullopt;
    return K->Zero.isSignBitSet();
  }
  if (C.isConst(A) && C.isConst(B))
    return C.constValue(A).trunc(Width) == C.constValue(B).trunc(Width);
  if (C.op(A) != C.op(B))
    return false;
  // These operations commute with reduction modulo 2^Width. Shifts, division,
  // comparisons and high extracts do not, so they cannot use this relation.
  const auto Op = C.op(A);
  if (Op != SymOp::Add && Op != SymOp::Mul && !isBitwise(Op))
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
      auto Same = sameLowBits(Term, Bs[I], Width, Remaining, Depth + 1);
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

std::optional<bool> SymKnownBits::containsNonwrappingSum(SymRef Sum,
                                                         SymRef Term,
                                                         unsigned &Remaining,
                                                         unsigned Depth) {
  if (C.op(Sum) != SymOp::Add)
    return false;
  llvm::APInt Maximum(C.width(Sum), 0);
  bool Includes = false;
  const auto SumOps = C.operands(Sum);
  for (auto O : SumOps) {
    auto K = infer(O, Remaining, Depth);
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

//===- SymExprCompare.cpp - Recover predicates from subtraction signs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymExprCompare.h"

#include "neverd/symbolic/Limits.h"

#include <algorithm>
#include <array>
#include <bit>

namespace neverd::symbolic::detail {
namespace {

struct Term {
  llvm::SmallVector<SymRef, 4> Factors;
  llvm::APInt Coefficient;
};
using Form =
    llvm::SmallVector<Term,
                      ::neverd::limits::symbolic::kSymExprCompareMaxTerms>;

/// Read-only, capped traversal. The Boolean table classifies a sign network;
/// the independent coefficient check establishes its subtraction relation.
class SignMatcher {
  const SymContext &Ctx;
  uint32_t Width;
  llvm::SmallVector<SymRef, ::neverd::limits::symbolic::kSymExprCompareMaxNodes>
      Nodes;
  llvm::SmallVector<SymRef, 3> Leaves;
  unsigned Edges = 0;

  bool chargeEdges(size_t Count) {
    if (Count > ::neverd::limits::symbolic::kSymExprCompareMaxEdges - Edges)
      return false;
    Edges += Count;
    return true;
  }

  bool addTerm(Form &F, SymRef R, bool Negated = false) {
    llvm::APInt Coefficient(Width, 1);
    if (Negated)
      Coefficient.negate();
    llvm::SmallVector<SymRef, 4> Factors;
    if (Ctx.isConst(R)) {
      Coefficient *= Ctx.constValue(R);
    } else if (Ctx.op(R) == SymOp::Mul) {
      auto Ops = Ctx.operands(R);
      if (Ops.size() > ::neverd::limits::symbolic::kSymExprCompareMaxTerms ||
          !chargeEdges(Ops.size()))
        return false;
      for (SymRef Factor : Ops) {
        if (Ctx.isConst(Factor))
          Coefficient *= Ctx.constValue(Factor);
        else
          Factors.push_back(Factor);
      }
    } else {
      Factors.push_back(R);
    }
    // Negating a coefficient of -1 exposes the original additive base again.
    if (Negated && Coefficient.isOne() && Factors.size() == 1 &&
        Ctx.op(Factors[0]) == SymOp::Add)
      return addSum(F, Factors[0]);
    F.push_back({std::move(Factors), std::move(Coefficient)});
    return F.size() <= ::neverd::limits::symbolic::kSymExprCompareMaxTerms;
  }

  bool addSum(Form &F, SymRef R) {
    if (Ctx.op(R) != SymOp::Add)
      return addTerm(F, R);
    auto Ops = Ctx.operands(R);
    if (Ops.size() > ::neverd::limits::symbolic::kSymExprCompareMaxTerms ||
        !chargeEdges(Ops.size()))
      return false;
    for (SymRef Term : Ops)
      if (!addTerm(F, Term))
        return false;
    return true;
  }

  static bool sameFactors(const Term &A, const Term &B) {
    return A.Factors == B.Factors;
  }

  static bool isDifference(const Form &D, const Form &X, const Form &MinusY) {
    // Compare the canonical sum d against x + (-y), without probing builders.
    // Checking each existing key also covers cancellation between x and -y.
    for (const Form *Keys : {&D, &X, &MinusY}) {
      for (const Term &Key : *Keys) {
        llvm::APInt Coefficient(Key.Coefficient.getBitWidth(), 0);
        for (const Term &T : D)
          if (sameFactors(Key, T))
            Coefficient += T.Coefficient;
        for (const Term &T : X)
          if (sameFactors(Key, T))
            Coefficient -= T.Coefficient;
        for (const Term &T : MinusY)
          if (sameFactors(Key, T))
            Coefficient -= T.Coefficient;
        if (!Coefficient.isZero())
          return false;
      }
    }
    return true;
  }

public:
  explicit SignMatcher(const SymContext &C, uint32_t W) : Ctx(C), Width(W) {}

  bool collect(llvm::ArrayRef<SymRef> Roots) {
    llvm::SmallVector<SymRef,
                      ::neverd::limits::symbolic::kSymExprCompareMaxNodes>
        Work(Roots.begin(), Roots.end());
    while (!Work.empty()) {
      SymRef R = Work.pop_back_val();
      if (std::find(Nodes.begin(), Nodes.end(), R) != Nodes.end())
        continue;
      if (Nodes.size() == ::neverd::limits::symbolic::kSymExprCompareMaxNodes ||
          Ctx.width(R) != Width)
        return false;
      Nodes.push_back(R);
      if (Ctx.isConst(R))
        continue;
      if (!isBitwise(Ctx.op(R))) {
        if (Leaves.size() == 3)
          return false;
        Leaves.push_back(R);
        continue;
      }
      auto Ops = Ctx.operands(R);
      if (!chargeEdges(Ops.size()))
        return false;
      Work.append(Ops.begin(), Ops.end());
    }
    if (Leaves.size() != 3 ||
        std::none_of(Leaves.begin(), Leaves.end(),
                     [&](SymRef R) { return Ctx.op(R) == SymOp::Add; }))
      return false;
    std::sort(Nodes.begin(), Nodes.end());
    std::sort(Leaves.begin(), Leaves.end());
    return Leaves.size() == 3;
  }

  struct Match {
    SymRef X, Y;
    bool Signed = false;
    bool Inverted = false;
  };

  std::optional<Match> match(llvm::ArrayRef<SymRef> Roots, SymOp RootOp,
                             bool Constant) {
    if (!collect(Roots))
      return std::nullopt;

    llvm::SmallVector<uint8_t,
                      ::neverd::limits::symbolic::kSymExprCompareMaxNodes>
        Values;
    const auto valueOf = [&](SymRef R) {
      return Values[std::lower_bound(Nodes.begin(), Nodes.end(), R) -
                    Nodes.begin()];
    };
    for (SymRef R : Nodes) {
      uint8_t Value = 0;
      if (R == Leaves[0])
        Value = 0xaa;
      else if (R == Leaves[1])
        Value = 0xcc;
      else if (R == Leaves[2])
        Value = 0xf0;
      else if (Ctx.isConst(R))
        Value = Ctx.constValue(R).isNegative() ? 0xff : 0;
      else {
        const SymOp Op = Ctx.op(R);
        Value = Op == SymOp::And ? 0xff : 0;
        for (SymRef Child : Ctx.operands(R)) {
          uint8_t V = valueOf(Child);
          if (Op == SymOp::And)
            Value &= V;
          else if (Op == SymOp::Or)
            Value |= V;
          else if (Op == SymOp::Xor)
            Value ^= V;
          else
            Value = ~V;
        }
      }
      Values.push_back(Value);
    }
    uint8_t BaseTable = Constant ? 0xff : 0;
    for (SymRef R : Roots) {
      if (RootOp == SymOp::And)
        BaseTable &= valueOf(R);
      else if (RootOp == SymOp::Or)
        BaseTable |= valueOf(R);
      else
        BaseTable ^= valueOf(R);
    }
    // The comparison tables are balanced. In particular, an overflow flag
    // alone cannot qualify, so its common sign test needs no coefficient work.
    if (std::popcount(unsigned(BaseTable)) != 4)
      return std::nullopt;

    std::array<Form, 3> Sums, Negations;
    bool FormsReady = false;
    // The result of a subtraction is interned after its operands. Prefer it
    // when one-bit modular arithmetic admits several valid orientations.
    for (unsigned D = 3; D-- > 0;) {
      if (Ctx.op(Leaves[D]) != SymOp::Add)
        continue;
      for (unsigned X = 0; X < 3; ++X) {
        if (X == D)
          continue;
        unsigned Y = 3 - D - X;
        uint8_t Table = 0;
        for (unsigned I = 0; I < 8; ++I) {
          unsigned Assignment =
              ((I & 1) << X) | (((I >> 1) & 1) << Y) | (((I >> 2) & 1) << D);
          Table |= ((BaseTable >> Assignment) & 1) << I;
        }
        bool Signed = Table == 0xb2 || Table == uint8_t(~0xb2);
        if (!Signed && Table != 0xd4 && Table != uint8_t(~0xd4))
          continue;
        if (!FormsReady) {
          for (unsigned I = 0; I < 3; ++I)
            if (!addSum(Sums[I], Leaves[I]) ||
                !addTerm(Negations[I], Leaves[I], true))
              return std::nullopt;
          FormsReady = true;
        }
        if (isDifference(Sums[D], Sums[X], Negations[Y]))
          return Match{Leaves[X], Leaves[Y], Signed,
                       Table != (Signed ? 0xb2 : 0xd4)};
      }
    }
    return std::nullopt;
  }
};

SymRef materialize(SymContext &Ctx, const SignMatcher::Match &M) {
  SymRef Result = M.Signed ? Ctx.mkSlt(M.X, M.Y) : Ctx.mkUlt(M.X, M.Y);
  return M.Inverted ? Ctx.mkNot(Result) : Result;
}

/// Convert an exact zero-or-one view back to its observed word. No arbitrary
/// cast, lower-bit slice, variable shift, or sign extension qualifies.
SymRef observedWord(const SymContext &Ctx, SymRef R) {
  // Byte-sized lifted booleans are tested against zero before BOOL operations.
  // That round trip preserves an underlying one-bit predicate exactly.
  if (Ctx.op(R) == SymOp::Not && Ctx.op(Ctx.operand(R, 0)) == SymOp::Eq) {
    SymRef Equality = Ctx.operand(R, 0);
    SymRef A = Ctx.operand(Equality, 0), B = Ctx.operand(Equality, 1);
    SymRef Value = Ctx.isConstZero(A) ? B : Ctx.isConstZero(B) ? A : SymRef();
    if (Value && Ctx.op(Value) == SymOp::ZExt &&
        Ctx.width(Ctx.operand(Value, 0)) == 1)
      R = Ctx.operand(Value, 0);
  }
  if (Ctx.op(R) == SymOp::ZExt)
    R = Ctx.operand(R, 0);
  if (Ctx.op(R) == SymOp::Extract && Ctx.width(R) == 1) {
    SymRef Word = Ctx.operand(R, 0);
    if (Ctx.node(R).Aux == Ctx.width(Word) - 1)
      return Word;
  }
  if (Ctx.op(R) == SymOp::Slt && Ctx.isConstZero(Ctx.operand(R, 1)))
    return Ctx.operand(R, 0);
  if (Ctx.op(R) == SymOp::LShr) {
    SymRef Word = Ctx.operand(R, 0), Count = Ctx.operand(R, 1);
    if (Ctx.isConst(Count) && Ctx.constValue(Count) == Ctx.width(Word) - 1)
      return Word;
  }
  return Ctx.width(R) == 1 ? R : SymRef();
}

} // namespace

SymRef recoverPackedEquality(SymContext &Ctx, SymRef Word, SymRef Literal) {
  if (!Ctx.isConst(Literal))
    return {};
  const unsigned Width = Ctx.width(Word);
  auto Observed = llvm::APInt::getAllOnes(Width);
  if (Ctx.op(Word) == SymOp::And && Ctx.numOperands(Word) == 2 &&
      Ctx.isConst(Ctx.operand(Word, 0))) {
    Observed = Ctx.constValue(Ctx.operand(Word, 0));
    Word = Ctx.operand(Word, 1);
  }
  if (Ctx.op(Word) != SymOp::Or)
    return {};
  llvm::APInt Forced(Width, 0), Used(Width, 0);
  struct Lane {
    SymRef Predicate;
    llvm::APInt Bit;
  };
  llvm::SmallVector<Lane, 8> Lanes;
  for (auto Value : Ctx.operands(Word)) {
    if (Ctx.isConst(Value)) {
      Forced |= Ctx.constValue(Value) & Observed;
      continue;
    }
    llvm::APInt Bit(Width, 1);
    if (Ctx.op(Value) == SymOp::Mul && Ctx.numOperands(Value) == 2 &&
        Ctx.isConst(Ctx.operand(Value, 0))) {
      Bit = Ctx.constValue(Ctx.operand(Value, 0));
      Value = Ctx.operand(Value, 1);
    }
    if (Ctx.op(Value) == SymOp::ZExt)
      Value = Ctx.operand(Value, 0);
    if (Ctx.width(Value) != 1 || !Bit.isPowerOf2())
      return {};
    Bit &= Observed;
    if (Bit.isZero())
      continue;
    if (!(Used & Bit).isZero())
      return {};
    Used |= Bit;
    Lanes.push_back({Value, std::move(Bit)});
  }
  if (!(Used & Forced).isZero())
    return {};
  const auto Target = Ctx.constValue(Literal);
  if ((Target & Forced) != Forced || !(Target & ~(Used | Forced)).isZero())
    return Ctx.mkFalse();
  llvm::SmallVector<SymRef, 8> Conditions;
  for (const auto &L : Lanes)
    Conditions.push_back((Target & L.Bit).isZero() ? Ctx.mkNot(L.Predicate)
                                                   : L.Predicate);
  return Conditions.empty() ? Ctx.mkTrue() : Ctx.mkAnd(Conditions);
}

SymRef recoverSignComparison(SymContext &Ctx, SymRef Word) {
  if (!isBitwise(Ctx.op(Word)))
    return {};
  // Two pairwise XORs sharing one opaque source describe an overflow-only
  // network. Its two true rows cannot match either balanced comparison table.
  if (Ctx.op(Word) == SymOp::And && Ctx.numOperands(Word) == 2) {
    SymRef A = Ctx.operand(Word, 0), B = Ctx.operand(Word, 1);
    if (Ctx.op(A) == SymOp::Xor && Ctx.op(B) == SymOp::Xor &&
        Ctx.numOperands(A) == 2 && Ctx.numOperands(B) == 2) {
      auto Left = Ctx.operands(A), Right = Ctx.operands(B);
      bool Opaque = true, Shared = false;
      for (SymRef R : Left)
        Opaque &= !isBitwise(Ctx.op(R));
      for (SymRef R : Right) {
        Opaque &= !isBitwise(Ctx.op(R));
        Shared |= R == Left[0] || R == Left[1];
      }
      if (Opaque && Shared)
        return {};
    }
  }
  SignMatcher Matcher(Ctx, Ctx.width(Word));
  auto M = Matcher.match({Word}, SymOp::Xor, false);
  return M ? materialize(Ctx, *M) : SymRef();
}

SymRef recoverObservedComparison(SymContext &Ctx, SymOp Op,
                                 llvm::ArrayRef<SymRef> Operands,
                                 const llvm::APInt &Constant) {
  const uint32_t OutputWidth = Constant.getBitWidth();
  if (Operands.size() < 2 ||
      Operands.size() > ::neverd::limits::symbolic::kSymExprCompareMaxTerms ||
      (OutputWidth != 1 && Op != SymOp::Xor) ||
      (!Constant.isZero() && !Constant.isOne()))
    return {};
  llvm::SmallVector<SymRef, ::neverd::limits::symbolic::kSymExprCompareMaxTerms>
      Roots;
  for (SymRef R : Operands) {
    SymRef Word = observedWord(Ctx, R);
    if (!Word || (!Roots.empty() && Ctx.width(Word) != Ctx.width(Roots[0])))
      return {};
    Roots.push_back(Word);
  }
  SignMatcher Matcher(Ctx, Ctx.width(Roots[0]));
  auto M = Matcher.match(Roots, Op, !Constant.isZero());
  return M ? Ctx.mkZExt(materialize(Ctx, *M), OutputWidth) : SymRef();
}

} // namespace neverd::symbolic::detail

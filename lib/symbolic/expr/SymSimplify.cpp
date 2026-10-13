//===- SymSimplify.cpp - Bounded symbolic expression normalization --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymSimplify.h"

#include "neverd/symbolic/SymKnownBits.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace neverd::symbolic {
namespace {
SymRef slice(SymContext &C, SymRef Value) {
  if (C.op(Value) != SymOp::Extract)
    return Value;
  const unsigned Low = C.node(Value).Aux, Width = C.width(Value);
  const auto Source = C.operand(Value, 0);
  const auto Op = C.op(Source);
  if (Op == SymOp::Ite) {
    const auto Condition = C.operand(Source, 0);
    const auto Yes = C.operand(Source, 1), No = C.operand(Source, 2);
    return C.mkIte(Condition, C.mkExtract(Yes, Low, Width),
                   C.mkExtract(No, Low, Width));
  }
  if (Op == SymOp::ZExt || Op == SymOp::SExt) {
    auto Inner = C.operand(Source, 0);
    const unsigned Narrow = C.width(Inner);
    if (Low >= Narrow)
      return Op == SymOp::ZExt
                 ? C.mkZero(Width)
                 : C.mkSExt(C.mkExtract(Inner, Narrow - 1, 1), Width);
    auto Part = C.mkExtract(Inner, Low, std::min(Width, Narrow - Low));
    return Op == SymOp::ZExt ? C.mkZExtOrTrunc(Part, Width)
                             : C.mkSExt(Part, Width);
  }
  if ((Op == SymOp::LShr || Op == SymOp::AShr) &&
      C.isConst(C.operand(Source, 1))) {
    auto Inner = C.operand(Source, 0);
    const unsigned Bits = C.width(Inner);
    auto Count = C.constValue(C.operand(Source, 1));
    if (Count.uge(Bits))
      return Op == SymOp::LShr
                 ? C.mkZero(Width)
                 : C.mkSExt(C.mkExtract(Inner, Bits - 1, 1), Width);
    const unsigned Shift = unsigned(Count.getZExtValue());
    if (Low >= Bits - Shift)
      return Op == SymOp::LShr
                 ? C.mkZero(Width)
                 : C.mkSExt(C.mkExtract(Inner, Bits - 1, 1), Width);
    auto Part =
        C.mkExtract(Inner, Low + Shift, std::min(Width, Bits - Shift - Low));
    return Op == SymOp::LShr ? C.mkZExtOrTrunc(Part, Width)
                             : C.mkSExt(Part, Width);
  }
  // A constant left shift is represented by the arithmetic builder as a
  // multiply by a power of two. Other products must retain their carries.
  if (Op == SymOp::Mul && C.numOperands(Source) == 2 &&
      C.isConst(C.operand(Source, 0))) {
    auto Factor = C.constValue(C.operand(Source, 0));
    if (Factor.isPowerOf2()) {
      const unsigned Shift = Factor.logBase2();
      if (Low < Shift && Width <= Shift - Low)
        return C.mkZero(Width);
      const unsigned Padding = Low < Shift ? Shift - Low : 0;
      auto Part = C.mkExtract(C.operand(Source, 1),
                              Low < Shift ? 0 : Low - Shift, Width - Padding);
      return Padding ? C.mkConcat(Part, C.mkZero(Padding)) : Part;
    }
  }
  // Unlike arithmetic, a bitwise slice cannot observe a carry from below it.
  if ((Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
       Op == SymOp::Not) &&
      C.numOperands(Source) <= 32) {
    // Builders can grow the operand arena. Own the input list before any
    // extraction allocates new nodes.
    const std::vector<SymRef> Children(C.operands(Source).begin(),
                                       C.operands(Source).end());
    std::vector<SymRef> Parts;
    for (auto Child : Children)
      Parts.push_back(C.mkExtract(Child, Low, Width));
    if (Op == SymOp::Not)
      return C.mkNot(Parts[0]);
    if (Op == SymOp::And)
      return C.mkAnd(Parts);
    if (Op == SymOp::Or)
      return C.mkOr(Parts);
    return C.mkXor(Parts);
  }
  return Value;
}

SymRef linearSum(SymContext &C, SymRef Value, unsigned &Work) {
  if ((C.op(Value) != SymOp::Add &&
       !(C.op(Value) == SymOp::Not &&
         C.op(C.operand(Value, 0)) == SymOp::Add)) ||
      C.width(Value) > 512)
    return Value;
  const unsigned Width = C.width(Value);
  llvm::APInt Constant(Width, 0);
  std::map<SymRef, llvm::APInt> Terms;
  std::vector<std::pair<SymRef, llvm::APInt>> Pending;
  Pending.emplace_back(Value, llvm::APInt(Width, 1));
  unsigned Visits = 0;
  while (!Pending.empty()) {
    if (!Work || ++Visits > 64)
      return Value;
    --Work;
    auto [Term, Coefficient] = std::move(Pending.back());
    Pending.pop_back();
    if (C.isConst(Term)) {
      Constant += Coefficient * C.constValue(Term);
      continue;
    }
    if (C.op(Term) == SymOp::Add && C.numOperands(Term) <= 16) {
      for (auto Child : C.operands(Term))
        Pending.emplace_back(Child, Coefficient);
      continue;
    }
    if (C.op(Term) == SymOp::Not) {
      Constant -= Coefficient;
      Pending.emplace_back(C.operand(Term, 0), -Coefficient);
      continue;
    }
    if (C.op(Term) == SymOp::Mul && C.numOperands(Term) == 2 &&
        C.isConst(C.operand(Term, 0))) {
      Pending.emplace_back(C.operand(Term, 1),
                           Coefficient * C.constValue(C.operand(Term, 0)));
      continue;
    }
    auto [At, Added] = Terms.try_emplace(Term, Coefficient);
    if (!Added)
      At->second += Coefficient;
  }
  std::vector<SymRef> Sum;
  if (!Constant.isZero())
    Sum.push_back(C.mkConst(Constant));
  for (const auto &[Term, Coefficient] : Terms)
    if (!Coefficient.isZero())
      Sum.push_back(C.mkMul(C.mkConst(Coefficient), Term));
  auto Result = Sum.empty() ? C.mkZero(Width) : C.mkAdd(Sum);
  // Keep the cheaper polarity of an affine word. This makes C-X-Y and
  // ~(~C+X+Y) share their compound inputs across arithmetic/bitwise uses.
  if (!Terms.empty()) {
    std::vector<SymRef> Negated;
    if (!Constant.isAllOnes())
      Negated.push_back(C.mkConst(~Constant));
    for (const auto &[Term, Coefficient] : Terms)
      if (!Coefficient.isZero())
        Negated.push_back(C.mkMul(C.mkConst(-Coefficient), Term));
    auto Other = C.mkNot(Negated.empty() ? C.mkZero(Width) : C.mkAdd(Negated));
    if (C.dagSize(Other) < C.dagSize(Result))
      Result = Other;
  }
  return C.dagSize(Result) <= C.dagSize(Value) ? Result : Value;
}

SymRef booleanArithmetic(SymContext &C, SymRef Value, unsigned &Work) {
  if (C.op(Value) == SymOp::Not) {
    auto Source = C.operand(Value, 0);
    const auto Op = C.op(Source);
    if ((Op == SymOp::And || Op == SymOp::Or) && C.numOperands(Source) <= 8) {
      const std::vector<SymRef> Children(C.operands(Source).begin(),
                                         C.operands(Source).end());
      std::vector<SymRef> Parts;
      for (auto Child : Children)
        Parts.push_back(linearSum(C, C.mkNot(Child), Work));
      auto Other = Op == SymOp::And ? C.mkOr(Parts) : C.mkAnd(Parts);
      if (C.dagSize(Other) <= C.dagSize(Value))
        Value = Other;
    }
  }
  if (C.op(Value) == SymOp::Add && C.numOperands(Value) <= 16) {
    std::vector<SymRef> Terms(C.operands(Value).begin(),
                              C.operands(Value).end());
    for (unsigned I = 0; I < Terms.size(); ++I)
      for (unsigned J = I + 1; J < Terms.size(); ++J) {
        auto A = Terms[I], B = Terms[J];
        if (C.op(A) == SymOp::And)
          std::swap(A, B);
        if (C.op(A) != SymOp::Or || C.op(B) != SymOp::And ||
            C.numOperands(A) != 2 || C.numOperands(B) != 2 ||
            C.operands(A) != C.operands(B))
          continue;
        // (X OR Y) + (X AND Y) equals X + Y at every bit width, including
        // carries. Match the complete pair, not merely overlapping inputs.
        const auto X = C.operand(A, 0), Y = C.operand(A, 1);
        Terms[I] = C.mkAdd(X, Y);
        Terms.erase(Terms.begin() + J);
        return C.mkAdd(Terms);
      }
  }
  if (C.op(Value) == SymOp::Add && C.numOperands(Value) == 2) {
    const auto A = C.operand(Value, 0), B = C.operand(Value, 1);
    for (auto [Part, Difference] : {std::pair{A, B}, std::pair{B, A}}) {
      if (C.op(Difference) != SymOp::Xor || C.numOperands(Difference) != 2)
        continue;
      const auto Left = C.operand(Difference, 0),
                 Right = C.operand(Difference, 1);
      auto Whole = Left == Part ? Right : Right == Part ? Left : SymRef{};
      // Part is a bit subset of Whole, so Part and (Part XOR Whole) have
      // disjoint one bits. Their sum cannot introduce a carry.
      if (Whole && C.mkAnd(Part, Whole) == Part)
        return Whole;
    }
  }
  return Value;
}

SymRef rewrite(SymContext &C, SymKnownBits &Known, SymRef Value,
               unsigned &Work) {
  Value = slice(C, Value);
  Value = linearSum(C, Value, Work);
  Value = booleanArithmetic(C, Value, Work);
  if (C.isConst(Value) || C.isVar(Value))
    return Value;
  if (auto Bits = Known.query(Value, Work); Bits && Bits->isConstant())
    return C.mkConst(Bits->One);
  const auto Op = C.op(Value);
  if ((Op == SymOp::And || Op == SymOp::Or) && C.numOperands(Value) == 2 &&
      C.isConst(C.operand(Value, 0))) {
    auto Source = C.operand(Value, 1);
    auto Bits = Known.query(Source, Work);
    const auto Mask = C.constValue(C.operand(Value, 0));
    if (Op == SymOp::And && Bits && (Mask | Bits->Zero).isAllOnes())
      return Source;
    if (Op == SymOp::And && Mask.isMask() && !Mask.isAllOnes()) {
      auto Part = slice(C, C.mkExtract(Source, 0, Mask.popcount()));
      return C.mkZExt(Part, C.width(Value));
    }
    if (Op == SymOp::Or && Bits) {
      const auto Missing = Mask & ~Bits->One;
      if (Missing.isZero())
        return Source;
      if ((Missing & ~Bits->Zero).isZero())
        return C.mkAdd(Source, C.mkConst(Missing));
    }
  }
  return Value;
}
} // namespace

SymRef simplifyExpression(SymContext &C, SymRef Root,
                          ExpressionSimplifyOptions Options) {
  if (!Root || Root.index() >= C.numNodes())
    throw std::invalid_argument("invalid symbolic normalization root");
  unsigned Work = Options.Work;
  SymKnownBits Known(C);
  for (unsigned Round = 0; Round < Options.Rounds && Work; ++Round) {
    std::set<uint32_t> Inventory;
    std::vector<SymRef> Pending{Root};
    uint64_t Edges = 0;
    // Arena indices are topological. Inventory completely before rebuilding;
    // a deep or oversized DAG never invokes recursive optimizer traversal.
    while (!Pending.empty()) {
      if (!Work)
        return Root;
      --Work;
      auto Value = Pending.back();
      Pending.pop_back();
      if (!Inventory.insert(Value.index()).second)
        continue;
      auto Children = C.operands(Value);
      Edges += Children.size();
      if (Inventory.size() > Options.Nodes || Edges > Options.Edges)
        return Root;
      Pending.insert(Pending.end(), Children.begin(), Children.end());
    }
    std::map<uint32_t, SymRef> Rebuilt;
    for (auto Index : Inventory) {
      auto Value = SymRef(Index);
      std::vector<SymRef> Children;
      for (auto Child : C.operands(Value))
        Children.push_back(Rebuilt.at(Child.index()));
      auto Current = Children.empty() ? Value : C.rebuild(Value, Children);
      // The complete inventory bounds the remaining assembly. Exhaustion
      // stops optional rewrites, not propagation of already proved children.
      // Returning the old root here discards all completed reductions.
      if (Work) {
        --Work;
        Current = rewrite(C, Known, Current, Work);
      }
      Rebuilt.emplace(Index, Current);
    }
    auto Result = Rebuilt.at(Root.index());
    if (Result == Root)
      break;
    Root = Result;
  }
  return Root;
}
} // namespace neverd::symbolic

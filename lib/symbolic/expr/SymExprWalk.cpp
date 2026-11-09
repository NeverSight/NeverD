//===- SymExprWalk.cpp - Traversal, inspection and substitution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the queries that walk an interned expression — DAG size, reading
/// cost and variable collection — together with substitution and the operator
/// re-application both substitution and the rewriters are built on.
///
//===----------------------------------------------------------------------===//

#include "../SymPrintShape.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace neverd::symbolic {

//===----------------------------------------------------------------------===//
// Inspection
//===----------------------------------------------------------------------===//

size_t SymContext::dagSize(SymRef R) const {
  llvm::SmallDenseSet<uint32_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    SymRef Cur = Work.pop_back_val();
    if (!Seen.insert(Cur.index()).second)
      continue;
    for (SymRef C : operands(Cur))
      Work.push_back(C);
  }
  return Seen.size();
}

size_t SymContext::readabilityCost(SymRef R) const {
  return readability(R).Nodes;
}

SymReadability SymContext::readability(SymRef R) const {
  constexpr size_t Ceiling = std::numeric_limits<size_t>::max();
  auto Add = [](size_t A, size_t B) {
    return B > Ceiling - A ? Ceiling : A + B;
  };

  // Interning appends a node only after all of its operands exist.  Extending
  // one prefix cache therefore computes every new cost in a single pass, even
  // when a solver asks again after interning more candidates.
  // Let the vector grow geometrically: reserving the exact current prefix on
  // every query makes interleaved construction and inspection quadratic.
  while (ReadabilityCosts.size() < Nodes.size()) {
    SymRef Current(static_cast<uint32_t>(ReadabilityCosts.size()));
    llvm::ArrayRef<SymRef> Ops = operands(Current);
    const SymOp Op = op(Current);
    const bool InfixChain = Op == SymOp::Add || Op == SymOp::Mul ||
                            Op == SymOp::And || Op == SymOp::Or ||
                            Op == SymOp::Xor;
    size_t Own = InfixChain ? Ops.size() - 1 : 1;
    // Not(Eq) is the single printed inequality operation.
    if (Op == SymOp::Not && op(Ops[0]) == SymOp::Eq)
      Own = 0;
    SymReadability Total{Own, Ops.empty() ? 0 : Own};
    const unsigned Lead =
        Op == SymOp::Add ? print_detail::leadingAddOperand(*this, Ops) : 0;
    const bool UnitMinus = print_detail::hasUnaryMinus(*this, Current);
    for (unsigned I = 0; I < Ops.size(); ++I) {
      SymRef Operand = Ops[I];
      assert(Operand.index() < ReadabilityCosts.size() &&
             "a symbolic node precedes one of its operands");
      if (I == 0 && UnitMinus)
        continue;
      SymReadability Child = ReadabilityCosts[Operand.index()];
      // The parent's subtraction takes the place of this unary sign. Adjust
      // before addition; subtracting from a saturated sum would undercount it.
      if (Op == SymOp::Add && I != Lead &&
          print_detail::hasUnaryMinus(*this, Operand) &&
          Child.Nodes != Ceiling) {
        --Child.Nodes;
        --Child.Operations;
      }
      Total.Nodes = Add(Total.Nodes, Child.Nodes);
      Total.Operations = Add(Total.Operations, Child.Operations);
    }
    if (Total.Nodes == Ceiling)
      Total.Operations = Ceiling;
    ReadabilityCosts.push_back(Total);
  }
  return ReadabilityCosts[R.index()];
}

uint32_t SymContext::minimumVariableId(SymRef R) const {
  while (MinimumVariables.size() <= R.index()) {
    const auto Current = SymRef(uint32_t(MinimumVariables.size()));
    uint32_t Minimum = std::numeric_limits<uint32_t>::max();
    if (isVar(Current))
      Minimum = varId(Current);
    else
      for (const auto Child : operands(Current))
        Minimum = std::min(Minimum, MinimumVariables[Child.index()]);
    MinimumVariables.push_back(Minimum);
  }
  return MinimumVariables[R.index()];
}

bool SymContext::containsVariable(SymRef R, uint32_t VarIdx) const {
  if (VarIdx >= Vars.size())
    return false;
  const auto Minimum = minimumVariableId(R);
  if (Minimum >= VarIdx)
    return Minimum == VarIdx;
  llvm::SmallDenseSet<uint32_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    const auto Current = Work.pop_back_val();
    const auto Least = MinimumVariables[Current.index()];
    if (Least == VarIdx)
      return true;
    if (Least > VarIdx || !Seen.insert(Current.index()).second)
      continue;
    const auto Children = operands(Current);
    Work.append(Children.begin(), Children.end());
  }
  return false;
}

void SymContext::collectVars(SymRef R,
                             llvm::SmallVectorImpl<uint32_t> &Out) const {
  llvm::SmallDenseSet<uint32_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    SymRef Cur = Work.pop_back_val();
    if (!Seen.insert(Cur.index()).second)
      continue;
    if (op(Cur) == SymOp::Var) {
      Out.push_back(varId(Cur));
      continue;
    }
    for (SymRef C : operands(Cur))
      Work.push_back(C);
  }
  std::sort(Out.begin(), Out.end());
  Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
}

//===----------------------------------------------------------------------===//
// Substitution
//===----------------------------------------------------------------------===//

SymRef SymContext::substitute(SymRef R,
                              const std::unordered_map<uint32_t, SymRef> &Map) {
  return substituteAll({R}, Map).front();
}

std::vector<SymRef>
SymContext::substituteAll(llvm::ArrayRef<SymRef> Roots,
                          const std::unordered_map<uint32_t, SymRef> &Map) {
  // Builders can grow operand storage while any root is rebuilt.
  std::vector<SymRef> Results(Roots.begin(), Roots.end());
  if (Map.empty())
    return Results;
  // Interned children always precede their parents. A subtree created before
  // every replacement key cannot contain a replacement, even when the key is
  // an interior expression rather than a variable. In particular, restoring
  // MBA placeholders need not revisit the original, unchanged input DAGs.
  uint32_t FirstReplacement = std::numeric_limits<uint32_t>::max();
  for (const auto &[Index, Value] : Map)
    FirstReplacement = std::min(FirstReplacement, Index);
  std::optional<uint32_t> ReplacedVariable;
  if (Map.size() == 1) {
    const auto Key = Map.begin()->first;
    if (Key < Nodes.size() && isVar(SymRef(Key)))
      ReplacedVariable = varId(SymRef(Key));
  }

  // Rebuild bottom-up over a topological order.  Going through the builders
  // means the result is canonical, so a substitution that makes two subterms
  // equal has that equality recognised immediately.
  llvm::SmallDenseMap<uint32_t, SymRef,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Done;

  llvm::SmallVector<SymRef, 8> NewOps;
  llvm::SmallVector<std::pair<SymRef, bool>, 32> Work;
  for (SymRef R : Results)
    if (R.index() >= FirstReplacement)
      Work.emplace_back(R, false);
  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (!Expanded) {
      // A null result marks a scheduled node. Check replacements only on
      // this first visit; replacement values themselves are not rewritten.
      auto [Position, Inserted] = Done.try_emplace(Cur.index(), SymRef());
      if (!Inserted)
        continue;
      if (auto Hit = Map.find(Cur.index()); Hit != Map.end()) {
        Position->second = Hit->second;
        continue;
      }
      // Every variable here follows the replaced one. Use the cached lower
      // bound only on this first visit; a membership walk per node would be
      // quadratic on a chain. Uncertain subtrees are rebuilt once below.
      if (ReplacedVariable && minimumVariableId(Cur) > *ReplacedVariable) {
        Position->second = Cur;
        continue;
      }
      if (numOperands(Cur) == 0) {
        Position->second = Cur;
        continue;
      }
      Work.emplace_back(Cur, true);
      for (SymRef C : operands(Cur))
        if (C.index() >= FirstReplacement)
          Work.emplace_back(C, false);
      continue;
    }

    NewOps.clear();
    NewOps.reserve(numOperands(Cur));
    bool Changed = false;
    for (SymRef C : operands(Cur)) {
      SymRef NC =
          C.index() < FirstReplacement ? C : Done.find(C.index())->second;
      Changed |= (NC != C);
      NewOps.push_back(NC);
    }
    Done.find(Cur.index())->second = Changed ? rebuild(Cur, NewOps) : Cur;
  }

  for (SymRef &R : Results)
    if (R.index() >= FirstReplacement)
      R = Done.find(R.index())->second;
  return Results;
}

SymRef SymContext::substituteVar(SymRef R, uint32_t VarIdx, SymRef Val) {
  if (!containsVariable(R, VarIdx))
    return R;

  SymRef VarRef = intern(SymOp::Var, Vars[VarIdx].Width, {}, VarIdx);
  std::unordered_map<uint32_t, SymRef> Map;
  Map.emplace(VarRef.index(), Val);
  return substitute(R, Map);
}

SymRef SymContext::rebuild(SymRef Orig, llvm::ArrayRef<SymRef> NewOps) {
  // Flat sums and products have already collected their terms and factors.
  // Other builders can defer work (for example, merged masks), even when
  // their edges are unchanged.
  if ((op(Orig) == SymOp::Add || op(Orig) == SymOp::Mul) &&
      NewOps == operands(Orig))
    return Orig;
  // Copied rather than referenced: every builder below can intern, which grows
  // the node vector and would invalidate a reference into it.
  const SymNode N = Nodes[Orig.index()];
  switch (N.Op) {
  case SymOp::Const:
  case SymOp::Var:
    return Orig;
  case SymOp::Add:
    return mkAdd(NewOps);
  case SymOp::Mul:
    return mkMul(NewOps);
  case SymOp::And:
    return mkAnd(NewOps);
  case SymOp::Or:
    return mkOr(NewOps);
  case SymOp::Xor:
    return mkXor(NewOps);
  case SymOp::Not:
    return mkNot(NewOps[0]);
  case SymOp::Shl:
    return mkShl(NewOps[0], NewOps[1]);
  case SymOp::LShr:
    return mkLShr(NewOps[0], NewOps[1]);
  case SymOp::AShr:
    return mkAShr(NewOps[0], NewOps[1]);
  case SymOp::UDiv:
    return mkUDiv(NewOps[0], NewOps[1]);
  case SymOp::SDiv:
    return mkSDiv(NewOps[0], NewOps[1]);
  case SymOp::URem:
    return mkURem(NewOps[0], NewOps[1]);
  case SymOp::SRem:
    return mkSRem(NewOps[0], NewOps[1]);
  case SymOp::Rol:
    return mkRol(NewOps[0], NewOps[1]);
  case SymOp::Ror:
    return mkRor(NewOps[0], NewOps[1]);
  case SymOp::Extract:
    return mkExtract(NewOps[0], static_cast<uint32_t>(N.Aux), N.Width);
  case SymOp::Concat:
    return mkConcat(NewOps);
  case SymOp::ZExt:
    return mkZExt(NewOps[0], N.Width);
  case SymOp::SExt:
    return mkSExt(NewOps[0], N.Width);
  case SymOp::Ite:
    return mkIte(NewOps[0], NewOps[1], NewOps[2]);
  case SymOp::Eq:
    return mkEq(NewOps[0], NewOps[1]);
  case SymOp::Ult:
    return mkUlt(NewOps[0], NewOps[1]);
  case SymOp::Ule:
    return mkUle(NewOps[0], NewOps[1]);
  case SymOp::Slt:
    return mkSlt(NewOps[0], NewOps[1]);
  case SymOp::Sle:
    return mkSle(NewOps[0], NewOps[1]);
  }
  llvm_unreachable("unhandled SymOp in rebuild");
}

} // namespace neverd::symbolic

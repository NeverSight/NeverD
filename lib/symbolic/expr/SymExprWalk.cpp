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

#include "neverd/symbolic/SymExpr.h"

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
  llvm::DenseSet<uint32_t> Seen;
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
  constexpr size_t Ceiling = std::numeric_limits<size_t>::max();

  // Interning appends a node only after all of its operands exist.  Extending
  // one prefix cache therefore computes every new cost in a single pass, even
  // when a solver asks again after interning more candidates.
  ReadabilityCosts.reserve(Nodes.size());
  while (ReadabilityCosts.size() < Nodes.size()) {
    SymRef Current(static_cast<uint32_t>(ReadabilityCosts.size()));
    size_t Total = isConst(Current) && isConstOnes(Current) ? 0 : 1;
    for (SymRef Operand : operands(Current)) {
      assert(Operand.index() < ReadabilityCosts.size() &&
             "a symbolic node precedes one of its operands");
      const size_t OperandCost = ReadabilityCosts[Operand.index()];
      if (OperandCost > Ceiling - Total) {
        Total = Ceiling;
        break;
      }
      Total += OperandCost;
    }
    ReadabilityCosts.push_back(Total);
  }
  return ReadabilityCosts[R.index()];
}

void SymContext::collectVars(SymRef R,
                             llvm::SmallVectorImpl<uint32_t> &Out) const {
  llvm::DenseSet<uint32_t> Seen;
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
  if (Map.empty())
    return R;

  // Rebuild bottom-up over a topological order.  Going through the builders
  // means the result is canonical, so a substitution that makes two subterms
  // equal has that equality recognised immediately.
  std::unordered_map<uint32_t, SymRef> Done;

  llvm::SmallVector<std::pair<SymRef, bool>, 32> Work{{R, false}};
  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (Done.count(Cur.index()))
      continue;

    auto Hit = Map.find(Cur.index());
    if (Hit != Map.end()) {
      Done.emplace(Cur.index(), Hit->second);
      continue;
    }

    if (!Expanded) {
      Work.emplace_back(Cur, true);
      for (SymRef C : operands(Cur))
        if (!Done.count(C.index()))
          Work.emplace_back(C, false);
      continue;
    }

    llvm::SmallVector<SymRef, 8> NewOps;
    bool Changed = false;
    for (SymRef C : operands(Cur)) {
      SymRef NC = Done.at(C.index());
      Changed |= (NC != C);
      NewOps.push_back(NC);
    }
    if (!Changed) {
      Done.emplace(Cur.index(), Cur);
      continue;
    }
    Done.emplace(Cur.index(), rebuild(Cur, NewOps));
  }

  return Done.at(R.index());
}

SymRef SymContext::substituteVar(SymRef R, uint32_t VarIdx, SymRef Val) {
  llvm::SmallVector<uint32_t, 8> Seen;
  collectVars(R, Seen);
  if (std::find(Seen.begin(), Seen.end(), VarIdx) == Seen.end())
    return R;

  SymRef VarRef = intern(SymOp::Var, Vars[VarIdx].Width, {}, VarIdx);
  std::unordered_map<uint32_t, SymRef> Map;
  Map.emplace(VarRef.index(), Val);
  return substitute(R, Map);
}

SymRef SymContext::rebuild(SymRef Orig, llvm::ArrayRef<SymRef> NewOps) {
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

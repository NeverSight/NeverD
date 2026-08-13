//===- DeadStoreAnalysis.cpp - Dead store elimination -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead store analysis and variable liveness for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

namespace {

struct ReducedAddr {
  MedVar Base;
  int64_t Offset = 0;
};

struct ExprDef {
  MedVar Var;
  const HighExpr *Value = nullptr;
};

template <typename Visitor>
void walkExprNodes(const HighExpr &Root, Visitor &&Visit) {
  std::set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{&Root};
  while (!Work.empty()) {
    const HighExpr *Expr = Work.back();
    Work.pop_back();
    if (!Seen.insert(Expr).second)
      continue;
    Visit(*Expr);
    for (auto It = Expr->Operands.rbegin(); It != Expr->Operands.rend(); ++It)
      if (*It)
        Work.push_back(It->get());
  }
}

bool sameMedVar(const MedVar &A, const MedVar &B) {
  return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer &&
         A.RegOff == B.RegOff;
}

const HighExpr *findUniqueDef(const std::vector<ExprDef> &Defs,
                              const MedVar &Var) {
  const HighExpr *Found = nullptr;
  for (const ExprDef &Def : Defs) {
    if (!sameMedVar(Def.Var, Var))
      continue;
    if (Found)
      return nullptr;
    Found = Def.Value;
  }
  return Found;
}

std::optional<int64_t> addOffset(int64_t A, int64_t B) {
  if ((B > 0 && A > std::numeric_limits<int64_t>::max() - B) ||
      (B < 0 && A < std::numeric_limits<int64_t>::min() - B))
    return std::nullopt;
  return A + B;
}

int64_t signedConstant(const HighExpr &Expr) {
  unsigned Bits = Expr.Type ? unsigned(Expr.Type->Size) * 8u : 64u;
  Bits = std::min(Bits, 64u);
  if (Bits == 0 || Bits == 64)
    return static_cast<int64_t>(Expr.ConstVal);
  uint64_t Mask = (uint64_t(1) << Bits) - 1;
  uint64_t Value = Expr.ConstVal & Mask;
  if (Value & (uint64_t(1) << (Bits - 1)))
    Value |= ~Mask;
  return static_cast<int64_t>(Value);
}

std::optional<ReducedAddr> reduceAddr(const HighExpr &Expr,
                                      const std::vector<ExprDef> &Defs,
                                      int64_t Offset = 0, int Depth = 0) {
  if (Depth > 128)
    return std::nullopt;
  if (Expr.Kind == ExprKind::Var) {
    if (const HighExpr *Def = findUniqueDef(Defs, Expr.Var))
      return reduceAddr(*Def, Defs, Offset, Depth + 1);
    return ReducedAddr{Expr.Var, Offset};
  }
  if (Expr.Kind == ExprKind::Cast && Expr.Operands.size() == 1)
    return reduceAddr(*Expr.Operands[0], Defs, Offset, Depth + 1);
  if (Expr.Kind != ExprKind::BinOp || Expr.Operands.size() != 2)
    return std::nullopt;

  const HighExpr &Left = *Expr.Operands[0];
  const HighExpr &Right = *Expr.Operands[1];
  const HighExpr *Base = nullptr;
  int64_t Delta = 0;
  if (Expr.Op == NdOp::INT_ADD && Left.Kind == ExprKind::Const &&
      Right.Kind != ExprKind::Const) {
    Base = &Right;
    Delta = signedConstant(Left);
  } else if (Expr.Op == NdOp::INT_ADD && Right.Kind == ExprKind::Const &&
             Left.Kind != ExprKind::Const) {
    Base = &Left;
    Delta = signedConstant(Right);
  } else if (Expr.Op == NdOp::INT_SUB && Right.Kind == ExprKind::Const &&
             Left.Kind != ExprKind::Const) {
    Base = &Left;
    int64_t Constant = signedConstant(Right);
    if (Constant == std::numeric_limits<int64_t>::min())
      return std::nullopt;
    Delta = -Constant;
  } else {
    return std::nullopt;
  }

  auto NewOffset = addOffset(Offset, Delta);
  return NewOffset ? reduceAddr(*Base, Defs, *NewOffset, Depth + 1)
                   : std::nullopt;
}

bool sameReducedAddr(const ReducedAddr &A, const ReducedAddr &B) {
  return sameMedVar(A.Base, B.Base) && A.Offset == B.Offset;
}

std::string reducedAddrKey(const ReducedAddr &Addr) {
  return "reduced:" + std::to_string(static_cast<unsigned>(Addr.Base.Kind)) +
         ":" + std::to_string(Addr.Base.Id) + ":" +
         std::to_string(Addr.Base.SSAVer) + ":" +
         std::to_string(Addr.Base.RegOff) + ":" +
         std::to_string(Addr.Offset);
}

bool matchesReducedAddr(const HighExpr &Expr,
                        const std::vector<ExprDef> &Defs,
                        const std::vector<ReducedAddr> &Candidates) {
  auto Reduced = reduceAddr(Expr, Defs);
  if (!Reduced)
    return false;
  return std::any_of(Candidates.begin(), Candidates.end(),
                     [&](const ReducedAddr &Candidate) {
                       return sameReducedAddr(*Reduced, Candidate);
                     });
}

void collectExprVars(const HighExpr &E, std::set<std::string> &Out,
                     VarNameFn VarFn) {
  walkExprNodes(E, [&](const HighExpr &Ex) {
    if (Ex.Kind == ExprKind::Var)
      Out.insert(VarFn(Ex.Var));
  });
}

void collectLoadAddrVars(const HighExpr &E, std::set<std::string> &Out,
                         VarNameFn VarFn) {
  walkExprNodes(E, [&](const HighExpr &Ex) {
    if (Ex.Kind == ExprKind::Load && !Ex.Operands.empty())
      collectExprVars(*Ex.Operands[0], Out, VarFn);
  });
}

} // anonymous namespace

void analyzeDeadStores(HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn) {
  State.DeadStmts.clear();
  State.DeadVars.clear();
  State.AddressKeys.clear();

  std::vector<ExprDef> Defs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var)
      Defs.push_back({S.Dst->Var, S.Val.get()});
  });

  auto RecordAddress = [&](const HighExpr &Addr) {
    auto Reduced = reduceAddr(Addr, Defs);
    State.AddressKeys[&Addr] =
        Reduced ? reducedAddrKey(*Reduced) : ExprFn(Addr);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Store && S.StoreAddr)
      RecordAddress(*S.StoreAddr);
    forEachExpr(S, [&](const ExprPtr &Expr) {
      if (!Expr)
        return;
      walkExprNodes(*Expr, [&](const HighExpr &Node) {
        if ((Node.Kind == ExprKind::Load || Node.Kind == ExprKind::Store) &&
            !Node.Operands.empty())
          RecordAddress(*Node.Operands[0]);
      });
    });
  });

  std::vector<ReducedAddr> ReducedLoads;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      walkExprNodes(*E, [&](const HighExpr &Expr) {
        if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty())
          if (auto Addr = reduceAddr(*Expr.Operands[0], Defs))
            ReducedLoads.push_back(*Addr);
      });
    });
  });

  std::set<std::string> LoadVars;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectLoadAddrVars(*E, LoadVars, VarFn);
    });
  });

  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Store || !S.StoreAddr)
      return;
    std::set<std::string> AddrVars;
    collectExprVars(*S.StoreAddr, AddrVars, VarFn);
    bool AnyLoaded = false;
    for (auto &V : AddrVars) {
      if (LoadVars.count(V)) {
        AnyLoaded = true;
        break;
      }
    }
    if (!AnyLoaded && !AddrVars.empty() &&
        !matchesReducedAddr(*S.StoreAddr, Defs, ReducedLoads))
      State.DeadStmts.insert(&S);
  });

  std::set<std::string> LoadedAddrs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        walkExprNodes(*E, [&](const HighExpr &Expr) {
          if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty())
            LoadedAddrs.insert(ExprFn(*Expr.Operands[0]));
        });
    });
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind != StmtKind::Store || !S.StoreAddr)
      return;
    std::string Addr = ExprFn(*S.StoreAddr);
    if (!LoadedAddrs.count(Addr) &&
        !matchesReducedAddr(*S.StoreAddr, Defs, ReducedLoads))
      State.DeadStmts.insert(&S);
  });

  bool Changed = true;
  while (Changed) {
    Changed = false;
    std::set<std::string> AliveVars;
    for (auto &P : Func.Params)
      AliveVars.insert(P.Name);

    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (E)
          collectExprVars(*E, AliveVars, VarFn);
      });
    });

    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var) {
        if (S.Val && S.Val->Kind == ExprKind::Call &&
            S.Val->IntrinsicId != Intrinsic::None) {
          if (!S.Val->IntrinsicOutputs.empty()) {
            for (auto &CO : S.Val->IntrinsicOutputs) {
              std::string COName = VarFn(CO);
              if (!AliveVars.count(COName)) {
                if (State.DeadVars.insert(COName).second)
                  Changed = true;
              }
            }
          }
          std::string DstName = VarFn(S.Dst->Var);
          if (!AliveVars.count(DstName)) {
            if (State.DeadVars.insert(DstName).second)
              Changed = true;
          }
          return;
        }
        if (S.Val && S.Val->Kind == ExprKind::Call)
          return;
        std::string Name = VarFn(S.Dst->Var);
        bool AnyAlive = AliveVars.count(Name);
        if (!AnyAlive && S.Val && !S.Val->IntrinsicOutputs.empty()) {
          for (auto &CO : S.Val->IntrinsicOutputs)
            if (AliveVars.count(VarFn(CO))) {
              AnyAlive = true;
              break;
            }
        }
        if (!AnyAlive) {
          State.DeadStmts.insert(&S);
          State.DeadVars.insert(Name);
          Changed = true;
        }
      }
    });
  }

  std::set<std::string> AssignedVars;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      AssignedVars.insert(VarFn(S.Dst->Var));
    if (S.Kind == StmtKind::Call && S.CallExpr &&
        !S.CallExpr->IntrinsicOutputs.empty())
      for (auto &CO : S.CallExpr->IntrinsicOutputs)
        AssignedVars.insert(VarFn(CO));
    if (S.Kind == StmtKind::Assign && S.Val && !S.Val->IntrinsicOutputs.empty())
      for (auto &CO : S.Val->IntrinsicOutputs)
        AssignedVars.insert(VarFn(CO));
  });

  std::set<std::string> AliveRhs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectExprVars(*E, AliveRhs, VarFn);
    });
  });

  std::set<std::string> ParamNames;
  for (auto &P : Func.Params)
    ParamNames.insert(P.Name);

  std::map<std::string, TypeRef> LiveUsed;
  collectUsedVars(Func.Body, LiveUsed, VarFn);
  for (auto &[Name, Ty] : LiveUsed) {
    if (ParamNames.count(Name))
      continue;
    if (AliveRhs.count(Name))
      continue;
    if (!AssignedVars.count(Name))
      State.DeadVars.insert(Name);
  }
}

} // namespace neverd

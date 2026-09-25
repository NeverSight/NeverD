//===- HighCVoidAnalysis.cpp - Void return analysis -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Void-return inference and dead-chain propagation for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"

#include <functional>
#include <map>

namespace neverd {

namespace {

bool isDebugTrapExpr(const HighExpr *E) {
  return E && E->Kind == ExprKind::Call &&
         (E->IntrinsicId == Intrinsic::Int3 ||
          E->IntrinsicId == Intrinsic::Ud2 ||
          E->IntrinsicId == Intrinsic::Int1);
}

bool isDebugTrapStmt(const HighStmt &Stmt) {
  if (Stmt.Kind == StmtKind::Call)
    return isDebugTrapExpr(Stmt.CallExpr.get());
  if ((Stmt.Kind == StmtKind::Assign || Stmt.Kind == StmtKind::ExprStmt) &&
      Stmt.Val)
    return isDebugTrapExpr(Stmt.Val.get());
  return false;
}

const HighExpr *stmtCallExpr(const HighStmt &Stmt) {
  if (Stmt.Kind == StmtKind::Call)
    return Stmt.CallExpr.get();
  if ((Stmt.Kind == StmtKind::Assign || Stmt.Kind == StmtKind::ExprStmt) &&
      Stmt.Val)
    return Stmt.Val.get();
  return nullptr;
}

void markCallsFollowedByTrap(HighCAnalysisState &State,
                             const std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I < Stmts.size(); ++I) {
    const HighStmt &S = Stmts[I];
    markCallsFollowedByTrap(State, S.Body);
    markCallsFollowedByTrap(State, S.ElseBody);
    for (const SwitchCase &C : S.Cases)
      markCallsFollowedByTrap(State, C.Body);
    markCallsFollowedByTrap(State, S.DefaultBody);
    for (const auto &ClauseBody : S.EHClauseBodies)
      markCallsFollowedByTrap(State, ClauseBody);

    const HighExpr *Call = stmtCallExpr(S);
    if (!Call || Call->Kind != ExprKind::Call ||
        Call->IntrinsicId != Intrinsic::None)
      continue;
    size_t J = I + 1;
    while (J < Stmts.size() && (State.DeadStmts.count(&Stmts[J]) ||
                                Stmts[J].Kind == StmtKind::Nop))
      ++J;
    if (J >= Stmts.size() || !isDebugTrapStmt(Stmts[J]))
      continue;
    State.InferredNoreturnCalls.insert(Call);
    if (Call->CallAddr != 0 && Call->CallAddr != InvalidVA)
      State.InferredNoreturnCallAddrs.insert(Call->CallAddr);
  }
}

} // namespace

bool isNoreturnCallExpr(const HighCAnalysisState &State, const HighExpr &E) {
  if (E.Kind != ExprKind::Call)
    return false;
  if (libc::isNoReturnFunction(E.CallTarget) || isX86FastFailCall(E))
    return true;
  if (State.InferredNoreturnCalls.count(&E) != 0)
    return true;
  return E.CallAddr != 0 && E.CallAddr != InvalidVA &&
         State.InferredNoreturnCallAddrs.count(E.CallAddr) != 0;
}

void analyzeInferredNoreturn(HighCAnalysisState &State, const HighFunc &Func,
                             VarNameFn VarFn) {
  State.InferredNoreturnCallAddrs.clear();
  State.InferredNoreturnCalls.clear();
  markCallsFollowedByTrap(State, Func.Body);
  std::map<std::string, const HighExpr *> VarSources;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Val)
      VarSources[VarFn(S.Dst->Var)] = S.Val.get();
  });
  auto IsBareReturnValue = [&](const HighExpr *Ret) -> bool {
    if (!Ret || Ret->Kind == ExprKind::Undef)
      return true;
    if (Ret->Kind != ExprKind::Var)
      return false;
    // Incoming EAX/RAX with no assignment is the MSVC `ret` success path on
    // a void cookie helper, not a value the caller can read.  A SSAVer-0
    // register that this function assigned (call result, etc.) is a real
    // value; treating it as bare made `call(); if (v0_0)` drop the dest.
    if (VarSources.count(VarFn(Ret->Var)))
      return false;
    return Ret->Var.Kind == MedVar::Reg && Ret->Var.SSAVer == 0;
  };
  bool HasBareReturn = false;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return && IsBareReturnValue(S.RetVal.get()))
      HasBareReturn = true;
  });
  if (!HasBareReturn)
    return;

  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Return || !S.RetVal)
      return;
    const HighExpr *Ret = S.RetVal.get();
    if (Ret->Kind == ExprKind::Call) {
      if (Ret->CallAddr != 0 && Ret->CallAddr != InvalidVA)
        State.InferredNoreturnCallAddrs.insert(Ret->CallAddr);
      return;
    }
    if (Ret->Kind != ExprKind::Var)
      return;
    auto It = VarSources.find(VarFn(Ret->Var));
    if (It == VarSources.end() || It->second->Kind != ExprKind::Call)
      return;
    if (It->second->CallAddr != 0 && It->second->CallAddr != InvalidVA)
      State.InferredNoreturnCallAddrs.insert(It->second->CallAddr);
  });
}

bool analyzeVoidReturn(const HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn) {
  if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
    return Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void;
  if (Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Void)
    return true;

  std::set<std::string> ParamNames;
  for (const HighParam &Param : Func.Params)
    ParamNames.insert(Param.Name);

  std::map<std::string, MedVar> SeenVars;
  std::set<const HighExpr *> SeenExprs;
  std::function<void(const HighExpr &)> CollectVars = [&](const HighExpr &E) {
    if (!SeenExprs.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Var)
      SeenVars.emplace(VarFn(E.Var), E.Var);
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        CollectVars(*Operand);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        CollectVars(*E);
    });
  });

  std::map<std::string, const HighExpr *> VarSources;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Val)
      VarSources[VarFn(S.Dst->Var)] = S.Val.get();
  });

  auto AddressKey = [&](const HighExpr &Addr) {
    auto It = State.AddressKeys.find(&Addr);
    return It == State.AddressKeys.end() ? ExprFn(Addr) : It->second;
  };
  std::set<std::string> SideeffectLoadKeys;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (!E || E->Kind != ExprKind::Call ||
          !isSideeffectIntrinsic(E->IntrinsicId))
        return;
      std::set<const HighExpr *> Seen;
      std::function<void(const HighExpr &)> CollectLoads =
          [&](const HighExpr &Operand) {
            if (!Seen.insert(&Operand).second)
              return;
            if (Operand.Kind == ExprKind::Load && !Operand.Operands.empty())
              SideeffectLoadKeys.insert(AddressKey(*Operand.Operands[0]));
            for (const ExprPtr &Child : Operand.Operands)
              if (Child)
                CollectLoads(*Child);
          };
      for (const ExprPtr &Operand : E->Operands)
        if (Operand)
          CollectLoads(*Operand);
    });
  });

  auto IsVoidSource = [&](const std::string &VName) -> bool {
    if (ParamNames.count(VName))
      return false;
    auto It = VarSources.find(VName);
    if (It != VarSources.end()) {
      auto *Src = It->second;
      if (Src->Kind == ExprKind::Call) {
        if (isMsvcCxxThrowCallName(Src->CallTarget) ||
            isNoreturnCallExpr(State, *Src))
          return true;
        // The x64 debug service and a system call with its register inputs
        // return their status in RAX.
        if (Src->IntrinsicId == Intrinsic::DebugService ||
            (Src->IntrinsicId == Intrinsic::Syscall && !Src->Operands.empty()))
          return false;
        if (Src->IntrinsicId != Intrinsic::None)
          return isSideeffectIntrinsic(Src->IntrinsicId) ||
                 !intrinsicCName(Src->IntrinsicId);
      }
      return false;
    }

    auto SeenIt = SeenVars.find(VName);
    if (SeenIt == SeenVars.end())
      return false;
    const MedVar &V = SeenIt->second;
    return V.Kind == MedVar::Reg && V.SSAVer == 0;
  };

  std::function<bool(const HighExpr &)> IsVoidExpr =
      [&](const HighExpr &E) -> bool {
    if (E.Kind == ExprKind::Undef)
      return true;
    if (E.Kind == ExprKind::Var)
      return IsVoidSource(VarFn(E.Var));
    if (E.Kind == ExprKind::BinOp && E.Op == NdOp::SELECT &&
        E.Operands.size() == 3)
      return IsVoidExpr(*E.Operands[1]) && IsVoidExpr(*E.Operands[2]);
    if (E.Kind == ExprKind::UnaryOp &&
        (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT ||
         E.Op == NdOp::SUBBYTES) &&
        !E.Operands.empty())
      return IsVoidExpr(*E.Operands[0]);
    if (E.Kind == ExprKind::Cast && !E.Operands.empty())
      return IsVoidExpr(*E.Operands[0]);
    if (E.Kind == ExprKind::Load && !E.Operands.empty()) {
      // A retained private-frame load is a value boundary. Its address may
      // depend only on SP after bounded forwarding stops expanding a chain;
      // that does not make the loaded value an incidental incoming register.
      if (State.CanElideFrameStores)
        return false;
      const HighExpr &AddrExpr = *E.Operands[0];
      std::string Rendered = ExprFn(AddrExpr);
      std::string Key = AddressKey(AddrExpr);
      auto DepIt = State.StoreFwdDeps.find(Rendered);
      if (DepIt == State.StoreFwdDeps.end()) {
        DepIt = State.ForwardedAddressDeps.find(Key);
        if (DepIt == State.ForwardedAddressDeps.end())
          return false;
      }
      for (const std::string &Name : DepIt->second)
        // A void side effect may leave its address operand in the native
        // return register even though the source function returns no value.
        if (!IsVoidSource(Name) &&
            !(ParamNames.count(Name) && SideeffectLoadKeys.count(Key)))
          return false;
      return true;
    }
    if (E.Kind == ExprKind::Call)
      return isMsvcCxxThrowCallName(E.CallTarget) ||
             isNoreturnCallExpr(State, E);
    return false;
  };

  bool HasReturn = false;
  bool AllVoid = true;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Return || !S.RetVal)
      return;
    HasReturn = true;
    if (IsVoidExpr(*S.RetVal))
      return;
    AllVoid = false;
  });
  return HasReturn && AllVoid;
}

void analyzeVoidDeadChain(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn) {
  std::set<std::string> NonReturnUses;
  std::set<const HighExpr *> SeenUses;
  std::function<void(const HighExpr &)> CollectUse = [&](const HighExpr &E) {
    if (!SeenUses.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Var)
      NonReturnUses.insert(VarFn(E.Var));
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        CollectUse(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return || State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        CollectUse(*E);
    });
  });

  std::function<void(const HighExpr &)> Collect = [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Var) {
      const std::string Name = VarFn(E.Var);
      if (!NonReturnUses.count(Name))
        State.DeadVars.insert(Name);
    }
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        Collect(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return && S.RetVal)
      Collect(*S.RetVal);
  });
}

void analyzeUnusedAssigns(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn) {
  auto ExprHasEffect = [](const HighExpr &E) -> bool {
    std::function<bool(const HighExpr &)> Walk = [&](const HighExpr &N) {
      if (N.Kind == ExprKind::Call || N.Kind == ExprKind::Store)
        return true;
      if (N.MemoryOrdering != NdMemoryOrdering::None)
        return true;
      for (const ExprPtr &Op : N.Operands)
        if (Op && Walk(*Op))
          return true;
      return false;
    };
    return Walk(E);
  };

  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 8) {
    Changed = false;
    std::map<std::string, TypeRef> Used;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (E)
          collectUsedVarsExpr(*E, Used, VarFn);
      });
    });
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      if (ExprHasEffect(*S.Val))
        return;
      if (Used.count(VarFn(S.Dst->Var)))
        return;
      State.DeadStmts.insert(&S);
      Changed = true;
    });
  }
}

} // namespace neverd

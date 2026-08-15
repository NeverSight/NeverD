//===- HighCleanup.cpp - HighIR cleanup passes -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HighIR cleanup passes: stack canary stripping, prologue/epilogue
/// removal, and trailing return insertion.  The main DCE logic lives
/// in HighDCE.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/Debug.h"

#include <algorithm>
#include <functional>
#include <set>

#define DEBUG_TYPE "neverd-high-cleanup"

namespace neverd {

//===----------------------------------------------------------------------===//
// Stack canary stripping
//===----------------------------------------------------------------------===//

void MedToHighConverter::stripStackCanary(HighFunc &Func) {
  auto IsStackChkCall = [](const HighStmt &S) -> bool {
    if (S.Kind == StmtKind::Call && S.CallExpr)
      return S.CallExpr->CallTarget.find("stack_chk_fail") != std::string::npos;
    if (S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Call)
      return S.Val->CallTarget.find("stack_chk_fail") != std::string::npos;
    return false;
  };
  auto ContainsStackChk = [&](const std::vector<HighStmt> &Body) -> bool {
    for (auto &S : Body)
      if (IsStackChkCall(S))
        return true;
    return false;
  };

  Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                 [&](const HighStmt &S) {
                                   if (S.Kind == StmtKind::If ||
                                       S.Kind == StmtKind::IfElse)
                                     if (ContainsStackChk(S.Body) ||
                                         ContainsStackChk(S.ElseBody))
                                       return true;
                                   return IsStackChkCall(S);
                                 }),
                  Func.Body.end());

  Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                 [](const HighStmt &S) {
                                   if (S.Kind != StmtKind::Store || !S.StoreVal)
                                     return false;
                                   if (S.MemoryOrdering !=
                                           NdMemoryOrdering::None ||
                                       S.StoreVal->hasOrderedMemoryAccess())
                                     return false;
                                   if (S.StoreVal->Kind != ExprKind::Load)
                                     return false;
                                   if (S.StoreVal->Operands.empty())
                                     return false;
                                   return S.StoreVal->Operands[0]->Kind ==
                                          ExprKind::Load;
                                 }),
                  Func.Body.end());

  for (size_t I = 0; I < Func.Body.size(); ++I) {
    auto &S = Func.Body[I];
    if (S.Kind != StmtKind::If)
      continue;
    if (S.Body.size() != 1 || S.Body[0].Kind != StmtKind::Return)
      continue;
    if (!S.Cond)
      continue;

    auto &Cond = S.Cond;
    if (Cond->hasOrderedMemoryAccess())
      continue;
    if (Cond->Kind != ExprKind::BinOp || Cond->Op != NdOp::INT_EQUAL)
      continue;
    if (Cond->Operands.size() != 2)
      continue;
    auto &LHS = Cond->Operands[0];
    auto &RHS = Cond->Operands[1];
    if (RHS->Kind == ExprKind::Const && RHS->ConstVal == 0 &&
        LHS->Kind == ExprKind::BinOp && LHS->Op == NdOp::INT_SUB &&
        LHS->Operands.size() == 2 && LHS->Operands[0]->Kind == ExprKind::Load) {
      auto Extracted = std::move(S.Body[0]);
      Func.Body[I] = std::move(Extracted);
    }
  }
}

//===----------------------------------------------------------------------===//
// Prologue / epilogue stripping
//===----------------------------------------------------------------------===//

void MedToHighConverter::stripPrologueEpilogue(HighFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);

  auto IsSPReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isStackPointer(V.RegOff);
  };
  auto IsFPReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isFramePointer(V.RegOff);
  };
  auto IsLRReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isLinkRegister(V.RegOff);
  };
  auto IsCalleeSaveStore = [&](const HighStmt &S) -> bool {
    if (S.Kind != StmtKind::Store || !S.StoreVal)
      return false;
    if (S.MemoryOrdering != NdMemoryOrdering::None ||
        S.StoreVal->hasOrderedMemoryAccess())
      return false;
    if (S.StoreVal->Kind != ExprKind::Var)
      return false;
    auto &V = S.StoreVal->Var;
    if (IsFPReg(V) || IsLRReg(V))
      return true;
    return V.Kind == MedVar::Reg && TRI.isCalleeSaveReg(V.RegOff);
  };

  Func.Body.erase(
      std::remove_if(Func.Body.begin(), Func.Body.end(), IsCalleeSaveStore),
      Func.Body.end());

  std::set<std::string> UsedFrameVars;
  std::function<void(const ExprPtr &)> CollectVarRefs;
  CollectVarRefs = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && (IsFPReg(E->Var) || IsSPReg(E->Var)))
      UsedFrameVars.insert(E->str());
    for (auto &Op : E->Operands)
      CollectVarRefs(Op);
  };
  std::function<void(const HighStmt &)> CollectStmtRefsPE;
  CollectStmtRefsPE = [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Val)
      CollectVarRefs(S.Val);
    if (S.Kind == StmtKind::Store) {
      CollectVarRefs(S.StoreAddr);
      CollectVarRefs(S.StoreVal);
    }
    if (S.Cond)
      CollectVarRefs(S.Cond);
    if (S.RetVal)
      CollectVarRefs(S.RetVal);
    if (S.CallExpr)
      CollectVarRefs(S.CallExpr);
    for (auto &Inner : S.Body)
      CollectStmtRefsPE(Inner);
    for (auto &Inner : S.ElseBody)
      CollectStmtRefsPE(Inner);
  };
  for (auto &S : Func.Body)
    CollectStmtRefsPE(S);

  std::set<std::string> FrameTemps;

  auto IsPrologueEpilogue = [&](const HighStmt &S) -> bool {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val) {
      if (S.Val->hasOrderedMemoryAccess())
        return false;
      if (S.Dst->Kind == ExprKind::Var && IsSPReg(S.Dst->Var) &&
          S.Val->Kind == ExprKind::BinOp &&
          (S.Val->Op == NdOp::INT_SUB || S.Val->Op == NdOp::INT_ADD) &&
          S.Val->Operands.size() == 2 &&
          S.Val->Operands[0]->Kind == ExprKind::Var &&
          IsSPReg(S.Val->Operands[0]->Var) &&
          S.Val->Operands[1]->Kind == ExprKind::Const) {
        return UsedFrameVars.count(S.Dst->str()) == 0;
      }
      if (S.Dst->Kind == ExprKind::Var && IsFPReg(S.Dst->Var))
        return UsedFrameVars.count(S.Dst->str()) == 0;
      if (S.Dst->Kind == ExprKind::Var && S.Dst->Var.Kind == MedVar::Temp &&
          S.Val->Kind == ExprKind::BinOp && S.Val->Op == NdOp::INT_ADD &&
          S.Val->Operands.size() == 2 &&
          S.Val->Operands[0]->Kind == ExprKind::Var) {
        auto &Base = S.Val->Operands[0]->Var;
        if (IsSPReg(Base) || FrameTemps.count(S.Val->Operands[0]->str())) {
          FrameTemps.insert(S.Dst->str());
          return true;
        }
      }
    }
    if (S.Kind == StmtKind::Store && S.StoreVal && S.StoreAddr &&
        S.StoreVal->Kind == ExprKind::Var) {
      if (S.MemoryOrdering != NdMemoryOrdering::None)
        return false;
      auto &V = S.StoreVal->Var;
      if (IsFPReg(V) || IsLRReg(V))
        return true;
      if (V.Kind == MedVar::Reg && TRI.isCalleeSaveReg(V.RegOff))
        return true;
    }
    return false;
  };

  auto IsEpilogueLoad = [&](const HighStmt &S) -> bool {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var)
      return false;
    if (S.Val->hasOrderedMemoryAccess())
      return false;
    if (IsLRReg(S.Dst->Var))
      return true;
    if (IsFPReg(S.Dst->Var))
      return UsedFrameVars.count(S.Dst->str()) == 0;
    return false;
  };

  std::function<void(std::vector<HighStmt> &)> StripRecursive;
  StripRecursive = [&](std::vector<HighStmt> &Stmts) {
    Stmts.erase(std::remove_if(Stmts.begin(), Stmts.end(),
                               [&](const HighStmt &S) {
                                 return IsPrologueEpilogue(S) ||
                                        IsEpilogueLoad(S);
                               }),
                Stmts.end());
    for (auto &S : Stmts) {
      StripRecursive(S.Body);
      StripRecursive(S.ElseBody);
      for (auto &C : S.Cases)
        StripRecursive(C.Body);
      StripRecursive(S.DefaultBody);
    }
  };
  StripRecursive(Func.Body);

  if (Func.Body.size() >= 2 && Func.Body.back().Kind == StmtKind::Return) {
    std::function<bool(const std::vector<HighStmt> &)> AllPathsReturn;
    AllPathsReturn = [&](const std::vector<HighStmt> &Stmts) -> bool {
      for (auto Rit = Stmts.rbegin(); Rit != Stmts.rend(); ++Rit) {
        if (Rit->Kind == StmtKind::Return)
          return true;
        if (Rit->Kind == StmtKind::IfElse)
          return AllPathsReturn(Rit->Body) && AllPathsReturn(Rit->ElseBody);
        if (Rit->Kind == StmtKind::While)
          return false;
        if (Rit->Kind == StmtKind::If && AllPathsReturn(Rit->Body))
          continue;
      }
      return false;
    };
    std::vector<HighStmt> BeforeLast(Func.Body.begin(), Func.Body.end() - 1);
    if (AllPathsReturn(BeforeLast))
      Func.Body.pop_back();
  }
}

//===----------------------------------------------------------------------===//
// Ensure trailing return
//===----------------------------------------------------------------------===//

void MedToHighConverter::ensureTrailingReturn(HighFunc &Func,
                                              const MedFunc &Med) {
  if (!Func.Body.empty() && Func.Body.back().Kind == StmtKind::Return)
    return;
  if (Func.Body.empty())
    return;

  ExprPtr RetExpr;

  for (auto &Blk : Med.Blocks) {
    bool HasRet = false;
    for (auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        HasRet = true;
        break;
      }
    if (!HasRet)
      continue;

    for (auto Rit = Blk.Ops.rbegin(); Rit != Blk.Ops.rend(); ++Rit) {
      if (Rit->Opcode == NdOp::RETURN)
        continue;
      if (Rit->Output.Kind == MedVar::Reg && Rit->Output.RegOff == 0 &&
          Rit->Output.Size > 0) {
        if (Rit->Opcode == NdOp::INT_ZEXT && Rit->NumInputs >= 1 &&
            Rit->Inputs[0].Kind == MedVar::Reg)
          RetExpr = HighExpr::makeVar(Rit->Inputs[0]);
        else
          RetExpr = HighExpr::makeVar(Rit->Output);
        break;
      }
    }
    if (!RetExpr) {
      for (auto &Phi : Blk.Phis) {
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == 0) {
          RetExpr = HighExpr::makeVar(Phi.Output);
          break;
        }
      }
    }
    if (RetExpr) {
      RetExpr = forceInlineExpr(RetExpr);
      break;
    }
  }

  if (!RetExpr) {
    for (int I = static_cast<int>(Func.Body.size()) - 1; I >= 0; --I) {
      auto &S = Func.Body[I];
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
          S.Dst->Var.Kind == MedVar::Reg && S.Dst->Var.RegOff == 0) {
        RetExpr = S.Dst;
        break;
      }
    }
  }

  HighStmt RetStmt;
  RetStmt.Kind = StmtKind::Return;
  RetStmt.RetVal = RetExpr;
  Func.Body.push_back(std::move(RetStmt));
}

} // namespace neverd

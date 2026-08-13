//===- CFStructurer.cpp - Control-flow structuring ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow structuring for HighIR: block-level dispatch loop and PHI
/// copy insertion.  Individual NdOp lowering helpers live in:
///   NdOpLowering.cpp         — STORE, CALL, INTRINSIC, COND_BR, BRANCH,
///                               generic-assign
///   NdOpCallIndLowering.cpp  — INDIR_CALL target resolution and lowering
///   NdOpReturnLowering.cpp   — RETURN value recovery
///   NdOpSwitchRecovery.cpp   — INDIR_BR / jump-table switch recovery
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <set>

namespace neverd {

//===----------------------------------------------------------------------===//
// insertPhiCopies
//===----------------------------------------------------------------------===//

void MedToHighConverter::insertPhiCopies(
    HighFunc &Func, const MedBlock &CurBlock, int BlkIdx, size_t BlkBodyStart,
    const std::map<int, std::vector<std::pair<MedVar, MedVar>>> &PhiCopies) {
  auto PIt = PhiCopies.find(BlkIdx);
  if (PIt == PhiCopies.end())
    return;

  size_t InsertPos = Func.Body.size();
  for (size_t K = Func.Body.size(); K > BlkBodyStart; --K) {
    auto &S = Func.Body[K - 1];
    if (S.Kind == StmtKind::Goto || S.Kind == StmtKind::Return ||
        (S.Kind == StmtKind::If && !S.Body.empty() &&
         S.Body[0].Kind == StmtKind::Goto)) {
      InsertPos = K - 1;
      break;
    }
  }

  std::set<uint64_t> TermRegOffs;
  if (InsertPos < Func.Body.size()) {
    auto &Term = Func.Body[InsertPos];
    std::set<std::pair<int, int>> Visited;
    std::function<void(const ExprPtr &)> Collect;
    Collect = [&](const ExprPtr &E) {
      if (!E)
        return;
      if (E->Kind == ExprKind::Var) {
        if (E->Var.Kind == MedVar::Reg)
          TermRegOffs.insert(E->Var.RegOff);
        auto Key = std::make_pair(E->Var.Id, E->Var.SSAVer);
        if (Key.first >= 0 && Visited.insert(Key).second) {
          auto DIt = DefExpr.find(Key);
          if (DIt != DefExpr.end())
            Collect(DIt->second);
        }
      }
      for (auto &Op : E->Operands)
        Collect(Op);
    };
    Collect(Term.Cond);
  }

  std::vector<HighStmt> PhiStmts;
  for (auto &[PhiOut, PhiArg] : PIt->second) {
    if (PhiOut.Kind == MedVar::Flag)
      continue;

    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = CurBlock.Ops.empty() ? 0 : CurBlock.Ops.back().Addr;
    S.Dst = HighExpr::makeVar(PhiOut);
    S.Val = medvarToExpr(PhiArg);
    S.IsPhiCopy = true;
    PhiStmts.push_back(std::move(S));
  }

  bool HasConflict = false;
  for (auto &PS : PhiStmts) {
    if (!PS.Dst || PS.Dst->Kind != ExprKind::Var)
      continue;
    if (PS.Dst->Var.Kind == MedVar::Reg &&
        TermRegOffs.count(PS.Dst->Var.RegOff)) {
      HasConflict = true;
      break;
    }
  }

  if (HasConflict && InsertPos < Func.Body.size() &&
      Func.Body[InsertPos].Kind == StmtKind::If && Func.Body[InsertPos].Cond) {
    auto &Term = Func.Body[InsertPos];
    MedVar TmpVar;
    TmpVar.Kind = MedVar::Temp;
    TmpVar.Id = limits::kPhiCondTempId;
    TmpVar.SSAVer = BlkIdx;
    TmpVar.Size = 1;

    HighStmt CondSave;
    CondSave.Kind = StmtKind::Assign;
    CondSave.Addr = Term.Addr;
    CondSave.Dst = HighExpr::makeVar(TmpVar);
    CondSave.Val = Term.Cond;

    Term.Cond = HighExpr::makeVar(TmpVar);

    Func.Body.insert(Func.Body.begin() + static_cast<long>(InsertPos),
                     std::move(CondSave));
    InsertPos++;
  }

  Func.Body.insert(Func.Body.begin() + static_cast<long>(InsertPos),
                   PhiStmts.begin(), PhiStmts.end());
}

//===----------------------------------------------------------------------===//
// structureControlFlow — block-level dispatch
//===----------------------------------------------------------------------===//

void MedToHighConverter::structureControlFlow(HighFunc &Func,
                                              const MedFunc &Med) {
  std::map<int, std::vector<std::pair<MedVar, MedVar>>> PhiCopies;
  for (auto &Block : Med.Blocks)
    for (auto &Phi : Block.Phis)
      for (auto &[PredId, Arg] : Phi.Args)
        PhiCopies[PredId].push_back({Phi.Output, Arg});

  JtConsumedBlocks.clear();

  VarKeySet PhiArgVars;
  for (auto &Block : Med.Blocks)
    for (auto &Phi : Block.Phis)
      for (auto &[PredId, Arg] : Phi.Args)
        if (Arg.Id >= 0)
          PhiArgVars.insert(varKey(Arg));

  for (int BlkIdx = 0; BlkIdx < static_cast<int>(Med.Blocks.size()); ++BlkIdx) {
    if (JtConsumedBlocks.count(BlkIdx))
      continue;
    auto &CurBlock = Med.Blocks[BlkIdx];
    size_t BlkBodyStart = Func.Body.size();
    std::set<size_t> IntrinsicSkip;
    for (size_t OpIdx = 0; OpIdx < CurBlock.Ops.size(); ++OpIdx) {
      if (IntrinsicSkip.count(OpIdx))
        continue;
      auto &CurOp = CurBlock.Ops[OpIdx];
      switch (CurOp.Opcode) {
      case NdOp::STORE:
        lowerStore(Func, CurOp);
        break;
      case NdOp::CALL:
        lowerCall(Func, CurBlock, CurOp);
        break;
      case NdOp::INTRINSIC:
        lowerIntrinsic(Func, CurBlock, CurOp, OpIdx, IntrinsicSkip);
        break;
      case NdOp::INDIR_CALL:
        lowerCallInd(Func, CurBlock, CurOp);
        break;
      case NdOp::RETURN:
        lowerReturn(Func, CurBlock, CurOp, Med);
        break;
      case NdOp::COND_BR:
        lowerCBranch(Func, CurOp);
        break;
      case NdOp::BRANCH:
        lowerBranch(Func, CurOp);
        break;
      case NdOp::INDIR_BR:
        lowerBranchInd(Func, CurBlock, CurOp, Med);
        break;
      case NdOp::NOP:
        break;
      default:
        lowerGenericAssign(Func, CurOp, PhiArgVars);
        break;
      }
    }

    insertPhiCopies(Func, CurBlock, BlkIdx, BlkBodyStart, PhiCopies);
  }
}

} // namespace neverd

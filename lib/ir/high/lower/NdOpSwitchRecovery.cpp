//===- NdOpSwitchRecovery.cpp - Switch / indirect branch lowering ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// INDIR_BR lowering: jump-table-based switch recovery and tail-call
/// detection for indirect branches.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedSwitchNorm.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// traceLoadedSwitchIndex — index behind a table load
//===----------------------------------------------------------------------===//

/// Trace the INDIR_BR target back to the scaled index of its table load, within
/// the dispatch block.  The table address is `base + index*scale`, so the index
/// is the non-constant operand of the INT_MULT/INT_LEFT that scales it; this
/// skips the table base and returns the genuine (still normalized) switch
/// index.  Returns false when the target does not flow from a scaled table
/// load.
static bool traceLoadedSwitchIndex(const MedBlock &Blk, const MedVar &Target,
                                   MedVar &Out) {
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &Op : Blk.Ops)
    if (!Op.Output.isConst() && Op.Output.Size > 0)
      Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Defs.find({V.Id, V.SSAVer});
    return It == Defs.end() ? nullptr : It->second;
  };

  std::vector<MedVar> Work{Target};
  std::set<std::pair<int, int>> Seen;
  int Steps = 0;
  while (!Work.empty() && Steps++ < 256) {
    MedVar V = Work.back();
    Work.pop_back();
    if (V.isConst())
      continue;
    if (!Seen.insert({V.Id, V.SSAVer}).second)
      continue;
    const MedOp *Op = defOf(V);
    if (!Op)
      continue;
    if (Op->Opcode == NdOp::INT_MULT || Op->Opcode == NdOp::INT_LEFT) {
      for (int I = 0; I < Op->NumInputs; ++I)
        if (!Op->Inputs[I].isConst()) {
          Out = Op->Inputs[I];
          return true;
        }
      continue;
    }
    for (int I = 0; I < Op->NumInputs; ++I)
      if (!Op->Inputs[I].isConst())
        Work.push_back(Op->Inputs[I]);
  }
  return false;
}

//===----------------------------------------------------------------------===//
// lowerSwitchFromJumpTable
//===----------------------------------------------------------------------===//

bool MedToHighConverter::lowerSwitchFromJumpTable(HighFunc &Func,
                                                  const MedBlock &CurBlock,
                                                  const MedOp &CurOp,
                                                  const MedFunc &Med,
                                                  const JumpTable &JT) {
  ExprPtr SwitchVar;

  // Recover the value the switch dispatches on as a MedVar, so an affine
  // normalization can be peeled off it below.  \p SwitchMedVar holds the
  // selected dispatch value; \p PeelOK is set only for the strategies whose
  // selection is a plain switch index (safe to peel), and cleared for a masked
  // dispatch (`switch(x & mask)`), whose selection is the pre-mask value and
  // must be preserved verbatim.
  MedVar SwitchMedVar;
  bool HaveMedVar = false;
  bool SelectorResolved = false;
  bool PeelOK = false;
  uint16_t SelectorSize = 0;

  auto PlanIt = Med.SwitchSelectorPlans.find(JT.InsnAddr);
  if (PlanIt != Med.SwitchSelectorPlans.end() &&
      PlanIt->second.PlanKind == MedSwitchSelectorPlan::Kind::Direct &&
      PlanIt->second.Selector.Size != 0 &&
      PlanIt->second.Selector.Size == PlanIt->second.ResultSize &&
      !PlanIt->second.Selector.isConst()) {
    SwitchMedVar = PlanIt->second.Selector;
    HaveMedVar = true;
    SelectorResolved = true;
    PeelOK = !JT.PreScaledIndex && !JT.TwoLevelIndex;
  } else if (PlanIt != Med.SwitchSelectorPlans.end() && JT.TwoTableSelect &&
             JT.CompositeSelectorUseRef &&
             PlanIt->second.PlanKind ==
                 MedSwitchSelectorPlan::Kind::SelectOffset &&
             PlanIt->second.Selector.Size != 0 &&
             PlanIt->second.Selector.Size == PlanIt->second.ResultSize &&
             !PlanIt->second.Selector.isConst() &&
             PlanIt->second.Condition.Size != 0 &&
             !PlanIt->second.Condition.isConst()) {
    auto Offset = std::make_shared<HighExpr>();
    Offset->Kind = ExprKind::BinOp;
    Offset->Op = NdOp::SELECT;
    Offset->Operands.push_back(medvarToExpr(PlanIt->second.Condition));
    Offset->Operands.push_back(HighExpr::makeConst(PlanIt->second.TrueOffset,
                                                   PlanIt->second.ResultSize));
    Offset->Operands.push_back(HighExpr::makeConst(PlanIt->second.FalseOffset,
                                                   PlanIt->second.ResultSize));
    Offset->Type = NdType::makeInt(PlanIt->second.ResultSize);
    SwitchVar = HighExpr::makeBinop(
        NdOp::INT_ADD, medvarToExpr(PlanIt->second.Selector), Offset);
    SelectorSize = PlanIt->second.ResultSize;
    SelectorResolved = true;
  } else if (!JT.SelectorUseRefs.empty() || JT.CompositeSelectorUseRef ||
             JT.PreScaledIndex || JT.TwoLevelIndex || JT.TwoTableSelect) {
    // These shapes cannot be reconstructed from the branch target: the
    // physical index register may have a later lifetime, and a two-level
    // target traces to the intermediate address-table index.  Missing or
    // ambiguous exact occurrence metadata is therefore a hard failure.
    return false;
  }

  // Prefer the resolver-identified index register (the one that addresses the
  // table load) over the blind backward scan below.  This is essential for
  // memory/register-indirect computed-goto dispatch where the index register
  // aliases the loaded branch-target register (AArch64 `and w,w,#n; ldr
  // x,[b,w]; br x`): the blind scan cannot tell the index from the target and
  // would fall back to a constant, collapsing the switch to a single edge.
  // Compact byte/halfword tables likewise dispatch on an index distinct from
  // the loaded entry, so both need this anchor.
  if (!SelectorResolved && JT.IndexRegOff >= 0) {
    for (int J = static_cast<int>(CurBlock.Ops.size()) - 1;
         J >= 0 && !HaveMedVar; --J) {
      auto &Prev = CurBlock.Ops[J];
      for (int I = 0; I < Prev.NumInputs; ++I) {
        if (Prev.Inputs[I].Kind == MedVar::Reg &&
            static_cast<int>(Prev.Inputs[I].RegOff) == JT.IndexRegOff) {
          SwitchMedVar = Prev.Inputs[I];
          HaveMedVar = true;
          SelectorResolved = true;
          PeelOK = true;
          break;
        }
      }
    }
  }

  // The genuine table index: the scaled operand of the table-address
  // computation feeding the INDIR_BR (`base + index*scale`).  This is the
  // reliable dispatch value for a plain relative/absolute table whose scaling
  // lives in the dispatch block while its `add`/`cmp` normalization sits in a
  // predecessor guard block — where the in-block heuristics below find nothing.
  if (!SelectorResolved && !JT.HasTargetBase && !JT.PreScaledIndex &&
      !CurBlock.Ops.empty()) {
    const MedOp &Br = CurBlock.Ops.back();
    if (Br.Opcode == NdOp::INDIR_BR && Br.NumInputs >= 1 &&
        traceLoadedSwitchIndex(CurBlock, Br.Inputs[0], SwitchMedVar)) {
      HaveMedVar = true;
      SelectorResolved = true;
      PeelOK = true;
    }
  }

  for (int J = static_cast<int>(CurBlock.Ops.size()) - 1;
       !SelectorResolved && J >= 0; --J) {
    auto &Prev = CurBlock.Ops[J];
    if ((Prev.Opcode == NdOp::INT_ZEXT || Prev.Opcode == NdOp::INT_SEXT) &&
        Prev.NumInputs >= 1 && Prev.Inputs[0].Kind == MedVar::Reg) {
      SwitchMedVar = Prev.Inputs[0];
      HaveMedVar = true;
      SelectorResolved = true;
      PeelOK = true;
      break;
    }
    if ((Prev.Opcode == NdOp::INT_AND || Prev.Opcode == NdOp::INT_SUB) &&
        Prev.NumInputs >= 2 && Prev.Inputs[0].Kind == MedVar::Reg &&
        Prev.Inputs[1].isConst()) {
      SwitchMedVar = Prev.Inputs[0];
      HaveMedVar = true;
      SelectorResolved = true;
      // A `sub` is switch-index normalization (`idx = x - lo`) whose base can
      // be peeled; a mask (`switch(x & m)`) confines the dispatch and must keep
      // its pre-mask value verbatim (peeling would drop the modular bound).
      PeelOK = (Prev.Opcode == NdOp::INT_SUB);
      break;
    }
    if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1 &&
        Prev.Inputs[0].Kind == MedVar::Reg && Prev.Output.Kind == MedVar::Reg) {
      SwitchMedVar = Prev.Inputs[0];
      HaveMedVar = true;
      SelectorResolved = true;
      PeelOK = true;
    }
  }

  // If we didn't find a switch variable in the current block, check
  // predecessor blocks for a bounds-check pattern (CMP / SUB feeding
  // the COND_BR) which often uses the switch variable.
  if (!SelectorResolved) {
    for (auto PredId : CurBlock.Preds) {
      if (PredId < 0 || PredId >= static_cast<int>(Med.Blocks.size()))
        continue;
      auto &PredBlk = Med.Blocks[PredId];
      for (auto PIt = PredBlk.Ops.rbegin(); PIt != PredBlk.Ops.rend(); ++PIt) {
        if ((PIt->Opcode == NdOp::INT_LESS || PIt->Opcode == NdOp::INT_SLESS ||
             PIt->Opcode == NdOp::INT_LESSEQUAL ||
             PIt->Opcode == NdOp::INT_SLESSEQUAL ||
             PIt->Opcode == NdOp::INT_SUB || PIt->Opcode == NdOp::INT_AND) &&
            PIt->NumInputs >= 2 && PIt->Inputs[0].Kind == MedVar::Reg &&
            PIt->Inputs[1].isConst()) {
          SwitchMedVar = PIt->Inputs[0];
          HaveMedVar = true;
          SelectorResolved = true;
          // A range guard (`cmp idx,N`) or subtract-normalization constrains
          // the switch index and is peelable; a mask (`and idx,m`) confines it
          // and must be kept verbatim.
          PeelOK = (PIt->Opcode != NdOp::INT_AND);
          break;
        }
      }
      if (HaveMedVar)
        break;
    }
  }

  // Present the switch on the *source* variable and its true case labels rather
  // than the zero-based table index.  A `switch` whose lowest label is not 0 is
  // lowered by normalizing the variable to a table index (`idx = x - lo`, or
  // `idx = x + k` for a negative-based switch); dispatching on `idx` yields
  // cases 0..N-1 instead of the real (possibly negative) labels.  Peeling that
  // affine step off the dispatch variable and shifting every label by the
  // inverse constant is an exact equivalence — the recovered switch selects the
  // same block for every input — that restores the original variable and
  // labels.  CaseLabels are exact keys in the selector's current coordinate;
  // an ordinary sparse/gapped table may therefore have explicit labels and
  // still require this inverse affine step.  Compact/pre-scaled and target-base
  // recipes use a different coordinate and are excluded below.
  const bool HaveLabels = JT.CaseLabels.size() == JT.Targets.size();
  std::vector<int64_t> Labels;
  Labels.reserve(JT.Targets.size());
  for (size_t K = 0; K < JT.Targets.size(); ++K)
    Labels.push_back(HaveLabels ? JT.CaseLabels[K] : static_cast<int64_t>(K));
  uint64_t LabelDelta = 0;
  if (HaveMedVar) {
    if (PeelOK && !JT.PreScaledIndex && !JT.HasTargetBase)
      SwitchMedVar = peelAffineSwitchVar(Med, SwitchMedVar, LabelDelta,
                                         /*MaxAffine=*/2, &Labels);
    SelectorSize = SwitchMedVar.Size;
    SwitchVar = medvarToExpr(SwitchMedVar);
  }

  if (!SwitchVar || SelectorSize == 0)
    return false;

  // Validate the final selector-coordinate bit-patterns before consuming any
  // target blocks.  A narrow selector cannot represent two distinct recovered
  // labels that truncate to the same value; keeping such a switch would make
  // HighIR ambiguous even when LLVM independently fails closed.
  auto CaseBits = uniqueSwitchCaseBitPatterns(
      Labels, LabelDelta, static_cast<unsigned>(SelectorSize) * 8u);
  if (!CaseBits)
    return false;

  std::map<va_t, int> TargetBlock;
  for (auto &MB : Med.Blocks) {
    if (!MB.Ops.empty())
      TargetBlock[MB.Ops.front().Addr] = MB.Id;
  }

  va_t BoundsCheckTarget = 0;
  for (auto PredId : CurBlock.Preds) {
    if (PredId < 0 || PredId >= static_cast<int>(Med.Blocks.size()))
      continue;
    auto &PredBlk = Med.Blocks[PredId];
    if (PredBlk.Ops.empty())
      continue;
    auto &LastOp = PredBlk.Ops.back();
    if (LastOp.Opcode == NdOp::COND_BR && LastOp.NumInputs >= 1 &&
        LastOp.Inputs[0].isConst()) {
      BoundsCheckTarget = LastOp.Inputs[0].ConstVal;
      break;
    }
  }

  int DefaultCaseIdx = -1;
  if (BoundsCheckTarget != 0) {
    for (size_t K = 0; K < JT.Targets.size(); ++K) {
      if (JT.Targets[K] == BoundsCheckTarget) {
        DefaultCaseIdx = static_cast<int>(K);
        break;
      }
    }
  }

  HighStmt SW;
  SW.Kind = StmtKind::Switch;
  SW.Addr = CurOp.Addr;
  SW.SwitchExpr = SwitchVar;
  for (size_t K = 0; K < JT.Targets.size(); ++K) {
    auto TIt = TargetBlock.find(JT.Targets[K]);
    std::vector<HighStmt> CaseBody;
    if (TIt != TargetBlock.end() && TIt->second >= 0 &&
        TIt->second < static_cast<int>(Med.Blocks.size())) {
      JtConsumedBlocks.insert(TIt->second);
      auto &CaseBlk = Med.Blocks[TIt->second];
      for (auto &COp : CaseBlk.Ops) {
        if (COp.Opcode == NdOp::RETURN) {
          HighStmt RS;
          RS.Kind = StmtKind::Return;
          RS.Addr = COp.Addr;
          if (COp.NumInputs >= 1)
            RS.RetVal = medvarToExpr(COp.Inputs[0]);
          CaseBody.push_back(std::move(RS));
        } else if (COp.Output.Id >= 0 && COp.Output.Size > 0 &&
                   COp.Output.Kind == MedVar::Reg && COp.Output.RegOff == 0) {
          HighStmt AS;
          AS.Kind = StmtKind::Assign;
          AS.Addr = COp.Addr;
          AS.Dst = HighExpr::makeVar(COp.Output);
          AS.Val = medOpToExpr(COp);
          CaseBody.push_back(std::move(AS));
        }
      }
    }
    if (CaseBody.empty()) {
      HighStmt GS;
      GS.Kind = StmtKind::Goto;
      GS.GotoTarget = JT.Targets[K];
      CaseBody.push_back(GS);
    }

    if (static_cast<int>(K) == DefaultCaseIdx) {
      SW.DefaultBody = std::move(CaseBody);
    } else {
      SwitchCase SC;
      SC.Value = (*CaseBits)[K];
      SC.Body = std::move(CaseBody);
      SW.Cases.push_back(std::move(SC));
    }
  }
  Func.Body.push_back(std::move(SW));
  return true;
}

//===----------------------------------------------------------------------===//
// lowerBranchInd
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerBranchInd(HighFunc &Func,
                                        const MedBlock &CurBlock,
                                        const MedOp &CurOp,
                                        const MedFunc &Med) {
  const bool MustFailClosed =
      Med.UnsafeIndirectBranchAddresses.count(CurOp.Addr) != 0;
  // Try to find a matching jump table for switch recovery.
  if (!MustFailClosed) {
    for (auto &JTE : JumpTables) {
      if (JTE.InsnAddr == CurOp.Addr && !JTE.Targets.empty() &&
          !JTE.MutatedUnsafe) {
        if (lowerSwitchFromJumpTable(Func, CurBlock, CurOp, Med, JTE))
          return;
      }
    }
  }

  // No jump table: tail call or plain indirect branch.
  bool IsTailCall = !MustFailClosed && CurBlock.Succs.empty();

  if (IsTailCall && CurOp.NumInputs >= 1) {
    ExprPtr TargetExpr = medvarToExpr(CurOp.Inputs[0]);

    size_t BrIdx = 0;
    for (size_t K = 0; K < CurBlock.Ops.size(); ++K) {
      if (&CurBlock.Ops[K] == &CurOp) {
        BrIdx = K;
        break;
      }
    }
    auto Args = collectCallArgs(CurBlock, BrIdx);

    std::string TargetName = "indirect_call";
    bool IsIndirect = true;
    int IndirectParam = -1;

    if (TargetExpr->Kind == ExprKind::Var &&
        TargetExpr->Var.Kind == MedVar::Reg) {
      int PIdx = regToArgIdx(TargetExpr->Var.RegOff);
      if (PIdx >= 0) {
        IndirectParam = PIdx;
        TargetName = "arg" + std::to_string(PIdx);
      } else {
        TargetName = TargetExpr->str();
      }
    } else if (TargetExpr->Kind == ExprKind::Const) {
      bool FoundName = false;
      if (FuncNames) {
        auto It = FuncNames->find(TargetExpr->ConstVal);
        if (It != FuncNames->end()) {
          TargetName = It->second;
          FoundName = true;
        }
      }
      if (FoundName)
        IsIndirect = false;
    }

    auto Call = HighExpr::makeCall(TargetName, CurOp.Addr, std::move(Args));
    Call->IsIndirectCall = IsIndirect;
    Call->IndirectParamIdx = IndirectParam;

    HighStmt RetStmt;
    RetStmt.Kind = StmtKind::Return;
    RetStmt.Addr = CurOp.Addr;
    RetStmt.RetVal = Call;
    Func.Body.push_back(std::move(RetStmt));
  } else {
    HighStmt S;
    S.Kind = StmtKind::Goto;
    S.Addr = CurOp.Addr;
    S.GotoTarget = InvalidVA;
    Func.Body.push_back(std::move(S));
  }
}

} // namespace neverd

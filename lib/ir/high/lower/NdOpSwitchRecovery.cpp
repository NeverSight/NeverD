//===- NdOpSwitchRecovery.cpp - Switch / indirect branch lowering ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// INDIR_BR lowering: jump-table-based switch recovery and explicit
/// refusal of unresolved indirect control transfers.
///
//===----------------------------------------------------------------------===//

#include "AnalysisDispatch.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedDispatchSlots.h"
#include "neverd/ir/med/MedSwitchNorm.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <optional>
#include <set>

namespace neverd {

//===----------------------------------------------------------------------===//
// Shared dispatch selectors
//===----------------------------------------------------------------------===//

MedDispatchContext MedToHighConverter::dispatchContext(const MedFunc &Med) {
  if (DispatchDefsFor != &Med) {
    DispatchDefsFor = &Med;
    DispatchDefs.clear();
    // The first definition of each value, as the LLVM route indexes them.
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (!Op.Output.isConst())
          DispatchDefs.try_emplace({static_cast<int>(Op.Output.Kind),
                                    Op.Output.Id, Op.Output.SSAVer},
                                   &Op);
  }
  MedDispatchContext Context;
  Context.Func = &Med;
  Context.Img = Image;
  Context.AddressBytes = Image && Image->getPointerSize() != 0
                             ? static_cast<uint16_t>(Image->getPointerSize())
                             : getTargetRegInfo(TargetArch).PointerSize;
  Context.DefOf = [this, &Med](const MedVar &V) -> const MedOp * {
    if (V.isConst() || Med.SkippedSSA)
      return nullptr;
    const auto It =
        DispatchDefs.find({static_cast<int>(V.Kind), V.Id, V.SSAVer});
    return It == DispatchDefs.end() ? nullptr : It->second;
  };
  Context.SlotKeyOf = [DefOf = Context.DefOf](const MedVar &V) {
    return medAddressSlotKey(V, DefOf);
  };
  // Only a value the GOT-base certificate names is its zero; a reload of one
  // leaves its table read unfolded, which fails closed.
  Context.ModelZero = [&Med](const MedVar &V) {
    return std::any_of(
        Med.ScalarAddressModels.begin(), Med.ScalarAddressModels.end(),
        [&](const MedScalarAddressModel &Model) {
          return Model.Model == RelocatedInstructionScalarModelOccurrence::
                                    ModelKind::I386ELFGOTBaseZero &&
                 Model.Value.Kind == V.Kind && Model.Value.Id == V.Id &&
                 Model.Value.SSAVer == V.SSAVer && Model.Value.Size == V.Size;
        });
  };
  return Context;
}

/// The selector dispatch block \p Block itself names: the resolver's index
/// register, the scaled index of the table load its target flows from, or the
/// constant entry it reads.  lowerSwitchFromJumpTable tries these first.
static std::optional<MedVar> blockSelector(const MedBlock &Block,
                                           const MedOp &Br, const JumpTable &JT,
                                           const MedDispatchContext &Context) {
  if (JT.IndexRegOff >= 0)
    for (auto Op = Block.Ops.rbegin(); Op != Block.Ops.rend(); ++Op)
      for (int I = 0; I < Op->NumInputs; ++I)
        if (Op->Inputs[I].Kind == MedVar::Reg &&
            static_cast<int>(Op->Inputs[I].RegOff) == JT.IndexRegOff)
          return Op->Inputs[I];
  if (Br.NumInputs < 1)
    return std::nullopt;
  if (!JT.HasTargetBase && !JT.PreScaledIndex)
    if (auto Index = findTableSwitchIndex(Context, Block, Br.Inputs[0]))
      return Index;
  return constTableLoadIndex(Context, Br.Inputs[0], Context.DefOf, JT,
                             /*RequireAuthenticatedLoad=*/false);
}

void MedToHighConverter::planDispatchSelectors(const MedFunc &Med) {
  DispatchSelectorVars.clear();
  DispatchSelectorCopies.clear();
  std::optional<MedDispatchContext> Context;
  for (const MedBlock &Block : Med.Blocks) {
    if (Block.Ops.empty() || Block.Preds.size() < 2)
      continue;
    const MedOp &Br = Block.Ops.back();
    if (Br.Opcode != NdOp::INDIR_BR || Br.NumInputs < 1 ||
        Br.Inputs[0].isConst() ||
        Med.UnsafeIndirectBranchAddresses.count(Br.Addr))
      continue;
    const JumpTable *JT = nullptr;
    for (const JumpTable &Table : Med.JumpTables)
      if (Table.InsnAddr == Br.Addr && !Table.Targets.empty() &&
          !Table.MutatedUnsafe) {
        JT = &Table;
        break;
      }
    if (!JT || JT->TwoTableSelect)
      continue;
    if (!Context)
      Context = dispatchContext(Med);
    if (blockSelector(Block, Br, *JT, *Context))
      continue;

    std::vector<std::pair<int, MedVar>> PerPred;
    unsigned Bytes = 0;
    const auto Plan = Med.SwitchSelectorPlans.find({JT->InsnAddr, Block.Id});
    if (Plan != Med.SwitchSelectorPlans.end()) {
      // An exact edge merge: one certified selector per predecessor.
      const MedSwitchSelectorPlan &Merge = Plan->second;
      if (Merge.PlanKind != MedSwitchSelectorPlan::Kind::EdgeMerged ||
          Merge.ResultSize == 0 ||
          Merge.EdgeSelectors.size() != Block.Preds.size())
        continue;
      const std::set<int> Expected(Block.Preds.begin(), Block.Preds.end());
      std::set<int> Planned;
      bool Valid = true;
      for (const auto &[Pred, Selector] : Merge.EdgeSelectors)
        Valid &= Selector.Size == Merge.ResultSize && !Selector.isConst() &&
                 Expected.count(Pred) && Planned.insert(Pred).second;
      if (!Valid || Planned != Expected)
        continue;
      PerPred = Merge.EdgeSelectors;
      Bytes = Merge.ResultSize;
    } else {
      // No occurrence names the selector: each -O0 goto site spills the
      // target its own table read selects to the slot the dispatch reloads.
      if (!JT->SelectorUseRefs.empty() || JT->CompositeSelectorUseRef ||
          JT->PreScaledIndex || JT->TwoLevelIndex || JT->HasTargetBase)
        continue;
      auto Shared = sharedDispatchSelectors(*Context, Block, Br, *JT);
      if (!Shared)
        continue;
      PerPred = std::move(Shared->PerPred);
      Bytes = Shared->MaxBytes;
    }
    if (Bytes == 0 || Bytes > 8)
      continue;

    MedVar Selector;
    Selector.Kind = MedVar::Temp;
    Selector.Id = NextHighTempId++;
    Selector.Size = static_cast<uint16_t>(Bytes);
    const TypeRef SelectorType = NdType::makeInt(Selector.Size, false);
    for (const auto &[Pred, Value] : PerPred) {
      // A narrower selector widens with zeros and a wider one keeps its low
      // bytes, as the LLVM route stores each into the dispatch slot.
      ExprPtr Widened;
      if (Value.isConst()) {
        const uint64_t Mask =
            Bytes == 8 ? ~uint64_t{0} : (uint64_t{1} << (Bytes * 8)) - 1;
        Widened = HighExpr::makeConst(Value.ConstVal & Mask, Selector.Size);
        Widened->Type = SelectorType;
      } else if (Value.Size == Bytes) {
        Widened = medvarToExpr(Value);
      } else if (Value.Size < Bytes) {
        Widened = HighExpr::makeUnary(NdOp::INT_ZEXT, medvarToExpr(Value));
        Widened->Type = SelectorType;
      } else {
        Widened = HighExpr::makeBinop(NdOp::SUBBYTES, medvarToExpr(Value),
                                      HighExpr::makeConst(0, 4));
        Widened->Type = SelectorType;
      }
      HighStmt Copy;
      Copy.Kind = StmtKind::Assign;
      const MedBlock &From = Med.Blocks[Pred];
      Copy.Addr = From.Ops.empty() ? From.StartAddr : From.Ops.back().Addr;
      Copy.Dst = HighExpr::makeVar(Selector, SelectorType);
      Copy.Val = std::move(Widened);
      DispatchSelectorCopies[{Pred, Block.Id}].push_back(std::move(Copy));
    }
    DispatchSelectorVars.emplace(std::make_pair(JT->InsnAddr, Block.Id),
                                 Selector);
  }
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

  auto PlanIt = Med.SwitchSelectorPlans.find({JT.InsnAddr, CurBlock.Id});
  const auto Planned = DispatchSelectorVars.find({JT.InsnAddr, CurBlock.Id});
  if (Planned != DispatchSelectorVars.end()) {
    // planDispatchSelectors gave a shared dispatch one selector, which each
    // predecessor sets on its edge.
    SwitchVar = HighExpr::makeVar(Planned->second,
                                  NdType::makeInt(Planned->second.Size, false));
    SelectorSize = Planned->second.Size;
    SelectorResolved = true;
  } else if (PlanIt != Med.SwitchSelectorPlans.end() &&
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
  const MedOp *Br = !CurBlock.Ops.empty() &&
                            CurBlock.Ops.back().Opcode == NdOp::INDIR_BR &&
                            CurBlock.Ops.back().NumInputs >= 1
                        ? &CurBlock.Ops.back()
                        : nullptr;
  std::optional<MedDispatchContext> Context;
  if (!SelectorResolved && Br)
    Context = dispatchContext(Med);
  if (!SelectorResolved && Br && !JT.HasTargetBase && !JT.PreScaledIndex)
    if (auto Index = findTableSwitchIndex(*Context, CurBlock, Br->Inputs[0])) {
      SwitchMedVar = *Index;
      HaveMedVar = true;
      SelectorResolved = true;
      PeelOK = true;
    }

  // -O0 dispatch with one goto site: the dispatch block only reloads the
  // target that site spilled, so the index lives in the predecessor, which
  // dominates the branch.
  if (!SelectorResolved && Br && CurBlock.Preds.size() == 1 &&
      CurBlock.Preds.front() >= 0 &&
      CurBlock.Preds.front() < static_cast<int>(Med.Blocks.size()))
    if (auto Slot = dispatchReloadSlot(*Context, CurBlock, *Br))
      if (auto Index = predecessorSwitchIndex(
              *Context, Med.Blocks[CurBlock.Preds.front()], *Slot)) {
        SwitchMedVar = *Index;
        HaveMedVar = true;
        SelectorResolved = true;
        PeelOK = true;
      }

  // A goto through one constant table entry (`jmp *tab+k`) dispatches on that
  // entry's case value.
  if (!SelectorResolved && Br)
    if (auto Index =
            constTableLoadIndex(*Context, Br->Inputs[0], Context->DefOf, JT,
                                /*RequireAuthenticatedLoad=*/false)) {
      SwitchMedVar = *Index;
      HaveMedVar = true;
      SelectorResolved = true;
    }

  // A shared dispatch reloads what several goto sites spilled; no value in
  // it or in one predecessor is the selector.  Without a planned one, leave
  // the transfer unresolved rather than guess one below.
  if (!SelectorResolved && Br && CurBlock.Preds.size() >= 2 &&
      dispatchReloadSlot(*Context, CurBlock, *Br))
    return false;

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
  // A dispatch by value has a case for every label it may reach and traps on
  // any other value: none of its labels is what an out-of-range index takes.
  if (BoundsCheckTarget != 0 && !JT.ValueDispatch) {
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
    // Dispatch owns the edge, not the successor's statements. A target can
    // also be reached by another case, a bounds guard or a loop. Its ordinary
    // block lowering must run once, preserving calls, stores, PHIs and exits.
    HighStmt Transfer;
    Transfer.Kind = StmtKind::Goto;
    Transfer.GotoTarget = JT.Targets[K];
    std::vector<HighStmt> CaseBody{std::move(Transfer)};

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
  HighFunc::SwitchLabels &Published = Func.SwitchLabelsByJump[CurOp.Addr];
  Published.Values = *CaseBits;
  Published.DefaultPosition = DefaultCaseIdx;
  Published.SelectorBits = static_cast<unsigned>(SelectorSize) * 8u;
  return true;
}

//===----------------------------------------------------------------------===//
// lowerBranchInd
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerBranchInd(HighFunc &Func,
                                        const MedBlock &CurBlock,
                                        const MedOp &CurOp,
                                        const MedFunc &Med) {
  // A jump that names only where it may go compares its target with each of
  // those labels, which the image's slots hold as the addresses they had
  // there.  Any other value has no label to go to.
  for (const JumpTable &Table : Med.JumpTables)
    if (Table.InsnAddr == CurOp.Addr && Table.ValueDispatch &&
        !Table.Targets.empty() &&
        Table.CaseLabels.size() == Table.Targets.size()) {
      if (!lowerSwitchFromJumpTable(Func, CurBlock, CurOp, Med, Table))
        break;
      HighStmt Trap;
      Trap.Kind = StmtKind::Goto;
      Trap.Addr = CurOp.Addr;
      Trap.GotoTarget = InvalidVA;
      Func.Body.back().DefaultBody.clear();
      Func.Body.back().DefaultBody.push_back(std::move(Trap));
      return;
    }

  const bool MustFailClosed =
      Med.UnsafeIndirectBranchAddresses.count(CurOp.Addr) != 0;
  // Try to find a matching jump table for switch recovery.
  if (!MustFailClosed) {
    for (auto &JTE : Med.JumpTables) {
      if (JTE.InsnAddr == CurOp.Addr && !JTE.Targets.empty() &&
          !JTE.MutatedUnsafe) {
        if (lowerSwitchFromJumpTable(Func, CurBlock, CurOp, Med, JTE))
          return;
      }
    }
  }

  if (Func.AnalysisOnly && CurOp.NumInputs == 1)
    for (const auto &Table : Med.JumpTables)
      if (Table.AnalysisOnly && Table.InsnAddr == CurOp.Addr) {
        MedVar Snapshot;
        Snapshot.Kind = MedVar::Temp;
        Snapshot.Id = NextHighTempId++;
        Snapshot.Size = CurOp.Inputs[0].Size;
        if (high_detail::appendAnalysisDispatch(
                Func, CurOp, Table, medvarToExpr(CurOp.Inputs[0]), Snapshot))
          return;
      }

  // CFG construction owns tail-call classification and emits an explicit
  // INDIR_CALL + RETURN pair when that classification succeeds. A remaining
  // INDIR_BR is an unresolved control transfer; an empty successor list must
  // not grant a second, weaker tail-call classification in the source route.
  HighStmt S;
  S.Kind = StmtKind::Goto;
  S.Addr = CurOp.Addr;
  S.GotoTarget = InvalidVA;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd

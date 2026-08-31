//===- MedLLVMSwitch.cpp - Jump-table switch lowering ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Jump-table switch lowering for MedLLVMEmitter: synthesizing the switch
/// selector for a shared -O0 computed-goto dispatch and for a two-table
/// select, and emitting the LLVM switch in emitJumpTableSwitch.  The index
/// recovery those build on lives in MedLLVMSwitchIndex.cpp.  The jump
/// tables themselves are resolved earlier by the low-IR JumpTableResolver;
/// this only lowers a resolved table to a switch.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedSwitchNorm.h"

#define DEBUG_TYPE "neverd-med-llvm-switch"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// Jump-table switch lowering
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::synthesizeSharedDispatchIndex(
    const MedBlock &Blk, const MedOp &BrOp, const JumpTable &JT,
    llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !CurFunc || Blk.Preds.size() < 2)
    return nullptr;
  auto SlotKey = reloadSlotKeyOf(Blk, BrOp);
  if (!SlotKey)
    return nullptr;

  // Recover every predecessor's index.  Bail (returning null) if any cannot be
  // resolved.  Predecessors indexing DIFFERENT tables (clang -O0 funnels every
  // `goto *p` in a function through one dispatch block, so two distinct tables
  // `t1[i]`/`t2[j]` can share it) would make the common-index merge unsound;
  // that case is detected and invalidated upstream in the resolver (where table
  // bases are robustly resolved cross-arch via foldRegConstant), so a
  // mixed-table dispatch never reaches here with targets.
  std::vector<std::pair<int, MedVar>> PerPred;
  unsigned MaxBytes = 0;
  std::optional<uint64_t> CommonBase;
  for (int PredId : Blk.Preds) {
    const MedBlock *Pred = nullptr;
    for (auto &B : CurMedFunc->Blocks)
      if (B.Id == PredId) {
        Pred = &B;
        break;
      }
    if (!Pred)
      return nullptr;
    auto Idx = tracePredSwitchIndex(*Pred, *SlotKey);
    if (Idx) {
      // A variable-index goto-site: enforce a single shared table.  Bail on a
      // POSITIVE table-base mismatch (two predecessors resolve to different
      // data-segment table VAs → different tables → a common-index merge would
      // mis-route, so trap loudly instead).  When a base cannot be resolved to
      // a data-segment constant (e.g. an ARM PC-relative literal-pool base),
      // proceed: the common single-table threaded-dispatch shape must keep
      // working.
      if (auto Base = predTableBaseVA(*Pred, *SlotKey)) {
        if (!CommonBase)
          CommonBase = Base;
        else if (*CommonBase != *Base)
          return nullptr;
      }
    } else {
      // A constant-index goto-site (`goto *tab[k]`, clang -O0 folds it to a
      // load from a constant slot address): recover the exact entry against
      // THIS table.  No base check is needed — the constant address resolves to
      // one table entry, and a constant into a different table maps out of
      // range (or misaligned) and bails, so it cannot mis-route.
      Idx = tracePredConstDispatchIndex(*Pred, *SlotKey, JT);
      if (!Idx)
        return nullptr;
    }
    MaxBytes = std::max<unsigned>(MaxBytes, Idx->Size ? Idx->Size : 4);
    PerPred.emplace_back(PredId, *Idx);
  }
  if (PerPred.size() < 2 || MaxBytes == 0)
    return nullptr;

  // A common stack slot communicates the per-predecessor index to the dispatch
  // block; each predecessor's store is deferred (inserted before its terminator
  // once all blocks are emitted).
  auto *Ty = sizeToType(MaxBytes);
  auto &Entry = CurFunc->getEntryBlock();
  llvm::IRBuilder<> AllocB(&Entry, Entry.begin());
  auto *Slot = AllocB.CreateAlloca(Ty, nullptr, "cgoto_idx");
  PendingDispatchStores.push_back({Slot, Ty, std::move(PerPred)});
  return Builder.CreateLoad(Ty, Slot, "cgoto_idx_v");
}

llvm::Value *
MedLLVMEmitter::synthesizeTwoTableSelector(const JumpTable &JT,
                                           llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !JT.CompositeSelectorUseRef)
    return nullptr;
  auto It = CurMedFunc->SwitchSelectorPlans.find(JT.InsnAddr);
  if (It == CurMedFunc->SwitchSelectorPlans.end())
    return nullptr;
  const MedSwitchSelectorPlan &Plan = It->second;
  if (Plan.PlanKind != MedSwitchSelectorPlan::Kind::SelectOffset ||
      Plan.Selector.Size == 0 || Plan.Selector.Size != Plan.ResultSize ||
      Plan.Selector.isConst() || Plan.Condition.Size == 0 ||
      Plan.Condition.isConst())
    return nullptr;

  llvm::Value *Index = getVar(Plan.Selector, Builder);
  llvm::Value *Condition = getVar(Plan.Condition, Builder);
  if (!Index || !Index->getType()->isIntegerTy() || !Condition ||
      !Condition->getType()->isIntegerTy())
    return nullptr;
  auto *Ty = llvm::cast<llvm::IntegerType>(Index->getType());
  if (Ty->getBitWidth() != Plan.ResultSize * CHAR_BIT)
    return nullptr;
  const unsigned Bits = Ty->getBitWidth();
  if (Bits < 64 &&
      ((Plan.TrueOffset >> Bits) != 0 || (Plan.FalseOffset >> Bits) != 0))
    return nullptr;
  if (!Condition->getType()->isIntegerTy(1))
    Condition = Builder.CreateICmpNE(
        Condition, llvm::ConstantInt::get(Condition->getType(), 0));
  llvm::Value *Offset = Builder.CreateSelect(
      Condition, llvm::ConstantInt::get(Ty, Plan.TrueOffset),
      llvm::ConstantInt::get(Ty, Plan.FalseOffset), "twotbl.offset");
  return Builder.CreateAdd(Index, Offset, "twotbl.idx");
}

bool MedLLVMEmitter::emitJumpTableSwitch(
    const MedBlock &Blk, const MedOp &BrOp,
    std::map<int, llvm::BasicBlock *> &BBMap, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc)
    return false;
  if (CurMedFunc->UnsafeIndirectBranchAddresses.count(BrOp.Addr))
    return false;

  const JumpTable *JT = nullptr;
  for (auto &T : CurMedFunc->JumpTables)
    if (T.InsnAddr == BrOp.Addr && !T.Targets.empty()) {
      JT = &T;
      break;
    }
  if (!JT)
    return false;

  // A runtime-permuted stack-materialised computed-goto table: the recovered
  // static targets no longer describe the runtime index->target mapping, so an
  // index-dispatch switch would silently pick the wrong case.  Refuse to build
  // it; the INDIR_BR path then emits a loud trap (its successors, the static
  // targets, keep it off the indirect-tail-call path).  Sound resolution would
  // need runtime value dispatch — a separate, documented gap (localtab-cgoto).
  if (JT->MutatedUnsafe)
    return false;

  if (BrOp.NumInputs < 1 || BrOp.Inputs[0].isConst())
    return false;

  std::optional<MedVar> IndexVar;
  auto indexFromSelectorPlan = [&]() -> std::optional<MedVar> {
    auto It = CurMedFunc->SwitchSelectorPlans.find(JT->InsnAddr);
    if (It == CurMedFunc->SwitchSelectorPlans.end() ||
        It->second.PlanKind != MedSwitchSelectorPlan::Kind::Direct ||
        It->second.Selector.Size == 0 || It->second.Selector.isConst() ||
        It->second.Selector.Size != It->second.ResultSize)
      return std::nullopt;
    return It->second.Selector;
  };
  auto edgeMergedIndexFromSelectorPlan = [&]() -> llvm::Value * {
    auto It = CurMedFunc->SwitchSelectorPlans.find(JT->InsnAddr);
    if (It == CurMedFunc->SwitchSelectorPlans.end() ||
        It->second.PlanKind != MedSwitchSelectorPlan::Kind::EdgeMerged ||
        It->second.ResultSize == 0 || It->second.EdgeSelectors.size() < 2 ||
        It->second.EdgeSelectors.size() != Blk.Preds.size())
      return nullptr;
    const MedSwitchSelectorPlan &Plan = It->second;
    std::set<int> ExpectedPreds(Blk.Preds.begin(), Blk.Preds.end());
    std::set<int> PlannedPreds;
    for (const auto &[Pred, Selector] : Plan.EdgeSelectors)
      if (Selector.Size != Plan.ResultSize || Selector.isConst() ||
          !ExpectedPreds.count(Pred) || !PlannedPreds.insert(Pred).second)
        return nullptr;
    if (PlannedPreds != ExpectedPreds) {
      return nullptr;
    }

    auto *Ty = sizeToType(Plan.ResultSize);
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocB(&Entry, Entry.begin());
    auto *Slot = AllocB.CreateAlloca(Ty, nullptr, "jt.edge.idx.slot");
    PendingDispatchStores.push_back({Slot, Ty, Plan.EdgeSelectors});
    return Builder.CreateLoad(Ty, Slot, "jt.edge.idx");
  };
  // Pull the switch index from the resolver-identified register (the one that
  // addresses the table load).
  auto indexFromResolverReg = [&]() -> std::optional<MedVar> {
    if (JT->IndexRegOff < 0)
      return std::nullopt;
    for (auto It = Blk.Ops.rbegin(); It != Blk.Ops.rend(); ++It)
      for (int I = 0; I < It->NumInputs; ++I)
        if (It->Inputs[I].Kind == MedVar::Reg &&
            static_cast<int>(It->Inputs[I].RegOff) == JT->IndexRegOff)
          return It->Inputs[I];
    return std::nullopt;
  };
  auto indexFromCompactTableLoad = [&]() -> std::optional<MedVar> {
    if (JT->TableLoadAddr == InvalidVA || !JT->HasBaseAddr ||
        JT->EntrySize == 0)
      return std::nullopt;

    const MedOp *TableLoad = nullptr;
    for (const MedOp &Op : Blk.Ops) {
      if (Op.Addr != JT->TableLoadAddr || Op.Opcode != NdOp::LOAD ||
          Op.Output.Size != JT->EntrySize || Op.NumInputs < 1)
        continue;
      if (TableLoad)
        return std::nullopt;
      TableLoad = &Op;
    }
    if (!TableLoad)
      return std::nullopt;

    MedVar Address = TableLoad->Inputs[TableLoad->NumInputs >= 2 ? 1 : 0];
    const MedOp *AddressDef = nullptr;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      AddressDef = lookupDef(Address);
      if (!AddressDef)
        return std::nullopt;
      if (AddressDef->Opcode == NdOp::COPY && AddressDef->NumInputs >= 1) {
        Address = AddressDef->Inputs[0];
        continue;
      }
      break;
    }
    if (!AddressDef || AddressDef->Opcode != NdOp::INT_ADD ||
        AddressDef->NumInputs < 2)
      return std::nullopt;

    std::optional<MedVar> Dynamic;
    for (int BaseSide = 0; BaseSide < 2; ++BaseSide) {
      auto Base = traceSSAConst(AddressDef->Inputs[BaseSide]);
      if (!Base || *Base != JT->BaseAddr)
        continue;
      if (Dynamic)
        return std::nullopt;
      Dynamic = AddressDef->Inputs[1 - BaseSide];
    }
    if (!Dynamic || Dynamic->isConst())
      return std::nullopt;

    // TBH and halfword compact tables scale the index in the address.  Peel
    // exactly that entry-size scale; do not use the later target-entry shift,
    // which is the distinct value this helper exists to avoid.
    const MedOp *Scale = lookupDef(*Dynamic);
    if (Scale &&
        (Scale->Opcode == NdOp::INT_MULT || Scale->Opcode == NdOp::INT_LEFT) &&
        Scale->NumInputs >= 2) {
      if (Scale->Opcode == NdOp::INT_MULT && Scale->Inputs[1].isConst() &&
          Scale->Inputs[1].ConstVal == JT->EntrySize &&
          !Scale->Inputs[0].isConst())
        return Scale->Inputs[0];
      if (Scale->Opcode == NdOp::INT_LEFT && Scale->Inputs[1].isConst() &&
          JT->EntrySize > 0 && Scale->Inputs[1].ConstVal < 64 &&
          (uint64_t{1} << Scale->Inputs[1].ConstVal) == JT->EntrySize &&
          !Scale->Inputs[0].isConst())
        return Scale->Inputs[0];
      return std::nullopt;
    }
    return Dynamic;
  };

  // A two-table dispatch synthesizes its byte-offset selector from the runtime
  // base select below; the single-table index recovery does not apply.
  if (!JT->TwoTableSelect) {
    // Once LowIR publishes an exact occurrence, it is the selector contract.
    // A missing final Med binding means the source op was deleted, duplicated,
    // or changed role; never fall back to a physical register/DAG guess.
    if (!JT->SelectorUseRefs.empty()) {
      IndexVar = indexFromSelectorPlan();
      const auto PlanIt = CurMedFunc->SwitchSelectorPlans.find(JT->InsnAddr);
      const bool HasEdgeMergedPlan =
          PlanIt != CurMedFunc->SwitchSelectorPlans.end() &&
          PlanIt->second.PlanKind == MedSwitchSelectorPlan::Kind::EdgeMerged;
      if (!IndexVar && PlanIt == CurMedFunc->SwitchSelectorPlans.end()) {
        // A source selector can disappear only because MedIR folded the exact
        // table-address occurrence to one constant slot (for example the
        // peeled first iteration of an i386 PIC switch).  Recover that slot
        // from the authenticated branch-target LOAD itself; every other
        // missing, duplicated, or role-changed occurrence still fails closed.
        IndexVar =
            traceConstBranchIndex(BrOp, *JT, /*RequireAuthenticatedLoad=*/true);
        if (!IndexVar)
          return false;
      }
      if (!IndexVar && !HasEdgeMergedPlan)
        return false;
    }
    // A two-level (index-byte) table composes its per-case targets from
    // `jmptab[idxtab[switchvar]]`; the switch condition is the *real* switch
    // variable that indexes idxtab, which the resolver recorded in IndexRegOff.
    // Tracing the branch target back (findSwitchIndex, below) would instead
    // find the intermediate address-table index, so anchor to the resolver
    // occurrence and require it — never fall through to register-number or
    // single-level tracers.
    if (!IndexVar && JT->TwoLevelIndex) {
      IndexVar = indexFromSelectorPlan();
      if (!IndexVar)
        return false;
    }
    // Compact byte/halfword tables (TargetBase set, 1/2-byte entries) scale the
    // loaded *entry* (`entry << k`), which findSwitchIndex would wrongly return
    // as the index, so anchor to the resolver register first.  Word tables
    // (4-byte entries added to a separate `adr` anchor) instead scale the
    // *index* into the load address and routinely reuse the resolver register
    // as the anchor and the branch target at -O0, so prefer findSwitchIndex,
    // which traces the scaled index cleanly through the table load.
    if (!IndexVar && JT->HasTargetBase && JT->EntrySize <= 2) {
      IndexVar = indexFromCompactTableLoad();
      if (!IndexVar)
        return false;
    }
    // A pre-scaled computed goto has no scale multiply in the table address, so
    // findSwitchIndex would latch onto an unrelated multiply in the dispatch
    // block (e.g. an LCG step); the index register the resolver identified
    // already holds the byte offset the switch dispatches on.  Consume only
    // the exact post-SSA selector plan; a stale physical register lifetime is
    // not a fallback identity.
    if (!IndexVar && JT->PreScaledIndex) {
      IndexVar = indexFromSelectorPlan();
      if (!IndexVar)
        return false;
    }
    // Regular absolute/relative tables: trace the branch target back to the
    // scaled index.
    if (!IndexVar)
      IndexVar = findSwitchIndex(Blk, BrOp.Inputs[0]);
    // -O0 shared/decoupled dispatch: the index lives in a predecessor goto-site
    // block (the dispatch block only reloads the spilled target).  Cross into
    // the single predecessor to recover it.  Tried before the resolver-register
    // fallback below: a reload dispatch parks the spilled *target* in the index
    // register on targets that reuse it (x86-64/i386 -O0 reload the target into
    // the same register that addressed the table — so the dispatch-block scan
    // would mistake the reloaded target for the index).  findSwitchIndexCross-
    // Block is gated on a frame reload, so a genuine same-block index/target
    // alias still falls through to the resolver register below.
    if (!IndexVar)
      IndexVar = findSwitchIndexCrossBlock(Blk, BrOp);
    // Computed-goto / threaded dispatch where the index register aliases the
    // loaded branch-target register (`and w,w,#n; ldr x,[b,w]; br x`): the
    // trace above cannot separate index from target, so fall back to the
    // resolver register, which is unambiguous.
    if (!IndexVar)
      IndexVar = indexFromResolverReg();
    // A single-site constant-index goto (`jmp *tab+k`, e.g. 32-bit x86/ARM -O0,
    // or a clang -O2 hoisted constant table read): no scaled index, so the
    // tracers above miss it.  Fold the branch target to one constant table
    // entry and dispatch on that entry's constant index (LLVM lowers the
    // resulting single-value switch to a direct branch).  Tried before the
    // bail/shared paths so single-predecessor constant branches resolve too.
    if (!IndexVar)
      IndexVar =
          traceConstBranchIndex(BrOp, *JT, /*RequireAuthenticatedLoad=*/false);
    // A shared multi-site -O0 dispatch (≥2 predecessors, each with its own
    // index) is recovered after target validation via
    // synthesizeSharedDispatchIndex; only bail here when there is no such
    // fallback (≤1 predecessor).
    if (!IndexVar && Blk.Preds.size() < 2) {
      return false;
    }
  }

  // Present the switch on the *source* variable and labels rather than the
  // zero-based table index.  A switch whose lowest case label is not 0 is
  // lowered by normalizing the variable to a table index (`idx = x - lo`, or
  // `idx = x + k` for a negative-based switch); dispatching on `idx` yields
  // cases 0..N-1 instead of the real (possibly negative) labels.  Peel that
  // affine step off the dispatch variable and shift every case label by the
  // inverse constant — an exact equivalence that restores the original variable
  // and labels.  Explicit CaseLabels are exact keys in the selector's current
  // coordinate, so an ordinary sparse/gapped table can still require this
  // inverse step.  Compact/pre-scaled/two-table forms use a different
  // coordinate and are excluded below.
  const bool HaveLabels = JT->CaseLabels.size() == JT->Targets.size();
  std::vector<int64_t> Labels;
  Labels.reserve(JT->Targets.size());
  for (size_t K = 0; K < JT->Targets.size(); ++K)
    Labels.push_back(HaveLabels ? JT->CaseLabels[K] : static_cast<int64_t>(K));
  uint64_t LabelDelta = 0;
  if (IndexVar && !JT->TwoTableSelect && !JT->PreScaledIndex &&
      !JT->HasTargetBase)
    IndexVar = peelAffineSwitchVar(*CurMedFunc, *IndexVar, LabelDelta,
                                   /*MaxAffine=*/2, &Labels);

  // Map each target address to its block, resolving through the dispatching
  // block's own successors first.  An x87 peeled/steady loop yields duplicate
  // blocks that share a start address (fixupFpuStack splits them by stack TOP),
  // so only the matching successor copy is the correct case target.  A target
  // that is not a successor (the ordinary single-copy table) falls back to a
  // whole-function address lookup, preserving the historical behavior.
  auto isSucc = [&](int Id) {
    return std::find(Blk.Succs.begin(), Blk.Succs.end(), Id) != Blk.Succs.end();
  };
  std::map<va_t, int> SuccAddrToBlock;
  for (auto &MB : CurMedFunc->Blocks)
    if (!MB.Ops.empty() && isSucc(MB.Id))
      SuccAddrToBlock.emplace(MB.Ops.front().Addr, MB.Id);

  auto blockForTarget = [&](va_t T) -> int {
    auto It = SuccAddrToBlock.find(T);
    if (It != SuccAddrToBlock.end())
      return It->second;
    for (auto &MB : CurMedFunc->Blocks)
      if (!MB.Ops.empty() && MB.Ops.front().Addr == T)
        return MB.Id;
    return -1;
  };

  std::vector<llvm::BasicBlock *> CaseBlocks;
  CaseBlocks.reserve(JT->Targets.size());
  for (va_t T : JT->Targets) {
    int BId = blockForTarget(T);
    if (BId < 0) {
      return false;
    }
    auto BIt = BBMap.find(BId);
    if (BIt == BBMap.end()) {
      return false;
    }
    CaseBlocks.push_back(BIt->second);
  }
  if (CaseBlocks.empty()) {
    return false;
  }

  llvm::Value *Index =
      JT->TwoTableSelect ? synthesizeTwoTableSelector(*JT, Builder)
      : IndexVar         ? getVar(*IndexVar, Builder)
      : !JT->SelectorUseRefs.empty()
          ? edgeMergedIndexFromSelectorPlan()
          : synthesizeSharedDispatchIndex(Blk, BrOp, *JT, Builder);
  if (!Index || !Index->getType()->isIntegerTy()) {
    return false;
  }
  auto *IdxTy = llvm::cast<llvm::IntegerType>(Index->getType());

  // The table address calculation wraps at the target's pointer width.  Some
  // i386 lifts represent preceding arithmetic in i64 (for example after a
  // zext), but comparing that widened, non-wrapping value against sign-extended
  // i32 case labels lets LLVM prove every negative case impossible.  Bring the
  // switch condition back into the machine-address domain before optimization.
  const unsigned PtrBits = getTargetRegInfo(TargetArch).PointerSize * 8;
  assert((PtrBits == 32 || PtrBits == 64) &&
         "jump-table switch requires a supported pointer width");
  if (IdxTy->getBitWidth() > PtrBits) {
    IdxTy = llvm::IntegerType::get(*Ctx, PtrBits);
    Index = Builder.CreateTrunc(Index, IdxTy, "jt.idx.machine");
  }

  const unsigned IdxBits = IdxTy->getBitWidth();
  auto CaseBits = uniqueSwitchCaseBitPatterns(Labels, LabelDelta, IdxBits);
  if (!CaseBits) {
    return false;
  }

  // A recovered table describes only the selectors whose target mapping was
  // proved.  Routing any gap, truncated composite domain, or stale/out-of-
  // range selector to case zero silently invents guest control flow.  Keep a
  // distinct loud default even when an upstream mask normally makes it
  // unreachable; LLVM may eliminate it only after proving that fact itself.
  auto *DefaultBB = llvm::BasicBlock::Create(*Ctx, "jt.default.trap", CurFunc);
  llvm::IRBuilder<> DefaultBuilder(DefaultBB);
  DefaultBuilder.CreateIntrinsic(llvm::Type::getVoidTy(*Ctx),
                                 llvm::Intrinsic::trap, {});
  DefaultBuilder.CreateUnreachable();
  auto *SW = Builder.CreateSwitch(Index, DefaultBB,
                                  static_cast<unsigned>(CaseBlocks.size()));
  for (size_t K = 0; K < CaseBlocks.size(); ++K)
    SW->addCase(llvm::ConstantInt::get(IdxTy, (*CaseBits)[K]), CaseBlocks[K]);
  return true;
}

} // namespace neverd

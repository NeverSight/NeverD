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

llvm::Value *MedLLVMEmitter::synthesizeTwoTableSelector(
    const MedBlock &Blk, const MedOp &BrOp, const JumpTable &JT,
    llvm::IRBuilder<> &Builder) {
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
  // Trace a value through COPY / extend / truncate to its real defining op.
  auto skipCopies = [&](MedVar V) -> const MedOp * {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      const MedOp *D = defOf(V);
      if (!D)
        return nullptr;
      if ((D->Opcode == NdOp::COPY || D->Opcode == NdOp::INT_ZEXT ||
           D->Opcode == NdOp::INT_SEXT || D->Opcode == NdOp::SUBBYTES) &&
          D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
        V = D->Inputs[0];
        continue;
      }
      return D;
    }
    return nullptr;
  };
  // Classify a select mask: 0 = base mask M (INT_NEG2-derived), 1 = the
  // negated ~M (INT_NOT-derived), -1 = not a mask.
  auto maskKind = [&](MedVar M) -> int {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      const MedOp *D = defOf(M);
      if (!D)
        return -1;
      if (D->Opcode == NdOp::INT_NOT)
        return 1;
      if (D->Opcode == NdOp::INT_NEG2)
        return 0;
      if (D->Opcode == NdOp::COPY && D->NumInputs >= 1 &&
          !D->Inputs[0].isConst()) {
        M = D->Inputs[0];
        continue;
      }
      return -1;
    }
    return -1;
  };

  // The branch target is the loaded table entry; trace it to the LOAD.
  const MedOp *LoadOp = nullptr;
  {
    MedVar V = BrOp.Inputs[0];
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      const MedOp *D = defOf(V);
      if (!D)
        break;
      if (D->Opcode == NdOp::LOAD) {
        LoadOp = D;
        break;
      }
      if (D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
        V = D->Inputs[0];
        continue;
      }
      break;
    }
  }
  if (!LoadOp || LoadOp->NumInputs < 1)
    return nullptr;
  const MedVar &AddrV = LoadOp->Inputs[LoadOp->NumInputs >= 2 ? 1 : 0];
  const MedOp *AddOp = skipCopies(AddrV);
  if (!AddOp || AddOp->Opcode != NdOp::INT_ADD || AddOp->NumInputs < 2)
    return nullptr;

  uint64_t D = JT.TwoTableOffset;
  bool HiPositive = JT.TwoTableHiPositive;

  // The base operand is the runtime-selected table pointer; the other is the
  // already byte-scaled index contribution.  selector = idx_bytes + (the higher
  // table is selected ? D : 0).
  for (int BaseW = 0; BaseW < 2; ++BaseW) {
    const MedOp *BD = skipCopies(AddOp->Inputs[BaseW]);
    if (!BD)
      continue;
    llvm::Value *IdxVal = getVar(AddOp->Inputs[1 - BaseW], Builder);
    if (!IdxVal || !IdxVal->getType()->isIntegerTy())
      continue;
    auto *Ty = llvm::cast<llvm::IntegerType>(IdxVal->getType());
    llvm::Value *SelOff = nullptr;

    if (BD->Opcode == NdOp::SELECT && BD->NumInputs >= 3) {
      // base = cond ? in1 : in2; the higher table is the positive (true) arm
      // when HiPositive.
      llvm::Value *C = getVar(BD->Inputs[0], Builder);
      if (!C)
        continue;
      if (!C->getType()->isIntegerTy(1))
        C = Builder.CreateICmpNE(C, llvm::ConstantInt::get(C->getType(), 0));
      llvm::Value *Dc = llvm::ConstantInt::get(Ty, D);
      llvm::Value *Z = llvm::ConstantInt::get(Ty, 0);
      SelOff = HiPositive ? Builder.CreateSelect(C, Dc, Z)
                          : Builder.CreateSelect(C, Z, Dc);
    } else if (BD->Opcode == NdOp::INT_OR && BD->NumInputs >= 2) {
      // base = (A & M) | (B & ~M); the offset is D masked by whichever mask
      // selects the higher table.
      const MedOp *And0 = skipCopies(BD->Inputs[0]);
      const MedOp *And1 = skipCopies(BD->Inputs[1]);
      if (!And0 || !And1 || And0->Opcode != NdOp::INT_AND ||
          And1->Opcode != NdOp::INT_AND || And0->NumInputs < 2 ||
          And1->NumInputs < 2)
        continue;
      auto pickMask = [&](const MedOp *A) -> std::optional<MedVar> {
        for (int W = 0; W < 2; ++W)
          if (maskKind(A->Inputs[W]) >= 0)
            return A->Inputs[W];
        return std::nullopt;
      };
      auto M0 = pickMask(And0), M1 = pickMask(And1);
      if (!M0 || !M1)
        continue;
      int K0 = maskKind(*M0), K1 = maskKind(*M1);
      if (K0 < 0 || K1 < 0 || K0 == K1)
        continue;
      MedVar BaseMask = (K0 == 0) ? *M0 : *M1;
      MedVar NotMask = (K0 == 1) ? *M0 : *M1;
      llvm::Value *Mv = getVar(HiPositive ? BaseMask : NotMask, Builder);
      if (!Mv || !Mv->getType()->isIntegerTy())
        continue;
      if (Mv->getType() != Ty)
        Mv = Builder.CreateZExtOrTrunc(Mv, Ty);
      SelOff = Builder.CreateAnd(Mv, llvm::ConstantInt::get(Ty, D));
    } else {
      continue;
    }

    if (SelOff)
      return Builder.CreateAdd(IdxVal, SelOff, "twotbl.idx");
  }
  return nullptr;
}

bool MedLLVMEmitter::emitJumpTableSwitch(
    const MedBlock &Blk, const MedOp &BrOp,
    std::map<int, llvm::BasicBlock *> &BBMap, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc)
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

  // A two-table dispatch synthesizes its byte-offset selector from the runtime
  // base select below; the single-table index recovery does not apply.
  if (!JT->TwoTableSelect) {
    // A two-level (index-byte) table composes its per-case targets from
    // `jmptab[idxtab[switchvar]]`; the switch condition is the *real* switch
    // variable that indexes idxtab, which the resolver recorded in IndexRegOff.
    // Tracing the branch target back (findSwitchIndex, below) would instead
    // find the intermediate address-table index, so anchor to the resolver
    // register and require it — never fall through to the single-level tracers.
    if (JT->TwoLevelIndex) {
      IndexVar = indexFromResolverReg();
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
    if (JT->TargetBase != 0 && JT->EntrySize <= 2)
      IndexVar = indexFromResolverReg();
    // A pre-scaled computed goto has no scale multiply in the table address, so
    // findSwitchIndex would latch onto an unrelated multiply in the dispatch
    // block (e.g. an LCG step); the index register the resolver identified
    // already holds the byte offset the switch dispatches on.
    if (!IndexVar && JT->PreScaledIndex)
      IndexVar = indexFromResolverReg();
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
      IndexVar = traceConstBranchIndex(BrOp, *JT);
    // A shared multi-site -O0 dispatch (≥2 predecessors, each with its own
    // index) is recovered after target validation via
    // synthesizeSharedDispatchIndex; only bail here when there is no such
    // fallback (≤1 predecessor).
    if (!IndexVar && Blk.Preds.size() < 2)
      return false;
  }

  // Present the switch on the *source* variable and labels rather than the
  // zero-based table index.  A switch whose lowest case label is not 0 is
  // lowered by normalizing the variable to a table index (`idx = x - lo`, or
  // `idx = x + k` for a negative-based switch); dispatching on `idx` yields
  // cases 0..N-1 instead of the real (possibly negative) labels.  Peel that
  // affine step off the dispatch variable and shift every case label by the
  // inverse constant — an exact equivalence that restores the original variable
  // and labels.  Skipped for the compact/pre-scaled/two-table forms, whose
  // labels the resolver already expresses in the pre-normalization coordinate.
  int64_t LabelDelta = 0;
  if (IndexVar && !JT->TwoTableSelect && !JT->PreScaledIndex &&
      JT->TargetBase == 0 && JT->CaseLabels.empty())
    IndexVar = peelAffineSwitchVar(*CurMedFunc, *IndexVar, LabelDelta);

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
    if (BId < 0)
      return false;
    auto BIt = BBMap.find(BId);
    if (BIt == BBMap.end())
      return false;
    CaseBlocks.push_back(BIt->second);
  }
  if (CaseBlocks.empty())
    return false;

  llvm::Value *Index =
      JT->TwoTableSelect ? synthesizeTwoTableSelector(Blk, BrOp, *JT, Builder)
      : IndexVar ? getVar(*IndexVar, Builder)
                 : synthesizeSharedDispatchIndex(Blk, BrOp, *JT, Builder);
  if (!Index || !Index->getType()->isIntegerTy())
    return false;
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

  // A masked index covers every case, so the default is unreachable; route it
  // to the first target to satisfy LLVM's required default destination.
  auto *SW = Builder.CreateSwitch(Index, CaseBlocks[0],
                                  static_cast<unsigned>(CaseBlocks.size()));
  const bool HaveLabels = JT->CaseLabels.size() == JT->Targets.size();
  unsigned IdxBits = IdxTy->getBitWidth();
  for (size_t K = 0; K < CaseBlocks.size(); ++K) {
    int64_t Label =
        (HaveLabels ? JT->CaseLabels[K] : static_cast<int64_t>(K)) + LabelDelta;
    // Truncate the (possibly negative or peeled) label to the index type's
    // width before building the constant: an LLVM switch case must match the
    // condition width exactly, and the unsigned ConstantInt path asserts when
    // the raw value does not fit without implicit truncation.  Masking yields
    // the correct two's-complement bit pattern the switch compares against.
    uint64_t Bits = static_cast<uint64_t>(Label);
    if (IdxBits < 64)
      Bits &= (1ULL << IdxBits) - 1;
    SW->addCase(llvm::ConstantInt::get(IdxTy, Bits), CaseBlocks[K]);
  }
  return true;
}

} // namespace neverd

//===- JumpTableResolverEmu.cpp - Path-based target emulation ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Path collection and NdOp-emulation fallbacks for jump-table resolution.
/// These members gather an unambiguous predecessor path, identify the switch
/// input across merges, and execute the real dispatch arithmetic per index.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/symbolic/SymDispatch.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// collectPathOps — gather ops along the predecessor path to INDIR_BR
//===----------------------------------------------------------------------===//

/// Walk backward through CFG predecessor blocks and collect all ops
/// along the dominant path leading to the INDIR_BR block.  The
/// resulting op sequence can be fed to the NdOp emulator for
/// cross-block switch-target computation via emulation-based jump
/// table resolution.
std::vector<LowOp> CFGBuilder::collectPathOps(va_t BranchBlockStart,
                                              va_t BranchInsnAddr) const {
  std::vector<LowOp> Result;

  // Collect ops within the INDIR_BR block first (up to and including
  // the INDIR_BR instruction).
  auto NextBlock = BlockStarts.upper_bound(BranchBlockStart);
  va_t BlkEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

  for (auto It = Insns.lower_bound(BranchBlockStart); It != Insns.end(); ++It) {
    if (It->first > BranchInsnAddr || It->first >= BlkEnd)
      break;
    for (auto &Op : It->second.Ops)
      Result.push_back(Op);
  }

  // Walk backward through single-predecessor blocks, collecting their ops.
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  va_t CurBlockStart = BranchBlockStart;
  int Depth = 0;

  while (Depth < limits::kMaxPathEmulationDepth &&
         static_cast<int>(Result.size()) < limits::kMaxPathEmulationOps) {

    std::vector<va_t> Preds;
    std::set<va_t> PredVisited = Visited;
    collectPredBlocks(CurBlockStart, PredVisited, Preds);

    // Only follow single-predecessor paths to avoid ambiguity.
    if (Preds.size() != 1)
      break;

    va_t PredStart = Preds[0];
    if (!Visited.insert(PredStart).second)
      break;

    auto PredNext = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (PredNext != BlockStarts.end()) ? *PredNext : InvalidVA;

    std::vector<LowOp> PredOps;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      for (auto &Op : It->second.Ops) {
        if (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::BRANCH)
          continue;
        PredOps.push_back(Op);
      }
    }

    // Prepend predecessor ops before current ops.
    PredOps.insert(PredOps.end(), Result.begin(), Result.end());
    Result = std::move(PredOps);

    CurBlockStart = PredStart;
    ++Depth;
  }

  LLVM_DEBUG(llvm::dbgs() << "  path-ops: collected " << Result.size()
                          << " ops across " << (Depth + 1) << " blocks\n");
  return Result;
}

//===----------------------------------------------------------------------===//
// findCommonSwitchVar — common-pred register identification
//===----------------------------------------------------------------------===//

/// Examine multiple predecessor paths to the INDIR_BR block and
/// identify register definitions common to all paths.  When the switch
/// variable flows through a merge point (multiple predecessors each
/// define it), this pinpoints the true switch register even when
/// quasi-copy tracing from a single path fails.
uint64_t CFGBuilder::findCommonSwitchVar(va_t BranchBlockStart,
                                         uint64_t BranchIndReg) const {
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2)
    return BranchIndReg;

  // For each predecessor, collect the set of registers it defines.
  std::vector<std::set<uint64_t>> PredDefs;
  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    std::set<uint64_t> Defs;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      for (auto &Op : It->second.Ops) {
        if (Op.Output.isReg())
          Defs.insert(Op.Output.Offset);
      }
    }
    PredDefs.push_back(std::move(Defs));
  }

  // Intersect: find registers defined in ALL predecessors.
  std::set<uint64_t> Common = PredDefs[0];
  for (size_t I = 1; I < PredDefs.size(); ++I) {
    std::set<uint64_t> Inter;
    for (uint64_t R : Common)
      if (PredDefs[I].count(R))
        Inter.insert(R);
    Common = std::move(Inter);
  }

  // If BranchIndReg (or a quasi-copy source of it from the INDIR_BR
  // block) is among the common defs, that confirms it as the switch var.
  if (Common.count(BranchIndReg))
    return BranchIndReg;

  // Otherwise, try to match through the branch block's ops: find which
  // common register feeds into BranchIndReg.
  auto NextBlock = BlockStarts.upper_bound(BranchBlockStart);
  va_t BlkEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

  for (auto It = Insns.lower_bound(BranchBlockStart); It != Insns.end(); ++It) {
    if (It->first >= BlkEnd)
      break;
    for (auto &Op : It->second.Ops) {
      if (Op.Output.isReg() && Op.Output.Offset == BranchIndReg) {
        for (int K = 0; K < Op.NumInputs; ++K) {
          if (Op.Inputs[K].isReg() && Common.count(Op.Inputs[K].Offset)) {
            LLVM_DEBUG(llvm::dbgs()
                       << "  common-pred: resolved switch var to reg 0x"
                       << llvm::utohexstr(Op.Inputs[K].Offset)
                       << " (common across " << Preds.size()
                       << " predecessors)\n");
            return Op.Inputs[K].Offset;
          }
        }
      }
    }
  }

  return BranchIndReg;
}

//===----------------------------------------------------------------------===//
// tryEmulatedResolution — NdOp emulation fallback
//===----------------------------------------------------------------------===//

std::vector<va_t> CFGBuilder::tryEmulatedResolution(const BinaryImage &Img,
                                                    const InsnRecord &Rec,
                                                    const JumpTableInfo &Info,
                                                    bool SelfBounding) {
  uint64_t IndexReg = InvalidVA;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
        Op.Inputs[0].isReg()) {
      IndexReg = Op.Inputs[0].Offset;
      break;
    }
  }
  if (IndexReg == InvalidVA)
    return {};

  uint64_t SwitchReg =
      quasiCopySource(Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, IndexReg);

  // When the INDIR_BR block has multiple predecessors, identify the true
  // switch register through path intersection (common-pred defs).
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt != BlockStarts.begin()) {
    --BlockIt;
    uint64_t CommonReg = findCommonSwitchVar(*BlockIt, SwitchReg);
    if (CommonReg != SwitchReg) {
      SwitchReg = CommonReg;
      LLVM_DEBUG(llvm::dbgs() << "  emu: using common-pred switch reg 0x"
                              << llvm::utohexstr(SwitchReg) << "\n");
    }
  }

  // Ask once whether this branch can go anywhere at all as the switch register
  // varies, before running it thousands of times to find out.  An indirect
  // tail call through a function pointer reaches the same destination for
  // every index; the distinct-target check further down already catches that,
  // but only after the whole limit has been emulated, and only by inferring
  // from the answers what could have been read off the code.
  //
  // The engine says no only when it carried out everything in the way and the
  // index appeared nowhere — neither in the target nor in the address of
  // anything loaded to compute it.
  {
    symbolic::SymContext SymCtx;
    if (!symbolic::dispatchVariesWithIndex(SymCtx, Rec.Ops, SwitchReg)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  emu: target does not vary with switch reg 0x"
                 << llvm::utohexstr(SwitchReg) << ", not a dispatch\n");
      return {};
    }
  }

  uint32_t Limit = Info.MaxEntries;
  if (Limit == 0 || Limit > limits::kMaxJumpTableEntries)
    Limit = limits::kMaxJumpTableEntries;

  std::vector<va_t> Targets;
  Targets.reserve(std::min(Limit, 64u));

  NdOpEmulator Emu(Img);
  Emu.setCallPreservedRegisters(callPreservedRegs(Img));

  // Emulate one target per candidate index into \p Out.  A self-bounding
  // (unbounded) scan must stop on a long run of identical targets: an
  // index-independent branch (a function-pointer tail call, not a switch)
  // emulates to the same destination for every index and would otherwise fill
  // the whole limit with one bogus target.  A bounded scan keeps every entry,
  // since a real switch legitimately repeats targets (several cases sharing a
  // body) within its known range.
  auto emulateSeries = [&](const std::vector<LowOp> &Ops,
                           std::vector<va_t> &Out) {
    va_t PrevTgt = InvalidVA;
    int DupRun = 0;
    for (uint32_t Idx = 0; Idx < Limit; ++Idx) {
      auto Tgt = Emu.computeTarget(Ops, SwitchReg, Idx);
      if (!Tgt || !isValidTarget(Img, *Tgt, CurrentFuncEntry))
        break;
      if (SelfBounding) {
        if (*Tgt == PrevTgt) {
          if (++DupRun > limits::kMaxDuplicateRun)
            break;
        } else {
          DupRun = 0;
          PrevTgt = *Tgt;
        }
      }
      Out.push_back(*Tgt);
    }
  };

  // Phase 1: Try single-instruction emulation (fast path).
  emulateSeries(Rec.Ops, Targets);

  sanityCheckTargets(Img, Targets);

  // Phase 2: Cross-block emulation when single-instruction emulation
  // fails.  Collect ops from predecessor blocks along the dominant
  // path and re-emulate with the full op sequence.
  if (Targets.size() < limits::kMinJumpTableEntries) {
    auto BIt = BlockStarts.upper_bound(Rec.Addr);
    if (BIt != BlockStarts.begin()) {
      --BIt;
      auto PathOps = collectPathOps(*BIt, Rec.Addr);
      if (PathOps.size() > Rec.Ops.size()) {
        std::vector<va_t> CrossBlockTargets;
        CrossBlockTargets.reserve(std::min(Limit, 64u));

        emulateSeries(PathOps, CrossBlockTargets);

        sanityCheckTargets(Img, CrossBlockTargets);
        if (CrossBlockTargets.size() > Targets.size()) {
          Targets = std::move(CrossBlockTargets);
          LLVM_DEBUG(llvm::dbgs()
                     << "  cross-block emu: recovered " << Targets.size()
                     << " entries via multi-block path\n");
        }
      }
    }
  }

  // A self-bounding scan must recover a genuine multi-way dispatch: require at
  // least kMinJumpTableEntries *distinct* targets so an indirect tail call
  // whose (single, constant) destination happens to validate is never
  // mismodeled as a switch.
  if (SelfBounding) {
    std::set<va_t> Distinct(Targets.begin(), Targets.end());
    if (Distinct.size() < limits::kMinJumpTableEntries)
      return {};
  }

  return Targets;
}

//===----------------------------------------------------------------------===//
// emulateGroundedTargets — execute the real dispatch per index
//===----------------------------------------------------------------------===//

std::vector<va_t> CFGBuilder::emulateGroundedTargets(const BinaryImage &Img,
                                                     const InsnRecord &Rec,
                                                     const JumpTableInfo &Info,
                                                     uint32_t Count,
                                                     bool &Grounded) {
  Grounded = false;
  std::vector<va_t> Out;
  if (Count == 0 || Count > limits::kMaxJumpTableEntries || Info.EntrySize == 0)
    return Out;

  // Collect the dispatch path: the INDIR_BR block plus its single-predecessor
  // chain (so a table base materialised in a dominating block is in scope).
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> Ops = collectPathOps(BlkStart, Rec.Addr);
  if (Ops.empty())
    return Out;

  // The INDIR_BR whose input register/temp holds the computed target.
  NdVar TargetVar;
  bool HaveTarget = false;
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
    if (Ops[I].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1 &&
        (Ops[I].Inputs[0].isReg() || Ops[I].Inputs[0].isTemp())) {
      TargetVar = Ops[I].Inputs[0];
      HaveTarget = true;
      break;
    }
  if (!HaveTarget)
    return Out;

  // Locate the table LOAD (last scaled `base + index*EntrySize` access) and the
  // variable feeding its scale — the *post-normalization* table index, i.e. the
  // value that, set to i, makes the load read table slot i.  Injecting there
  // (rather than at the resolver's traced-to-source index register) skips the
  // switch-variable normalization while preserving the base materialisation and
  // the post-load transform, so the emulated target is exactly what the
  // processor computes for slot i.
  NdVar InjVar;
  int LoadPos = -1;
  auto peelCopy = [&](int D) {
    for (int G = 0;
         D >= 0 && Ops[D].Opcode == NdOp::COPY && Ops[D].NumInputs >= 1 &&
         (Ops[D].Inputs[0].isReg() || Ops[D].Inputs[0].isTemp()) &&
         G < limits::kMaxQuasiCopyDepth;
         ++G)
      D = reachingDefIdx(Ops, D - 1, Ops[D].Inputs[0]);
    return D;
  };
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0 && LoadPos < 0; --I) {
    if (Ops[I].Opcode != NdOp::LOAD || Ops[I].NumInputs < 1 ||
        Ops[I].Output.Size != Info.EntrySize)
      continue;
    const NdVar &AddrV =
        (Ops[I].NumInputs >= 2) ? Ops[I].Inputs[1] : Ops[I].Inputs[0];
    int AddIdx = peelCopy(reachingDefIdx(Ops, I - 1, AddrV));
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;
    for (int W = 0; W < 2 && LoadPos < 0; ++W) {
      int SD = peelCopy(reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[W]));
      if (SD < 0)
        continue;
      const LowOp &S = Ops[SD];
      uint64_t Scale = 0;
      if (S.Opcode == NdOp::INT_MULT && S.NumInputs >= 2 &&
          S.Inputs[1].isConst())
        Scale = S.Inputs[1].Offset;
      else if (S.Opcode == NdOp::INT_LEFT && S.NumInputs >= 2 &&
               S.Inputs[1].isConst() && S.Inputs[1].Offset < 6)
        Scale = 1ull << S.Inputs[1].Offset;
      else
        continue;
      if (Scale != Info.EntrySize)
        continue;
      if (S.Inputs[0].isReg() || S.Inputs[0].isTemp()) {
        InjVar = S.Inputs[0];
        LoadPos = I;
      }
    }
  }
  if (LoadPos < 0)
    return Out;

  // Split point: the last write to the injected index variable before the table
  // LOAD.  The prefix (through that write) materialises the base and other
  // loop-invariant registers; the tail (after it) is re-run per index with the
  // injected value overriding the normalization's output.
  int LastDef = -1;
  for (int I = LoadPos - 1; I >= 0; --I)
    if (Ops[I].Output.Space == InjVar.Space &&
        Ops[I].Output.Offset == InjVar.Offset) {
      LastDef = I;
      break;
    }
  std::vector<LowOp> Prefix(Ops.begin(), Ops.begin() + (LastDef + 1));
  std::vector<LowOp> Tail(Ops.begin() + (LastDef + 1), Ops.end());

  NdOpEmulator Emu(Img);
  Emu.setCallPreservedRegisters(callPreservedRegs(Img));
  Emu.setLoadCollect(true);
  std::vector<va_t> Targets;
  Targets.reserve(Count);
  for (uint32_t I = 0; I < Count; ++I) {
    Emu.reset();
    Emu.run(Prefix);
    Emu.setRegister(InjVar.Offset, I);
    Emu.run(Tail);

    // Grounding: the emulation must have read exactly table slot i.  A wrong
    // injection point or an unmaterialised base reads a different address, so
    // this ties the emulated target to the recovered table and makes adoption
    // safe regardless of how the injection site was chosen.
    const uint64_t EntryStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (I != 0 &&
        EntryStride > (std::numeric_limits<uint64_t>::max() - Info.BaseAddr) /
                          uint64_t(I))
      return {};
    uint64_t Slot = Info.BaseAddr + uint64_t(I) * EntryStride;
    bool Hit = false;
    for (auto &L : Emu.getLoadRecords())
      if (L.Addr == Slot) {
        Hit = true;
        break;
      }
    if (!Hit)
      return {};
    auto T = Emu.getRegister(TargetVar.Offset);
    if (!T || !isValidTarget(Img, *T, CurrentFuncEntry))
      return {};
    Targets.push_back(*T);
  }

  Grounded = true;
  return Targets;
}

} // namespace neverd

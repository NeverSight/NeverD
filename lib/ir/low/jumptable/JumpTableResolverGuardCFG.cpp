//===- JumpTableResolverGuardCFG.cpp - Predecessor-block guards -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guard strategies that search the dispatch block's CFG predecessors rather
/// than the linear prefix: the breadth-first predecessor walk, the dual-path
/// default-value split, and the duplicated ("unrolled") guard whose tightest
/// common bound is shared by every predecessor COND_BR.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// inferBoundsFromCFGGuards — walk CFG predecessor chain for guards
//===----------------------------------------------------------------------===//

/// Collect block-start addresses of all predecessor blocks that branch to
/// the given target address (either via direct branch or conditional
/// fall-through).
void CFGBuilder::collectPredBlocks(va_t TargetBlockStart,
                                   const std::set<va_t> &Visited,
                                   std::vector<va_t> &Out) const {
  for (auto &[Addr, IRec] : Insns) {
    if (!IRec.IsBranch || IRec.IsCall)
      continue;
    bool Targets = (IRec.BranchTarget == TargetBlockStart);
    if (IRec.IsCond && !Targets) {
      va_t Fall = Addr + IRec.Size;
      if (Fall == TargetBlockStart)
        Targets = true;
    }
    if (!Targets)
      continue;
    if (Visited.count(Addr))
      continue;
    auto PB = BlockStarts.upper_bound(Addr);
    if (PB != BlockStarts.begin()) {
      --PB;
      Out.push_back(*PB);
    }
  }
}

bool CFGBuilder::inferBoundsFromCFGGuards(const InsnRecord &Rec,
                                          JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);

  uint32_t Best = 0;

  std::vector<va_t> Worklist;
  collectPredBlocks(BranchBlockStart, Visited, Worklist);

  int Depth = 0;
  while (!Worklist.empty() && Depth < limits::kMaxGuardPredDepth) {
    std::vector<va_t> NextWorklist;
    for (va_t PredStart : Worklist) {
      if (!Visited.insert(PredStart).second)
        continue;

      auto NextBlock = BlockStarts.upper_bound(PredStart);
      va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

      for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
        if (It->first >= PredEnd)
          break;
        Best = findBestBound(It->second.Ops, Best, Info.IndexReg);
        uint32_t Compound = traceCompoundGuard(It->second.Ops);
        if (Compound > 1 && Compound <= limits::kMaxJumpTableEntries) {
          if (Best == 0 || Compound < Best)
            Best = Compound;
        }
      }

      if (Best == 0)
        collectPredBlocks(PredStart, Visited, NextWorklist);
    }
    Worklist = std::move(NextWorklist);
    ++Depth;
  }

  if (Best > 0 && (Info.MaxEntries == 0 || Best < Info.MaxEntries)) {
    Info.MaxEntries = Best;
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// tryDualPathRecovery — default-value path detection
//===----------------------------------------------------------------------===//

/// When the standard guard analysis fails to produce a bound, check for
/// a dual-path pattern: the block containing the INDIR_BR has two
/// predecessor paths, one carrying a default constant and one carrying
/// the real switch computation.  A COND_BR at the split point acts as
/// the guard for switches with an explicit default path.
bool CFGBuilder::tryDualPathRecovery(const InsnRecord &Rec,
                                     JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  // Collect all predecessor blocks that branch into our switch block.
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2 || Preds.size() > limits::kMaxDualPathPreds)
    return false;

  // Look for the pattern: one predecessor ends with a COND_BR that
  // gates a constant-producing path vs. a computation path.
  // The COND_BR predecessor that has a bound comparison is our guard.
  uint32_t BestBound = 0;
  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      auto &IRec = It->second;
      if (!IRec.IsBranch || !IRec.IsCond)
        continue;

      // This pred has a COND_BR — scan its ops for a guard bound.
      uint32_t Bound = findBestBound(IRec.Ops, 0, Info.IndexReg);
      if (Bound > 0 && (BestBound == 0 || Bound < BestBound))
        BestBound = Bound;

      // Also scan the ops preceding the COND_BR in this block.
      for (auto InnerIt = Insns.lower_bound(PredStart); InnerIt != It;
           ++InnerIt) {
        Bound = findBestBound(InnerIt->second.Ops, BestBound, Info.IndexReg);
        if (Bound > 0 && (BestBound == 0 || Bound < BestBound))
          BestBound = Bound;
      }
    }
  }

  if (BestBound == 0)
    return false;

  Info.MaxEntries = BestBound;
  LLVM_DEBUG(llvm::dbgs() << "  dual-path: found guard bound " << BestBound
                          << " from " << Preds.size() << " predecessors\n");
  return true;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromUnrolledGuard — detect duplicated guard across preds
//===----------------------------------------------------------------------===//

/// When multiple predecessor blocks each terminate with a COND_BR, and
/// each carries a guard comparison on the switch variable, the guard
/// has been "unrolled" (duplicated).  This detects that pattern and
/// extracts the tightest common bound.
bool CFGBuilder::inferBoundsFromUnrolledGuard(const InsnRecord &Rec,
                                              JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2 ||
      static_cast<int>(Preds.size()) > limits::kMaxUnrolledGuardPreds)
    return false;

  // Every predecessor must end with a COND_BR for this to be an
  // unrolled guard pattern.
  uint32_t CommonBound = 0;
  int CBranchCount = 0;

  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    bool FoundCBranch = false;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      auto &IRec = It->second;
      if (!IRec.IsBranch || !IRec.IsCond)
        continue;
      FoundCBranch = true;
      ++CBranchCount;

      uint32_t PredBound = findBestBound(IRec.Ops, 0, Info.IndexReg);
      if (PredBound == 0) {
        for (auto InnerIt = Insns.lower_bound(PredStart); InnerIt != It;
             ++InnerIt)
          PredBound =
              findBestBound(InnerIt->second.Ops, PredBound, Info.IndexReg);
      }

      if (PredBound > 0) {
        if (CommonBound == 0 || PredBound < CommonBound)
          CommonBound = PredBound;
      }
    }
    if (!FoundCBranch)
      return false;
  }

  if (CBranchCount < 2 || CommonBound == 0)
    return false;

  if (Info.MaxEntries == 0 || CommonBound < Info.MaxEntries) {
    Info.MaxEntries = CommonBound;
    LLVM_DEBUG(llvm::dbgs()
               << "  unrolled-guard: found common bound " << CommonBound
               << " across " << Preds.size() << " predecessor COND_BRs\n");
    return true;
  }
  return false;
}

} // namespace neverd

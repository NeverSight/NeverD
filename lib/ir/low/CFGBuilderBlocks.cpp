//===- CFGBuilderBlocks.cpp - Basic-block formation ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns the decoded instruction map into basic blocks: cutting blocks at
/// conditional fall-throughs, materializing the blocks between consecutive
/// boundaries, wiring ordinary successor and predecessor edges, and
/// renumbering so the function entry is block 0.  Exceptional edges are added
/// in CFGBuilderException.cpp; see CFGBuilder.cpp for the exploration that
/// produces the instructions.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <vector>

namespace neverd {

std::set<va_t> detail::refineRelocationRootSuppressionOwners(
    std::set<va_t> Owners, const SuppressionOwnerReachabilityQuery &Query,
    size_t MaxRounds) {
  size_t Rounds = 0;
  for (;;) {
    const std::set<va_t> Reachable = Query(Owners);
    std::set<va_t> Verified;
    std::set_intersection(Owners.begin(), Owners.end(), Reachable.begin(),
                          Reachable.end(),
                          std::inserter(Verified, Verified.end()));
    if (Verified == Owners)
      return Owners;
    Owners = std::move(Verified);
    if (++Rounds < MaxRounds)
      continue;
    Owners.clear();
    (void)Query(Owners);
    return Owners;
  }
}

void detail::mergeRelativeRelocationRootSources(
    const std::vector<std::pair<va_t, va_t>> &SortedSources,
    const std::set<va_t> &Roots, std::map<va_t, std::set<va_t>> &RootSources) {
  for (va_t Root : Roots) {
    const auto Begin = std::lower_bound(
        SortedSources.begin(), SortedSources.end(), Root,
        [](const std::pair<va_t, va_t> &Source, va_t Candidate) {
          return Source.first < Candidate;
        });
    const auto End = std::upper_bound(
        Begin, SortedSources.end(), Root,
        [](va_t Candidate, const std::pair<va_t, va_t> &Source) {
          return Candidate < Source.first;
        });
    for (auto It = Begin; It != End; ++It)
      RootSources[Root].insert(It->second);
  }
}

void CFGBuilder::splitBlocks() {
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.IsBranch && Rec.IsCond)
      BlockStarts.insert(Addr + Rec.Size);
  }
}

void CFGBuilder::normalizeEntryBlock(LowFunc &Func) {
  if (Func.Blocks.empty())
    return;

  auto EntryIt = std::find_if(
      Func.Blocks.begin(), Func.Blocks.end(),
      [&](const LowBlock &Blk) { return Blk.StartAddr == Func.Entry; });
  if (EntryIt == Func.Blocks.end())
    llvm::report_fatal_error("CFGBuilder: function entry block is missing");

  const size_t EntryIndex =
      static_cast<size_t>(std::distance(Func.Blocks.begin(), EntryIt));
  const size_t NumBlocks = Func.Blocks.size();
  std::vector<size_t> NewToOld;
  NewToOld.reserve(NumBlocks);
  NewToOld.push_back(EntryIndex);
  for (size_t I = 0; I < NumBlocks; ++I)
    if (I != EntryIndex)
      NewToOld.push_back(I);

  std::vector<int> OldToNew(NumBlocks, -1);
  for (size_t NewId = 0; NewId < NumBlocks; ++NewId) {
    const int OldId = Func.Blocks[NewToOld[NewId]].Id;
    if (OldId < 0 || static_cast<size_t>(OldId) >= NumBlocks ||
        OldToNew[OldId] != -1)
      llvm::report_fatal_error("CFGBuilder: invalid block identity");
    OldToNew[OldId] = static_cast<int>(NewId);
  }

  std::vector<LowBlock> Normalized;
  Normalized.reserve(NumBlocks);
  for (size_t NewId = 0; NewId < NumBlocks; ++NewId) {
    LowBlock Blk = std::move(Func.Blocks[NewToOld[NewId]]);
    auto RemapEdges = [&](std::vector<int> &Edges) {
      for (int &Id : Edges) {
        if (Id < 0 || static_cast<size_t>(Id) >= NumBlocks || OldToNew[Id] < 0)
          llvm::report_fatal_error("CFGBuilder: invalid block edge");
        Id = OldToNew[Id];
      }
    };
    RemapEdges(Blk.Succs);
    RemapEdges(Blk.Preds);
    Blk.Id = static_cast<int>(NewId);
    Normalized.push_back(std::move(Blk));
  }
  Func.Blocks = std::move(Normalized);
}

void CFGBuilder::prepareRelativeRelocationRootSourceCache() {
  ++RelativeRelocationRootSourceCacheLookupCountForTesting;
  if (!CurrentImg)
    return;
  const size_t SlotCount = CurrentImg->RelCodeRelocSlots.size();
  if (RelativeRelocationRootSourceCachePrepared &&
      RelativeRelocationRootSourceCacheImage == CurrentImg &&
      RelativeRelocationRootSourceCacheArch == CurrentImg->Arch &&
      RelativeRelocationRootSourceCacheMode == CurrentImg->Mode &&
      RelativeRelocationRootSourceCacheSlotCount == SlotCount)
    return;

  RelativeRelocationRootSourceCachePrepared = true;
  RelativeRelocationRootSourceCacheImage = CurrentImg;
  RelativeRelocationRootSourceCacheArch = CurrentImg->Arch;
  RelativeRelocationRootSourceCacheMode = CurrentImg->Mode;
  RelativeRelocationRootSourceCacheSlotCount = SlotCount;
  RelativeRelocationRootSourceCache.clear();
  ++RelativeRelocationRootSourceCacheBuildCountForTesting;

  // The current width-unambiguous producers are ELF x86-64 PC32 and ELF
  // AArch64 PREL32.  Mach-O i386 stores 1/2/4-byte vanilla PC-relative fields
  // in the same width-free set, while future ARM PREL31/REL32 producers need
  // their own decoding contract.  Unsupported formats cache a prepared empty
  // index and conservatively retain every boundary.
  if (!CurrentImg->isELF() ||
      (CurrentImg->Arch != Arch::X64 && CurrentImg->Arch != Arch::AArch64))
    return;

  std::vector<std::pair<va_t, va_t>> Sources;
  Sources.reserve(SlotCount);
  for (va_t Slot : CurrentImg->RelCodeRelocSlots) {
    const uint8_t *Bytes = CurrentImg->readVA(Slot, sizeof(uint32_t));
    if (!Bytes) {
      // One unreadable member makes the global source inventory incomplete.
      // Publishing the readable subset could hide a root whose unexamined
      // slot is an independent source, so disable all relative suppression.
      return;
    }
    const uint32_t Encoded = uint32_t(Bytes[0]) | (uint32_t(Bytes[1]) << 8) |
                             (uint32_t(Bytes[2]) << 16) |
                             (uint32_t(Bytes[3]) << 24);
    const int64_t Displacement = static_cast<int32_t>(Encoded);
    va_t Root = InvalidVA;
    if (Displacement >= 0) {
      const uint64_t Delta = static_cast<uint64_t>(Displacement);
      if (Delta > InvalidVA - Slot)
        continue;
      Root = Slot + Delta;
    } else {
      const uint64_t Delta = static_cast<uint64_t>(-Displacement);
      if (Delta > Slot)
        continue;
      Root = Slot - Delta;
    }
    Root = normalizeCodeAddress(Root, CurrentImg->Arch, CurrentImg->Mode);
    Sources.emplace_back(Root, Slot);
  }
  std::sort(Sources.begin(), Sources.end());
  RelativeRelocationRootSourceCache = std::move(Sources);
}

void CFGBuilder::rebuildBlocks(LowFunc &Func) {
  Func.Blocks.clear();
  Func.JumpTables.clear();

  // A boundary can name an address recursive descent never decoded: the end of
  // a guarded range an exception table declared, or the fall-through of a
  // conditional branch at the edge of the mapped image.  A block there would
  // carry no instruction and so would stand for nothing, so it is dropped.  The
  // entry survives unconditionally — the function is defined by it.
  std::vector<va_t> Starts;
  Starts.reserve(BlockStarts.size());
  for (va_t Start : BlockStarts)
    if (Start == Func.Entry || Insns.count(Start))
      Starts.push_back(Start);
  std::sort(Starts.begin(), Starts.end());

  std::map<va_t, int> AddrToBlock;
  for (size_t I = 0; I < Starts.size(); ++I) {
    LowBlock Blk;
    Blk.Id = static_cast<int>(I);
    Blk.StartAddr = Starts[I];

    va_t End = (I + 1 < Starts.size()) ? Starts[I + 1] : InvalidVA;

    for (auto It = Insns.lower_bound(Starts[I]); It != Insns.end(); ++It) {
      if (It->first >= End)
        break;
      const uint64_t FirstOp = static_cast<uint64_t>(Blk.Ops.size());
      for (auto &Op : It->second.Ops)
        Blk.Ops.push_back(Op);
      Blk.InstructionBoundaries.push_back(
          makeInstructionBoundary(It->second, FirstOp));
      Blk.EndAddr = It->first + It->second.Size;
    }

    AddrToBlock[Starts[I]] = Blk.Id;
    Func.Blocks.push_back(std::move(Blk));
  }

  linkSuccessors(Func, AddrToBlock);

  // Jump-table targets are discovered speculatively: decoding a case can add
  // a backedge or sibling definition that invalidates the proof which admitted
  // a later slot.  The instruction cache intentionally retains decoded bytes
  // so a subsequently tightened table can be re-evaluated without decoding
  // churn, but the public CFG must not retain blocks reachable only through a
  // stale target.  Keep ordinary reachability from the function's persistent
  // roots (entry, address-taken labels, exception entries) and remap the graph
  // before exceptional edges and jump-table metadata are published.
  std::vector<bool> Reachable(Func.Blocks.size(), false);
  std::queue<int> Worklist;
  std::map<va_t, int> InsnToBlock;
  for (const LowBlock &Block : Func.Blocks)
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
      InsnToBlock[Boundary.Address] = Block.Id;
  auto Flood = [&] {
    while (!Worklist.empty()) {
      const int Block = Worklist.front();
      Worklist.pop();
      for (int Succ : Func.Blocks[Block].Succs)
        if (Succ >= 0 && Succ < static_cast<int>(Func.Blocks.size()) &&
            !Reachable[Succ]) {
          Reachable[Succ] = true;
          Worklist.push(Succ);
        }
    }
  };
  std::set<va_t> CurrentReachableInsns;
  auto RefreshReachableInsns = [&] {
    CurrentReachableInsns.clear();
    for (const auto &[InsnAddr, BlockId] : InsnToBlock)
      if (BlockId >= 0 && BlockId < static_cast<int>(Reachable.size()) &&
          Reachable[BlockId])
        CurrentReachableInsns.insert(InsnAddr);
  };
  // Resolve conditional code roots and inline-table ownership together.  A
  // purely additive closure is order-dependent: adding a code root can make a
  // table owner reachable, which retroactively proves that root was table
  // bytes.  Keep the set of discovered active owners monotone, but recompute
  // reachability from durable roots after every owner growth.  A
  // self-bootstrapped cycle (table bytes make their own owner reachable) is
  // therefore rejected conservatively instead of oscillating or depending on
  // map key order.
  std::set<va_t> ActiveTableOwners;
  std::set<va_t> CurrentAnalysisRoots;
  for (;;) {
    std::fill(Reachable.begin(), Reachable.end(), false);
    Worklist = std::queue<int>();
    CurrentAnalysisRoots.clear();
    auto RelocationRootSuppressed = [&](va_t Root) {
      if (DurableCFGRoots.count(Root))
        return false;
      auto Sources = RelocationCFGRootSources.find(Root);
      if (Sources == RelocationCFGRootSources.end() || Sources->second.empty())
        return false;
      return std::all_of(
          Sources->second.begin(), Sources->second.end(), [&](va_t Slot) {
            return std::any_of(
                ActiveTableOwners.begin(), ActiveTableOwners.end(),
                [&](va_t BranchAddr) {
                  auto Info = ResolvedTableInfo.find(BranchAddr);
                  return Info != ResolvedTableInfo.end() &&
                         std::binary_search(
                             Info->second.SuppressibleRelocationSlots.begin(),
                             Info->second.SuppressibleRelocationSlots.end(),
                             Slot);
                });
          });
    };
    for (va_t Root : PersistentCFGRoots) {
      if (RelocationRootSuppressed(Root))
        continue;
      auto It = AddrToBlock.find(Root);
      if (It == AddrToBlock.end() || Reachable[It->second])
        continue;
      Reachable[It->second] = true;
      Worklist.push(It->second);
      CurrentAnalysisRoots.insert(Root);
    }
    Flood();

    for (bool Added = true; Added;) {
      Added = false;
      for (const auto &[Target, Sources] : DiscoveredCodeRefSources) {
        if (resolvedJumpTableOwnsStorageAddress(Target, &ActiveTableOwners)
                .value_or(false))
          continue;
        auto TargetBlock = AddrToBlock.find(Target);
        if (TargetBlock == AddrToBlock.end() || Reachable[TargetBlock->second])
          continue;
        const bool HasReachableSource =
            std::any_of(Sources.begin(), Sources.end(), [&](va_t Source) {
              auto SourceBlock = InsnToBlock.find(Source);
              return SourceBlock != InsnToBlock.end() &&
                     Reachable[SourceBlock->second];
            });
        if (!HasReachableSource)
          continue;
        Reachable[TargetBlock->second] = true;
        Worklist.push(TargetBlock->second);
        CurrentAnalysisRoots.insert(Target);
        Added = true;
      }
      Flood();
    }
    RefreshReachableInsns();

    bool OwnerAdded = false;
    for (const auto &[BranchAddr, Info] : ResolvedTableInfo) {
      (void)Info;
      auto Rec = Insns.find(BranchAddr);
      if (Rec == Insns.end() || Rec->second.JumpTableTargets.empty() ||
          !CurrentReachableInsns.count(BranchAddr))
        continue;
      OwnerAdded |= ActiveTableOwners.insert(BranchAddr).second;
    }
    if (!OwnerAdded)
      break;
  }

  if (std::find(Reachable.begin(), Reachable.end(), false) != Reachable.end()) {
    std::vector<int> OldToNew(Func.Blocks.size(), -1);
    std::vector<LowBlock> Pruned;
    Pruned.reserve(static_cast<size_t>(
        std::count(Reachable.begin(), Reachable.end(), true)));
    for (size_t Old = 0; Old < Func.Blocks.size(); ++Old) {
      if (!Reachable[Old])
        continue;
      OldToNew[Old] = static_cast<int>(Pruned.size());
      Pruned.push_back(std::move(Func.Blocks[Old]));
    }
    for (LowBlock &Block : Pruned) {
      std::vector<int> Succs;
      Succs.reserve(Block.Succs.size());
      for (int Succ : Block.Succs)
        if (Succ >= 0 && Succ < static_cast<int>(OldToNew.size()) &&
            OldToNew[Succ] >= 0)
          Succs.push_back(OldToNew[Succ]);
      Block.Succs = std::move(Succs);
      Block.Preds.clear();
      Block.Id = OldToNew[Block.Id];
    }
    for (LowBlock &Block : Pruned)
      for (int Succ : Block.Succs)
        Pruned[Succ].Preds.push_back(Block.Id);
    Func.Blocks = std::move(Pruned);
  }

  // Relative code relocations carry S+A-P in each physical slot.  During
  // speculative table recovery the relocation coordinate P+(S+A-P) can be
  // decoded as a provisional root even though the published table correctly
  // dispatches through its base.  If that coordinate lands inside a real
  // instruction, forming blocks before removing the stale root makes the real
  // case fall through into the overlapping decode stream and defeats the
  // block-level reachability pruning above.
  //
  // Build a final, instruction-level view instead.  Relocation evidence is
  // source-counted: a root is hidden only when every absolute or relative
  // source slot is owned by a reachable final table.  Persistent state remains
  // untouched so a later proof loss can restore the boundary.
  std::map<va_t, std::set<va_t>> FinalRelocationRootSources =
      RelocationCFGRootSources;
  prepareRelativeRelocationRootSourceCache();
  detail::mergeRelativeRelocationRootSources(RelativeRelocationRootSourceCache,
                                             BlockStarts,
                                             FinalRelocationRootSources);

  struct EffectiveCFGView {
    std::set<va_t> ReachableInsns;
    std::set<va_t> AnalysisRoots;
    std::set<va_t> IndependentBoundaries;
    std::set<va_t> SuppressedRelocationRoots;
  };

  auto BuildEffectiveCFGView = [&](const std::set<va_t> &SuppressionOwners) {
    EffectiveCFGView Result;
    std::set<va_t> OwnedRelocationSlots;
    for (va_t BranchAddr : SuppressionOwners) {
      auto InfoIt = ResolvedTableInfo.find(BranchAddr);
      if (InfoIt == ResolvedTableInfo.end())
        continue;
      const JumpTableInfo &Info = InfoIt->second;
      OwnedRelocationSlots.insert(Info.SuppressibleRelocationSlots.begin(),
                                  Info.SuppressibleRelocationSlots.end());
      const auto Branch = Insns.find(BranchAddr);
      if (!CurrentImg || !Info.IsRelative || !Info.HasBaseAddr ||
          Info.EntrySize != sizeof(uint32_t) || Branch == Insns.end() ||
          Branch->second.JumpTableTargets.empty())
        continue;
      const uint64_t Stride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      if (Stride < Info.EntrySize)
        continue;
      auto AddExactSlot = [&](uint32_t I) {
        if (I != 0 && Stride > (InvalidVA - Info.BaseAddr) / I)
          return;
        const va_t Slot = Info.BaseAddr + uint64_t(I) * Stride;
        if (!CurrentImg->RelCodeRelocSlots.count(Slot) ||
            (ProtectedJumpTableRelocationSlots &&
             ProtectedJumpTableRelocationSlots->count(Slot)))
          return;
        OwnedRelocationSlots.insert(Slot);
      };
      if (!Info.EntryIndices.empty()) {
        if (Info.EntryIndices.size() != Branch->second.JumpTableTargets.size())
          continue;
        for (uint32_t I : Info.EntryIndices)
          AddExactSlot(I);
      } else if (!Info.RuntimeSlotIndices.empty()) {
        if (Info.RuntimeSlotIndices.size() !=
            Branch->second.JumpTableTargets.size())
          continue;
        for (uint32_t I : Info.RuntimeSlotIndices)
          AddExactSlot(I);
      } else {
        if (Branch->second.JumpTableTargets.size() >
            std::numeric_limits<uint32_t>::max())
          continue;
        for (size_t I = 0; I < Branch->second.JumpTableTargets.size(); ++I)
          AddExactSlot(static_cast<uint32_t>(I));
      }
    }

    for (const auto &[Root, Sources] : FinalRelocationRootSources) {
      if (DurableCFGRoots.count(Root) || Sources.empty())
        continue;
      if (std::all_of(Sources.begin(), Sources.end(), [&](va_t Slot) {
            return OwnedRelocationSlots.count(Slot) != 0;
          }))
        Result.SuppressedRelocationRoots.insert(Root);
    }

    std::queue<va_t> InsnWorklist;
    auto QueueInsn = [&](va_t Addr) {
      if (!Insns.count(Addr) || !Result.ReachableInsns.insert(Addr).second)
        return false;
      InsnWorklist.push(Addr);
      return true;
    };
    auto QueueIndependentRoot = [&](va_t Root) {
      if (Result.SuppressedRelocationRoots.count(Root))
        return;
      if (QueueInsn(Root))
        Result.AnalysisRoots.insert(Root);
      if (Insns.count(Root))
        Result.IndependentBoundaries.insert(Root);
    };
    for (va_t Root : PersistentCFGRoots)
      QueueIndependentRoot(Root);
    for (const auto &[Root, Sources] : FinalRelocationRootSources) {
      (void)Sources;
      QueueIndependentRoot(Root);
    }

    auto QueueControlTarget = [&](va_t Target) {
      if (Target == InvalidVA)
        return;
      Result.IndependentBoundaries.insert(Target);
      QueueInsn(Target);
    };
    auto FloodInstructions = [&] {
      while (!InsnWorklist.empty()) {
        const va_t Addr = InsnWorklist.front();
        InsnWorklist.pop();
        const auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        const InsnRecord &Rec = It->second;
        if (Rec.IsRet && Rec.IsCond && Rec.IsBranch) {
          QueueControlTarget(Rec.BranchTarget);
        } else if (Rec.IsRet) {
          continue;
        } else if (Rec.IsBranch && Rec.IsIndirect) {
          if (Rec.IsCond)
            QueueControlTarget(Rec.Addr + Rec.Size);
          for (va_t Target : Rec.JumpTableTargets)
            QueueControlTarget(Target);
        } else if (Rec.IsBranch && !Rec.IsIndirect && !Rec.IsCall) {
          if (Rec.IsCond)
            QueueControlTarget(Rec.Addr + Rec.Size);
          QueueControlTarget(Rec.BranchTarget);
        } else if (!Rec.IsBranch || Rec.IsCall) {
          if (!Rec.IsNoReturnCall || Rec.IsCond)
            QueueInsn(Rec.Addr + Rec.Size);
        }
      }
    };
    FloodInstructions();

    for (bool Added = true; Added;) {
      Added = false;
      for (const auto &[Target, Sources] : DiscoveredCodeRefSources) {
        if (resolvedJumpTableOwnsStorageAddress(Target, &SuppressionOwners)
                .value_or(false))
          continue;
        const bool HasReachableSource =
            std::any_of(Sources.begin(), Sources.end(), [&](va_t Source) {
              return Result.ReachableInsns.count(Source) != 0;
            });
        if (!HasReachableSource || !Insns.count(Target))
          continue;
        Result.AnalysisRoots.insert(Target);
        Result.IndependentBoundaries.insert(Target);
        Added |= QueueInsn(Target);
      }
      FloodInstructions();
    }
    return Result;
  };

  // Suppression authority is decreasing: once filtering proves that an owner
  // was reachable only through the overlap stream it tried to hide, revoke
  // that owner and restore its roots.  The shared helper bounds adversarial
  // owner chains and rebuilds with no suppression on exhaustion.
  EffectiveCFGView FinalView;
  const std::set<va_t> FinalSuppressionOwners =
      detail::refineRelocationRootSuppressionOwners(
          ActiveTableOwners, [&](const std::set<va_t> &Owners) {
            FinalView = BuildEffectiveCFGView(Owners);
            std::set<va_t> ReachableOwners;
            for (va_t BranchAddr : Owners) {
              const auto Rec = Insns.find(BranchAddr);
              if (Rec != Insns.end() && !Rec->second.JumpTableTargets.empty() &&
                  FinalView.ReachableInsns.count(BranchAddr))
                ReachableOwners.insert(BranchAddr);
            }
            return ReachableOwners;
          });
  (void)FinalSuppressionOwners;

  // Relative relocation roots are deliberately absent from OrdinaryCFGRoots
  // during selector proof: a protected/excluded physical slot must not
  // bootstrap its table domain.  Grant the ordinary address-taken role only
  // in this final immutable view, after suppression is settled and the exact
  // root is known to be decoded and independently published.
  std::set<va_t> FinalOrdinaryAnalysisRoots = OrdinaryCFGRoots;
  for (const auto &[Root, Sources] : FinalRelocationRootSources)
    if (!Sources.empty() && !FinalView.SuppressedRelocationRoots.count(Root) &&
        FinalView.AnalysisRoots.count(Root) && Insns.count(Root))
      FinalOrdinaryAnalysisRoots.insert(Root);

  std::vector<va_t> EffectiveStarts;
  EffectiveStarts.reserve(BlockStarts.size());
  for (va_t Start : BlockStarts) {
    if (Start != Func.Entry && !FinalView.ReachableInsns.count(Start))
      continue;
    if (Start != Func.Entry && !Insns.count(Start))
      continue;
    if (FinalView.SuppressedRelocationRoots.count(Start) &&
        !FinalView.IndependentBoundaries.count(Start))
      continue;
    EffectiveStarts.push_back(Start);
  }
  std::sort(EffectiveStarts.begin(), EffectiveStarts.end());

  Func.Blocks.clear();
  std::map<va_t, int> EffectiveAddrToBlock;
  for (size_t I = 0; I < EffectiveStarts.size(); ++I) {
    LowBlock Block;
    Block.Id = static_cast<int>(I);
    Block.StartAddr = EffectiveStarts[I];
    const va_t End =
        I + 1 < EffectiveStarts.size() ? EffectiveStarts[I + 1] : InvalidVA;
    for (auto It = Insns.lower_bound(EffectiveStarts[I]); It != Insns.end();
         ++It) {
      if (It->first >= End)
        break;
      if (!FinalView.ReachableInsns.count(It->first))
        continue;
      const uint64_t FirstOp = static_cast<uint64_t>(Block.Ops.size());
      Block.Ops.insert(Block.Ops.end(), It->second.Ops.begin(),
                       It->second.Ops.end());
      Block.InstructionBoundaries.push_back(
          makeInstructionBoundary(It->second, FirstOp));
      Block.EndAddr = It->first + It->second.Size;
    }
    EffectiveAddrToBlock[Block.StartAddr] = Block.Id;
    Func.Blocks.push_back(std::move(Block));
  }
  linkSuccessors(Func, EffectiveAddrToBlock);
  PublishedBlockStarts = std::move(EffectiveStarts);
  CurrentReachableInsns = std::move(FinalView.ReachableInsns);
  CurrentAnalysisRoots = std::move(FinalView.AnalysisRoots);

  normalizeEntryBlock(Func);
  PublishedReachableInsns = std::move(CurrentReachableInsns);
  Func.ModuleAnalysisRoots.clear();
  for (va_t Root : CurrentAnalysisRoots)
    if (PublishedReachableInsns.count(Root))
      Func.ModuleAnalysisRoots.insert(Root);

  // Publish the positive ordinary-entry role independently from the complete
  // root inventory.  A disconnected address-taken root inherits that role
  // only when an ordinary-reachable instruction actually takes its address;
  // an exception-only handler cannot bootstrap another handler's RETURN into
  // ordinary returned-value evidence.
  Func.OrdinaryModuleAnalysisRoots.clear();
  std::vector<bool> OrdinaryReachable(Func.Blocks.size(), false);
  std::queue<int> OrdinaryWorklist;
  auto QueueOrdinaryRoot = [&](va_t Root) {
    LowBlock *Block = Func.blockFor(Root);
    if (!Block)
      return false;
    Func.OrdinaryModuleAnalysisRoots.insert(Root);
    if (!OrdinaryReachable[Block->Id]) {
      OrdinaryReachable[Block->Id] = true;
      OrdinaryWorklist.push(Block->Id);
    }
    return true;
  };
  auto FloodOrdinary = [&]() {
    while (!OrdinaryWorklist.empty()) {
      const int BlockId = OrdinaryWorklist.front();
      OrdinaryWorklist.pop();
      for (int Succ : Func.Blocks[BlockId].Succs)
        if (Succ >= 0 && Succ < static_cast<int>(Func.Blocks.size()) &&
            !OrdinaryReachable[Succ]) {
          OrdinaryReachable[Succ] = true;
          OrdinaryWorklist.push(Succ);
        }
    }
  };
  for (va_t Root : Func.ModuleAnalysisRoots)
    if (FinalOrdinaryAnalysisRoots.count(Root))
      QueueOrdinaryRoot(Root);
  FloodOrdinary();
  for (bool Added = true; Added;) {
    Added = false;
    for (const auto &[Target, Sources] : DiscoveredCodeRefSources) {
      if (!Func.ModuleAnalysisRoots.count(Target) ||
          Func.OrdinaryModuleAnalysisRoots.count(Target))
        continue;
      const bool HasOrdinarySource =
          std::any_of(Sources.begin(), Sources.end(), [&](va_t Source) {
            LowBlock *SourceBlock = Func.blockFor(Source);
            return SourceBlock && OrdinaryReachable[SourceBlock->Id];
          });
      if (!HasOrdinarySource)
        continue;
      Added |= QueueOrdinaryRoot(Target);
    }
    FloodOrdinary();
  }
  linkExceptionalSuccessors(Func);
  extractJumpTables(Func);
  fixupFpuStack(Func);
}

//===----------------------------------------------------------------------===//
// fixupFpuStack — the x86/x86-64 x87 stack-pointer (TOP) fixup — is
// architecture-gated, so it is defined in CFGBuilderX86Fpu.cpp following the
// target-dispatch split used by the jump-table detectors.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// linkSuccessors — wire up block successor/predecessor edges
//===----------------------------------------------------------------------===//

void CFGBuilder::linkSuccessors(LowFunc &Func,
                                const std::map<va_t, int> &AddrToBlock) {
  for (size_t I = 0; I < Func.Blocks.size(); ++I) {
    auto &Blk = Func.Blocks[I];
    if (Blk.Ops.empty() && Blk.InstructionBoundaries.empty())
      continue;

    va_t LastAddr = !Blk.InstructionBoundaries.empty()
                        ? Blk.InstructionBoundaries.back().Address
                        : Blk.Ops.back().Addr;
    auto It = Insns.find(LastAddr);
    if (It == Insns.end())
      continue;
    auto &Rec = It->second;

    if (Rec.IsRet && Rec.IsCond && Rec.IsBranch) {
      auto BIt = AddrToBlock.find(Rec.BranchTarget);
      if (Rec.BranchTarget != InvalidVA && BIt != AddrToBlock.end())
        Blk.Succs.push_back(BIt->second);
    } else if (Rec.IsRet) {
      // Terminal — no successors.
    } else if (Rec.IsBranch && Rec.IsIndirect) {
      if (Rec.IsCond) {
        const va_t Fall = Rec.Addr + Rec.Size;
        auto FIt = AddrToBlock.find(Fall);
        if (FIt != AddrToBlock.end())
          Blk.Succs.push_back(FIt->second);
      }
      for (va_t T : Rec.JumpTableTargets) {
        auto TIt = AddrToBlock.find(T);
        if (TIt != AddrToBlock.end())
          Blk.Succs.push_back(TIt->second);
      }
    } else if (Rec.IsBranch && !Rec.IsIndirect && !Rec.IsCall) {
      if (Rec.IsCond) {
        va_t Fall = Rec.Addr + Rec.Size;
        auto FIt = AddrToBlock.find(Fall);
        if (FIt != AddrToBlock.end())
          Blk.Succs.push_back(FIt->second);
      }
      auto BIt = AddrToBlock.find(Rec.BranchTarget);
      if (Rec.BranchTarget != InvalidVA && BIt != AddrToBlock.end())
        Blk.Succs.push_back(BIt->second);
    } else if (!Rec.IsBranch || Rec.IsCall) {
      // An unconditional no-return call ends control flow.  A conditional
      // call still reaches the next block when its predicate is false.
      if ((!Rec.IsNoReturnCall || Rec.IsCond) && I + 1 < Func.Blocks.size())
        Blk.Succs.push_back(static_cast<int>(I + 1));
    }

    // A predicated non-control ARM instruction uses an instruction-local
    // `COND_BR next, !predicate`: its encoded target and architectural
    // fallthrough are intentionally the same block.  Keep one logical CFG
    // edge while the flattened LowOps retain both micro-CFG paths.  Stable
    // deduplication also prevents duplicate jump-table destinations from
    // manufacturing duplicate phi inputs.
    std::vector<int> UniqueSuccs;
    UniqueSuccs.reserve(Blk.Succs.size());
    for (int Succ : Blk.Succs)
      if (std::find(UniqueSuccs.begin(), UniqueSuccs.end(), Succ) ==
          UniqueSuccs.end())
        UniqueSuccs.push_back(Succ);
    Blk.Succs = std::move(UniqueSuccs);

    for (int S : Blk.Succs) {
      if (S >= 0 && S < static_cast<int>(Func.Blocks.size()))
        Func.Blocks[S].Preds.push_back(Blk.Id);
    }
  }
}

// Exceptional successor/predecessor linking is defined in
// CFGBuilderException.cpp.

} // namespace neverd

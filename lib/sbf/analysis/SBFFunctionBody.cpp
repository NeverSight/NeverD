//===- SBFFunctionBody.cpp - Semantic SBF function CFG views -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFFunctionBody.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

constexpr size_t kNoBlock = std::numeric_limits<size_t>::max();

uint64_t encodedBlockSize(const BasicBlock &Block) {
  if (Block.EndSlot < Block.StartSlot)
    return 0;
  const uint64_t SlotCount = Block.EndSlot - Block.StartSlot;
  if (SlotCount > std::numeric_limits<uint64_t>::max() / kInstructionSize)
    return std::numeric_limits<uint64_t>::max();
  return SlotCount * kInstructionSize;
}

uint64_t saturatingAdd(uint64_t Left, uint64_t Right) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return std::numeric_limits<uint64_t>::max();
  return Left + Right;
}

} // namespace

llvm::ArrayRef<size_t>
FunctionBodyIndex::FlatAdjacency::operator[](size_t Node) const {
  if (Offsets.empty() || Node >= Offsets.size() - 1)
    return {};
  return llvm::ArrayRef(Targets).slice(Offsets[Node],
                                       Offsets[Node + 1] - Offsets[Node]);
}

FunctionBodyIndex::FlatAdjacency FunctionBodyIndex::buildFlatAdjacency(
    size_t Count, llvm::ArrayRef<FlatEdge> Edges, bool Reverse) {
  FlatAdjacency Result;
  Result.Offsets.assign(Count + 1, 0);
  for (const FlatEdge &Edge : Edges) {
    const size_t From = Reverse ? Edge.second : Edge.first;
    if (From < Count)
      ++Result.Offsets[From + 1];
  }
  std::partial_sum(Result.Offsets.begin(), Result.Offsets.end(),
                   Result.Offsets.begin());
  Result.Targets.resize(Result.Offsets.back());
  std::vector<size_t> Next = Result.Offsets;
  for (const FlatEdge &Edge : Edges) {
    const size_t From = Reverse ? Edge.second : Edge.first;
    const size_t To = Reverse ? Edge.first : Edge.second;
    if (From < Count && To < Count)
      Result.Targets[Next[From]++] = To;
  }
  return Result;
}

FunctionBodyIndex::FunctionBodyIndex(const SBFProgram &Program) {
  const size_t BlockCount = Program.Low.Blocks.size();
  const size_t FunctionCount = Program.High.Functions.size();
  Stats.IndexedBlockCount = BlockCount;
  Stats.IndexedFunctionCount = FunctionCount;
  if (Program.High.BlockOwners.size() == BlockCount)
    FunctionOwners = Program.High.BlockOwners;

  llvm::DenseMap<size_t, size_t> BlocksByStartSlot;
  BlocksByStartSlot.reserve(BlockCount);
  BlockByteSizes.assign(BlockCount, 0);
  for (const BasicBlock &Block : Program.Low.Blocks) {
    if (Block.ID >= BlockCount)
      continue;
    BlocksByStartSlot.try_emplace(Block.StartSlot, Block.ID);
    BlockByteSizes[Block.ID] = encodedBlockSize(Block);
  }

  FunctionEntryBlocks.assign(FunctionCount, kNoBlock);
  FunctionIDsByEntrySlot.reserve(FunctionCount);
  llvm::BitVector IsFunctionEntry(BlockCount);
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    const Function &Function = Program.High.Functions[FunctionID];
    FunctionIDsByEntrySlot.try_emplace(Function.EntrySlot, FunctionID);
    const auto Block = BlocksByStartSlot.find(Function.EntrySlot);
    if (Block == BlocksByStartSlot.end())
      continue;
    FunctionEntryBlocks[FunctionID] = Block->second;
    IsFunctionEntry.set(Block->second);
  }

  // An entry is seeded by its virtual loader/caller predecessor. Every real
  // edge entering any function entry is therefore a semantic boundary, not
  // intraprocedural reachability from another function.
  std::vector<FlatEdge> IntraproceduralEdges;
  IntraproceduralEdges.reserve(Program.Low.Edges.size());
  for (const CFGEdge &Edge : Program.Low.Edges)
    if (Edge.From < BlockCount && Edge.To && *Edge.To < BlockCount &&
        getEdgeKindInfo(Edge.Kind).IsIntraprocedural &&
        !IsFunctionEntry.test(*Edge.To))
      IntraproceduralEdges.emplace_back(Edge.From, *Edge.To);
  Stats.IndexedEdgeCount = IntraproceduralEdges.size();

  Successors = buildFlatAdjacency(BlockCount, IntraproceduralEdges);
  Predecessors =
      buildFlatAdjacency(BlockCount, IntraproceduralEdges, /*Reverse=*/true);

  // The owner vector is an optimization authority, so validate the invariants
  // on which exact batched sizing depends before retaining the non-owning
  // view. Malformed/manual IR falls back to ordinary bounded reachability.
  if (!FunctionOwners.empty()) {
    bool ValidOwners = true;
    for (size_t FunctionID = 0; FunctionID < FunctionEntryBlocks.size();
         ++FunctionID) {
      const size_t Entry = FunctionEntryBlocks[FunctionID];
      if (Entry != kNoBlock && FunctionOwners[Entry] != FunctionID) {
        ValidOwners = false;
        break;
      }
    }
    for (const FlatEdge &Edge : IntraproceduralEdges) {
      if (!ValidOwners)
        break;
      const size_t FromOwner = FunctionOwners[Edge.first];
      const size_t ToOwner = FunctionOwners[Edge.second];
      if (FromOwner < FunctionCount) {
        ValidOwners =
            ToOwner == FromOwner || ToOwner == HighIR::AmbiguousFunction;
      } else if (FromOwner == HighIR::AmbiguousFunction) {
        ValidOwners = ToOwner == HighIR::AmbiguousFunction;
      }
    }
    if (!ValidOwners)
      FunctionOwners = {};
  }

  if (FunctionOwners.size() == BlockCount) {
    llvm::BitVector IsAmbiguousReachable(FunctionCount);
    for (size_t Block = 0; Block < BlockCount; ++Block) {
      const size_t Owner = FunctionOwners[Block];
      if (Owner >= FunctionCount)
        continue;
      for (size_t Next : Successors[Block])
        if (FunctionOwners[Next] == HighIR::AmbiguousFunction) {
          IsAmbiguousReachable.set(Owner);
          break;
        }
    }
    AmbiguousReachableFunctions.reserve(IsAmbiguousReachable.count());
    for (int FunctionID = IsAmbiguousReachable.find_first(); FunctionID >= 0;
         FunctionID = IsAmbiguousReachable.find_next(FunctionID))
      AmbiguousReachableFunctions.push_back(static_cast<size_t>(FunctionID));
  } else {
    AmbiguousReachableFunctions.reserve(FunctionCount);
    for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID)
      if (FunctionEntryBlocks[FunctionID] != kNoBlock)
        AmbiguousReachableFunctions.push_back(FunctionID);
  }

  SeedOffsets.assign(BlockCount + 1, 0);
  for (size_t EntryBlock : FunctionEntryBlocks)
    if (EntryBlock != kNoBlock)
      ++SeedOffsets[EntryBlock + 1];
  std::partial_sum(SeedOffsets.begin(), SeedOffsets.end(), SeedOffsets.begin());
  SeedFunctions.resize(SeedOffsets.back());
  std::vector<size_t> NextSeed = SeedOffsets;
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    const size_t EntryBlock = FunctionEntryBlocks[FunctionID];
    if (EntryBlock != kNoBlock)
      SeedFunctions[NextSeed[EntryBlock]++] = FunctionID;
  }

  // Count scalar graph/index entries, including one logical entry per dense
  // map row. This stays O(B + E + F) for every CFG, including nested joins
  // whose exact transitive reachability relation contains Theta(B * F) pairs.
  Stats.ResidentIndexEntryCount =
      FunctionIDsByEntrySlot.size() + FunctionEntryBlocks.size() +
      Successors.Offsets.size() + Successors.Targets.size() +
      Predecessors.Offsets.size() + Predecessors.Targets.size() +
      SeedOffsets.size() + SeedFunctions.size() +
      AmbiguousReachableFunctions.size() + BlockByteSizes.size();
}

llvm::BitVector
FunctionBodyIndex::reachableSet(size_t Root,
                                const FlatAdjacency &Adjacency) const {
  const size_t BlockCount = BlockByteSizes.size();
  llvm::BitVector Visited(BlockCount);
  if (Root >= BlockCount)
    return Visited;

  std::vector<size_t> Worklist;
  Worklist.push_back(Root);
  Visited.set(Root);
  while (!Worklist.empty()) {
    const size_t Block = Worklist.back();
    Worklist.pop_back();
    for (size_t Next : Adjacency[Block])
      if (!Visited.test(Next)) {
        Visited.set(Next);
        Worklist.push_back(Next);
      }
  }
  return Visited;
}

size_t FunctionBodyIndex::functionID(const Function &Function) const {
  const auto Entry = FunctionIDsByEntrySlot.find(Function.EntrySlot);
  return Entry != FunctionIDsByEntrySlot.end() ? Entry->second : kNoBlock;
}

uint64_t FunctionBodyIndex::byteSize(const Function &Function) const {
  const size_t FunctionID = functionID(Function);
  if (FunctionID >= FunctionEntryBlocks.size())
    return 0;

  uint64_t Result = 0;
  const llvm::BitVector Reachable =
      reachableSet(FunctionEntryBlocks[FunctionID], Successors);
  for (int Block = Reachable.find_first(); Block >= 0;
       Block = Reachable.find_next(Block))
    Result = saturatingAdd(Result, BlockByteSizes[static_cast<size_t>(Block)]);
  return Result;
}

FunctionBodyIndex::ByteSizeBatch
FunctionBodyIndex::byteSizes(size_t BlockVisitBudget) const {
  ByteSizeBatch Result;
  const size_t FunctionCount = FunctionEntryBlocks.size();
  const size_t BlockCount = BlockByteSizes.size();
  Result.Bytes.assign(FunctionCount, 0);
  Result.Exact.resize(FunctionCount);

  std::optional<llvm::BitVector> Visited;
  std::vector<size_t> Worklist;
  std::vector<size_t> Touched;

  const auto SumReachable = [&](llvm::ArrayRef<size_t> Seeds,
                                uint64_t &Size) -> bool {
    // Callers short-circuit at the global budget before entering here. Keep a
    // second guard so future consumers cannot allocate a whole-program
    // workspace after exhaustion.
    if (Result.BlockVisitCount == BlockVisitBudget) {
      Result.BudgetExhausted = true;
      return false;
    }
    ++Result.ReachabilityQueryCount;
    if (!Visited) {
      Visited.emplace(BlockCount);
      ++Result.WorkspaceInitializationCount;
    }
    Worklist.clear();
    Touched.clear();
    Worklist.reserve(Seeds.size());
    for (size_t Seed : Seeds)
      if (Seed < BlockCount && !Visited->test(Seed)) {
        Visited->set(Seed);
        Worklist.push_back(Seed);
        Touched.push_back(Seed);
      }

    uint64_t Candidate = 0;
    bool Complete = true;
    while (!Worklist.empty()) {
      if (Result.BlockVisitCount == BlockVisitBudget) {
        Result.BudgetExhausted = true;
        Complete = false;
        break;
      }
      const size_t Block = Worklist.back();
      Worklist.pop_back();
      ++Result.BlockVisitCount;
      Candidate = saturatingAdd(Candidate, BlockByteSizes[Block]);
      for (size_t Next : Successors[Block])
        if (!Visited->test(Next)) {
          Visited->set(Next);
          Worklist.push_back(Next);
          Touched.push_back(Next);
        }
    }
    for (size_t Block : Touched)
      Visited->reset(Block);
    if (!Complete)
      return false;
    Size = Candidate;
    return true;
  };

  if (FunctionOwners.size() != BlockCount) {
    for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID)
      if (FunctionEntryBlocks[FunctionID] == kNoBlock)
        Result.Exact.set(FunctionID);
    for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
      const size_t Entry = FunctionEntryBlocks[FunctionID];
      if (Entry == kNoBlock)
        continue;
      if (Result.BlockVisitCount == BlockVisitBudget) {
        Result.BudgetExhausted = true;
        break;
      }
      uint64_t Size = 0;
      if (SumReachable(llvm::ArrayRef<size_t>(&Entry, 1), Size)) {
        Result.Bytes[FunctionID] = Size;
        Result.Exact.set(FunctionID);
      }
    }
    return Result;
  }

  // Unique ownership is already the exact forward-provenance solution. Sum
  // that partition once, then query only the shared-tail frontier of each
  // function. This turns the common "many entries, one tail" shape from
  // F*(B+E) into B+E+F without materializing the transitive closure.
  std::vector<uint64_t> OwnedBytes(FunctionCount, 0);
  std::vector<std::vector<size_t>> SharedFrontiers(FunctionCount);
  for (size_t Block = 0; Block < BlockCount; ++Block) {
    const size_t Owner = FunctionOwners[Block];
    if (Owner < FunctionCount)
      OwnedBytes[Owner] =
          saturatingAdd(OwnedBytes[Owner], BlockByteSizes[Block]);
    for (size_t Next : Successors[Block])
      if (Owner < FunctionCount &&
          FunctionOwners[Next] == HighIR::AmbiguousFunction)
        SharedFrontiers[Owner].push_back(Next);
  }

  std::map<std::vector<size_t>, std::vector<size_t>> FunctionsByFrontier;
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    std::vector<size_t> &Frontier = SharedFrontiers[FunctionID];
    std::sort(Frontier.begin(), Frontier.end());
    Frontier.erase(std::unique(Frontier.begin(), Frontier.end()),
                   Frontier.end());
    if (Frontier.empty()) {
      Result.Bytes[FunctionID] = OwnedBytes[FunctionID];
      Result.Exact.set(FunctionID);
      continue;
    }
    FunctionsByFrontier[Frontier].push_back(FunctionID);
  }

  for (const auto &[Frontier, FunctionIDs] : FunctionsByFrontier) {
    if (Result.BlockVisitCount == BlockVisitBudget) {
      Result.BudgetExhausted = true;
      break;
    }
    uint64_t SharedBytes = 0;
    if (!SumReachable(Frontier, SharedBytes))
      continue;
    for (size_t FunctionID : FunctionIDs) {
      Result.Bytes[FunctionID] =
          saturatingAdd(OwnedBytes[FunctionID], SharedBytes);
      Result.Exact.set(FunctionID);
    }
  }
  return Result;
}

FunctionBodyIndex::BlockGroupFunctionBatch
FunctionBodyIndex::functionsForBlockGroups(
    llvm::ArrayRef<BlockGroupQuery> Groups, size_t BlockVisitBudget,
    size_t OutputRelationBudget) const {
  ++Stats.BlockFunctionBatchQueryCount;
  BlockGroupFunctionBatch Result;
  const size_t GroupCount = Groups.size();
  const size_t BlockCount = BlockByteSizes.size();
  const size_t FunctionCount = FunctionEntryBlocks.size();

  std::vector<std::vector<size_t>> FunctionsByGroup(GroupCount);
  std::vector<std::vector<size_t>> AmbiguousBlocksByGroup(GroupCount);
  llvm::DenseSet<std::pair<size_t, size_t>> SeenRelations;

  const auto ClearRelation = [&] {
    Result.FunctionOffsets.clear();
    Result.FunctionIDs.clear();
    for (std::vector<size_t> &Functions : FunctionsByGroup) {
      std::vector<size_t>().swap(Functions);
    }
  };
  const auto RecordReverseStatistics = [&] {
    Stats.ReverseReachabilityQueryCount += Result.ReverseGroupTraversalCount;
    if (Result.ReverseGroupTraversalCount != 0)
      Stats.ReverseWorkspaceInitializationCount +=
          Result.WorkspaceInitializationCount;
  };
  const auto AddRelation = [&](size_t GroupID, size_t FunctionID) {
    const std::pair<size_t, size_t> Relation = {GroupID, FunctionID};
    if (SeenRelations.contains(Relation))
      return true;
    if (Result.OutputRelationCount == OutputRelationBudget) {
      Result.OutputBudgetExhausted = true;
      return false;
    }
    SeenRelations.insert(Relation);
    FunctionsByGroup[GroupID].push_back(FunctionID);
    ++Result.OutputRelationCount;
    return true;
  };
  const auto FlattenRelation = [&] {
    Result.FunctionOffsets.assign(GroupCount + 1, 0);
    for (size_t GroupID = 0; GroupID < GroupCount; ++GroupID) {
      std::vector<size_t> &Functions = FunctionsByGroup[GroupID];
      std::sort(Functions.begin(), Functions.end());
      Result.FunctionOffsets[GroupID + 1] =
          Result.FunctionOffsets[GroupID] + Functions.size();
    }
    Result.FunctionIDs.reserve(Result.FunctionOffsets.back());
    for (const std::vector<size_t> &Functions : FunctionsByGroup)
      Result.FunctionIDs.insert(Result.FunctionIDs.end(), Functions.begin(),
                                Functions.end());
  };

  size_t AmbiguousGroupCount = 0;
  for (size_t GroupID = 0; GroupID < GroupCount; ++GroupID) {
    std::vector<size_t> Requested(Groups[GroupID].Blocks.begin(),
                                  Groups[GroupID].Blocks.end());
    Requested.erase(std::remove_if(Requested.begin(), Requested.end(),
                                   [BlockCount](size_t Block) {
                                     return Block >= BlockCount;
                                   }),
                    Requested.end());
    std::sort(Requested.begin(), Requested.end());
    Requested.erase(std::unique(Requested.begin(), Requested.end()),
                    Requested.end());
    for (size_t Block : Requested) {
      if (FunctionOwners.size() == BlockCount) {
        const size_t Owner = FunctionOwners[Block];
        if (Owner < FunctionCount) {
          if (!AddRelation(GroupID, Owner)) {
            ClearRelation();
            return Result;
          }
          continue;
        }
        if (Owner == HighIR::NoFunction)
          continue;
      }
      AmbiguousBlocksByGroup[GroupID].push_back(Block);
    }
    if (!AmbiguousBlocksByGroup[GroupID].empty())
      ++AmbiguousGroupCount;
  }

  if (AmbiguousGroupCount == 0 || AmbiguousReachableFunctions.empty()) {
    FlattenRelation();
    return Result;
  }

  // A reusable whole-program visited set is the only dense query workspace.
  // Charge its logical initialization before allocation. At least one block
  // must then be visited, so equality cannot start an exact traversal.
  if (BlockCount >= BlockVisitBudget) {
    Result.VisitBudgetExhausted = true;
    ClearRelation();
    return Result;
  }
  Result.ChargedBlockWork = BlockCount;
  llvm::BitVector Visited(BlockCount);
  ++Result.WorkspaceInitializationCount;
  std::vector<size_t> Worklist;
  std::vector<size_t> Touched;

  const auto Traverse = [&](llvm::ArrayRef<size_t> Seeds,
                            const FlatAdjacency &Adjacency, auto &&VisitBlock) {
    Worklist.clear();
    Touched.clear();
    Worklist.reserve(Seeds.size());
    for (size_t Seed : Seeds)
      if (Seed < BlockCount && !Visited.test(Seed)) {
        Visited.set(Seed);
        Worklist.push_back(Seed);
        Touched.push_back(Seed);
      }

    bool Complete = true;
    while (!Worklist.empty()) {
      if (Result.ChargedBlockWork == BlockVisitBudget) {
        Result.VisitBudgetExhausted = true;
        Complete = false;
        break;
      }
      const size_t Block = Worklist.back();
      Worklist.pop_back();
      ++Result.ChargedBlockWork;
      ++Result.BlockVisitCount;
      if (!VisitBlock(Block)) {
        Complete = false;
        break;
      }
      for (size_t Next : Adjacency[Block])
        if (!Visited.test(Next)) {
          Visited.set(Next);
          Worklist.push_back(Next);
          Touched.push_back(Next);
        }
    }
    for (size_t Block : Touched)
      Visited.reset(Block);
    return Complete;
  };

  // Choose the smaller exact provenance direction. Validated unique owners
  // let the forward side ignore target-only functions that cannot enter any
  // shared body; without that authority the candidate list conservatively
  // contains every valid function root.
  if (AmbiguousGroupCount <= AmbiguousReachableFunctions.size()) {
    for (size_t GroupID = 0; GroupID < GroupCount; ++GroupID) {
      const std::vector<size_t> &Seeds = AmbiguousBlocksByGroup[GroupID];
      if (Seeds.empty())
        continue;
      ++Result.ReverseGroupTraversalCount;
      if (!Traverse(Seeds, Predecessors, [&](size_t Block) {
            const llvm::ArrayRef<size_t> Functions =
                llvm::ArrayRef(SeedFunctions)
                    .slice(SeedOffsets[Block],
                           SeedOffsets[Block + 1] - SeedOffsets[Block]);
            for (size_t FunctionID : Functions)
              if (!AddRelation(GroupID, FunctionID))
                return false;
            return true;
          })) {
        ClearRelation();
        RecordReverseStatistics();
        return Result;
      }
    }
  } else {
    FlatAdjacency GroupsByBlock;
    GroupsByBlock.Offsets.assign(BlockCount + 1, 0);
    for (const std::vector<size_t> &Blocks : AmbiguousBlocksByGroup)
      for (size_t Block : Blocks)
        ++GroupsByBlock.Offsets[Block + 1];
    std::partial_sum(GroupsByBlock.Offsets.begin(), GroupsByBlock.Offsets.end(),
                     GroupsByBlock.Offsets.begin());
    GroupsByBlock.Targets.resize(GroupsByBlock.Offsets.back());
    std::vector<size_t> Next = GroupsByBlock.Offsets;
    for (size_t GroupID = 0; GroupID < GroupCount; ++GroupID)
      for (size_t Block : AmbiguousBlocksByGroup[GroupID])
        GroupsByBlock.Targets[Next[Block]++] = GroupID;

    for (size_t FunctionID : AmbiguousReachableFunctions) {
      if (FunctionID >= FunctionEntryBlocks.size())
        continue;
      const size_t Entry = FunctionEntryBlocks[FunctionID];
      if (Entry == kNoBlock)
        continue;
      ++Result.FunctionTraversalCount;
      if (!Traverse(llvm::ArrayRef<size_t>(&Entry, 1), Successors,
                    [&](size_t Block) {
                      for (size_t GroupID : GroupsByBlock[Block])
                        if (!AddRelation(GroupID, FunctionID))
                          return false;
                      return true;
                    })) {
        ClearRelation();
        return Result;
      }
    }
  }

  FlattenRelation();
  RecordReverseStatistics();
  return Result;
}

std::vector<size_t> FunctionBodyIndex::blocks(const Function &Function) const {
  const size_t FunctionID = functionID(Function);
  if (FunctionID >= FunctionEntryBlocks.size())
    return {};
  const llvm::BitVector Reachable =
      reachableSet(FunctionEntryBlocks[FunctionID], Successors);
  std::vector<size_t> Result;
  Result.reserve(Reachable.count());
  for (int Block = Reachable.find_first(); Block >= 0;
       Block = Reachable.find_next(Block))
    Result.push_back(static_cast<size_t>(Block));
  return Result;
}

std::vector<size_t> FunctionBodyIndex::functionsForBlock(size_t BlockID) const {
  return functionsForAnyBlock(llvm::ArrayRef<size_t>(&BlockID, 1));
}

std::vector<size_t>
FunctionBodyIndex::functionsForAnyBlock(llvm::ArrayRef<size_t> BlockIDs) const {
  ++Stats.BlockFunctionBatchQueryCount;
  std::vector<size_t> Result;
  const size_t BlockCount = BlockByteSizes.size();
  std::vector<size_t> Requested(BlockIDs.begin(), BlockIDs.end());
  Requested.erase(std::remove_if(Requested.begin(), Requested.end(),
                                 [BlockCount](size_t Block) {
                                   return Block >= BlockCount;
                                 }),
                  Requested.end());
  std::sort(Requested.begin(), Requested.end());
  Requested.erase(std::unique(Requested.begin(), Requested.end()),
                  Requested.end());

  std::optional<llvm::BitVector> Reachable;
  std::vector<size_t> Worklist;
  for (size_t Block : Requested) {
    if (FunctionOwners.size() == BlockCount) {
      const size_t Owner = FunctionOwners[Block];
      if (Owner < FunctionEntryBlocks.size()) {
        Result.push_back(Owner);
        continue;
      }
      if (Owner == HighIR::NoFunction)
        continue;
    }
    if (!Reachable) {
      Reachable.emplace(BlockCount);
      ++Stats.ReverseWorkspaceInitializationCount;
    }
    if (!Reachable->test(Block)) {
      Reachable->set(Block);
      Worklist.push_back(Block);
    }
  }

  if (!Worklist.empty())
    ++Stats.ReverseReachabilityQueryCount;
  while (!Worklist.empty()) {
    const size_t Block = Worklist.back();
    Worklist.pop_back();
    for (size_t Previous : Predecessors[Block])
      if (!Reachable->test(Previous)) {
        Reachable->set(Previous);
        Worklist.push_back(Previous);
      }
  }

  if (Reachable)
    for (int ReachableBlock = Reachable->find_first(); ReachableBlock >= 0;
         ReachableBlock = Reachable->find_next(ReachableBlock)) {
      const size_t Block = static_cast<size_t>(ReachableBlock);
      const llvm::ArrayRef<size_t> Seeds =
          llvm::ArrayRef(SeedFunctions)
              .slice(SeedOffsets[Block],
                     SeedOffsets[Block + 1] - SeedOffsets[Block]);
      Result.insert(Result.end(), Seeds.begin(), Seeds.end());
    }
  std::sort(Result.begin(), Result.end());
  Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
  return Result;
}

} // namespace neverd::sbf

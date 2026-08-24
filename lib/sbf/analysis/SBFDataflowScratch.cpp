//===- SBFDataflowScratch.cpp - Solana SBF scratch fixed point ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Propagates the scratch-memory model across the intra-function CFG to a
/// fixed point, and replays one block's instructions against the state that
/// reaches it so a caller can read the machine state at each of them.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFDataflow.h"

#include <algorithm>
#include <deque>
#include <numeric>
#include <utility>
#include <vector>

namespace neverd::sbf {

//===----------------------------------------------------------------------===//
// ScratchFlow
//===----------------------------------------------------------------------===//
ScratchFlow::ScratchFlow(const SBFProgram &Program,
                         const MedInstructionIndex &Index) {
  const size_t Count =
      std::min(Program.Med.Blocks.size(), Program.Low.Blocks.size());
  Stats.IndexedBlockCount = Count;
  Entry.resize(Count);
  if (Count == 0)
    return;

  // A vector<SmallVector> pays an inline two-edge allocation at every block,
  // even though a deployable program may contain 1.31 million blocks. Build a
  // flat CSR index so resident storage is exactly O(B + E) scalar entries.
  std::vector<size_t> SuccessorOffsets(Count + 1, 0);
  std::vector<char> HasPredecessor(Count, 0);
  std::vector<char> IsRoot(Count, 0);
  for (const CFGEdge &Edge : Program.Low.Edges) {
    if (!Edge.To || *Edge.To >= Count || Edge.From >= Count)
      continue;
    if (Edge.Kind == EdgeKind::Call)
      IsRoot[*Edge.To] = 1;
    if (!getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
      continue;
    ++SuccessorOffsets[Edge.From + 1];
    HasPredecessor[*Edge.To] = 1;
  }
  std::partial_sum(SuccessorOffsets.begin(), SuccessorOffsets.end(),
                   SuccessorOffsets.begin());
  std::vector<size_t> SuccessorTargets(SuccessorOffsets.back());
  std::vector<size_t> NextSuccessor = SuccessorOffsets;
  for (const CFGEdge &Edge : Program.Low.Edges)
    if (Edge.To && *Edge.To < Count && Edge.From < Count &&
        getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
      SuccessorTargets[NextSuccessor[Edge.From]++] = *Edge.To;
  Stats.IndexedEdgeCount = SuccessorTargets.size();
  Stats.SuccessorIndexEntryCount =
      SuccessorOffsets.size() + SuccessorTargets.size();

  const auto Replay = [&](size_t ID, const ScratchState &In) {
    MachineState State;
    State.Registers = Program.Med.Blocks[ID].Inputs;
    State.Scratch = In;
    const MedBlock &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const MedInstruction *Instruction = Index.find(Slot))
        applyTransfer(*Instruction, State, Program.ExecutableImage);
    return std::move(State.Scratch);
  };

  std::vector<std::shared_ptr<const ScratchState>> Exit(Count);
  const auto Charge = [](const std::shared_ptr<const ScratchState> &State) {
    if (!State || State->Memory.empty())
      return uint64_t{0};
    // Account for the shared object and its control block in addition to the
    // MemoryModel-owned buffers and tree nodes. Logical references are charged
    // independently even when their shared_ptr happens to alias.
    constexpr uint64_t SharedControlBlockWords = 2;
    return State->Memory.retainedByteEstimate() +
           (sizeof(ScratchState) - sizeof(MemoryModel)) +
           SharedControlBlockWords * sizeof(void *);
  };
  const auto CanRetain = [&](const std::shared_ptr<const ScratchState> &Old,
                             uint64_t NewCharge) {
    const uint64_t OldCharge = Charge(Old);
    if (OldCharge > Stats.RetainedByteEstimate)
      return false;
    const uint64_t WithoutOld = Stats.RetainedByteEstimate - OldCharge;
    if (WithoutOld > kScratchFlowRetainedByteBudget ||
        NewCharge > kScratchFlowRetainedByteBudget - WithoutOld)
      return false;
    Stats.RetainedByteEstimate = WithoutOld + NewCharge;
    Stats.PeakRetainedByteEstimate =
        std::max(Stats.PeakRetainedByteEstimate, Stats.RetainedByteEstimate);
    return true;
  };
  const auto WidenToUnknown = [&] {
    // Empty is Unknown in this must lattice: it claims no byte. Releasing all
    // payload-bearing roots and using that state at every block is therefore
    // a sound widened fixed point. Per-block replay can still recover stores
    // performed earlier in the same block.
    Stats.Precision = ScratchFlowPrecision::WidenedToUnknown;
    Stats.RetainedByteEstimate = 0;
    std::fill(Entry.begin(), Entry.end(), Empty);
    Exit.clear();
    Exit.shrink_to_fit();
  };
  std::vector<char> Queued(Count, 0);
  for (size_t ID = 0; ID < Count; ++ID)
    if (!HasPredecessor[ID])
      IsRoot[ID] = 1;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Block.ID < Count && Program.Low.EntrySlot >= Block.StartSlot &&
        Program.Low.EntrySlot < Block.EndSlot) {
      IsRoot[Block.ID] = 1;
      break;
    }
  for (const Function &Function : Program.High.Functions)
    for (size_t BlockID : Program.High.ownedBlocks(Function)) {
      if (BlockID >= Count)
        continue;
      const MedBlock &Block = Program.Med.Blocks[BlockID];
      if (Block.StartSlot == Function.EntrySlot)
        IsRoot[BlockID] = 1;
    }
  std::deque<size_t> Worklist;
  for (size_t ID = 0; ID < Count; ++ID)
    if (IsRoot[ID]) {
      Entry[ID] = Empty;
      Worklist.push_back(ID);
      Queued[ID] = 1;
    }

  // Unreached is a separate lattice bottom. Once a block is reached, every
  // recomputation can only remove a proven byte, so the bounded MemoryModel
  // lattice reaches its fixed point naturally. A
  // step cap would be unsound here: returning a half-converged must-fact can
  // claim a byte is constant after another path has disproved it.
  while (!Worklist.empty()) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    Queued[ID] = 0;

    if (!Entry[ID])
      continue;
    ScratchState NewExit = Replay(ID, *Entry[ID]);
    if (Exit[ID] && NewExit == *Exit[ID])
      continue;
    std::shared_ptr<const ScratchState> NextExit;
    if (NewExit == *Entry[ID])
      NextExit = Entry[ID];
    else
      NextExit = std::make_shared<const ScratchState>(std::move(NewExit));
    if (!CanRetain(Exit[ID], Charge(NextExit))) {
      WidenToUnknown();
      return;
    }
    Exit[ID] = std::move(NextExit);

    for (size_t EdgeIndex = SuccessorOffsets[ID];
         EdgeIndex < SuccessorOffsets[ID + 1]; ++EdgeIndex) {
      const size_t Successor = SuccessorTargets[EdgeIndex];
      bool EntryChanged = false;
      if (!Entry[Successor]) {
        if (!CanRetain(Entry[Successor], Charge(Exit[ID]))) {
          WidenToUnknown();
          return;
        }
        Entry[Successor] = Exit[ID];
        EntryChanged = true;
      } else if (Entry[Successor] != Exit[ID]) {
        ScratchState Merged = *Entry[Successor];
        Merged.meet(*Exit[ID]);
        if (Merged != *Entry[Successor]) {
          std::shared_ptr<const ScratchState> NextEntry;
          if (Merged == *Exit[ID])
            NextEntry = Exit[ID];
          else
            NextEntry = std::make_shared<const ScratchState>(std::move(Merged));
          if (!CanRetain(Entry[Successor], Charge(NextEntry))) {
            WidenToUnknown();
            return;
          }
          Entry[Successor] = std::move(NextEntry);
          EntryChanged = true;
        }
      }
      if (!EntryChanged || Queued[Successor])
        continue;
      Worklist.push_back(Successor);
      Queued[Successor] = 1;
    }
  }
}

const ScratchState &ScratchFlow::entryState(size_t BlockID) const {
  return BlockID < Entry.size() && Entry[BlockID] ? *Entry[BlockID] : Unreached;
}

void replayBlock(
    const MedInstructionIndex &Index, const MedBlock &Block,
    const ScratchState &Entry, const ProgramImage &Image,
    llvm::function_ref<void(const MedInstruction &, const MachineState &)>
        Visit) {
  MachineState State;
  State.Registers = Block.Inputs;
  State.Scratch = Entry;
  for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
    const MedInstruction *Instruction = Index.find(Slot);
    if (!Instruction)
      continue;
    Visit(*Instruction, State);
    applyTransfer(*Instruction, State, Image);
  }
}

} // namespace neverd::sbf

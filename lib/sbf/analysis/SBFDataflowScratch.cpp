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

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

bool isIntraFunctionEdge(EdgeKind Kind) {
  switch (Kind) {
  case EdgeKind::Fallthrough:
  case EdgeKind::BranchTaken:
  case EdgeKind::Branch:
    return true;
  case EdgeKind::Call:
  case EdgeKind::IndirectCall:
  case EdgeKind::Return:
  case EdgeKind::Invalid:
    return false;
  }
  return false;
}

} // namespace

//===----------------------------------------------------------------------===//
// ScratchFlow
//===----------------------------------------------------------------------===//
ScratchFlow::ScratchFlow(const SBFProgram &Program,
                         const MedInstructionIndex &Index) {
  const size_t Count =
      std::min(Program.Med.Blocks.size(), Program.Low.Blocks.size());
  Entry.resize(Count);
  if (Count == 0 || Count > kMaxScratchFlowBlocks)
    return;

  std::vector<llvm::SmallVector<size_t, 2>> Predecessors(Count);
  std::vector<llvm::SmallVector<size_t, 2>> Successors(Count);
  for (const CFGEdge &Edge : Program.Low.Edges) {
    if (!Edge.To || *Edge.To >= Count || Edge.From >= Count ||
        !isIntraFunctionEdge(Edge.Kind))
      continue;
    Predecessors[*Edge.To].push_back(Edge.From);
    Successors[Edge.From].push_back(*Edge.To);
  }

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

  std::vector<ScratchState> Exit(Count);
  std::vector<char> Computed(Count, 0);
  std::vector<char> Queued(Count, 0);
  std::deque<size_t> Worklist;
  for (size_t ID = 0; ID < Count; ++ID)
    if (Predecessors[ID].empty()) {
      Worklist.push_back(ID);
      Queued[ID] = 1;
    }

  // Each recomputation can only drop facts, so the analysis settles on its
  // own; the cap only keeps a malformed CFG from costing unbounded time.
  const size_t StepLimit = Count * 8 + 64;
  for (size_t Step = 0; Step < StepLimit && !Worklist.empty(); ++Step) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    Queued[ID] = 0;

    ScratchState Merged;
    bool Any = false;
    for (size_t Predecessor : Predecessors[ID]) {
      if (!Computed[Predecessor])
        continue;
      if (!Any) {
        Merged = Exit[Predecessor];
        Any = true;
        continue;
      }
      Merged.meet(Exit[Predecessor]);
    }
    // A block whose predecessors are all still unknown waits for one of them
    // rather than claiming the empty state as a fact about this path.
    if (!Any && !Predecessors[ID].empty())
      continue;

    if (Computed[ID] && Merged == Entry[ID])
      continue;
    Entry[ID] = std::move(Merged);
    Exit[ID] = Replay(ID, Entry[ID]);
    Computed[ID] = 1;

    for (size_t Successor : Successors[ID]) {
      if (Queued[Successor])
        continue;
      Worklist.push_back(Successor);
      Queued[Successor] = 1;
    }
  }
}

const ScratchState &ScratchFlow::entryState(size_t BlockID) const {
  return BlockID < Entry.size() ? Entry[BlockID] : Unreached;
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

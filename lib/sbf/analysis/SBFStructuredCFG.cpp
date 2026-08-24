//===- SBFStructuredCFG.cpp - SBF reducible control-flow plan -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFStructuredCFG.h"

#include "llvm/ADT/BitVector.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace neverd::sbf {
namespace {

bool isConditional(Operation Op) {
  switch (Op) {
  case Operation::Eq:
  case Operation::Ne:
  case Operation::UGt:
  case Operation::UGe:
  case Operation::ULt:
  case Operation::ULe:
  case Operation::SGt:
  case Operation::SGe:
  case Operation::SLt:
  case Operation::SLe:
  case Operation::Set:
    return true;
  default:
    return false;
  }
}

class Structurer {
public:
  explicit Structurer(const SBFProgram &Program)
      : Program(Program), Terminators(Program.Low.Blocks.size()),
        IfRegions(Program.Low.Blocks.size(), kNoIndex),
        LoopRegions(Program.Low.Blocks.size(), kNoIndex),
        TakenSuccessors(Program.Low.Blocks.size(), kNoIndex),
        FallthroughSuccessors(Program.Low.Blocks.size(), kNoIndex),
        Emitted(Program.Low.Blocks.size()),
        DirectLoopBlocks(Program.High.Regions.size()),
        NestedLoopBlocks(Program.High.Regions.size()),
        ValidatedLoops(Program.High.Regions.size()) {
    buildIndices();
  }

  std::optional<StructuredControlFlow> build() {
    if (!IndicesValid || Program.Low.Blocks.empty())
      return std::nullopt;
    for (const MedInstruction &Instruction : Program.Med.Instructions) {
      if (Instruction.Op == Operation::Invalid ||
          Instruction.Op == Operation::CallX ||
          (Instruction.Op == Operation::Call &&
           Instruction.Call != CallKind::Syscall))
        return std::nullopt;
    }

    const BasicBlock *Entry = nullptr;
    for (const BasicBlock &Block : Program.Low.Blocks)
      if (Program.Low.EntrySlot >= Block.StartSlot &&
          Program.Low.EntrySlot < Block.EndSlot) {
        Entry = &Block;
        break;
      }
    if (!Entry)
      return std::nullopt;

    StructuredControlFlow Result;
    Result.Nodes.reserve(Program.Low.Blocks.size());
    std::vector<BuildTask> Work;
    Work.push_back(BuildTask::sequence(Entry->ID, std::nullopt, nullptr,
                                       kNoIndex, SequenceLink::Entry, 0));
    while (!Work.empty()) {
      BuildTask Task = Work.back();
      Work.pop_back();
      if (Task.Kind == BuildTaskKind::ValidateLoop) {
        if (!validateLoop(Task.LoopRegion))
          return std::nullopt;
        continue;
      }
      if (!buildSequence(Task, Result, Work))
        return std::nullopt;
    }
    for (const BasicBlock &Block : Program.Low.Blocks)
      if (Block.Reachable &&
          (Block.ID >= Emitted.size() || !Emitted.test(Block.ID)))
        return std::nullopt;
    for (size_t RegionIndex = 0; RegionIndex < Program.High.Regions.size();
         ++RegionIndex)
      if (Program.High.Regions[RegionIndex].Kind == RegionKind::Loop &&
          !ValidatedLoops.test(RegionIndex))
        return std::nullopt;
    return Result;
  }

private:
  static constexpr size_t kNoIndex = StructuredNode::NoNode;
  static constexpr size_t kAmbiguousIndex = kNoIndex - 1;

  enum class SequenceLink : uint8_t { Entry, Next, Body, Alternative };
  enum class BuildTaskKind : uint8_t { Sequence, ValidateLoop };

  struct BuildTask {
    BuildTaskKind Kind = BuildTaskKind::Sequence;
    size_t Current = kNoIndex;
    std::optional<size_t> Stop;
    const Region *Allowed = nullptr;
    size_t Owner = kNoIndex;
    SequenceLink Link = SequenceLink::Entry;
    size_t Depth = 0;
    size_t LoopRegion = kNoIndex;

    static BuildTask sequence(size_t Current, std::optional<size_t> Stop,
                              const Region *Allowed, size_t Owner,
                              SequenceLink Link, size_t Depth) {
      BuildTask Task;
      Task.Current = Current;
      Task.Stop = Stop;
      Task.Allowed = Allowed;
      Task.Owner = Owner;
      Task.Link = Link;
      Task.Depth = Depth;
      return Task;
    }

    static BuildTask validate(size_t LoopRegion) {
      BuildTask Task;
      Task.Kind = BuildTaskKind::ValidateLoop;
      Task.LoopRegion = LoopRegion;
      return Task;
    }
  };

  static void recordUnique(size_t &Index, size_t Value) {
    Index = Index == kNoIndex ? Value : kAmbiguousIndex;
  }

  void buildIndices() {
    std::vector<const MedInstruction *> BySlot(Program.Low.Instructions.size());
    for (const MedInstruction &Instruction : Program.Med.Instructions) {
      if (Instruction.Slot >= BySlot.size() || BySlot[Instruction.Slot]) {
        IndicesValid = false;
        continue;
      }
      BySlot[Instruction.Slot] = &Instruction;
    }
    for (size_t Index = 0; Index < Program.Low.Blocks.size(); ++Index) {
      const BasicBlock &Block = Program.Low.Blocks[Index];
      if (Block.ID != Index || Block.StartSlot > Block.EndSlot ||
          Block.EndSlot > BySlot.size()) {
        IndicesValid = false;
        continue;
      }
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
        if (BySlot[Slot])
          Terminators[Index] = BySlot[Slot];
    }

    for (const CFGEdge &Edge : Program.Low.Edges) {
      if (!Edge.To || Edge.From >= Program.Low.Blocks.size())
        continue;
      if (Edge.Kind == EdgeKind::BranchTaken)
        recordUnique(TakenSuccessors[Edge.From], *Edge.To);
      else if (Edge.Kind == EdgeKind::Fallthrough)
        recordUnique(FallthroughSuccessors[Edge.From], *Edge.To);
    }

    for (size_t Index = 0; Index < Program.High.Regions.size(); ++Index) {
      const Region &Recovered = Program.High.Regions[Index];
      if (Recovered.HeaderBlock >= Program.Low.Blocks.size()) {
        IndicesValid = false;
        continue;
      }
      size_t *RegionIndex = nullptr;
      if (Recovered.Kind == RegionKind::If) {
        if (!Recovered.Blocks.empty())
          IndicesValid = false;
        RegionIndex = &IfRegions[Recovered.HeaderBlock];
      } else if (Recovered.Kind == RegionKind::Loop) {
        if (!Recovered.Blocks.empty() || !Recovered.FunctionIndex ||
            *Recovered.FunctionIndex >= Program.High.Functions.size() ||
            Recovered.BlockCount == 0 ||
            Recovered.LoopPreorder >= Recovered.LoopSubtreeEnd ||
            Recovered.LatchCount == 0 ||
            Recovered.LatchOffset > Program.High.LoopLatches.size() ||
            Recovered.LatchCount >
                Program.High.LoopLatches.size() - Recovered.LatchOffset ||
            !Program.High.loopContains(Recovered, Recovered.HeaderBlock))
          IndicesValid = false;
        if (Recovered.ParentRegion) {
          if (*Recovered.ParentRegion >= Program.High.Regions.size()) {
            IndicesValid = false;
          } else {
            const Region &Parent =
                Program.High.Regions[*Recovered.ParentRegion];
            if (Parent.Kind != RegionKind::Loop ||
                Parent.FunctionIndex != Recovered.FunctionIndex ||
                Parent.LoopPreorder >= Recovered.LoopPreorder ||
                Recovered.LoopSubtreeEnd > Parent.LoopSubtreeEnd ||
                Parent.BlockCount < Recovered.BlockCount)
              IndicesValid = false;
          }
        }
        for (size_t Latch : Program.High.latches(Recovered))
          if (!Program.High.loopContains(Recovered, Latch))
            IndicesValid = false;
        RegionIndex = &LoopRegions[Recovered.HeaderBlock];
      }
      if (RegionIndex)
        recordUnique(*RegionIndex, Index);
    }
  }

  const BasicBlock *block(size_t ID) const {
    if (ID >= Program.Low.Blocks.size() || Program.Low.Blocks[ID].ID != ID)
      return nullptr;
    return &Program.Low.Blocks[ID];
  }

  const MedInstruction *terminator(size_t BlockID) const {
    return BlockID < Terminators.size() ? Terminators[BlockID] : nullptr;
  }

  const Region *region(size_t Header, RegionKind Kind) const {
    if (Header >= Program.Low.Blocks.size())
      return nullptr;
    const size_t Index = Kind == RegionKind::If     ? IfRegions[Header]
                         : Kind == RegionKind::Loop ? LoopRegions[Header]
                                                    : kNoIndex;
    if (Index >= Program.High.Regions.size())
      return nullptr;
    return &Program.High.Regions[Index];
  }

  std::optional<size_t> successor(size_t BlockID, EdgeKind Kind) const {
    if (BlockID >= Program.Low.Blocks.size())
      return std::nullopt;
    const size_t Result =
        Kind == EdgeKind::BranchTaken   ? TakenSuccessors[BlockID]
        : Kind == EdgeKind::Fallthrough ? FallthroughSuccessors[BlockID]
                                        : kNoIndex;
    return Result < Program.Low.Blocks.size() ? std::optional<size_t>(Result)
                                              : std::nullopt;
  }

  bool contains(const Region &Loop, size_t BlockID) const {
    return Program.High.loopContains(Loop, BlockID);
  }

  bool markEmitted(size_t BlockID) {
    if (BlockID >= Emitted.size() || Emitted.test(BlockID))
      return false;
    Emitted.set(BlockID);
    if (BlockID < Program.High.BlockLoops.size()) {
      const size_t LoopRegion = Program.High.BlockLoops[BlockID];
      if (LoopRegion != HighIR::NoRegion) {
        if (LoopRegion >= Program.High.Regions.size() ||
            Program.High.Regions[LoopRegion].Kind != RegionKind::Loop)
          return false;
        ++DirectLoopBlocks[LoopRegion];
      }
    }
    return true;
  }

  bool attach(StructuredControlFlow &Result, const BuildTask &Task,
              size_t First) {
    size_t *Link = nullptr;
    if (Task.Link == SequenceLink::Entry) {
      Link = &Result.Entry;
    } else {
      if (Task.Owner >= Result.Nodes.size())
        return false;
      StructuredNode &Owner = Result.Nodes[Task.Owner];
      Link = Task.Link == SequenceLink::Next   ? &Owner.Next
             : Task.Link == SequenceLink::Body ? &Owner.Body
                                               : &Owner.Alternative;
    }
    if (*Link != kNoIndex)
      return false;
    *Link = First;
    return true;
  }

  bool append(StructuredControlFlow &Result, const BuildTask &Task,
              size_t &Tail, StructuredNode Node, size_t &Index) {
    Index = Result.Nodes.size();
    Result.Nodes.push_back(std::move(Node));
    Result.MaximumDepth = std::max(Result.MaximumDepth, Task.Depth);
    if (Tail == kNoIndex) {
      if (!attach(Result, Task, Index))
        return false;
    } else {
      if (Tail >= Result.Nodes.size() || Result.Nodes[Tail].Next != kNoIndex)
        return false;
      Result.Nodes[Tail].Next = Index;
    }
    Tail = Index;
    return true;
  }

  bool validateLoop(size_t LoopRegion) {
    if (LoopRegion >= Program.High.Regions.size() ||
        ValidatedLoops.test(LoopRegion))
      return false;
    const Region &Loop = Program.High.Regions[LoopRegion];
    if (Loop.Kind != RegionKind::Loop || !Loop.Blocks.empty() ||
        !contains(Loop, Loop.HeaderBlock) ||
        DirectLoopBlocks[LoopRegion] > Loop.BlockCount ||
        NestedLoopBlocks[LoopRegion] >
            Loop.BlockCount - DirectLoopBlocks[LoopRegion])
      return false;
    const size_t EmittedCount =
        DirectLoopBlocks[LoopRegion] + NestedLoopBlocks[LoopRegion];
    if (EmittedCount != Loop.BlockCount)
      return false;
    ValidatedLoops.set(LoopRegion);
    if (!Loop.ParentRegion)
      return true;
    const size_t ParentRegion = *Loop.ParentRegion;
    if (ParentRegion >= Program.High.Regions.size() ||
        EmittedCount > Program.High.Regions[ParentRegion].BlockCount ||
        NestedLoopBlocks[ParentRegion] >
            Program.High.Regions[ParentRegion].BlockCount - EmittedCount)
      return false;
    NestedLoopBlocks[ParentRegion] += EmittedCount;
    return true;
  }

  bool buildLoop(const BuildTask &Task, const Region &Loop,
                 StructuredControlFlow &Result, std::vector<BuildTask> &Work,
                 size_t &Tail) {
    const size_t HeaderID = Task.Current;
    const BasicBlock *Header = block(HeaderID);
    const MedInstruction *Terminator = terminator(HeaderID);
    const auto Taken = successor(HeaderID, EdgeKind::BranchTaken);
    const auto Fallthrough = successor(HeaderID, EdgeKind::Fallthrough);
    if (!Header || !Terminator || !isConditional(Terminator->Op) || !Taken ||
        !Fallthrough || !contains(Loop, HeaderID))
      return false;
    const bool TakenInside = contains(Loop, *Taken);
    const bool FallthroughInside = contains(Loop, *Fallthrough);
    if (TakenInside == FallthroughInside)
      return false;
    const size_t BodyEntry = TakenInside ? *Taken : *Fallthrough;
    const size_t Exit = TakenInside ? *Fallthrough : *Taken;
    if (Task.Allowed && !contains(*Task.Allowed, Exit))
      return false;
    if (!markEmitted(HeaderID))
      return false;

    StructuredNode Node;
    Node.Kind = StructuredNodeKind::Loop;
    Node.Block = HeaderID;
    Node.ConditionTrueEntersBody = TakenInside;
    size_t NodeIndex = kNoIndex;
    if (!append(Result, Task, Tail, std::move(Node), NodeIndex))
      return false;

    Work.push_back(BuildTask::sequence(Exit, Task.Stop, Task.Allowed, NodeIndex,
                                       SequenceLink::Next, Task.Depth));
    Work.push_back(BuildTask::validate(LoopRegions[HeaderID]));
    Work.push_back(BuildTask::sequence(BodyEntry, HeaderID, &Loop, NodeIndex,
                                       SequenceLink::Body, Task.Depth + 1));
    return true;
  }

  bool buildIf(const BuildTask &Task, const Region &If,
               StructuredControlFlow &Result, std::vector<BuildTask> &Work,
               size_t &Tail) {
    const size_t HeaderID = Task.Current;
    const BasicBlock *Header = block(HeaderID);
    const MedInstruction *Terminator = terminator(HeaderID);
    const auto Taken = successor(HeaderID, EdgeKind::BranchTaken);
    const auto Fallthrough = successor(HeaderID, EdgeKind::Fallthrough);
    if (!If.Blocks.empty() || !Header || !Terminator ||
        !isConditional(Terminator->Op) || !Taken || !Fallthrough)
      return false;
    if (Task.Allowed && If.ExitBlock && *If.ExitBlock != HeaderID &&
        !contains(*Task.Allowed, *If.ExitBlock))
      return false;
    if (!markEmitted(HeaderID))
      return false;

    StructuredNode Node;
    Node.Kind = StructuredNodeKind::If;
    Node.Block = HeaderID;
    size_t NodeIndex = kNoIndex;
    if (!append(Result, Task, Tail, std::move(Node), NodeIndex))
      return false;

    if (If.ExitBlock)
      Work.push_back(BuildTask::sequence(*If.ExitBlock, Task.Stop, Task.Allowed,
                                         NodeIndex, SequenceLink::Next,
                                         Task.Depth));
    Work.push_back(BuildTask::sequence(*Fallthrough, If.ExitBlock, Task.Allowed,
                                       NodeIndex, SequenceLink::Alternative,
                                       Task.Depth + 1));
    Work.push_back(BuildTask::sequence(*Taken, If.ExitBlock, Task.Allowed,
                                       NodeIndex, SequenceLink::Body,
                                       Task.Depth + 1));
    return true;
  }

  bool buildSequence(const BuildTask &Task, StructuredControlFlow &Result,
                     std::vector<BuildTask> &Work) {
    size_t Current = Task.Current;
    size_t Tail = kNoIndex;
    for (;;) {
      if (Task.Stop && Current == *Task.Stop)
        return true;
      const BasicBlock *Block = block(Current);
      if (!Block || !Block->Reachable ||
          (Task.Allowed && !contains(*Task.Allowed, Current)))
        return false;

      if (const Region *Loop = region(Current, RegionKind::Loop)) {
        BuildTask CurrentTask = Task;
        CurrentTask.Current = Current;
        return buildLoop(CurrentTask, *Loop, Result, Work, Tail);
      }

      const MedInstruction *Terminator = terminator(Current);
      if (!Terminator)
        return false;
      if (isConditional(Terminator->Op)) {
        const Region *If = region(Current, RegionKind::If);
        if (!If)
          return false;
        BuildTask CurrentTask = Task;
        CurrentTask.Current = Current;
        return buildIf(CurrentTask, *If, Result, Work, Tail);
      }

      if (!markEmitted(Current))
        return false;
      StructuredNode Node;
      Node.Kind = StructuredNodeKind::Block;
      Node.Block = Current;
      size_t NodeIndex = kNoIndex;
      if (!append(Result, Task, Tail, std::move(Node), NodeIndex))
        return false;

      if (Terminator->Op == Operation::Exit)
        return Block->Successors.empty();
      if (Block->Successors.size() != 1)
        return false;
      Current = Block->Successors.front();
    }
  }

  const SBFProgram &Program;
  std::vector<const MedInstruction *> Terminators;
  std::vector<size_t> IfRegions;
  std::vector<size_t> LoopRegions;
  std::vector<size_t> TakenSuccessors;
  std::vector<size_t> FallthroughSuccessors;
  llvm::BitVector Emitted;
  std::vector<size_t> DirectLoopBlocks;
  std::vector<size_t> NestedLoopBlocks;
  llvm::BitVector ValidatedLoops;
  bool IndicesValid = true;
};

} // namespace

std::optional<StructuredControlFlow>
buildStructuredControlFlow(const SBFProgram &Program) {
  return Structurer(Program).build();
}

} // namespace neverd::sbf

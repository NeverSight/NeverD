//===- SBFStructuredCFG.cpp - SBF reducible control-flow plan -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFStructuredCFG.h"

#include <map>
#include <set>

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
  explicit Structurer(const SBFProgram &Program) : Program(Program) {
    for (const MedInstruction &Instruction : Program.Med.Instructions)
      BySlot[Instruction.Slot] = &Instruction;
  }

  std::optional<StructuredControlFlow> build() {
    if (Program.Low.Blocks.empty())
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
    if (!buildSequence(Entry->ID, std::nullopt, nullptr, Result.Body))
      return std::nullopt;
    for (const BasicBlock &Block : Program.Low.Blocks)
      if (Block.Reachable && !Emitted.contains(Block.ID))
        return std::nullopt;
    return Result;
  }

private:
  const BasicBlock *block(size_t ID) const {
    if (ID >= Program.Low.Blocks.size() || Program.Low.Blocks[ID].ID != ID)
      return nullptr;
    return &Program.Low.Blocks[ID];
  }

  const MedInstruction *terminator(const BasicBlock &Block) const {
    const MedInstruction *Last = nullptr;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      auto It = BySlot.find(Slot);
      if (It != BySlot.end())
        Last = It->second;
    }
    return Last;
  }

  const Region *region(size_t Header, RegionKind Kind) const {
    const Region *Found = nullptr;
    for (const Region &Candidate : Program.High.Regions) {
      if (Candidate.HeaderBlock != Header || Candidate.Kind != Kind)
        continue;
      if (Found)
        return nullptr;
      Found = &Candidate;
    }
    return Found;
  }

  std::optional<size_t> successor(size_t BlockID, EdgeKind Kind) const {
    std::optional<size_t> Result;
    for (const CFGEdge &Edge : Program.Low.Edges) {
      if (Edge.From != BlockID || Edge.Kind != Kind || !Edge.To)
        continue;
      if (Result)
        return std::nullopt;
      Result = *Edge.To;
    }
    return Result;
  }

  bool buildLoop(size_t HeaderID, const Region &Loop,
                 const std::set<size_t> *Allowed,
                 std::vector<StructuredNode> &Output, size_t &Next) {
    const BasicBlock *Header = block(HeaderID);
    const MedInstruction *Terminator = Header ? terminator(*Header) : nullptr;
    if (!Header || !Terminator || !isConditional(Terminator->Op))
      return false;
    const auto Taken = successor(HeaderID, EdgeKind::BranchTaken);
    const auto Fallthrough = successor(HeaderID, EdgeKind::Fallthrough);
    if (!Taken || !Fallthrough)
      return false;

    const std::set<size_t> Members(Loop.Blocks.begin(), Loop.Blocks.end());
    if (!Members.contains(HeaderID))
      return false;
    const bool TakenInside = Members.contains(*Taken);
    const bool FallthroughInside = Members.contains(*Fallthrough);
    if (TakenInside == FallthroughInside)
      return false;
    const size_t BodyEntry = TakenInside ? *Taken : *Fallthrough;
    const size_t Exit = TakenInside ? *Fallthrough : *Taken;
    if (Allowed && !Allowed->contains(Exit))
      return false;

    if (!Emitted.insert(HeaderID).second)
      return false;
    StructuredNode Node;
    Node.Kind = StructuredNodeKind::Loop;
    Node.Block = HeaderID;
    Node.ConditionTrueEntersBody = TakenInside;
    if (!buildSequence(BodyEntry, HeaderID, &Members, Node.Body))
      return false;
    for (size_t Member : Members)
      if (Member != HeaderID) {
        const BasicBlock *MemberBlock = block(Member);
        if (MemberBlock && MemberBlock->Reachable && !Emitted.contains(Member))
          return false;
      }
    Output.push_back(std::move(Node));
    Next = Exit;
    return true;
  }

  bool buildIf(size_t HeaderID, const Region &If,
               const std::set<size_t> *Allowed,
               std::vector<StructuredNode> &Output,
               std::optional<size_t> &Next) {
    const BasicBlock *Header = block(HeaderID);
    const MedInstruction *Terminator = Header ? terminator(*Header) : nullptr;
    const auto Taken = successor(HeaderID, EdgeKind::BranchTaken);
    const auto Fallthrough = successor(HeaderID, EdgeKind::Fallthrough);
    if (!Header || !Terminator || !isConditional(Terminator->Op) || !Taken ||
        !Fallthrough)
      return false;
    if (Allowed && If.ExitBlock && *If.ExitBlock != HeaderID &&
        !Allowed->contains(*If.ExitBlock))
      return false;
    if (!Emitted.insert(HeaderID).second)
      return false;

    StructuredNode Node;
    Node.Kind = StructuredNodeKind::If;
    Node.Block = HeaderID;
    if (!buildSequence(*Taken, If.ExitBlock, Allowed, Node.Body) ||
        !buildSequence(*Fallthrough, If.ExitBlock, Allowed, Node.Alternative))
      return false;
    Output.push_back(std::move(Node));
    Next = If.ExitBlock;
    return true;
  }

  bool buildSequence(size_t Current, std::optional<size_t> Stop,
                     const std::set<size_t> *Allowed,
                     std::vector<StructuredNode> &Output) {
    for (;;) {
      if (Stop && Current == *Stop)
        return true;
      const BasicBlock *Block = block(Current);
      if (!Block || !Block->Reachable ||
          (Allowed && !Allowed->contains(Current)))
        return false;

      if (const Region *Loop = region(Current, RegionKind::Loop)) {
        size_t Next = 0;
        if (!buildLoop(Current, *Loop, Allowed, Output, Next))
          return false;
        Current = Next;
        continue;
      }

      const MedInstruction *Terminator = terminator(*Block);
      if (!Terminator)
        return false;
      if (isConditional(Terminator->Op)) {
        const Region *If = region(Current, RegionKind::If);
        std::optional<size_t> Next;
        if (!If || !buildIf(Current, *If, Allowed, Output, Next))
          return false;
        if (!Next)
          return true;
        Current = *Next;
        continue;
      }

      if (!Emitted.insert(Current).second)
        return false;
      StructuredNode Node;
      Node.Kind = StructuredNodeKind::Block;
      Node.Block = Current;
      Output.push_back(std::move(Node));

      if (Terminator->Op == Operation::Exit)
        return Block->Successors.empty();
      if (Block->Successors.size() != 1)
        return false;
      Current = Block->Successors.front();
    }
  }

  const SBFProgram &Program;
  std::map<size_t, const MedInstruction *> BySlot;
  std::set<size_t> Emitted;
};

} // namespace

std::optional<StructuredControlFlow>
buildStructuredControlFlow(const SBFProgram &Program) {
  return Structurer(Program).build();
}

} // namespace neverd::sbf

//===- SBFAnalyzerRegions.cpp - SBF dominators, regions, and HighIR -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds the dominator and post-dominator trees of the recovered CFG, reads
/// the loop and two-armed regions out of them, and assembles the HighIR view
/// of functions, calls, syscalls, and read-only strings.
///
//===----------------------------------------------------------------------===//

#include "SBFAnalyzerDetail.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

constexpr size_t kNoBlock = std::numeric_limits<size_t>::max();

// Immediate-dominator tree. The previous representation kept one
// std::set<size_t> of (post)dominators per block, which needs O(blocks^2)
// set nodes — real programs with tens of thousands of blocks exhaust the
// host's memory. This is the classic iterative algorithm over reverse
// postorder (Cooper, Harvey, Kennedy, "A Simple, Fast Dominance
// Algorithm"), which is O(blocks) in space and near-linear in practice.
struct DominatorTree {
  size_t Root = kNoBlock;
  std::vector<size_t> IDom;   // immediate dominator, kNoBlock when unreachable
  std::vector<size_t> Depth;  // depth in the dominator tree
  std::vector<size_t> RPONum; // block -> reverse postorder number, kNoBlock
                              // when unreachable from Root
};

template <typename SuccessorsFn, typename PredecessorsFn>
DominatorTree buildDominatorTree(size_t Count, size_t Root,
                                 SuccessorsFn &&Successors,
                                 PredecessorsFn &&Predecessors) {
  DominatorTree Tree;
  Tree.Root = Root;
  Tree.IDom.assign(Count, kNoBlock);
  Tree.Depth.assign(Count, 0);
  Tree.RPONum.assign(Count, kNoBlock);

  // Iterative postorder walk from Root; reverse postorder numbers entry = 0.
  std::vector<size_t> Postorder;
  Postorder.reserve(Count);
  std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
  std::vector<std::vector<size_t>> Adjacent(Count);
  while (!Stack.empty()) {
    auto &[Node, Next] = Stack.back();
    const std::vector<size_t> &Succs = Adjacent[Node].empty() && Next == 0
                                           ? (Adjacent[Node] = Successors(Node))
                                           : Adjacent[Node];
    if (Next < Succs.size()) {
      const size_t Succ = Succs[Next++];
      if (Tree.RPONum[Succ] == kNoBlock && Succ != Root) {
        Tree.RPONum[Succ] = 0; // mark visited
        Stack.push_back({Succ, 0});
      }
      continue;
    }
    Postorder.push_back(Node);
    Stack.pop_back();
  }
  for (size_t I = 0; I < Postorder.size(); ++I)
    Tree.RPONum[Postorder[Postorder.size() - 1 - I]] = I;

  auto Intersect = [&](size_t B1, size_t B2) {
    while (B1 != B2) {
      while (Tree.RPONum[B1] > Tree.RPONum[B2])
        B1 = Tree.IDom[B1];
      while (Tree.RPONum[B2] > Tree.RPONum[B1])
        B2 = Tree.IDom[B2];
    }
    return B1;
  };

  Tree.IDom[Root] = Root;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t I = Postorder.size(); I-- > 0;) {
      const size_t ID = Postorder[I];
      if (ID == Root)
        continue;
      size_t NewIDom = kNoBlock;
      for (size_t Pred : Predecessors(ID)) {
        if (Tree.RPONum[Pred] == kNoBlock || Tree.IDom[Pred] == kNoBlock)
          continue;
        NewIDom = NewIDom == kNoBlock ? Pred : Intersect(NewIDom, Pred);
      }
      if (NewIDom != kNoBlock && NewIDom != Tree.IDom[ID]) {
        Tree.IDom[ID] = NewIDom;
        Changed = true;
      }
    }
  }
  // Depths in RPO order: the immediate dominator always has a smaller
  // reverse postorder number than the block itself.
  std::vector<size_t> RPO(Postorder.size());
  for (size_t I = 0; I < Postorder.size(); ++I)
    RPO[Tree.RPONum[Postorder[I]]] = Postorder[I];
  for (size_t ID : RPO)
    if (ID != Root)
      Tree.Depth[ID] = Tree.Depth[Tree.IDom[ID]] + 1;
  return Tree;
}

// Whether A dominates B under Tree (both must be reachable from the root).
bool dominates(const DominatorTree &Tree, size_t A, size_t B) {
  if (A >= Tree.IDom.size() || B >= Tree.IDom.size())
    return false;
  if (Tree.RPONum[A] == kNoBlock || Tree.RPONum[B] == kNoBlock)
    return false;
  size_t Cur = B;
  while (Tree.Depth[Cur] >= Tree.Depth[A]) {
    if (Cur == A)
      return true;
    if (Cur == Tree.Root)
      return false;
    Cur = Tree.IDom[Cur];
  }
  return false;
}

// Deepest common ancestor of A and B in Tree, i.e. the (post)dominator
// candidate with the most (post)dominators. A node that is unreachable from
// the root stands for the full node set, so the other side wins outright.
std::optional<size_t> nearestCommonDominator(const DominatorTree &Tree,
                                             size_t A, size_t B) {
  const bool AReachable = A < Tree.IDom.size() && Tree.RPONum[A] != kNoBlock;
  const bool BReachable = B < Tree.IDom.size() && Tree.RPONum[B] != kNoBlock;
  if (!AReachable && !BReachable)
    return std::nullopt;
  if (!AReachable)
    return B;
  if (!BReachable)
    return A;
  while (Tree.Depth[A] > Tree.Depth[B]) {
    A = Tree.IDom[A];
    if (A == kNoBlock)
      return std::nullopt;
  }
  while (Tree.Depth[B] > Tree.Depth[A]) {
    B = Tree.IDom[B];
    if (B == kNoBlock)
      return std::nullopt;
  }
  while (A != B) {
    if (A == Tree.Root || B == Tree.Root)
      return Tree.Root;
    A = Tree.IDom[A];
    B = Tree.IDom[B];
    if (A == kNoBlock || B == kNoBlock)
      return std::nullopt;
  }
  return A;
}

void recoverRegions(SBFProgram &Program) {
  const size_t Count = Program.Low.Blocks.size();
  if (Count == 0)
    return;
  std::set<size_t> Reachable;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Block.Reachable)
      Reachable.insert(Block.ID);
  size_t EntryBlock = 0;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Program.Low.EntrySlot >= Block.StartSlot &&
        Program.Low.EntrySlot < Block.EndSlot) {
      EntryBlock = Block.ID;
      break;
    }

  auto CFGSuccessors = [&](size_t ID) {
    std::vector<size_t> Result;
    for (size_t Successor : Program.Low.Blocks[ID].Successors)
      if (Program.Low.Blocks[Successor].Reachable)
        Result.push_back(Successor);
    return Result;
  };
  auto CFGPredecessors = [&](size_t ID) {
    std::vector<size_t> Result;
    for (size_t Pred : Program.Low.Blocks[ID].Predecessors)
      if (Program.Low.Blocks[Pred].Reachable)
        Result.push_back(Pred);
    return Result;
  };
  const DominatorTree Dominators =
      buildDominatorTree(Count, EntryBlock, CFGSuccessors, CFGPredecessors);

  // Post-dominators are dominators of the reversed graph. A virtual root
  // (index Count) links every exit block so multiple exits share one tree.
  std::set<size_t> Exits;
  for (const BasicBlock &Block : Program.Low.Blocks) {
    if (!Block.Reachable)
      continue;
    const bool HasReachableSuccessor = std::any_of(
        Block.Successors.begin(), Block.Successors.end(),
        [&](size_t Successor) { return Reachable.contains(Successor); });
    if (!HasReachableSuccessor)
      Exits.insert(Block.ID);
  }
  const size_t VirtualRoot = Count;
  auto RevSuccessors = [&](size_t ID) {
    if (ID == VirtualRoot)
      return std::vector<size_t>(Exits.begin(), Exits.end());
    return CFGPredecessors(ID);
  };
  auto RevPredecessors = [&](size_t ID) {
    std::vector<size_t> Result;
    if (ID == VirtualRoot)
      return Result;
    Result = CFGSuccessors(ID);
    if (Exits.contains(ID))
      Result.push_back(VirtualRoot); // virtual root -> exit edge
    return Result;
  };
  const DominatorTree PostDominators = buildDominatorTree(
      Count + 1, VirtualRoot, RevSuccessors, RevPredecessors);

  std::set<std::pair<size_t, size_t>> SeenLoops;
  for (const BasicBlock &Source : Program.Low.Blocks) {
    if (!Source.Reachable)
      continue;
    for (size_t Target : Source.Successors) {
      if (!dominates(Dominators, Target, Source.ID) ||
          !SeenLoops.insert({Target, Source.ID}).second)
        continue;
      std::set<size_t> Loop{Target, Source.ID};
      std::deque<size_t> Work;
      if (Source.ID != Target)
        Work.push_back(Source.ID);
      while (!Work.empty()) {
        const size_t ID = Work.front();
        Work.pop_front();
        for (size_t Pred : Program.Low.Blocks[ID].Predecessors)
          if (Program.Low.Blocks[Pred].Reachable && Loop.insert(Pred).second &&
              Pred != Target)
            Work.push_back(Pred);
      }
      Region Region;
      Region.Kind = RegionKind::Loop;
      Region.HeaderBlock = Target;
      Region.Blocks.assign(Loop.begin(), Loop.end());
      for (size_t ID : Loop)
        for (size_t Successor : Program.Low.Blocks[ID].Successors)
          if (!Loop.contains(Successor)) {
            Region.ExitBlock = Successor;
            break;
          }
      Program.High.Regions.push_back(std::move(Region));
    }
  }

  // Having two successors does not mean a block chose between them. A block
  // ending in an internal call has two as well — the callee and the
  // instruction after the call — and the callee is not an alternative to the
  // fallthrough: both run, one after the other. Reading that pair as a
  // two-armed region invents a branch the program does not contain and puts
  // the entire callee inside one of its arms.
  //
  // The edge kinds say which pair is a choice, so they are what this reads.
  // The successor list cannot answer it: it is deduplicated and carries no
  // kind.
  struct ConditionalArms {
    std::optional<size_t> Taken;
    std::optional<size_t> Fallthrough;
    bool Disqualified = false;
  };
  std::vector<ConditionalArms> Arms(Program.Low.Blocks.size());
  for (const CFGEdge &Edge : Program.Low.Edges) {
    ConditionalArms &Block = Arms[Edge.From];
    std::optional<size_t> *Arm =
        Edge.Kind == EdgeKind::BranchTaken   ? &Block.Taken
        : Edge.Kind == EdgeKind::Fallthrough ? &Block.Fallthrough
                                             : nullptr;
    if (!Arm || !Edge.To || *Arm)
      Block.Disqualified = true;
    else
      *Arm = *Edge.To;
  }

  for (const BasicBlock &Block : Program.Low.Blocks) {
    const ConditionalArms &Choice = Arms[Block.ID];
    if (Choice.Disqualified || !Choice.Taken || !Choice.Fallthrough ||
        *Choice.Taken == *Choice.Fallthrough)
      continue;
    const size_t Left = *Choice.Taken;
    const size_t Right = *Choice.Fallthrough;
    std::optional<size_t> Join =
        nearestCommonDominator(PostDominators, Left, Right);
    if (Join && *Join == VirtualRoot)
      Join = std::nullopt; // the branches reach disjoint exits
    Region Region;
    Region.Kind = RegionKind::If;
    Region.HeaderBlock = Block.ID;
    Region.ExitBlock = Join;
    std::set<size_t> Members{Block.ID};
    std::deque<size_t> Work{Left, Right};
    while (!Work.empty()) {
      const size_t ID = Work.front();
      Work.pop_front();
      if ((Join && ID == *Join) || !Members.insert(ID).second)
        continue;
      for (size_t Successor : Program.Low.Blocks[ID].Successors)
        if (Reachable.contains(Successor))
          Work.push_back(Successor);
    }
    Region.Blocks.assign(Members.begin(), Members.end());
    Program.High.Regions.push_back(std::move(Region));
  }
}

} // namespace

namespace analyzer_detail {

void recoverHighIR(const BinaryImage &Image, SBFProgram &Program) {
  std::set<size_t> Entries{Program.Low.EntrySlot};
  for (const LowInstruction &Instruction : Program.Low.Instructions)
    if (Instruction.CallTarget)
      Entries.insert(*Instruction.CallTarget);
  for (const Symbol &Symbol : Image.Symbols) {
    if (!Symbol.IsFunc)
      continue;
    const auto Slot = addressToSlot(Program.Image, Symbol.Addr);
    if (!Slot || *Slot >= Program.Low.Instructions.size())
      continue;
    // A symbol table can name the second half of a wide load. Nothing rejects
    // it at link time, but a function starting there begins in the middle of
    // an instruction: the first thing decoded is the tail of the load, and
    // every instruction after it is read four bytes out of phase. A recovered
    // body built from that is fiction, so the symbol is reported and dropped
    // rather than followed.
    if (Program.Low.Instructions[*Slot].IsContinuation) {
      Program.Low.Diagnostics.push_back(
          {DiagnosticSeverity::Warning, *Slot, Symbol.Addr,
           "function symbol '" + Symbol.Name +
               "' starts inside a wide load and cannot begin a function"});
      continue;
    }
    Entries.insert(*Slot);
  }

  // The slot→block map and the call-free successor adjacency are identical
  // for every entry. Build them once; rebuilding them per entry (and
  // rescanning every edge per visited block) is quadratic and does not
  // terminate in reasonable time on large production programs.
  std::vector<size_t> SlotToBlock(Program.Low.Instructions.size(), kNoBlock);
  for (const BasicBlock &Block : Program.Low.Blocks)
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      SlotToBlock[Slot] = Block.ID;
  std::vector<std::vector<size_t>> CallFreeSuccessors(
      Program.Low.Blocks.size());
  for (const CFGEdge &Edge : Program.Low.Edges)
    if (Edge.To && Edge.Kind != EdgeKind::Call)
      CallFreeSuccessors[Edge.From].push_back(*Edge.To);
  std::vector<size_t> VisitEpoch(Program.Low.Blocks.size());
  size_t CurrentEpoch = 0;

  for (size_t EntrySlot : Entries) {
    Function Function;
    Function.EntrySlot = EntrySlot;
    Function.Address = Program.Low.TextAddress + EntrySlot * kInstructionSize;
    if (const Symbol *Symbol = findFunctionSymbol(Image, Function.Address))
      Function.Name = Symbol->Name;
    else if (EntrySlot == Program.Low.EntrySlot)
      Function.Name = kEntrySymbolName.str();
    else
      Function.Name = syntheticFunctionName(Function.Address);

    const size_t EntryBlock =
        EntrySlot < SlotToBlock.size() ? SlotToBlock[EntrySlot] : kNoBlock;
    if (EntryBlock != kNoBlock) {
      if (CurrentEpoch == std::numeric_limits<size_t>::max()) {
        std::fill(VisitEpoch.begin(), VisitEpoch.end(), 0);
        CurrentEpoch = 1;
      } else {
        ++CurrentEpoch;
      }
      std::deque<size_t> Work{EntryBlock};
      while (!Work.empty()) {
        size_t ID = Work.front();
        Work.pop_front();
        if (VisitEpoch[ID] == CurrentEpoch)
          continue;
        VisitEpoch[ID] = CurrentEpoch;
        Function.Blocks.push_back(ID);
        for (size_t To : CallFreeSuccessors[ID])
          Work.push_back(To);
      }
    }
    Program.High.Functions.push_back(std::move(Function));
  }
  std::sort(Program.High.Functions.begin(), Program.High.Functions.end(),
            [](const Function &L, const Function &R) {
              return L.EntrySlot < R.EntrySlot;
            });

  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    if (Instruction.Info &&
        (Instruction.Info->Op == Operation::Load ||
         Instruction.Info->Op == Operation::Store) &&
        (Instruction.Src == kFirstArgumentRegister ||
         Instruction.Dst == kFirstArgumentRegister))
      Program.High.UsesAccounts = true;
    if (Instruction.Call == CallKind::None)
      continue;
    Program.High.Calls.push_back({Instruction.Slot, Instruction.CallTarget,
                                  Instruction.Call, Instruction.ResolvedName});
    if (Instruction.Call == CallKind::Syscall) {
      Program.High.Syscalls.push_back(
          {Instruction.Slot, Instruction.SyscallHash, Instruction.Syscall});
      if (Instruction.Syscall &&
          Instruction.Syscall->Category == SyscallCategory::CPI)
        Program.High.UsesCPI = true;
    }
  }

  for (const ProgramRegion &Region : Program.ExecutableImage.regions()) {
    for (const ProgramSectionSpan &Section : Region.Sections) {
      if (Section.Executable || Section.Offset > Region.Bytes.size() ||
          Section.Size > Region.Bytes.size() - Section.Offset)
        continue;
      const llvm::ArrayRef<uint8_t> Bytes =
          llvm::ArrayRef(Region.Bytes).slice(Section.Offset, Section.Size);
      size_t Start = 0;
      while (Start < Bytes.size()) {
        while (Start < Bytes.size() &&
               !llvm::isPrint(static_cast<char>(Bytes[Start])))
          ++Start;
        size_t End = Start;
        while (End < Bytes.size() &&
               llvm::isPrint(static_cast<char>(Bytes[End])))
          ++End;
        if (End - Start >= 4)
          Program.High.Strings.push_back(
              {Region.Address + Section.Offset + Start,
               std::string(reinterpret_cast<const char *>(Bytes.data() + Start),
                           End - Start)});
        Start = End + (End < Bytes.size());
      }
    }
  }
  recoverRegions(Program);
}

} // namespace analyzer_detail
} // namespace neverd::sbf

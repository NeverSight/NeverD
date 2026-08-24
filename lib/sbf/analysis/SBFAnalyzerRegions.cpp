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
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cassert>
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
constexpr size_t kMaximumIntraproceduralSuccessorCount = 2;

// Exit summaries retain count, sum, and squared sum so a unique loop exit is
// recognized without storing one target per enclosing loop.  A block has at
// most the taken and fallthrough intraprocedural successors, and deployable
// bytecode contains at most kMaxInstructions blocks.  Prove here that every
// intermediate fits the signed difference accumulator at the protocol limit.
static_assert(kMaxInstructions <= std::numeric_limits<int64_t>::max() /
                                      kMaxInstructions / kMaxInstructions /
                                      kMaximumIntraproceduralSuccessorCount,
              "loop exit summaries must fit at the deployable bytecode limit");

} // namespace

namespace analyzer_detail {

// Immediate-dominator tree. The previous representation kept one
// std::set<size_t> of (post)dominators per block, which needs O(blocks^2)
// set nodes — real programs with tens of thousands of blocks exhaust the
// host's memory. Construction below uses Lengauer-Tarjan; interval and
// heavy-light indices make its two hot query forms constant and logarithmic
// while retaining linear auxiliary storage.
template <typename SuccessorsFn, typename PredecessorsFn>
DominatorTree buildDominatorTreeImpl(size_t Count, size_t Root,
                                     SuccessorsFn &&Successors,
                                     PredecessorsFn &&Predecessors) {
  DominatorTree Tree;
  Tree.Root = Root;
  Tree.IDom.assign(Count, kNoBlock);
  Tree.Depth.assign(Count, 0);
  Tree.Preorder.assign(Count, kNoBlock);
  Tree.SubtreeEnd.assign(Count, kNoBlock);
  if (Root >= Count)
    return Tree;

  // Lengauer-Tarjan dominators. The DFS and union-find compression are both
  // iterative so a hostile one-million-block chain cannot overflow the host
  // stack. This avoids the quadratic worst case of repeatedly intersecting
  // a deep, changing immediate-dominator chain.
  std::vector<size_t> Vertex;
  Vertex.reserve(Count);
  {
    std::vector<size_t> DFSNum(Count);
    std::vector<size_t> Parent(Count, kNoBlock);
    std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
    DFSNum[Root] = 1;
    Vertex.push_back(Root);
    while (!Stack.empty()) {
      auto &[Node, Next] = Stack.back();
      const auto &Succs = Successors(Node);
      if (Next < Succs.size()) {
        const size_t Succ = Succs[Next++];
        if (Succ >= Count || DFSNum[Succ] != 0)
          continue;
        Parent[Succ] = Node;
        DFSNum[Succ] = Vertex.size() + 1;
        Vertex.push_back(Succ);
        Stack.push_back({Succ, 0});
        continue;
      }
      Stack.pop_back();
    }

    std::vector<size_t> Semi = DFSNum;
    std::vector<size_t> Label(Count, kNoBlock);
    std::vector<size_t> Ancestor(Count, kNoBlock);
    std::vector<size_t> BucketHead(Count, kNoBlock);
    std::vector<size_t> BucketNext(Count, kNoBlock);
    for (size_t Node : Vertex)
      Label[Node] = Node;

    auto Eval = [&](size_t Node) {
      if (Ancestor[Node] == kNoBlock)
        return Label[Node];
      llvm::SmallVector<size_t, 16> Path;
      size_t Cursor = Node;
      while (Ancestor[Cursor] != kNoBlock &&
             Ancestor[Ancestor[Cursor]] != kNoBlock) {
        Path.push_back(Cursor);
        Cursor = Ancestor[Cursor];
      }
      for (size_t I = Path.size(); I-- > 0;) {
        const size_t Current = Path[I];
        const size_t CurrentParent = Ancestor[Current];
        if (Semi[Label[CurrentParent]] < Semi[Label[Current]])
          Label[Current] = Label[CurrentParent];
        Ancestor[Current] = Ancestor[CurrentParent];
      }
      return Label[Node];
    };

    for (size_t I = Vertex.size(); I-- > 1;) {
      const size_t Node = Vertex[I];
      for (size_t Pred : Predecessors(Node)) {
        if (Pred >= Count || DFSNum[Pred] == 0)
          continue;
        const size_t Candidate = Eval(Pred);
        Semi[Node] = std::min(Semi[Node], Semi[Candidate]);
      }
      const size_t BucketRoot = Vertex[Semi[Node] - 1];
      BucketNext[Node] = BucketHead[BucketRoot];
      BucketHead[BucketRoot] = Node;
      const size_t NodeParent = Parent[Node];
      Ancestor[Node] = NodeParent;
      size_t Pending = BucketHead[NodeParent];
      while (Pending != kNoBlock) {
        const size_t NextPending = BucketNext[Pending];
        const size_t Candidate = Eval(Pending);
        Tree.IDom[Pending] =
            Semi[Candidate] < Semi[Pending] ? Candidate : NodeParent;
        Pending = NextPending;
      }
      BucketHead[NodeParent] = kNoBlock;
    }
    for (size_t I = 1; I < Vertex.size(); ++I) {
      const size_t Node = Vertex[I];
      const size_t SemiNode = Vertex[Semi[Node] - 1];
      if (Tree.IDom[Node] != SemiNode)
        Tree.IDom[Node] = Tree.IDom[Tree.IDom[Node]];
    }
  }
  Tree.IDom[Root] = Root;

  // Number the immediate-dominator tree once. Dominance is then the standard
  // subtree interval test instead of a walk proportional to CFG depth.
  std::vector<size_t> ChildOffsets(Count + 1);
  for (size_t ID : Vertex)
    if (ID != Root && Tree.IDom[ID] != kNoBlock)
      ++ChildOffsets[Tree.IDom[ID] + 1];
  for (size_t ID = 1; ID < ChildOffsets.size(); ++ID)
    ChildOffsets[ID] += ChildOffsets[ID - 1];
  std::vector<size_t> Children(Vertex.size() - 1);
  {
    std::vector<size_t> NextChild = ChildOffsets;
    for (size_t ID : Vertex)
      if (ID != Root && Tree.IDom[ID] != kNoBlock)
        Children[NextChild[Tree.IDom[ID]]++] = ID;
  }
  size_t NextPreorder = 0;
  std::vector<std::pair<size_t, size_t>> TreeStack{{Root, ChildOffsets[Root]}};
  Tree.Preorder[Root] = NextPreorder++;
  while (!TreeStack.empty()) {
    auto &[Node, NextChild] = TreeStack.back();
    if (NextChild < ChildOffsets[Node + 1]) {
      const size_t Child = Children[NextChild++];
      Tree.Preorder[Child] = NextPreorder++;
      Tree.Depth[Child] = Tree.Depth[Node] + 1;
      TreeStack.push_back({Child, ChildOffsets[Child]});
      continue;
    }
    Tree.SubtreeEnd[Node] = NextPreorder;
    TreeStack.pop_back();
  }

  // Heavy-light decomposition keeps nearest-common-dominator queries
  // logarithmic without a block-count-times-log(block-count) ancestor table.
  // Moving between chains always crosses a light edge, so there are at most
  // log2(reachable-blocks) such moves. This matters at the 10 MiB protocol
  // ceiling, where bytecode can contain more than a million basic blocks.
  Tree.ChainHead.assign(Count, kNoBlock);
  Tree.ChainHead[Root] = Root;
  std::vector<size_t> HeadWork{Root};
  while (!HeadWork.empty()) {
    const size_t Node = HeadWork.back();
    HeadWork.pop_back();
    size_t HeavyChild = kNoBlock;
    size_t HeavySize = 0;
    for (size_t Child : llvm::ArrayRef(Children).slice(
             ChildOffsets[Node], ChildOffsets[Node + 1] - ChildOffsets[Node])) {
      const size_t ChildSize = Tree.SubtreeEnd[Child] - Tree.Preorder[Child];
      if (ChildSize > HeavySize) {
        HeavyChild = Child;
        HeavySize = ChildSize;
      }
    }
    for (size_t Child : llvm::ArrayRef(Children).slice(
             ChildOffsets[Node], ChildOffsets[Node + 1] - ChildOffsets[Node])) {
      Tree.ChainHead[Child] =
          Child == HeavyChild ? Tree.ChainHead[Node] : Child;
      HeadWork.push_back(Child);
    }
  }
  return Tree;
}

DominatorTree
buildDominatorTree(llvm::ArrayRef<std::vector<size_t>> Successors,
                   llvm::ArrayRef<std::vector<size_t>> Predecessors,
                   size_t Root) {
  assert(Successors.size() == Predecessors.size() &&
         "dominator graph directions must have equal vertex counts");
  auto SuccessorRange = [&](size_t Node) -> const std::vector<size_t> & {
    return Successors[Node];
  };
  auto PredecessorRange = [&](size_t Node) -> const std::vector<size_t> & {
    return Predecessors[Node];
  };
  return buildDominatorTreeImpl(Successors.size(), Root, SuccessorRange,
                                PredecessorRange);
}

// Whether A dominates B under Tree (both must be reachable from the root).
bool dominates(const DominatorTree &Tree, size_t A, size_t B) {
  if (A >= Tree.Preorder.size() || B >= Tree.Preorder.size())
    return false;
  if (Tree.Preorder[A] == kNoBlock || Tree.Preorder[B] == kNoBlock)
    return false;
  return Tree.Preorder[A] <= Tree.Preorder[B] &&
         Tree.Preorder[B] < Tree.SubtreeEnd[A];
}

// Deepest common ancestor of A and B in Tree, i.e. the (post)dominator
// candidate with the most (post)dominators. A node that is unreachable from
// the root stands for the full node set, so the other side wins outright.
std::optional<size_t> nearestCommonDominator(const DominatorTree &Tree,
                                             size_t A, size_t B) {
  const bool AReachable = A < Tree.IDom.size() && Tree.Preorder[A] != kNoBlock;
  const bool BReachable = B < Tree.IDom.size() && Tree.Preorder[B] != kNoBlock;
  if (!AReachable && !BReachable)
    return std::nullopt;
  if (!AReachable)
    return B;
  if (!BReachable)
    return A;
  while (Tree.ChainHead[A] != Tree.ChainHead[B]) {
    const size_t AHead = Tree.ChainHead[A];
    const size_t BHead = Tree.ChainHead[B];
    if (Tree.Depth[AHead] > Tree.Depth[BHead])
      A = Tree.IDom[AHead];
    else
      B = Tree.IDom[BHead];
  }
  return Tree.Depth[A] < Tree.Depth[B] ? A : B;
}

} // namespace analyzer_detail

namespace {

using analyzer_detail::buildDominatorTree;
using analyzer_detail::dominates;
using analyzer_detail::DominatorTree;
using analyzer_detail::nearestCommonDominator;

struct IntraproceduralEdge {
  size_t To = kNoBlock;
  EdgeKind Kind = EdgeKind::Invalid;
};

struct FlatEdge {
  size_t From = kNoBlock;
  size_t To = kNoBlock;
};

struct FlatAdjacency {
  std::vector<size_t> Offsets;
  std::vector<size_t> Targets;

  llvm::ArrayRef<size_t> operator[](size_t Node) const {
    if (Node + 1 >= Offsets.size())
      return {};
    return llvm::ArrayRef(Targets).slice(Offsets[Node],
                                         Offsets[Node + 1] - Offsets[Node]);
  }
};

FlatAdjacency buildFlatAdjacency(size_t Count, llvm::ArrayRef<FlatEdge> Edges,
                                 bool Reverse = false) {
  FlatAdjacency Result;
  Result.Offsets.assign(Count + 1, 0);
  for (const FlatEdge &Edge : Edges) {
    const size_t From = Reverse ? Edge.To : Edge.From;
    if (From < Count)
      ++Result.Offsets[From + 1];
  }
  for (size_t ID = 1; ID < Result.Offsets.size(); ++ID)
    Result.Offsets[ID] += Result.Offsets[ID - 1];
  Result.Targets.resize(Result.Offsets.back());
  std::vector<size_t> Next = Result.Offsets;
  for (const FlatEdge &Edge : Edges) {
    const size_t From = Reverse ? Edge.To : Edge.From;
    const size_t To = Reverse ? Edge.From : Edge.To;
    if (From < Count)
      Result.Targets[Next[From]++] = To;
  }
  return Result;
}

bool isAcyclic(const FlatAdjacency &Successors,
               const FlatAdjacency &Predecessors) {
  if (Successors.Offsets.size() != Predecessors.Offsets.size())
    return false;
  const size_t Count =
      Successors.Offsets.empty() ? 0 : Successors.Offsets.size() - 1;
  std::vector<size_t> PendingPredecessors(Count);
  std::vector<size_t> Ready;
  Ready.reserve(Count);
  for (size_t Block = 0; Block < Count; ++Block) {
    PendingPredecessors[Block] = Predecessors[Block].size();
    if (PendingPredecessors[Block] == 0)
      Ready.push_back(Block);
  }
  size_t Visited = 0;
  while (!Ready.empty()) {
    const size_t Block = Ready.back();
    Ready.pop_back();
    ++Visited;
    for (size_t Successor : Successors[Block]) {
      assert(PendingPredecessors[Successor] != 0 &&
             "topological predecessor count underflow");
      if (--PendingPredecessors[Successor] == 0)
        Ready.push_back(Successor);
    }
  }
  return Visited == Count;
}

struct IntraproceduralEdgeIndex {
  std::vector<size_t> Offsets;
  std::vector<IntraproceduralEdge> Edges;

  llvm::ArrayRef<IntraproceduralEdge> outgoing(size_t Block) const {
    if (Block + 1 >= Offsets.size())
      return {};
    return llvm::ArrayRef(Edges).slice(Offsets[Block],
                                       Offsets[Block + 1] - Offsets[Block]);
  }
};

IntraproceduralEdgeIndex buildIntraproceduralEdgeIndex(const LowIR &Low) {
  IntraproceduralEdgeIndex Result;
  const size_t Count = Low.Blocks.size();
  Result.Offsets.assign(Count + 1, 0);
  for (const CFGEdge &Edge : Low.Edges)
    if (Edge.To && Edge.From < Count && *Edge.To < Count &&
        getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
      ++Result.Offsets[Edge.From + 1];
  for (size_t ID = 1; ID < Result.Offsets.size(); ++ID)
    Result.Offsets[ID] += Result.Offsets[ID - 1];
  Result.Edges.resize(Result.Offsets.back());
  std::vector<size_t> Next = Result.Offsets;
  for (const CFGEdge &Edge : Low.Edges)
    if (Edge.To && Edge.From < Count && *Edge.To < Count &&
        getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
      Result.Edges[Next[Edge.From]++] = {*Edge.To, Edge.Kind};
  return Result;
}

struct ConditionalArms {
  size_t Taken = kNoBlock;
  size_t Fallthrough = kNoBlock;
  bool Disqualified = false;
};

struct CompactLoop {
  size_t Header = kNoBlock;
  size_t Parent = kNoBlock;
  size_t Preorder = 0;
  size_t SubtreeEnd = 0;
  size_t Depth = 0;
  size_t ChainHead = kNoBlock;
  size_t DirectBlockCount = 0;
  size_t BlockCount = 0;
  int64_t ExitEdgeCount = 0;
  int64_t ExitTargetSum = 0;
  int64_t ExitTargetSquareSum = 0;
};

size_t findRepresentative(std::vector<size_t> &Representatives, size_t ID) {
  size_t Root = ID;
  while (Representatives[Root] != Root)
    Root = Representatives[Root];
  while (Representatives[ID] != ID) {
    const size_t Parent = Representatives[ID];
    Representatives[ID] = Root;
    ID = Parent;
  }
  return Root;
}

void recoverRegions(SBFProgram &Program) {
  const size_t GlobalCount = Program.Low.Blocks.size();
  Program.High.Regions.clear();
  Program.High.LoopLatches.clear();
  Program.High.BlockLoops.assign(GlobalCount, HighIR::NoRegion);
  if (GlobalCount == 0)
    return;

  // Calls belong to the interprocedural call graph, not a function's CFG.
  // A flat outgoing index projects each uniquely owned function in O(B + E)
  // without one host allocation per global block.
  const IntraproceduralEdgeIndex Outgoing =
      buildIntraproceduralEdgeIndex(Program.Low);

  std::vector<size_t> GlobalToLocal(GlobalCount, kNoBlock);
  for (size_t FunctionIndex = 0; FunctionIndex < Program.High.Functions.size();
       ++FunctionIndex) {
    const Function &Function = Program.High.Functions[FunctionIndex];
    const llvm::ArrayRef<size_t> Blocks = Program.High.ownedBlocks(Function);
    if (Blocks.empty())
      continue;

    bool ValidFunction = true;
    size_t EntryBlock = kNoBlock;
    for (size_t LocalID = 0; LocalID < Blocks.size(); ++LocalID) {
      const size_t GlobalID = Blocks[LocalID];
      if (GlobalID >= GlobalCount ||
          Program.High.BlockOwners[GlobalID] != FunctionIndex) {
        ValidFunction = false;
        break;
      }
      GlobalToLocal[GlobalID] = LocalID;
      const BasicBlock &Block = Program.Low.Blocks[GlobalID];
      if (Function.EntrySlot >= Block.StartSlot &&
          Function.EntrySlot < Block.EndSlot)
        EntryBlock = LocalID;
    }
    if (!ValidFunction || EntryBlock == kNoBlock) {
      for (size_t GlobalID : Blocks)
        if (GlobalID < GlobalCount)
          GlobalToLocal[GlobalID] = kNoBlock;
      continue;
    }

    const size_t Count = Blocks.size();
    std::vector<FlatEdge> InternalEdges;
    std::vector<FlatEdge> ExternalEdges;
    std::vector<ConditionalArms> Arms(Count);
    for (size_t From = 0; From < Count; ++From) {
      for (const IntraproceduralEdge &Edge : Outgoing.outgoing(Blocks[From])) {
        const size_t To = GlobalToLocal[Edge.To];
        if (To == kNoBlock) {
          ExternalEdges.push_back({From, Edge.To});
          Arms[From].Disqualified = true;
          continue;
        }
        InternalEdges.push_back({From, To});
        size_t *Arm = Edge.Kind == EdgeKind::BranchTaken ? &Arms[From].Taken
                      : Edge.Kind == EdgeKind::Fallthrough
                          ? &Arms[From].Fallthrough
                          : nullptr;
        if (!Arm || *Arm != kNoBlock)
          Arms[From].Disqualified = true;
        else
          *Arm = To;
      }
    }
    const FlatAdjacency Successors = buildFlatAdjacency(Count, InternalEdges);
    const FlatAdjacency Predecessors =
        buildFlatAdjacency(Count, InternalEdges, true);
    const FlatAdjacency ExternalSuccessors =
        buildFlatAdjacency(Count, ExternalEdges);
    auto SuccessorRange = [&](size_t ID) { return Successors[ID]; };
    auto PredecessorRange = [&](size_t ID) { return Predecessors[ID]; };
    const bool HasConditionalCandidate =
        llvm::any_of(Arms, [](const ConditionalArms &Choice) {
          return !Choice.Disqualified && Choice.Taken != kNoBlock &&
                 Choice.Fallthrough != kNoBlock &&
                 Choice.Taken != Choice.Fallthrough;
        });
    const bool Acyclic = isAcyclic(Successors, Predecessors);
    if (Acyclic && !HasConditionalCandidate) {
      for (size_t GlobalID : Blocks)
        GlobalToLocal[GlobalID] = kNoBlock;
      continue;
    }

    std::optional<DominatorTree> Dominators;
    if (!Acyclic)
      Dominators.emplace(analyzer_detail::buildDominatorTreeImpl(
          Count, EntryBlock, SuccessorRange, PredecessorRange));

    // Post-dominators are dominators of the reversed graph. A virtual root
    // links every function-local exit so multiple exits share one tree. An
    // edge into an ambiguously owned shared tail is a local exit.
    const size_t VirtualRoot = Count;
    std::optional<DominatorTree> PostDominators;
    if (HasConditionalCandidate) {
      std::vector<size_t> Exits;
      for (size_t ID = 0; ID < Count; ++ID)
        if (Successors[ID].empty() || !ExternalSuccessors[ID].empty())
          Exits.push_back(ID);
      std::vector<FlatEdge> ReverseEdges;
      ReverseEdges.reserve(InternalEdges.size() + Exits.size());
      for (const FlatEdge &Edge : InternalEdges)
        ReverseEdges.push_back({Edge.To, Edge.From});
      for (size_t Exit : Exits)
        ReverseEdges.push_back({VirtualRoot, Exit});
      const FlatAdjacency ReverseSuccessors =
          buildFlatAdjacency(Count + 1, ReverseEdges);
      const FlatAdjacency ReversePredecessors =
          buildFlatAdjacency(Count + 1, ReverseEdges, true);
      auto ReverseSuccessorRange = [&](size_t ID) {
        return ReverseSuccessors[ID];
      };
      auto ReversePredecessorRange = [&](size_t ID) {
        return ReversePredecessors[ID];
      };
      PostDominators.emplace(analyzer_detail::buildDominatorTreeImpl(
          Count + 1, VirtualRoot, ReverseSuccessorRange,
          ReversePredecessorRange));
    }

    if (Dominators) {
      // Collect every backedge by header, then discover the natural-loop
      // forest in dominator-depth order. This is the compact half of LLVM
      // LoopInfo: each block maps only to its innermost loop and an outer
      // traversal skips an already discovered subloop. Inclusive per-loop
      // block vectors are deliberately not populated because nested loops
      // make them quadratic.
      std::vector<FlatEdge> BackedgeEdges;
      for (const FlatEdge &Edge : InternalEdges)
        if (dominates(*Dominators, Edge.To, Edge.From))
          BackedgeEdges.push_back({Edge.To, Edge.From});
      const FlatAdjacency Backedges = buildFlatAdjacency(Count, BackedgeEdges);
      size_t MaximumHeaderDepth = 0;
      size_t HeaderCount = 0;
      for (size_t Header = 0; Header < Count; ++Header)
        if (!Backedges[Header].empty()) {
          MaximumHeaderDepth =
              std::max(MaximumHeaderDepth, Dominators->Depth[Header]);
          ++HeaderCount;
        }
      std::vector<size_t> HeaderDepthOffsets(MaximumHeaderDepth + 2);
      for (size_t Header = 0; Header < Count; ++Header)
        if (!Backedges[Header].empty())
          ++HeaderDepthOffsets[Dominators->Depth[Header] + 1];
      for (size_t Depth = 1; Depth < HeaderDepthOffsets.size(); ++Depth)
        HeaderDepthOffsets[Depth] += HeaderDepthOffsets[Depth - 1];
      std::vector<size_t> HeaderOrder(HeaderCount);
      {
        std::vector<size_t> NextHeader = HeaderDepthOffsets;
        for (size_t Header = 0; Header < Count; ++Header)
          if (!Backedges[Header].empty())
            HeaderOrder[NextHeader[Dominators->Depth[Header]]++] = Header;
      }
      std::reverse(HeaderOrder.begin(), HeaderOrder.end());

      std::vector<CompactLoop> Loops;
      Loops.reserve(HeaderOrder.size());
      std::vector<size_t> Representatives;
      Representatives.reserve(HeaderOrder.size());
      std::vector<size_t> InnermostLoop(Count, kNoBlock);
      std::vector<size_t> ReverseWork;
      for (size_t Header : HeaderOrder) {
        const size_t LoopID = Loops.size();
        Loops.push_back({});
        Loops.back().Header = Header;
        Representatives.push_back(LoopID);
        const llvm::ArrayRef<size_t> Latches = Backedges[Header];
        ReverseWork.assign(Latches.begin(), Latches.end());
        while (!ReverseWork.empty()) {
          size_t Pred = ReverseWork.back();
          ReverseWork.pop_back();
          const size_t Mapped = InnermostLoop[Pred];
          if (Mapped == kNoBlock) {
            if (Dominators->Preorder[Pred] == kNoBlock)
              continue;
            InnermostLoop[Pred] = LoopID;
            if (Pred != Header)
              for (size_t Incoming : Predecessors[Pred])
                ReverseWork.push_back(Incoming);
            continue;
          }

          const size_t Subloop = findRepresentative(Representatives, Mapped);
          if (Subloop == LoopID)
            continue;
          Loops[Subloop].Parent = LoopID;
          const size_t SubloopHeader = Loops[Subloop].Header;
          for (size_t Incoming : Predecessors[SubloopHeader]) {
            const size_t IncomingLoop = InnermostLoop[Incoming];
            if (IncomingLoop == kNoBlock ||
                findRepresentative(Representatives, IncomingLoop) != Subloop)
              ReverseWork.push_back(Incoming);
          }
          Representatives[Subloop] = LoopID;
        }
      }

      std::vector<FlatEdge> LoopChildEdges;
      std::vector<size_t> LoopRoots;
      for (size_t LoopID = 0; LoopID < Loops.size(); ++LoopID) {
        if (Loops[LoopID].Parent == kNoBlock)
          LoopRoots.push_back(LoopID);
        else
          LoopChildEdges.push_back({Loops[LoopID].Parent, LoopID});
      }
      const FlatAdjacency LoopChildren =
          buildFlatAdjacency(Loops.size(), LoopChildEdges);
      std::vector<size_t> LoopPostorder;
      LoopPostorder.reserve(Loops.size());
      size_t NextLoopPreorder = 0;
      for (size_t RootLoop : LoopRoots) {
        Loops[RootLoop].Preorder = NextLoopPreorder++;
        std::vector<std::pair<size_t, size_t>> Stack{{RootLoop, 0}};
        while (!Stack.empty()) {
          auto &[LoopID, NextChild] = Stack.back();
          const llvm::ArrayRef<size_t> Children = LoopChildren[LoopID];
          if (NextChild < Children.size()) {
            const size_t Child = Children[NextChild++];
            Loops[Child].Depth = Loops[LoopID].Depth + 1;
            Loops[Child].Preorder = NextLoopPreorder++;
            Stack.push_back({Child, 0});
            continue;
          }
          Loops[LoopID].SubtreeEnd = NextLoopPreorder;
          LoopPostorder.push_back(LoopID);
          Stack.pop_back();
        }
      }

      // A linear-space heavy-light index answers loop-forest LCAs for exit-edge
      // path aggregation. Each CFG edge updates the loops it exits through one
      // tree-difference pair instead of walking every enclosing loop.
      std::vector<size_t> HeadWork = LoopRoots;
      for (size_t RootLoop : LoopRoots)
        Loops[RootLoop].ChainHead = RootLoop;
      while (!HeadWork.empty()) {
        const size_t LoopID = HeadWork.back();
        HeadWork.pop_back();
        size_t HeavyChild = kNoBlock;
        size_t HeavySize = 0;
        for (size_t Child : LoopChildren[LoopID]) {
          const size_t ChildSize =
              Loops[Child].SubtreeEnd - Loops[Child].Preorder;
          if (ChildSize > HeavySize) {
            HeavyChild = Child;
            HeavySize = ChildSize;
          }
        }
        for (size_t Child : LoopChildren[LoopID]) {
          Loops[Child].ChainHead =
              Child == HeavyChild ? Loops[LoopID].ChainHead : Child;
          HeadWork.push_back(Child);
        }
      }
      auto NearestCommonLoop = [&](size_t A,
                                   size_t B) -> std::optional<size_t> {
        while (Loops[A].ChainHead != Loops[B].ChainHead) {
          const size_t AHead = Loops[A].ChainHead;
          const size_t BHead = Loops[B].ChainHead;
          if (Loops[AHead].Depth >= Loops[BHead].Depth) {
            if (Loops[AHead].Parent == kNoBlock)
              return std::nullopt;
            A = Loops[AHead].Parent;
          } else {
            if (Loops[BHead].Parent == kNoBlock)
              return std::nullopt;
            B = Loops[BHead].Parent;
          }
        }
        return Loops[A].Depth < Loops[B].Depth ? A : B;
      };
      auto AddExitDelta = [&](size_t LoopID, int64_t Sign,
                              size_t GlobalTarget) {
        const int64_t Target = static_cast<int64_t>(GlobalTarget);
        Loops[LoopID].ExitEdgeCount += Sign;
        Loops[LoopID].ExitTargetSum += Sign * Target;
        Loops[LoopID].ExitTargetSquareSum += Sign * Target * Target;
      };
      auto RecordExitPath = [&](size_t SourceLoop, size_t TargetLoop,
                                size_t GlobalTarget) {
        const std::optional<size_t> Stop =
            TargetLoop == kNoBlock ? std::nullopt
                                   : NearestCommonLoop(SourceLoop, TargetLoop);
        AddExitDelta(SourceLoop, 1, GlobalTarget);
        if (Stop)
          AddExitDelta(*Stop, -1, GlobalTarget);
      };
      for (const FlatEdge &Edge : InternalEdges) {
        const size_t SourceLoop = InnermostLoop[Edge.From];
        if (SourceLoop != kNoBlock)
          RecordExitPath(SourceLoop, InnermostLoop[Edge.To], Blocks[Edge.To]);
      }
      for (const FlatEdge &Edge : ExternalEdges) {
        const size_t SourceLoop = InnermostLoop[Edge.From];
        if (SourceLoop != kNoBlock)
          RecordExitPath(SourceLoop, kNoBlock, Edge.To);
      }

      for (size_t Block = 0; Block < Count; ++Block)
        if (InnermostLoop[Block] != kNoBlock)
          ++Loops[InnermostLoop[Block]].DirectBlockCount;
      for (size_t LoopID : LoopPostorder) {
        CompactLoop &Loop = Loops[LoopID];
        Loop.BlockCount += Loop.DirectBlockCount;
        if (Loop.Parent == kNoBlock)
          continue;
        CompactLoop &Parent = Loops[Loop.Parent];
        Parent.BlockCount += Loop.BlockCount;
        Parent.ExitEdgeCount += Loop.ExitEdgeCount;
        Parent.ExitTargetSum += Loop.ExitTargetSum;
        Parent.ExitTargetSquareSum += Loop.ExitTargetSquareSum;
      }

      const size_t FirstLoopRegion = Program.High.Regions.size();
      for (size_t LoopID = 0; LoopID < Loops.size(); ++LoopID) {
        const CompactLoop &Loop = Loops[LoopID];
        Region Recovered;
        Recovered.Kind = RegionKind::Loop;
        Recovered.FunctionIndex = FunctionIndex;
        Recovered.HeaderBlock = Blocks[Loop.Header];
        if (Loop.Parent != kNoBlock)
          Recovered.ParentRegion = FirstLoopRegion + Loop.Parent;
        Recovered.LoopPreorder = Loop.Preorder;
        Recovered.LoopSubtreeEnd = Loop.SubtreeEnd;
        Recovered.BlockCount = Loop.BlockCount;
        Recovered.LatchOffset = Program.High.LoopLatches.size();
        for (size_t Latch : Backedges[Loop.Header])
          Program.High.LoopLatches.push_back(Blocks[Latch]);
        Recovered.LatchCount =
            Program.High.LoopLatches.size() - Recovered.LatchOffset;
        if (Loop.ExitEdgeCount > 0 &&
            Loop.ExitTargetSum % Loop.ExitEdgeCount == 0) {
          const int64_t Candidate = Loop.ExitTargetSum / Loop.ExitEdgeCount;
          if (Candidate >= 0 &&
              Loop.ExitTargetSquareSum ==
                  Loop.ExitEdgeCount * Candidate * Candidate &&
              static_cast<uint64_t>(Candidate) < GlobalCount)
            Recovered.ExitBlock = static_cast<size_t>(Candidate);
        }
        Program.High.Regions.push_back(std::move(Recovered));
      }
      for (size_t Block = 0; Block < Count; ++Block)
        if (InnermostLoop[Block] != kNoBlock)
          Program.High.BlockLoops[Blocks[Block]] =
              FirstLoopRegion + InnermostLoop[Block];
    }

    // Having two successors does not mean a block chose between them. Calls
    // are absent from this graph; only the typed taken/fallthrough pair can
    // form a two-armed region.
    for (size_t Header = 0; Header < Count; ++Header) {
      const ConditionalArms &Choice = Arms[Header];
      if (Choice.Disqualified || Choice.Taken == kNoBlock ||
          Choice.Fallthrough == kNoBlock || Choice.Taken == Choice.Fallthrough)
        continue;
      const size_t Left = Choice.Taken;
      const size_t Right = Choice.Fallthrough;
      // A conditional edge to a dominator is a natural-loop latch, not one
      // arm of a separate If region.
      if (Dominators && (dominates(*Dominators, Left, Header) ||
                         dominates(*Dominators, Right, Header)))
        continue;
      std::optional<size_t> Join =
          nearestCommonDominator(*PostDominators, Left, Right);
      if (Join && *Join == VirtualRoot)
        Join = std::nullopt;

      Region Recovered;
      Recovered.Kind = RegionKind::If;
      Recovered.FunctionIndex = FunctionIndex;
      Recovered.HeaderBlock = Blocks[Header];
      if (Join)
        Recovered.ExitBlock = Blocks[*Join];
      Program.High.Regions.push_back(std::move(Recovered));
    }

    for (size_t GlobalID : Blocks)
      GlobalToLocal[GlobalID] = kNoBlock;
  }
}

} // namespace

namespace analyzer_detail {

void recoverHighIR(DecodeContext &Context) {
  SBFProgram &Program = Context.Program;
  std::set<size_t> Entries;
  for (int Slot = Context.FunctionEntrySlots.find_first(); Slot >= 0;
       Slot = Context.FunctionEntrySlots.find_next(Slot))
    Entries.insert(static_cast<size_t>(Slot));
  // An entry into an LDDW continuation is an official runtime fault path.  It
  // is represented for lookup and diagnostics, but never enters the trusted
  // entry index used to partition ordinary function bodies.
  Entries.insert(Program.Low.EntrySlot);

  // Build the slot index and call-free adjacency once.  The ownership walk
  // below claims every block at most once, so function storage and traversal
  // remain linear in the CFG plus the number of function entries.
  std::vector<size_t> SlotToBlock(Program.Low.Instructions.size(), kNoBlock);
  for (const BasicBlock &Block : Program.Low.Blocks)
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      SlotToBlock[Slot] = Block.ID;
  std::vector<std::vector<size_t>> CallFreeSuccessors(
      Program.Low.Blocks.size());
  for (const CFGEdge &Edge : Program.Low.Edges)
    if (Edge.To && getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
      CallFreeSuccessors[Edge.From].push_back(*Edge.To);
  Program.High.Functions.clear();
  Program.High.FunctionBlocks.clear();
  for (size_t EntrySlot : Entries) {
    Function Function;
    Function.EntrySlot = EntrySlot;
    Function.Address = Program.Low.TextAddress + EntrySlot * kInstructionSize;
    if (const Symbol *Symbol = Context.findFunctionSymbol(Function.Address))
      Function.Name = Symbol->Name;
    else if (EntrySlot == Program.Low.EntrySlot)
      Function.Name = kEntrySymbolName.str();
    else
      Function.Name = syntheticFunctionName(Function.Address);
    Program.High.Functions.push_back(std::move(Function));
  }

  // Function indices are stable from this point onward.  Reserve every entry
  // block for its function before traversal so an earlier function cannot
  // claim a later function's entry through fallthrough or a branch.
  std::vector<size_t> EntryBlockOwners(Program.Low.Blocks.size(),
                                       HighIR::NoFunction);
  for (size_t FunctionID = 0; FunctionID < Program.High.Functions.size();
       ++FunctionID) {
    const size_t EntrySlot = Program.High.Functions[FunctionID].EntrySlot;
    const size_t EntryBlock =
        EntrySlot < SlotToBlock.size() ? SlotToBlock[EntrySlot] : kNoBlock;
    if (EntryBlock != kNoBlock)
      EntryBlockOwners[EntryBlock] = FunctionID;
  }

  Program.High.BlockOwners.assign(Program.Low.Blocks.size(),
                                  HighIR::NoFunction);
  std::deque<size_t> OwnershipWorklist;
  for (size_t FunctionID = 0; FunctionID < Program.High.Functions.size();
       ++FunctionID) {
    const Function &Function = Program.High.Functions[FunctionID];
    const size_t EntryBlock = Function.EntrySlot < SlotToBlock.size()
                                  ? SlotToBlock[Function.EntrySlot]
                                  : kNoBlock;
    if (EntryBlock == kNoBlock)
      continue;
    Program.High.BlockOwners[EntryBlock] = FunctionID;
    OwnershipWorklist.push_back(EntryBlock);
  }

  // Propagate every function entry simultaneously.  A block reached from two
  // entries is shared, not owned by whichever entry happened to be visited
  // first.  Function-entry blocks are hard boundaries: an edge into one never
  // transfers the caller's ownership through the callee.
  while (!OwnershipWorklist.empty()) {
    const size_t ID = OwnershipWorklist.front();
    OwnershipWorklist.pop_front();
    const size_t Incoming = Program.High.BlockOwners[ID];
    for (size_t To : CallFreeSuccessors[ID]) {
      if (To >= Program.High.BlockOwners.size() ||
          EntryBlockOwners[To] != HighIR::NoFunction)
        continue;
      size_t &Current = Program.High.BlockOwners[To];
      const size_t Merged = Current == HighIR::NoFunction ? Incoming
                            : Current == Incoming ? Current
                                                  : HighIR::AmbiguousFunction;
      if (Merged == Current)
        continue;
      Current = Merged;
      OwnershipWorklist.push_back(To);
    }
  }

  std::vector<size_t> BlockCounts(Program.High.Functions.size());
  for (const size_t Owner : Program.High.BlockOwners)
    if (Owner < BlockCounts.size())
      ++BlockCounts[Owner];

  size_t TotalOwnedBlocks = 0;
  for (size_t FunctionID = 0; FunctionID < Program.High.Functions.size();
       ++FunctionID) {
    Function &Function = Program.High.Functions[FunctionID];
    Function.BlockOffset = TotalOwnedBlocks;
    Function.BlockCount = BlockCounts[FunctionID];
    TotalOwnedBlocks += Function.BlockCount;
  }
  Program.High.FunctionBlocks.resize(TotalOwnedBlocks);
  std::vector<size_t> WriteOffsets;
  WriteOffsets.reserve(Program.High.Functions.size());
  for (const Function &Function : Program.High.Functions)
    WriteOffsets.push_back(Function.BlockOffset);
  for (size_t BlockID = 0; BlockID < Program.High.BlockOwners.size();
       ++BlockID) {
    const size_t Owner = Program.High.BlockOwners[BlockID];
    if (Owner < WriteOffsets.size())
      Program.High.FunctionBlocks[WriteOffsets[Owner]++] = BlockID;
    else if (Owner == HighIR::AmbiguousFunction) {
      const BasicBlock &Block = Program.Low.Blocks[BlockID];
      Program.Low.Diagnostics.push_back(
          {DiagnosticSeverity::Warning, Block.StartSlot,
           Program.Low.TextAddress + Block.StartSlot * kInstructionSize,
           "basic block is reachable from multiple function entries and has "
           "no unique function owner",
           ValidationRule::None});
    }
  }

  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    if (Instruction.isInvalid())
      continue;
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
    if (dispatchesRuntimeSyscall(Instruction.Call, Instruction.Dispatch)) {
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

//===- DataFlow.cpp - Bounded SSA neighborhoods and impact slices --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Structural dependency observations. Postdominators describe paths to an
/// observed exit; missing exits and unsupported effects remain explicit gaps.
//===----------------------------------------------------------------------===//

#include "neverd/diff/DataFlow.h"

#include "FlowContract.h"
#include "FlowParameterViews.h"
#include "FlowProgress.h"

#include "neverd/diff/StableHash.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <stdexcept>

namespace neverd::diff {
namespace {
namespace limits = neverd::limits::diff;
struct Stopped {
  ComparisonState State;
};
class WorkBudget {
  uint64_t &Used;
  const DataFlowOptions &Options;
  const ComparisonControl &Control;

public:
  WorkBudget(uint64_t &Used, const DataFlowOptions &Options,
             const ComparisonControl &Control)
      : Used(Used), Options(Options), Control(Control) {
    poll();
  }
  void poll() const {
    if (auto State = Control.stop())
      throw Stopped{*State};
  }
  void charge(uint64_t Amount = 1) {
    if (Amount > Options.MaxWork - Used)
      throw Stopped{ComparisonState::ResourceLimited};
    const auto Before = Used;
    Used += Amount;
    if (Before / limits::kFlowPollWork != Used / limits::kFlowPollWork)
      poll();
  }
};

/// Controllers of each block. The synthetic exit has no source instruction.
std::vector<std::vector<uint32_t>>
controllers(const DataFlowGraph &G, WorkBudget &Work, uint32_t &Gaps) {
  const uint32_t Count = G.Blocks.size(), Exit = Count;
  std::vector<std::vector<uint32_t>> Pred(Count), Out(Count);
  std::vector<uint32_t> Queue;
  llvm::BitVector CanExit(Count + 1);
  CanExit.set(Exit);
  for (uint32_t B = 0; B < Count; ++B) {
    Work.charge();
    Gaps |= G.Blocks[B].Gaps;
    if (G.Blocks[B].Successors.empty()) {
      CanExit.set(B);
      Queue.push_back(B);
    }
    for (auto S : G.Blocks[B].Successors) {
      Work.charge();
      Pred[S].push_back(B);
    }
  }
  for (size_t I = 0; I < Queue.size(); ++I)
    for (auto P : Pred[Queue[I]]) {
      Work.charge();
      if (!CanExit.test(P)) {
        CanExit.set(P);
        Queue.push_back(P);
      }
    }
  if (CanExit.count() != Count + 1)
    Gaps |= FlowNoExit;
  const uint64_t Words = (Count + 1 + 63) / 64;
  std::vector<llvm::BitVector> Post(Count + 1, CanExit);
  Post[Exit].reset();
  Post[Exit].set(Exit);
  for (uint32_t B = 0; B < Count; ++B)
    if (!CanExit.test(B)) {
      Post[B].reset();
      Post[B].set(B);
    }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (uint32_t B = Count; B-- > 0;) {
      Work.charge(Words);
      if (!CanExit.test(B))
        continue;
      auto Next = CanExit;
      if (G.Blocks[B].Successors.empty())
        Next = Post[Exit];
      for (auto S : G.Blocks[B].Successors) {
        Work.charge(Words);
        Next &= Post[S];
      }
      Next.set(B);
      if (Next != Post[B]) {
        Post[B] = std::move(Next);
        Changed = true;
      }
    }
  }
  std::vector<uint32_t> Immediate(Count, Exit);
  for (uint32_t B = 0; B < Count; ++B) {
    unsigned Best = 0;
    for (auto D : Post[B].set_bits()) {
      Work.charge(Words);
      if (D != B && Post[D].count() > Best) {
        Immediate[B] = D;
        Best = Post[D].count();
      }
    }
  }
  for (uint32_t B = 0; B < Count; ++B) {
    const auto &Block = G.Blocks[B];
    if (Block.Successors.size() < 2)
      continue;
    if (Block.Terminator == NoIndex) {
      Gaps |= FlowUnknownInput;
      continue;
    }
    for (auto S : Block.Successors) {
      Work.charge();
      if (Post[B].test(S))
        continue;
      uint32_t At = S;
      for (uint32_t Steps = 0; At != Immediate[B] && At != Exit; ++Steps) {
        Work.charge();
        if (Steps == Count) {
          Gaps |= FlowNoExit;
          break;
        }
        Out[At].push_back(Block.Terminator);
        At = Immediate[At];
      }
    }
  }
  return Out;
}
} // namespace

uint64_t flowExtraWork(const FlowNode &N) {
  uint64_t Work = N.Semantics.size() + N.Origins.size();
  for (const auto &I : N.Inputs)
    Work += I.Semantics.size();
  return Work;
}

std::vector<llvm::StringRef> flowGapNames(uint32_t Mask) {
  std::vector<llvm::StringRef> Names;
  for (const auto &[Bit, Name] :
       {std::pair{FlowUnknownInput, "unknown_input"},
        {FlowMemoryAlias, "memory_alias"},
        {FlowCallEffects, "call_effects"},
        {FlowIndirectControl, "indirect_control"},
        {FlowExceptionalControl, "exceptional_control"},
        {FlowNativeCoverage, "native_coverage"},
        {FlowNoExit, "no_observed_exit"}})
    if (Mask & Bit)
      Names.push_back(Name);
  return Names;
}

std::optional<std::string> DataFlowOptions::validate() const {
  if (!MaxNodes || MaxNodes > limits::kMaxFlowNodes || !MaxBlocks ||
      MaxBlocks > limits::kMaxFlowBlocks || !MaxWork ||
      MaxWork > limits::kMaxFlowWork || !Rounds ||
      Rounds > limits::kMaxFlowRounds)
    return "dataflow allowance is outside the admitted range";
  return {};
}
std::optional<std::string>
DataFlowGraph::validate(const DataFlowOptions &Options,
                        const ComparisonControl &Control) const {
  Control.check();
  if (auto Error = Options.validate())
    return Error;
  if (Nodes.size() > Options.MaxNodes || Blocks.size() > Options.MaxBlocks)
    return "dataflow snapshot exceeds its node/block allowance";
  if ((Profile != DataFlowProfile && !isLLVMDataFlowProfile(Profile)) ||
      Revision.empty() || ABI.empty() ||
      Revision.size() > limits::kMaxFlowIdentityBytes ||
      ABI.size() > limits::kMaxFlowIdentityBytes || (Gaps & ~AllFlowGaps))
    return "dataflow snapshot has an invalid profile, identity or gap mask";
  if (Blocks.empty() || EntryBlock >= Blocks.size())
    return "dataflow snapshot has no valid entry block";
  uint64_t Inputs = 0;
  try {
    if (auto Error = validateParameterViews(*this, Options, [&](uint64_t N) {
          Control.check();
          if (N > Options.MaxWork - Inputs)
            throw DataFlowInterrupted(ComparisonState::ResourceLimited);
          Inputs += N;
        }))
      return Error;
  } catch (const DataFlowInterrupted &) {
    return "parameter views exceed the work allowance";
  }
  for (uint32_t BI = 0; BI < Blocks.size(); ++BI) {
    Control.check();
    const auto &B = Blocks[BI];
    if (B.Gaps & ~AllFlowGaps)
      return "dataflow block has an unknown gap mask";
    if (B.End < B.Begin ||
        (B.Terminator != NoIndex &&
         (B.Terminator >= Nodes.size() || Nodes[B.Terminator].Block != BI)))
      return "dataflow block has an invalid extent or terminator";
    Inputs += B.Successors.size();
    if (Inputs > Options.MaxWork)
      return "dataflow edges exceed the work allowance";
    for (auto S : B.Successors)
      if (S >= Blocks.size())
        return "dataflow CFG has an invalid successor";
  }
  for (const auto &N : Nodes) {
    Control.check();
    Inputs += N.Semantics.size() + N.Origins.size();
    if (Inputs > Options.MaxWork ||
        N.Semantics.size() > limits::kMaxFlowSemanticsBytes)
      return "dataflow operation metadata exceeds its allowance";
    if (Profile == DataFlowProfile
            ? (!N.Semantics.empty() || !N.Origins.empty())
            : (N.Semantics.empty() || N.OriginSequence != -1 || N.Address))
      return "dataflow operation metadata disagrees with its profile";
    for (size_t I = 0; I < N.Origins.size(); ++I)
      if (N.Origins[I].Sequence < 0 || (I && N.Origins[I] <= N.Origins[I - 1]))
        return "dataflow origins must be valid, sorted and unique";
    if (N.Block >= Blocks.size() || N.Opcode.empty() || N.OriginSequence < -1 ||
        N.Opcode.size() > limits::kMaxFlowOpcodeBytes ||
        (N.Gaps & ~AllFlowGaps))
      return "dataflow operation has no valid block or opcode";
    if (N.CallArgumentBegin == NoIndex
            ? N.CallArgumentCount != 0
            : ((N.Opcode != "CALL" && N.Opcode != "INDIR_CALL") || N.Phi ||
               N.Commutative ||
               uint64_t(N.CallArgumentBegin) + N.CallArgumentCount >
                   N.Inputs.size()))
      return "dataflow call argument range has no valid owning call";
    Inputs += N.Inputs.size();
    if (Inputs > Options.MaxWork)
      return "dataflow operands exceed the work allowance";
    for (const auto &I : N.Inputs) {
      Control.check();
      Inputs += I.Semantics.size();
      if (Inputs > Options.MaxWork ||
          I.Semantics.size() > limits::kMaxFlowSemanticsBytes ||
          (Profile == DataFlowProfile ? !I.Semantics.empty()
                                      : I.Semantics.empty()))
        return "dataflow operand metadata disagrees with its profile or "
               "allowance";
      if (unsigned(I.Type) > unsigned(FlowOperand::Kind::Unknown) ||
          ((I.Type == FlowOperand::Kind::Node) != (I.Node != NoIndex)) ||
          (I.Type == FlowOperand::Kind::Node && I.Node >= Nodes.size()) ||
          (I.Predecessor != NoIndex && I.Predecessor >= Blocks.size()))
        return "dataflow operand has an invalid definition or predecessor";
      if (I.Predecessor != NoIndex &&
          (!N.Phi ||
           !llvm::is_contained(Blocks[I.Predecessor].Successors, N.Block)))
        return "dataflow PHI input is not attached to an incoming CFG edge";
      if (hasParameterViews(Profile) &&
          I.Type == FlowOperand::Kind::Parameter) {
        if (I.Value >= ParameterViews.size())
          return "dataflow parameter has no source argument record";
        const auto &V = ParameterViews[I.Value];
        if (I.Bytes != (V.ViewBits + 7) / 8 || I.Provenance || I.AddressOwner ||
            I.Semantics != parameterViewSemantics(V))
          return "dataflow parameter disagrees with its observed bit view";
      }
    }
  }
  try {
    if (auto Error = validateFlowProgress(*this, Options, [&](uint64_t N) {
          Control.check();
          if (N > Options.MaxWork - Inputs)
            throw DataFlowInterrupted(ComparisonState::ResourceLimited);
          Inputs += N;
        }))
      return Error;
  } catch (const DataFlowInterrupted &) {
    return "progress evidence exceeds the work allowance";
  }
  return {};
}

FlowFingerprints fingerprintDataFlow(const DataFlowGraph &G,
                                     const DataFlowOptions &Options,
                                     const ComparisonControl &Control) {
  FlowFingerprints R;
  try {
    if (auto Error = G.validate(Options, Control))
      throw std::invalid_argument(*Error);
    WorkBudget Work(R.Work, Options, Control);
    std::vector<uint64_t> Local, Keys, BlockKeys(G.Blocks.size());
    std::vector<uint8_t> Closed(G.Nodes.size());
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> Incoming(
        G.Blocks.size());
    for (uint32_t B = 0; B < G.Blocks.size(); ++B)
      for (uint32_t I = 0; I < G.Blocks[B].Successors.size(); ++I) {
        Work.charge();
        Incoming[G.Blocks[B].Successors[I]].emplace_back(B, I);
      }
    for (const auto &N : G.Nodes) {
      Work.charge(1 + N.Semantics.size() + N.Origins.size());
      Local.push_back(detail::flowOperationKey(N));
    }
    Keys = Local;
    for (unsigned Round = 0; Round < Options.Rounds; ++Round) {
      auto Next = Local;
      auto NextClosed = Closed;
      auto NextBlocks = BlockKeys;
      for (uint32_t B = 0; B < G.Blocks.size(); ++B) {
        Work.charge();
        const auto &Block = G.Blocks[B];
        StableHash H;
        H.add(Block.Gaps).add(Block.Successors.size());
        if (Block.Terminator != NoIndex)
          H.add(Keys[Block.Terminator]);
        for (auto S : Block.Successors) {
          Work.charge();
          H.add(BlockKeys[S]);
        }
        std::vector<uint64_t> Predecessors;
        for (const auto &[P, Position] : Incoming[B]) {
          Work.charge();
          StableHash Edge;
          Edge.add(BlockKeys[P]).add(Position);
          if (G.Blocks[P].Terminator != NoIndex)
            Edge.add(Keys[G.Blocks[P].Terminator]);
          Predecessors.push_back(Edge.value());
        }
        std::sort(Predecessors.begin(), Predecessors.end(),
                  [&](auto A, auto B) {
                    Work.charge();
                    return A < B;
                  });
        for (auto P : Predecessors)
          H.add(P);
        NextBlocks[B] = H.value();
      }
      for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
        Work.charge();
        const auto &Node = G.Nodes[N];
        bool Complete = !G.Gaps && !Node.Gaps;
        std::vector<uint64_t> Inputs;
        for (const auto &I : Node.Inputs) {
          Work.charge(1 + I.Semantics.size());
          StableHash H;
          H.add(uint8_t(I.Type)).add(I.Bytes).add(I.Provenance);
          if (!I.Semantics.empty())
            H.add(I.Semantics);
          if (I.Type == FlowOperand::Kind::Node)
            H.add(Keys[I.Node]);
          else if (I.Type != FlowOperand::Kind::Unknown)
            H.add(I.Value).add(I.AddressOwner);
          if (I.Predecessor != NoIndex)
            H.add(BlockKeys[I.Predecessor]);
          Inputs.push_back(H.value());
          Complete &= I.Type != FlowOperand::Kind::Unknown &&
                      (I.Type != FlowOperand::Kind::Node || Closed[I.Node]);
        }
        if (Node.Commutative || Node.Phi)
          std::sort(Inputs.begin(), Inputs.end(), [&](auto A, auto B) {
            Work.charge();
            return A < B;
          });
        StableHash H;
        H.add(Local[N]);
        for (auto I : Inputs)
          H.add(I);
        Next[N] = H.value();
        NextClosed[N] = Complete;
      }
      Keys = std::move(Next);
      BlockKeys = std::move(NextBlocks);
      Closed = std::move(NextClosed);
      Work.charge(Keys.size() * 2);
      R.Keys = Keys;
      R.OpenDataFrontiers.clear();
      for (auto C : Closed)
        R.OpenDataFrontiers.push_back(!C);
      ++R.CompletedRounds;
    }
    Work.poll();
  } catch (const Stopped &Stop) {
    R.State = Stop.State;
  } catch (const ComparisonInterrupted &Stop) {
    R.State = Stop.State;
  }
  return R;
}

FlowSlice sliceDataFlow(const DataFlowGraph &G, llvm::ArrayRef<uint32_t> Seeds,
                        SliceDirection Direction, bool IncludeControl,
                        const DataFlowOptions &Options,
                        const ComparisonControl &Control) {
  if (auto Error = G.validate(Options))
    throw std::invalid_argument(*Error);
  for (auto S : Seeds)
    if (S >= G.Nodes.size())
      throw std::invalid_argument("slice seed is not a dataflow node");
  FlowSlice R;
  R.Gaps = G.Gaps;
  try {
    WorkBudget Work(R.Work, Options, Control);
    std::vector<std::vector<uint32_t>> Controls(G.Blocks.size());
    if (IncludeControl) {
      Controls = controllers(G, Work, R.Gaps);
      R.ControlDependencies = true;
    }
    std::vector<std::vector<uint32_t>> Edges(G.Nodes.size());
    const auto Edge = [&](uint32_t Definition, uint32_t Use) {
      Work.charge();
      if (Direction == SliceDirection::Backward)
        Edges[Use].push_back(Definition);
      else
        Edges[Definition].push_back(Use);
    };
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      Work.charge(1 + flowExtraWork(G.Nodes[N]));
      for (const auto &I : G.Nodes[N].Inputs)
        if (I.Type == FlowOperand::Kind::Node)
          Edge(I.Node, N);
      for (auto C : Controls[G.Nodes[N].Block])
        Edge(C, N);
    }
    llvm::BitVector Visited(G.Nodes.size());
    std::vector<uint32_t> Queue;
    for (auto S : Seeds)
      if (!Visited.test(S)) {
        Visited.set(S);
        Queue.push_back(S);
      }
    for (size_t I = 0; I < Queue.size(); ++I) {
      Work.charge();
      const auto N = Queue[I];
      R.Nodes.push_back(N);
      R.Gaps |= G.Nodes[N].Gaps | G.Blocks[G.Nodes[N].Block].Gaps;
      for (auto D : Edges[N]) {
        Work.charge();
        if (!Visited.test(D)) {
          Visited.set(D);
          Queue.push_back(D);
        }
      }
    }
    Work.poll();
  } catch (const Stopped &Stop) {
    R.State = Stop.State;
  }
  llvm::sort(R.Nodes);
  return R;
}

} // namespace neverd::diff

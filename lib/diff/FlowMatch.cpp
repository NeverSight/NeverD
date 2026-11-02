//===- FlowMatch.cpp - Exact checks for finite SSA correspondences -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A sparse, bounded bijection preserves DAG sharing and ordered operands.
/// Commutative ambiguity is exposed rather than resolved by SSA numbering.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowMatch.h"

#include "FlowContract.h"

#include "neverd/diff/StableHash.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {
namespace {
struct Stopped {
  ComparisonState State;
};
struct Refused {
  FlowMatchOutcome Outcome;
  const char *Reason;
};
class Check {
  const DataFlowGraph &A, &B;
  const FlowFingerprints &AK, &BK;
  const DataFlowOptions &Options;
  const ComparisonControl &Control;
  FlowNeighborhoodMatch &Out;
  llvm::DenseMap<uint32_t, uint32_t> NodesA, NodesB, BlocksA, BlocksB, Depths,
      BlockDepths;
  struct Pending {
    uint32_t A, B;
    unsigned Depth;
  };
  std::vector<Pending> Queue;
  std::vector<Pending> BlockQueue;
  using IncomingEdges = std::vector<std::vector<std::pair<uint32_t, uint32_t>>>;
  IncomingEdges IncomingA, IncomingB;

  void poll() {
    if (auto Stop = Control.stop())
      throw Stopped{*Stop};
  }
  void charge(uint64_t Amount = 1) {
    if (Amount > Options.MaxWork - Out.Work)
      throw Stopped{ComparisonState::ResourceLimited};
    const auto Before = Out.Work;
    Out.Work += Amount;
    if (Before / limits::diff::kFlowPollWork !=
        Out.Work / limits::diff::kFlowPollWork)
      poll();
  }
  void same(bool Condition, const char *Reason) {
    charge();
    if (!Condition)
      throw Refused{FlowMatchOutcome::Different, Reason};
  }
  void pair(llvm::DenseMap<uint32_t, uint32_t> &Forward,
            llvm::DenseMap<uint32_t, uint32_t> &Reverse, uint32_t X,
            uint32_t Y) {
    charge();
    const auto [F, AddedF] = Forward.try_emplace(X, Y);
    const auto [R, AddedR] = Reverse.try_emplace(Y, X);
    same((AddedF || F->second == Y) && (AddedR || R->second == X),
         "shared definitions or PHI blocks have conflicting counterparts");
  }
  IncomingEdges incoming(const DataFlowGraph &G) {
    IncomingEdges Edges(G.Blocks.size());
    for (uint32_t B = 0; B < G.Blocks.size(); ++B) {
      charge();
      for (uint32_t E = 0; E < G.Blocks[B].Successors.size(); ++E) {
        charge();
        Edges[G.Blocks[B].Successors[E]].emplace_back(B, E);
      }
    }
    return Edges;
  }
  std::vector<std::pair<uint32_t, uint32_t>>
  orderedIncoming(const DataFlowGraph &G, const FlowFingerprints &Keys,
                  const IncomingEdges &Edges, uint32_t Block) {
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> Ordered;
    for (const auto &[Pred, Position] : Edges[Block]) {
      charge();
      const auto &B = G.Blocks[Pred];
      const auto Key =
          StableHash()
              .add(Position)
              .add(B.Gaps)
              .add(B.Successors.size())
              .add(B.Terminator == NoIndex ? 0 : Keys.Keys[B.Terminator])
              .value();
      Ordered.push_back({Key, {Pred, Position}});
    }
    std::sort(Ordered.begin(), Ordered.end(),
              [&](const auto &X, const auto &Y) {
                charge();
                return X.first < Y.first;
              });
    std::vector<std::pair<uint32_t, uint32_t>> Result;
    for (size_t I = 0; I < Ordered.size(); ++I) {
      charge();
      if (I && Ordered[I - 1].first == Ordered[I].first &&
          Ordered[I - 1].second != Ordered[I].second)
        throw Refused{
            FlowMatchOutcome::Ambiguous,
            "PHI control predecessors have tied possible counterparts"};
      Result.push_back(Ordered[I].second);
    }
    return Result;
  }
  void block(Pending P) {
    pair(BlocksA, BlocksB, P.A, P.B);
    auto [At, New] = BlockDepths.try_emplace(P.A, P.Depth);
    if (!New && At->second >= P.Depth)
      return;
    At->second = P.Depth;
    const auto &X = A.Blocks[P.A], &Y = B.Blocks[P.B];
    same(X.Gaps == Y.Gaps && X.Successors.size() == Y.Successors.size(),
         "PHI incoming control shapes differ");
    Out.Gaps |= X.Gaps | Y.Gaps;
    if (!P.Depth)
      return;
    same((X.Terminator == NoIndex) == (Y.Terminator == NoIndex),
         "PHI incoming control terminators differ");
    if (X.Terminator != NoIndex)
      Queue.push_back({X.Terminator, Y.Terminator, P.Depth - 1});
    if (IncomingA.empty()) {
      IncomingA = incoming(A);
      IncomingB = incoming(B);
    }
    const auto XI = orderedIncoming(A, AK, IncomingA, P.A),
               YI = orderedIncoming(B, BK, IncomingB, P.B);
    same(XI.size() == YI.size(),
         "PHI incoming control predecessor counts differ");
    for (size_t I = 0; I < XI.size(); ++I) {
      same(XI[I].second == YI[I].second,
           "PHI values flow from different branch arms");
      BlockQueue.push_back({XI[I].first, YI[I].first, P.Depth - 1});
    }
  }
  uint64_t operandKey(const DataFlowGraph &G, const FlowFingerprints &Keys,
                      const FlowOperand &I, uint32_t Block) {
    charge(1 + I.Semantics.size());
    StableHash H;
    H.add(uint8_t(I.Type)).add(I.Bytes).add(I.Provenance);
    if (!I.Semantics.empty())
      H.add(I.Semantics);
    if (I.Type == FlowOperand::Kind::Node)
      H.add(Keys.Keys[I.Node]);
    else if (I.Type != FlowOperand::Kind::Unknown)
      H.add(I.Value).add(I.AddressOwner);
    H.add(I.Predecessor != NoIndex);
    if (I.Predecessor != NoIndex) {
      const auto &P = G.Blocks[I.Predecessor];
      H.add(P.Gaps).add(P.Successors.size());
      H.add(P.Terminator == NoIndex ? 0 : Keys.Keys[P.Terminator]);
      for (unsigned E = 0; E < P.Successors.size(); ++E) {
        charge();
        if (P.Successors[E] == Block)
          H.add(E);
      }
    }
    return H.value();
  }
  std::vector<uint32_t> order(const DataFlowGraph &G,
                              const FlowFingerprints &Keys, const FlowNode &N) {
    std::vector<std::pair<uint64_t, uint32_t>> Ordered;
    for (uint32_t I = 0; I < N.Inputs.size(); ++I)
      Ordered.emplace_back(operandKey(G, Keys, N.Inputs[I], N.Block), I);
    if (N.Commutative || N.Phi) {
      std::sort(Ordered.begin(), Ordered.end(),
                [&](const auto &X, const auto &Y) {
                  charge();
                  return X.first < Y.first;
                });
      for (size_t I = 1; I < Ordered.size(); ++I) {
        charge();
        if (Ordered[I - 1].first == Ordered[I].first &&
            N.Inputs[Ordered[I - 1].second] != N.Inputs[Ordered[I].second])
          throw Refused{
              FlowMatchOutcome::Ambiguous,
              "tied operand neighborhoods have distinct possible definitions"};
      }
    }
    std::vector<uint32_t> Result;
    for (const auto &[Key, Index] : Ordered) {
      (void)Key;
      charge();
      Result.push_back(Index);
    }
    return Result;
  }
  void phiEdge(uint32_t NodeA, uint32_t PredA, uint32_t NodeB, uint32_t PredB,
               unsigned Depth) {
    pair(BlocksA, BlocksB, NodeA, NodeB);
    pair(BlocksA, BlocksB, PredA, PredB);
    const auto &X = A.Blocks[PredA], &Y = B.Blocks[PredB];
    same(X.Gaps == Y.Gaps && X.Successors.size() == Y.Successors.size(),
         "PHI predecessor control shapes differ");
    for (size_t I = 0; I < X.Successors.size(); ++I)
      same((X.Successors[I] == NodeA) == (Y.Successors[I] == NodeB),
           "PHI values arrive on different ordered control edges");
    same((X.Terminator == NoIndex) == (Y.Terminator == NoIndex),
         "PHI predecessor terminators differ");
    Out.Gaps |= X.Gaps | Y.Gaps;
    if (X.Terminator != NoIndex)
      Queue.push_back({X.Terminator, Y.Terminator, Depth});
    BlockQueue.push_back({PredA, PredB, Depth});
  }
  void node(Pending P) {
    charge();
    pair(NodesA, NodesB, P.A, P.B);
    auto [At, New] = Depths.try_emplace(P.A, P.Depth);
    if (!New && At->second >= P.Depth)
      return;
    At->second = P.Depth;
    const auto &X = A.Nodes[P.A], &Y = B.Nodes[P.B];
    charge(X.Semantics.size() + Y.Semantics.size());
    same(detail::sameFlowOperation(X, Y),
         "operation, width or effect metadata differ");
    Out.Gaps |=
        X.Gaps | Y.Gaps | A.Blocks[X.Block].Gaps | B.Blocks[Y.Block].Gaps;
    same(X.Inputs.size() == Y.Inputs.size(), "operation arities differ");
    if (!P.Depth) {
      Out.OpenDataFrontier |= !X.Inputs.empty();
      return;
    }
    const auto XI = order(A, AK, X), YI = order(B, BK, Y);
    for (size_t N = 0; N < XI.size(); ++N) {
      const auto &U = X.Inputs[XI[N]], &V = Y.Inputs[YI[N]];
      charge(U.Semantics.size() + V.Semantics.size());
      same(detail::sameFlowOperandShape(U, V),
           "operand kind, width or provenance differ");
      same((U.Predecessor == NoIndex) == (V.Predecessor == NoIndex),
           "PHI incoming-edge availability differs");
      if (U.Predecessor != NoIndex)
        phiEdge(X.Block, U.Predecessor, Y.Block, V.Predecessor, P.Depth - 1);
      if (U.Type == FlowOperand::Kind::Node)
        Queue.push_back({U.Node, V.Node, P.Depth - 1});
      else if (U.Type == FlowOperand::Kind::Unknown)
        Out.OpenDataFrontier = true;
      else
        same(U.Value == V.Value && U.AddressOwner == V.AddressOwner,
             "scalar bits, parameter positions or address owners differ");
    }
  }

public:
  Check(const DataFlowGraph &A, const FlowFingerprints &AK,
        const DataFlowGraph &B, const FlowFingerprints &BK,
        const DataFlowOptions &Options, const ComparisonControl &Control,
        FlowNeighborhoodMatch &Out)
      : A(A), B(B), AK(AK), BK(BK), Options(Options), Control(Control),
        Out(Out) {}
  void run(uint32_t RootA, uint32_t RootB) {
    poll();
    Out.Gaps = A.Gaps | B.Gaps;
    Out.OpenDataFrontier =
        AK.OpenDataFrontiers[RootA] || BK.OpenDataFrontiers[RootB];
    Queue.push_back({RootA, RootB, Options.Rounds});
    size_t NI = 0, BI = 0;
    while (NI < Queue.size() || BI < BlockQueue.size()) {
      while (NI < Queue.size())
        node(Queue[NI++]);
      while (BI < BlockQueue.size())
        block(BlockQueue[BI++]);
    }
    std::vector<std::pair<uint32_t, uint32_t>> Nodes, Blocks;
    for (auto P : NodesA) {
      charge();
      Nodes.emplace_back(P.first, P.second);
    }
    for (auto P : BlocksA) {
      charge();
      Blocks.emplace_back(P.first, P.second);
    }
    for (auto *Rows : {&Nodes, &Blocks})
      std::sort(Rows->begin(), Rows->end(), [&](const auto &X, const auto &Y) {
        charge();
        return X < Y;
      });
    poll();
    Out.OpenDataFrontier |= Out.Gaps != 0;
    Out.Nodes = std::move(Nodes);
    Out.PhiBlocks = std::move(Blocks);
    Out.Outcome = FlowMatchOutcome::Matched;
  }
};
} // namespace

FlowMatchInput::FlowMatchInput(const DataFlowGraph &G,
                               const FlowFingerprints &K,
                               const DataFlowOptions &Options,
                               const ComparisonControl &Control)
    : Graph(G), Keys(K), Rounds(Options.Rounds) {
  if (auto Error = G.validate(Options, Control))
    throw std::invalid_argument(*Error);
  if (K.State != ComparisonState::Complete || K.CompletedRounds != Rounds ||
      K.Keys.size() != G.Nodes.size() ||
      K.OpenDataFrontiers.size() != G.Nodes.size())
    throw std::invalid_argument("dataflow neighborhood needs completed "
                                "fingerprints at the requested depth");
}

FlowNeighborhoodMatch
matchDataFlowNeighborhood(const FlowMatchInput &Primary, uint32_t RootA,
                          const FlowMatchInput &Secondary, uint32_t RootB,
                          uint64_t MaxWork, const ComparisonControl &Control) {
  const auto &A = Primary.Graph, &B = Secondary.Graph;
  if (Primary.Rounds != Secondary.Rounds ||
      MaxWork > limits::diff::kMaxFlowWork)
    throw std::invalid_argument(
        "dataflow neighborhood depth or work allowance is incompatible");
  DataFlowOptions Options;
  Options.Rounds = Primary.Rounds;
  Options.MaxWork = MaxWork;
  if (RootA >= A.Nodes.size() || RootB >= B.Nodes.size())
    throw std::invalid_argument("dataflow neighborhood root is absent");
  FlowNeighborhoodMatch Out;
  if (A.Profile != B.Profile || A.ABI != B.ABI) {
    Out.Outcome = FlowMatchOutcome::Unsupported;
    Out.Reason = "dataflow profile or ABI pair is not admitted";
    return Out;
  }
  try {
    Check(A, Primary.Keys, B, Secondary.Keys, Options, Control, Out)
        .run(RootA, RootB);
  } catch (const Stopped &Stop) {
    Out.State = Stop.State;
    Out.Reason = "neighborhood verification stopped";
  } catch (const Refused &Stop) {
    Out.Outcome = Stop.Outcome;
    Out.Reason = Stop.Reason;
  }
  return Out;
}
FlowNeighborhoodMatch matchDataFlowNeighborhood(
    const DataFlowGraph &A, uint32_t RootA, const FlowFingerprints &AK,
    const DataFlowGraph &B, uint32_t RootB, const FlowFingerprints &BK,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  const FlowMatchInput Primary(A, AK, Options), Secondary(B, BK, Options);
  return matchDataFlowNeighborhood(Primary, RootA, Secondary, RootB,
                                   Options.MaxWork, Control);
}
} // namespace neverd::diff

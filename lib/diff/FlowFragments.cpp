//===- FlowFragments.cpp - Finite SSA fragments -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Candidate keys stop at explicit typed inputs. Exact verification shares one
/// bijection across included and external definitions, preserving DAG sharing.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowFragments.h"

#include "FlowContract.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <stdexcept>

namespace neverd::diff {
namespace {
struct Stopped {
  ComparisonState State;
};
struct Refused {
  FlowMatchOutcome Outcome;
  const char *Reason;
};
class Budget {
  uint64_t &Used;
  uint64_t Maximum;
  const ComparisonControl &Control;

public:
  Budget(uint64_t &Used, uint64_t Maximum, const ComparisonControl &Control)
      : Used(Used), Maximum(Maximum), Control(Control) {}
  void poll() const {
    if (const auto Stop = Control.stop())
      throw Stopped{*Stop};
  }
  void charge(uint64_t Count = 1) {
    if (Count > Maximum - Used)
      throw Stopped{ComparisonState::ResourceLimited};
    const auto Before = Used;
    Used += Count;
    if (Before / limits::diff::kFlowPollWork !=
        Used / limits::diff::kFlowPollWork)
      poll();
  }
};
bool admitted(const FlowNode &N, Budget &Work) {
  Work.charge(1 + N.Inputs.size());
  if (N.Phi || N.Gaps || N.MemoryOrdering || N.MemoryAddressSpace ||
      N.CallArgumentBegin != NoIndex || N.CallArgumentCount ||
      (!N.ResultBytes && N.Opcode != "RETURN" && N.Opcode != "ret") ||
      N.Opcode == "LOAD" || N.Opcode == "STORE" || N.Opcode == "CALL" ||
      N.Opcode == "INDIR_CALL" || N.Opcode == "BRANCH" ||
      N.Opcode == "COND_BR" || N.Opcode == "INDIR_BR" || N.Opcode == "load" ||
      N.Opcode == "store" || N.Opcode == "call" || N.Opcode == "br" ||
      N.Opcode == "switch")
    return false;
  for (const auto &I : N.Inputs)
    if (I.Type == FlowOperand::Kind::Unknown || I.Predecessor != NoIndex)
      return false;
  return true;
}
uint64_t operandKey(const FlowOperand &I, const FlowFragmentLayer *Children,
                    Budget &Work) {
  Work.charge(1 + I.Semantics.size());
  StableHash H;
  H.add(uint8_t(I.Type)).add(I.Bytes).add(I.Provenance);
  if (!I.Semantics.empty())
    H.add(I.Semantics);
  if (I.Type == FlowOperand::Kind::Node) {
    if (Children)
      H.add(Children->Keys[I.Node]);
  } else {
    H.add(I.Value).add(I.AddressOwner);
  }
  return H.value();
}
std::vector<uint32_t> operandOrder(const FlowNode &N,
                                   const FlowFragmentLayer *Children,
                                   Budget &Work) {
  std::vector<std::pair<uint64_t, uint32_t>> Keys;
  for (uint32_t I = 0; I < N.Inputs.size(); ++I)
    Keys.emplace_back(operandKey(N.Inputs[I], Children, Work), I);
  if (N.Commutative) {
    std::sort(Keys.begin(), Keys.end(), [&](const auto &A, const auto &B) {
      Work.charge();
      return A.first < B.first;
    });
    for (size_t I = 1; I < Keys.size(); ++I) {
      Work.charge();
      if (Keys[I - 1].first == Keys[I].first &&
          N.Inputs[Keys[I - 1].second] != N.Inputs[Keys[I].second])
        throw Refused{FlowMatchOutcome::Ambiguous,
                      "fragment inputs have tied distinct definitions"};
    }
  }
  std::vector<uint32_t> Order;
  for (const auto &[Key, I] : Keys) {
    (void)Key;
    Work.charge();
    Order.push_back(I);
  }
  return Order;
}
class Check {
  const DataFlowGraph &A, &B;
  const FlowFragmentFingerprints &AK, &BK;
  FlowNeighborhoodMatch &Out;
  Budget Work;
  llvm::DenseMap<uint32_t, uint32_t> Forward, Reverse, Depths;
  struct Pending {
    uint32_t A, B;
    unsigned Depth;
  };
  std::vector<Pending> Queue;

  void same(bool Good, const char *Reason) {
    Work.charge();
    if (!Good)
      throw Refused{FlowMatchOutcome::Different, Reason};
  }
  void bind(uint32_t X, uint32_t Y) {
    Work.charge();
    const auto [F, NewF] = Forward.try_emplace(X, Y);
    const auto [R, NewR] = Reverse.try_emplace(Y, X);
    same((NewF || F->second == Y) && (NewR || R->second == X),
         "fragment internal or external definitions have conflicting bindings");
  }
  void node(Pending P) {
    Work.charge();
    bind(P.A, P.B);
    auto [At, New] = Depths.try_emplace(P.A, P.Depth);
    if (!New && At->second >= P.Depth)
      return;
    At->second = P.Depth;
    const auto &X = A.Nodes[P.A], &Y = B.Nodes[P.B];
    same(admitted(X, Work) && admitted(Y, Work),
         "fragment reaches a PHI, effect, control or unknown input");
    Work.charge(X.Semantics.size() + Y.Semantics.size());
    same(detail::sameFlowOperation(X, Y) && X.Inputs.size() == Y.Inputs.size(),
         "fragment operation or exact contract differs");
    Out.Gaps |= A.Blocks[X.Block].Gaps | B.Blocks[Y.Block].Gaps;
    const auto XI =
        operandOrder(X, P.Depth > 1 ? &AK.Layers[P.Depth - 2] : nullptr, Work);
    const auto YI =
        operandOrder(Y, P.Depth > 1 ? &BK.Layers[P.Depth - 2] : nullptr, Work);
    for (size_t I = 0; I < XI.size(); ++I) {
      const auto &U = X.Inputs[XI[I]], &V = Y.Inputs[YI[I]];
      Work.charge(U.Semantics.size() + V.Semantics.size());
      same(detail::sameFlowOperandShape(U, V),
           "fragment operand type, width or provenance differs");
      if (U.Type == FlowOperand::Kind::Node) {
        bind(U.Node, V.Node);
        if (P.Depth > 1) {
          Work.charge();
          Queue.push_back({U.Node, V.Node, P.Depth - 1});
        }
      } else {
        same(U.Value == V.Value && U.AddressOwner == V.AddressOwner,
             "fragment scalar bits, parameters or address owners differ");
      }
    }
  }
  bool hasCycle() {
    llvm::DenseMap<uint32_t, uint32_t> Counts;
    llvm::DenseMap<uint32_t, std::vector<uint32_t>> Users;
    std::vector<uint32_t> Ready;
    for (const auto &[N, Depth] : Depths) {
      (void)Depth;
      Work.charge();
      auto &Count = Counts[N];
      for (const auto &I : A.Nodes[N].Inputs) {
        Work.charge();
        if (I.Type == FlowOperand::Kind::Node && Depths.contains(I.Node)) {
          ++Count;
          Users[I.Node].push_back(N);
        }
      }
      if (!Count)
        Ready.push_back(N);
    }
    for (size_t I = 0; I < Ready.size(); ++I) {
      Work.charge();
      for (auto N : Users[Ready[I]]) {
        Work.charge();
        if (!--Counts[N])
          Ready.push_back(N);
      }
    }
    return Ready.size() != Depths.size();
  }

public:
  Check(const DataFlowGraph &A, const FlowFragmentFingerprints &AK,
        const DataFlowGraph &B, const FlowFragmentFingerprints &BK,
        uint64_t MaxWork, const ComparisonControl &Control,
        FlowNeighborhoodMatch &Out)
      : A(A), B(B), AK(AK), BK(BK), Out(Out), Work(Out.Work, MaxWork, Control) {
  }
  void run(uint32_t RootA, uint32_t RootB, unsigned Depth) {
    Work.poll();
    Out.Gaps = A.Gaps | B.Gaps;
    Work.charge();
    Queue.push_back({RootA, RootB, Depth});
    for (size_t I = 0; I < Queue.size(); ++I)
      node(Queue[I]);
    std::vector<std::pair<uint32_t, uint32_t>> Nodes, Frontier;
    for (const auto &[X, Y] : Forward) {
      Work.charge();
      (Depths.contains(X) ? Nodes : Frontier).emplace_back(X, Y);
    }
    for (auto *Pairs : {&Nodes, &Frontier})
      std::sort(Pairs->begin(), Pairs->end(),
                [&](const auto &X, const auto &Y) {
                  Work.charge();
                  return X < Y;
                });
    const bool Cycle = hasCycle();
    Work.poll();
    Out.OpenDataFrontier = !Frontier.empty() || Out.Gaps || Cycle;
    Out.Nodes = std::move(Nodes);
    Out.Frontier = std::move(Frontier);
    Out.Outcome = FlowMatchOutcome::Matched;
  }
};
} // namespace

FlowFragmentFingerprints
fingerprintFlowFragments(const DataFlowGraph &G, const DataFlowOptions &Options,
                         const ComparisonControl &Control) {
  FlowFragmentFingerprints Out;
  try {
    if (auto Error = G.validate(Options, Control))
      throw std::invalid_argument(*Error);
    Budget Work(Out.Work, Options.MaxWork, Control);
    Work.poll();
    std::vector<uint64_t> Local;
    std::vector<uint8_t> Allowed;
    for (const auto &N : G.Nodes) {
      Work.charge(1 + N.Semantics.size() + N.Origins.size());
      Local.push_back(detail::flowOperationKey(N));
      Allowed.push_back(admitted(N, Work));
    }
    for (unsigned Depth = 0; Depth < Options.Rounds; ++Depth) {
      Work.charge(G.Nodes.size() * 2);
      FlowFragmentLayer Layer;
      Layer.Keys.resize(G.Nodes.size());
      Layer.Admitted.resize(G.Nodes.size());
      const auto *Children = Depth ? &Out.Layers.back() : nullptr;
      for (uint32_t I = 0; I < G.Nodes.size(); ++I) {
        Work.charge();
        if (!Allowed[I])
          continue;
        const auto &N = G.Nodes[I];
        bool Complete = true;
        std::vector<uint64_t> Inputs;
        for (const auto &V : N.Inputs) {
          Inputs.push_back(operandKey(V, Children, Work));
          if (Children && V.Type == FlowOperand::Kind::Node &&
              !Children->Admitted[V.Node])
            Complete = false;
        }
        if (!Complete)
          continue;
        if (N.Commutative)
          std::sort(Inputs.begin(), Inputs.end(), [&](auto X, auto Y) {
            Work.charge();
            return X < Y;
          });
        StableHash H;
        H.add(Local[I]).add(N.Commutative);
        for (auto V : Inputs) {
          Work.charge();
          H.add(V);
        }
        Layer.Keys[I] = H.value();
        Layer.Admitted[I] = true;
      }
      Out.Layers.push_back(std::move(Layer));
    }
    Work.poll();
  } catch (const Stopped &Stop) {
    Out.State = Stop.State;
    Out.Layers.clear();
  } catch (const ComparisonInterrupted &Stop) {
    Out.State = Stop.State;
    Out.Layers.clear();
  }
  return Out;
}

FlowFragmentInput::FlowFragmentInput(const DataFlowGraph &G,
                                     const FlowFragmentFingerprints &K,
                                     const DataFlowOptions &Options,
                                     const ComparisonControl &Control)
    : Graph(G), Keys(K) {
  if (auto Error = G.validate(Options, Control))
    throw std::invalid_argument(*Error);
  if (K.State != ComparisonState::Complete || K.Layers.size() != Options.Rounds)
    throw std::invalid_argument("fragment input needs completed depth keys");
  for (const auto &L : K.Layers)
    if (L.Keys.size() != G.Nodes.size() || L.Admitted.size() != G.Nodes.size())
      throw std::invalid_argument("fragment depth keys do not cover the graph");
}
FlowNeighborhoodMatch matchFlowFragment(const FlowFragmentInput &Primary,
                                        uint32_t RootA,
                                        const FlowFragmentInput &Secondary,
                                        uint32_t RootB, unsigned Depth,
                                        uint64_t MaxWork,
                                        const ComparisonControl &Control) {
  const auto &A = Primary.Graph, &B = Secondary.Graph;
  if (!Depth || Depth > Primary.Keys.Layers.size() ||
      Depth > Secondary.Keys.Layers.size() ||
      MaxWork > limits::diff::kMaxFlowWork || RootA >= A.Nodes.size() ||
      RootB >= B.Nodes.size())
    throw std::invalid_argument("fragment root, depth or allowance is invalid");
  FlowNeighborhoodMatch Out;
  if (A.Profile != B.Profile || A.ABI != B.ABI) {
    Out.Outcome = FlowMatchOutcome::Unsupported;
    Out.Reason = "fragment profile or ABI pair is not admitted";
    return Out;
  }
  try {
    Check(A, Primary.Keys, B, Secondary.Keys, MaxWork, Control, Out)
        .run(RootA, RootB, Depth);
  } catch (const Stopped &Stop) {
    Out.State = Stop.State;
    Out.Reason = "fragment verification stopped";
  } catch (const Refused &Stop) {
    Out.Outcome = Stop.Outcome;
    Out.Reason = Stop.Reason;
  }
  return Out;
}
} // namespace neverd::diff

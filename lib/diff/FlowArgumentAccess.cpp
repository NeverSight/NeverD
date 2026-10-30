//===- FlowArgumentAccess.cpp - Local address relationships ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Memoize same-width affine SSA addresses and join their memory occurrences
/// to recovered call arguments without modeling callee effects or memory SSA.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowArgumentAccess.h"

#include "neverd/ir/low/LowIR.h"

#include <algorithm>
#include <compare>
#include <stdexcept>
#include <utility>

namespace neverd::diff {
namespace {
struct Stopped {
  ComparisonState State;
};
struct Address {
  FlowOperand::Kind Kind = FlowOperand::Kind::Unknown;
  uint64_t Root = 0, Offset = 0;
  uint16_t Bytes = 0;
  bool valid() const { return Kind != FlowOperand::Kind::Unknown; }
  auto operator<=>(const Address &) const = default;
};
uint64_t mask(uint16_t Bytes) {
  return Bytes == sizeof(uint64_t) ? UINT64_MAX
                                   : (uint64_t(1) << (Bytes * 8)) - 1;
}
bool scalar(const FlowOperand &I, uint16_t Bytes) {
  return I.Type == FlowOperand::Kind::Constant && I.Bytes == Bytes &&
         I.Provenance == uint8_t(ConstantAddressProvenance::Scalar);
}
} // namespace

std::optional<std::string> FlowArgumentAccessOptions::validate() const {
  if (!MaxRecords || MaxRecords > limits::diff::kMaxFlowAccessRecords ||
      !MaxWork || MaxWork > limits::diff::kMaxFlowWork)
    return "argument access allowance is outside its bounds";
  return {};
}

FlowArgumentAccesses observeFlowArgumentAccesses(
    const DataFlowGraph &G, const FlowArgumentAccessOptions &O,
    const DataFlowOptions &GraphOptions, const ComparisonControl &Control) {
  if (auto Error = O.validate())
    throw std::invalid_argument(*Error);
  if (G.Profile != DataFlowProfile)
    throw std::invalid_argument("argument accesses require med-ssa-v2");
  FlowArgumentAccesses R;
  const auto Poll = [&] {
    if (auto Stop = Control.stop())
      throw Stopped{*Stop};
  };
  const auto Charge = [&](uint64_t Count = 1) {
    if (Count > O.MaxWork - R.Work)
      throw Stopped{ComparisonState::ResourceLimited};
    const auto Before = R.Work;
    R.Work += Count;
    if (Before / limits::diff::kFlowPollWork !=
        R.Work / limits::diff::kFlowPollWork)
      Poll();
  };
  try {
    Poll();
    if (auto Error = G.validate(GraphOptions, Control))
      throw std::invalid_argument(*Error);
    R.Gaps = G.Gaps;
    for (const auto &B : G.Blocks) {
      Charge(1 + B.Successors.size());
      R.Gaps |= B.Gaps;
    }
    // One dependent address per affine definition. Explicit iterative walks
    // avoid a host-stack dependency on the length or order of SSA definitions.
    std::vector<Address> Keys(G.Nodes.size());
    std::vector<uint32_t> Dependency(G.Nodes.size(), NoIndex);
    std::vector<uint64_t> Offsets(G.Nodes.size());
    std::vector<uint8_t> State(G.Nodes.size());
    const auto Operand = [&](const FlowOperand &I) -> Address {
      if (!I.Bytes || I.Bytes > sizeof(uint64_t))
        return {};
      if (I.Type == FlowOperand::Kind::Parameter)
        return {I.Type, I.Value, 0, I.Bytes};
      if (I.Type == FlowOperand::Kind::Node &&
          G.Nodes[I.Node].ResultBytes == I.Bytes)
        return Keys[I.Node];
      return {};
    };
    std::vector<uint32_t> ReturnQueue;
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      const auto &Node = G.Nodes[N];
      Charge(1 + Node.Inputs.size());
      R.Gaps |= Node.Gaps;
      if (Node.Opcode == "RETURN")
        for (const auto &I : Node.Inputs)
          if (I.Type == FlowOperand::Kind::Node &&
              G.Nodes[I.Node].ResultBytes == I.Bytes)
            ReturnQueue.push_back(I.Node);
      const auto Bytes = Node.ResultBytes;
      State[N] = 2;
      if (!Bytes || Bytes > sizeof(uint64_t) || Node.Opcode == "CALL" ||
          Node.Opcode == "INDIR_CALL" || Node.Opcode == "INTRINSIC")
        continue;
      Keys[N] = {FlowOperand::Kind::Node, N, 0, Bytes};
      const FlowOperand *Base = nullptr;
      uint64_t Offset = 0;
      if (Node.Opcode == "COPY" && Node.Inputs.size() == 1 &&
          Node.Inputs[0].Bytes == Bytes)
        Base = &Node.Inputs[0];
      else if ((Node.Opcode == "INT_ADD" || Node.Opcode == "INT_SUB") &&
               Node.Inputs.size() == 2) {
        if (scalar(Node.Inputs[1], Bytes)) {
          Base = &Node.Inputs[0];
          Offset = Node.Opcode == "INT_SUB" ? -Node.Inputs[1].Value
                                            : Node.Inputs[1].Value;
        } else if (Node.Opcode == "INT_ADD" && scalar(Node.Inputs[0], Bytes)) {
          Base = &Node.Inputs[1];
          Offset = Node.Inputs[0].Value;
        }
      }
      if (!Base || Base->Bytes != Bytes)
        continue;
      if (Base->Type == FlowOperand::Kind::Node &&
          G.Nodes[Base->Node].ResultBytes == Bytes) {
        Dependency[N] = Base->Node;
        Offsets[N] = Offset;
        State[N] = 0;
      } else if (Base->Type == FlowOperand::Kind::Parameter) {
        Keys[N] = {Base->Type, Base->Value, Offset & mask(Bytes), Bytes};
      } else if (Base->Type == FlowOperand::Kind::Constant) {
        // Literal addresses require their own provenance contract. This
        // observation follows shared definitions and parameters only.
        Keys[N] = {};
      }
      // A raw unknown remains this definition's own root. Two unknown inputs
      // at separate operations cannot become the same incoming stack value.
    }
    std::vector<uint32_t> Pending;
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      Charge();
      uint32_t At = N;
      while (State[At] == 0) {
        Charge();
        State[At] = 1;
        Pending.push_back(At);
        At = Dependency[At];
      }
      if (State[At] == 1) {
        // A cyclic affine chain has no established root.
        for (auto P : Pending) {
          Charge();
          Keys[P] = {};
          State[P] = 2;
        }
        Pending.clear();
      }
      while (!Pending.empty()) {
        Charge();
        At = Pending.back();
        Pending.pop_back();
        Keys[At] = Keys[Dependency[At]];
        if (Keys[At].valid())
          Keys[At].Offset =
              (Keys[At].Offset + Offsets[At]) & mask(Keys[At].Bytes);
        State[At] = 2;
      }
    }
    // A value-preserving path through COPY/PHI is observable without deciding
    // which predecessor executes. Arithmetic and memory effects stop the path.
    std::vector<uint8_t> Returned(G.Nodes.size());
    for (size_t I = 0; I < ReturnQueue.size(); ++I) {
      Charge();
      const auto N = ReturnQueue[I];
      if (Returned[N])
        continue;
      Returned[N] = 1;
      const auto &Node = G.Nodes[N];
      if ((Node.Opcode != "COPY" || Node.Inputs.size() != 1) && !Node.Phi)
        continue;
      for (const auto &Input : Node.Inputs) {
        Charge();
        if (Input.Type == FlowOperand::Kind::Node &&
            Input.Bytes == Node.ResultBytes &&
            G.Nodes[Input.Node].ResultBytes == Input.Bytes)
          ReturnQueue.push_back(Input.Node);
      }
    }
    std::vector<std::pair<Address, uint32_t>> Accesses;
    Accesses.reserve(G.Nodes.size());
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      Charge();
      const auto &Node = G.Nodes[N];
      if (Node.MemoryAddressSpace || Node.MemoryOrdering ||
          !((Node.Opcode == "LOAD" && Node.Inputs.size() == 1 &&
             Node.ResultBytes) ||
            (Node.Opcode == "STORE" && Node.Inputs.size() == 2 &&
             Node.Inputs[1].Bytes)))
        continue;
      const auto Key = Operand(Node.Inputs[0]);
      if (Key.valid())
        Accesses.emplace_back(Key, N);
    }
    std::sort(Accesses.begin(), Accesses.end(),
              [&](const auto &A, const auto &B) {
                Charge();
                return A < B;
              });
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      Charge();
      const auto &Call = G.Nodes[N];
      if (Call.CallArgumentBegin == NoIndex)
        continue;
      for (uint32_t A = 0; A < Call.CallArgumentCount; ++A) {
        Charge();
        const auto Key = Operand(Call.Inputs[Call.CallArgumentBegin + A]);
        if (!Key.valid())
          continue;
        const auto Found =
            std::lower_bound(Accesses.begin(), Accesses.end(), Key,
                             [&](const auto &Entry, const Address &Wanted) {
                               Charge();
                               return Entry.first < Wanted;
                             });
        for (auto At = Found; At != Accesses.end() && At->first == Key; ++At) {
          Charge();
          const auto Access = At->second;
          if (R.Records.size() == O.MaxRecords)
            throw Stopped{ComparisonState::ResourceLimited};
          const auto &Node = G.Nodes[Access];
          const bool Write = Node.Opcode == "STORE";
          R.Records.push_back({N, A, Access,
                               Write ? Node.Inputs[1].Bytes : Node.ResultBytes,
                               Write, !Write && bool(Returned[Access])});
        }
      }
    }
    Poll();
    R.Complete = true;
  } catch (const Stopped &Stop) {
    R.State = Stop.State;
  } catch (const ComparisonInterrupted &Stop) {
    R.State = Stop.State;
  }
  return R;
}
} // namespace neverd::diff

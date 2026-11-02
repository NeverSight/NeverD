//===- FlowBinding.cpp - Authenticate renumbered LLVM PHIs ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep exact instruction order and source facts while permitting PHIs to be
/// renumbered. This is a complete bijection, never an equivalence heuristic.
//===----------------------------------------------------------------------===//

#include "FlowBinding.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::diff {
bool sameLLVMGraphAfterPhiRenumbering(
    const DataFlowGraph &Saved, const DataFlowGraph &Fresh,
    const DataFlowOptions &Options,
    llvm::function_ref<void(uint64_t)> ChargeWork,
    const ComparisonControl &Control) {
  const auto Charge = [&](uint64_t Amount) {
    Control.check();
    ChargeWork(Amount);
    Control.check();
  };
  Charge(1);
  if (!isLLVMDataFlowProfile(Saved.Profile) || Saved.Profile != Fresh.Profile ||
      Saved.Nodes.size() != Fresh.Nodes.size() ||
      Saved.Blocks.size() != Fresh.Blocks.size())
    return false;
  if (Fresh.validate(Options, Control))
    return false;
  const auto Count = Saved.Nodes.size();
  Charge(3 * uint64_t(Count));
  std::vector<uint32_t> ToFresh(Count, NoIndex), ToSaved(Count, NoIndex), Queue;
  Queue.reserve(Count);
  const auto Pair = [&](uint32_t A, uint32_t B) {
    Charge(1);
    if (Saved.Nodes[A].Phi != Fresh.Nodes[B].Phi ||
        Saved.Nodes[A].Block != Fresh.Nodes[B].Block)
      return false;
    if (ToFresh[A] != NoIndex || ToSaved[B] != NoIndex)
      return ToFresh[A] == B && ToSaved[B] == A;
    ToFresh[A] = B;
    ToSaved[B] = A;
    Queue.push_back(A);
    return true;
  };
  // Every ordinary instruction is an anchor in its existing order and block.
  // Only PHIs may acquire different positions; never search or guess a match.
  uint32_t A = 0, B = 0;
  while (A < Count || B < Count) {
    while (A < Count && Saved.Nodes[A].Phi) {
      Charge(1);
      ++A;
    }
    while (B < Count && Fresh.Nodes[B].Phi) {
      Charge(1);
      ++B;
    }
    if (A == Count || B == Count) {
      if (A != B)
        return false;
      break;
    }
    if (!Pair(A++, B++))
      return false;
  }
  for (size_t At = 0; At < Queue.size(); ++At) {
    Charge(1);
    const auto &X = Saved.Nodes[Queue[At]],
               &Y = Fresh.Nodes[ToFresh[Queue[At]]];
    if (X.Inputs.size() != Y.Inputs.size())
      return false;
    for (size_t I = 0; I < X.Inputs.size(); ++I) {
      Charge(1);
      if (X.Inputs[I].Type != Y.Inputs[I].Type)
        return false;
      if (X.Inputs[I].Type == FlowOperand::Kind::Node &&
          !Pair(X.Inputs[I].Node, Y.Inputs[I].Node))
        return false;
    }
  }
  // A dead PHI-only cycle has no anchored identity. Refuse it instead of
  // turning a structural authentication check into graph-isomorphism search.
  if (Queue.size() != Count)
    return false;
  const auto Payload = [&](const DataFlowGraph &G) {
    Charge(1 + G.Revision.size() + G.Profile.size() + G.ABI.size() +
           G.Nodes.size() + G.Blocks.size() + G.ParameterViews.size());
    for (const auto &N : G.Nodes) {
      Charge(1 + N.Opcode.size() + N.Semantics.size() + N.Origins.size() +
             N.Inputs.size());
      for (const auto &I : N.Inputs)
        Charge(1 + I.Semantics.size());
    }
    for (const auto &Block : G.Blocks)
      Charge(1 + Block.Successors.size());
    for (const auto &View : G.ParameterViews)
      Charge(1 + View.Attributes.size());
    if (G.Progress)
      Charge(1 + G.Progress->SourceContract.size() +
             G.Progress->ObservedContract.size() + G.Progress->Loops.size());
  };
  Payload(Fresh);
  auto Reordered = Fresh;
  for (auto &N : Reordered.Nodes) {
    Charge(1);
    for (auto &Input : N.Inputs) {
      Charge(1);
      if (Input.Type == FlowOperand::Kind::Node)
        Input.Node = ToSaved[Input.Node];
    }
  }
  for (auto &Block : Reordered.Blocks) {
    Charge(1);
    if (Block.Terminator != NoIndex)
      Block.Terminator = ToSaved[Block.Terminator];
  }
  if (Reordered.Progress)
    for (auto &Loop : Reordered.Progress->Loops) {
      Charge(1);
      Loop.Counter = ToSaved[Loop.Counter];
      Loop.Update = ToSaved[Loop.Update];
      Loop.Compare = ToSaved[Loop.Compare];
    }
  // Consume the inverse permutation in place; each swap fixes one position.
  // References are already translated, and no second node array is needed.
  for (uint32_t I = 0; I < Count; ++I) {
    Charge(1);
    while (ToSaved[I] != I) {
      Charge(1);
      const auto Other = ToSaved[I];
      std::swap(Reordered.Nodes[I], Reordered.Nodes[Other]);
      std::swap(ToSaved[I], ToSaved[Other]);
    }
  }
  Payload(Saved);
  // Default equality includes all present and future metadata fields. Only
  // the node-number fields above have been transformed; retain Saved itself.
  return Saved == Reordered;
}
} // namespace neverd::diff

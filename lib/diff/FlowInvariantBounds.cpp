//===- FlowInvariantBounds.cpp - Pure input-derived bound DAGs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A fixed coherent choice of input/undef leaves gives a fixed pure bound or
/// UB/poison. PHIs and freeze can change that choice on re-evaluation and are
/// barriers even in an unselected arm. Select itself may mask poison. Cache
/// both acceptance and rejection without recursion or repeated affine walks.
//===----------------------------------------------------------------------===//

#include "FlowInvariantBounds.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Operator.h"

#include <string>
#include <utility>

namespace neverd::diff {
FlowInvariantBounds::FlowInvariantBounds(
    const DataFlowGraph &Graph, llvm::StringRef Contract,
    llvm::function_ref<void(uint64_t)> Charge)
    : G(Graph), Contract(Contract), Charge(Charge) {
  Charge(G.Nodes.size());
  Cache.resize(G.Nodes.size());
}

void FlowInvariantBounds::visit(uint32_t Node) {
  const auto &N = G.Nodes[Node];
  Charge(1 + N.Opcode.size());
  auto &E = Cache[Node];
  if (N.Phi || N.Opcode == "freeze" || N.Opcode == "ret" || N.Opcode == "br" ||
      N.Opcode == "switch") {
    E.Status = State::Invalid;
    return;
  }
  E.Status = State::Active;
  Stack.push_back({Node});
}

void FlowInvariantBounds::finish(uint32_t Node) {
  const auto &N = G.Nodes[Node];
  auto &E = Cache[Node];
  E.Status = State::Invariant;
  E.Bound = {Node, 0};
  if (N.Opcode != "add" && N.Opcode != "sub")
    return;
  const auto *Base = &N.Inputs[0], *Offset = &N.Inputs[1];
  if (N.Opcode == "add" && Base->Type == FlowOperand::Kind::Constant)
    std::swap(Base, Offset);
  // Parameter translations remain whole basis nodes; the complete v6 proof
  // has priority. K-B and casts are also valid bases, never affine peelings.
  if (Base->Type != FlowOperand::Kind::Node ||
      Offset->Type != FlowOperand::Kind::Constant)
    return;
  Charge(1 + N.Semantics.size() + Offset->Semantics.size());
  llvm::StringRef Type(Offset->Semantics), Suffix(N.Semantics);
  unsigned Width = 0, Flags = 0;
  constexpr unsigned WrapFlags =
      llvm::OverflowingBinaryOperator::NoSignedWrap |
      llvm::OverflowingBinaryOperator::NoUnsignedWrap;
  if (!Type.consume_front("i") || Type.getAsInteger(10, Width) ||
      !Suffix.consume_front(Contract) ||
      !Suffix.consume_front(";result=" + Offset->Semantics + ";flags=") ||
      Suffix.getAsInteger(10, Flags) || Suffix != std::to_string(Flags) ||
      (Flags & ~WrapFlags))
    return;
  const auto &Parent = Cache[Base->Node].Bound;
  const llvm::APInt Delta(Width, Offset->Value);
  E.Bound = {Parent.Node, (llvm::APInt(Width, Parent.Offset) +
                           (N.Opcode == "sub" ? -Delta : Delta))
                              .getZExtValue()};
}

std::optional<FlowInvariantBound>
FlowInvariantBounds::observe(const FlowOperand &Input) {
  Charge(1);
  if (Input.Type != FlowOperand::Kind::Node)
    return {};
  const auto Root = Input.Node;
  if (Cache[Root].Status == State::Unknown)
    visit(Root);
  while (!Stack.empty()) {
    Charge(1);
    auto &Top = Stack.back();
    auto &E = Cache[Top.Node];
    const auto &N = G.Nodes[Top.Node];
    if (Top.Next == N.Inputs.size()) {
      finish(Top.Node);
      Stack.pop_back();
      continue;
    }
    const auto &Value = N.Inputs[Top.Next];
    Charge(1 + Value.Semantics.size());
    bool Valid = false;
    switch (Value.Type) {
    case FlowOperand::Kind::Node:
      if (Cache[Value.Node].Status == State::Unknown) {
        visit(Value.Node);
        continue;
      }
      Valid = Cache[Value.Node].Status == State::Invariant;
      break;
    case FlowOperand::Kind::Parameter:
    case FlowOperand::Kind::Constant:
      Valid = true;
      break;
    case FlowOperand::Kind::Unknown: {
      // The shared finite validator has checked the exact scalar width.
      const auto Kind = llvm::StringRef(Value.Semantics).split(';').second;
      Valid = Kind == "undef" || Kind == "poison";
      break;
    }
    default:
      break;
    }
    if (Valid)
      ++Top.Next;
    else {
      // Active children are cycles; invalid children include cached barriers.
      E.Status = State::Invalid;
      Stack.pop_back();
    }
  }
  if (Cache[Root].Status == State::Invariant)
    return Cache[Root].Bound;
  return {};
}
} // namespace neverd::diff

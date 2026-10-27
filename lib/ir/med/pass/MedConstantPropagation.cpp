//===- MedConstantPropagation.cpp - Stable MedIR value propagation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file Propagates invariant SSA constants and their width-view origins.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedConstantPropagation.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int>;
// Constant operands never enter the definition map. Reserve that kind for
// sentinels so every nonconstant id/SSA version, including INT_MAX, stays
// legal.
struct ConstantKeyInfo : llvm::DenseMapInfo<Key> {
  static constexpr Key getEmptyKey() { return {MedVar::Const, -1, -1}; }
  static constexpr Key getTombstoneKey() { return {MedVar::Const, -2, -2}; }
};
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }

enum class Knowledge { Pending, Constant, Varying };
struct ConstantNode {
  MedVar Output;
  std::vector<MedVar> Inputs;
  llvm::SmallVector<ConstantNode *, 1> Users;
  Knowledge State = Knowledge::Pending;
  MedVar Value;
  bool Queued = false;
  /// A width view of the single input, an extension or the bytes SUBBYTES
  /// takes from ViewOffset, rather than a copy or a PHI.
  NdOp View = NdOp::COPY;
  uint64_t ViewOffset = 0;
  /// The value passes through a width view on the way here.
  bool FromView = false;
};

/// The constant \p View makes of constant \p In, sized \p Size: the view
/// of a register that `xor edx, edx` zeroes and the next instruction reads
/// as RDX is still that constant, and a number like it.  A view of an
/// address is no address, and none is made.
std::optional<MedVar> viewOfConstant(NdOp View, uint64_t ViewOffset,
                                     const MedVar &In, uint16_t Size) {
  if (In.Provenance != ConstantAddressProvenance::Unknown &&
      In.Provenance != ConstantAddressProvenance::Scalar)
    return std::nullopt;
  uint64_t Bits = In.ConstVal;
  const unsigned InBits = In.Size * 8u;
  if (InBits < 64)
    Bits &= (uint64_t(1) << InBits) - 1;
  if (View == NdOp::SUBBYTES)
    Bits = ViewOffset < 8 ? Bits >> (ViewOffset * 8) : 0;
  else if (View == NdOp::INT_SEXT && InBits && InBits < 64 &&
           ((Bits >> (InBits - 1)) & 1))
    Bits |= ~uint64_t(0) << InBits;
  if (Size < 8)
    Bits &= (uint64_t(1) << (Size * 8u)) - 1;
  return MedVar::makeConst(Bits, Size, In.Provenance);
}
} // namespace

std::optional<bool> selfComparisonConstant(NdOp Opcode, const MedVar &A,
                                           const MedVar &B) {
  if ((A.Kind != MedVar::Reg && A.Kind != MedVar::Temp &&
       A.Kind != MedVar::Param && A.Kind != MedVar::Const) ||
      (!A.isConst() && A.Id < 0) || !A.Size || A.Size != B.Size || A != B)
    return std::nullopt;
  switch (Opcode) {
  case NdOp::INT_EQUAL:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
    return true;
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_SBOR:
    return false;
  default:
    return std::nullopt;
  }
}

namespace {
bool hasCompleteOrdinaryPredecessors(const MedBlock &Block,
                                     const std::set<int> &Incoming) {
  if (Block.Preds.empty() || Block.Preds.size() != Incoming.size())
    return false;
  llvm::SmallVector<int, 4> Predecessors(Block.Preds.begin(),
                                         Block.Preds.end());
  std::sort(Predecessors.begin(), Predecessors.end());
  return std::equal(Predecessors.begin(), Predecessors.end(), Incoming.begin());
}

bool hasCompleteOrdinaryPhiArguments(const PhiNode &Phi,
                                     const std::set<int> &Incoming) {
  if (Phi.ExceptionalEntry || !Phi.Output.Size ||
      Phi.Args.size() != Incoming.size())
    return false;
  llvm::SmallVector<int, 4> Edges;
  for (const auto &[Pred, Value] : Phi.Args) {
    if (Value.Size != Phi.Output.Size)
      return false;
    Edges.push_back(Pred);
  }
  std::sort(Edges.begin(), Edges.end());
  // Equality with the unique incoming edges also rejects duplicate arguments.
  return std::equal(Edges.begin(), Edges.end(), Incoming.begin());
}
} // namespace

bool hasCompleteOrdinaryPhiInputs(const MedBlock &Block, const PhiNode &Phi,
                                  const std::set<int> &Incoming) {
  return hasCompleteOrdinaryPredecessors(Block, Incoming) &&
         hasCompleteOrdinaryPhiArguments(Phi, Incoming);
}

bool propagateInvariantConstants(MedFunc &Func) {
  if (Func.Blocks.size() >
      ::neverd::limits::ir::kMedConstantPropagationMaxDefinitions)
    return false;
  size_t Budget = ::neverd::limits::ir::
      kMedConstantPropagationPropagateInvariantConstantsBudget;
  size_t Definitions = 0;
  std::map<int, const MedBlock *> Blocks;
  std::map<int, std::set<int>> Incoming;
  for (const auto &Block : Func.Blocks) {
    Definitions += Block.Ops.size() + Block.Phis.size();
    if (Definitions >
            ::neverd::limits::ir::kMedConstantPropagationMaxDefinitions ||
        Block.Id < 0 || !Blocks.emplace(Block.Id, &Block).second ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return false;
    for (int Successor : Block.Succs) {
      if (!Budget--)
        return false;
      Incoming[Successor].insert(Block.Id);
    }
  }
  for (const auto &[Id, Predecessors] : Incoming)
    if (!Blocks.count(Id))
      return false;

  // Keep the large nodes outside the hash table: resizing the SSA index need
  // only move keys and positions, while node storage stays contiguous.
  llvm::DenseMap<Key, size_t, ConstantKeyInfo> DefinitionIndex;
  std::vector<ConstantNode> Nodes;
  Nodes.reserve(Definitions);
  auto Find = [&](const MedVar &Value) -> ConstantNode * {
    auto It = DefinitionIndex.find(key(Value));
    return It == DefinitionIndex.end() ? nullptr : &Nodes[It->second];
  };
  auto Add = [&](const MedVar &Output, std::vector<MedVar> Inputs, bool Valid) {
    if (Output.isConst() || Output.Id < 0 || !Output.Size)
      return;
    auto [It, Inserted] =
        DefinitionIndex.try_emplace(key(Output), Nodes.size());
    if (Inserted)
      Nodes.emplace_back();
    auto &Node = Nodes[It->second];
    if (!Inserted) {
      Node.State = Knowledge::Varying;
      Node.Inputs.clear();
      return;
    }
    Node.Output = Output;
    Node.Inputs = std::move(Inputs);
    if (!Valid || Output.Size > 8 || Node.Inputs.empty())
      Node.State = Knowledge::Varying;
  };
  for (const auto &Block : Func.Blocks) {
    if (!Block.Phis.empty()) {
      const auto &Predecessors = Incoming[Block.Id];
      // Every PHI in this block shares the same control-flow predecessors.
      // Prove that invariant once, then check each PHI's own arguments.
      const bool Complete =
          hasCompleteOrdinaryPredecessors(Block, Predecessors);
      for (const auto &Phi : Block.Phis) {
        if (Phi.Args.size() > Budget)
          return false;
        Budget -= Phi.Args.size();
        std::vector<MedVar> Values;
        for (const auto &[Predecessor, Value] : Phi.Args)
          Values.push_back(Value);
        // The dispatcher's entry brings a value no predecessor supplies.
        Add(Phi.Output, std::move(Values),
            Complete && hasCompleteOrdinaryPhiArguments(Phi, Predecessors));
      }
    }
    for (const auto &Op : Block.Ops) {
      if (Op.NumInputs > Op.Inputs.size())
        return false;
      const bool Plain =
          Op.RegistrationRoot == MedOp::RegistrationRootKind::None &&
          !Op.Dead && !Op.SourceCallHint &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      const bool Copy = Plain && Op.Opcode == NdOp::COPY && Op.NumInputs == 1;
      // A constant holds a word: a view into a wider value (a vector lane, a
      // register pair, a double-word dividend) has none.
      const bool Extend =
          Plain &&
          (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
          Op.NumInputs == 1 && Op.Inputs[0].Size &&
          Op.Inputs[0].Size < Op.Output.Size && Op.Output.Size <= 8;
      const bool Slice =
          Plain && Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[0].Size <= 8 &&
          Op.Inputs[1].ConstVal + Op.Output.Size <= Op.Inputs[0].Size;
      const bool Tracked = Copy || Extend || Slice;
      Add(Op.Output,
          Tracked ? std::vector{Op.Inputs[0]} : std::vector<MedVar>{}, Tracked);
      if ((Extend || Slice) && !Op.Output.isConst() && Op.Output.Id >= 0)
        if (auto *Node = Find(Op.Output)) {
          Node->View = Op.Opcode;
          Node->ViewOffset = Slice ? Op.Inputs[1].ConstVal : 0;
        }
    }
  }

  // No nodes are inserted after this point, so their addresses stay stable.
  // Preserve the ordered-map visitation order, including users and pending
  // cycles: it also determines when the existing shared budget is exhausted.
  std::vector<ConstantNode *> Ordered;
  Ordered.reserve(Nodes.size());
  for (auto &Node : Nodes)
    Ordered.push_back(&Node);
  std::sort(Ordered.begin(), Ordered.end(), [](const auto *A, const auto *B) {
    return key(A->Output) < key(B->Output);
  });
  std::deque<ConstantNode *> Work;
  auto Enqueue = [&](ConstantNode *Node) {
    if (!Node->Queued) {
      Node->Queued = true;
      Work.push_back(Node);
    }
  };
  for (auto *Item : Ordered) {
    auto &Node = *Item;
    for (const auto &Input : Node.Inputs) {
      if (!Budget--)
        return false;
      if (!Input.isConst())
        if (auto *Definition = Find(Input))
          Definition->Users.push_back(&Node);
    }
    Enqueue(&Node);
  }
  auto Drain = [&] {
    while (!Work.empty()) {
      if (!Budget--)
        return false;
      auto &Node = *Work.front();
      Work.pop_front();
      Node.Queued = false;
      if (Node.State == Knowledge::Varying)
        continue;
      Knowledge State = Knowledge::Pending;
      MedVar Value;
      for (const auto &Input : Node.Inputs) {
        if (!Budget--)
          return false;
        if (Node.View == NdOp::COPY && Input.Size != Node.Output.Size) {
          State = Knowledge::Varying;
          break;
        }
        const MedVar *Constant = &Input;
        if (!Input.isConst()) {
          const auto *Definition = Find(Input);
          if (!Definition || Definition->Output.Size != Input.Size ||
              Definition->State == Knowledge::Varying) {
            State = Knowledge::Varying;
            break;
          }
          if (Definition->State == Knowledge::Pending)
            continue;
          Constant = &Definition->Value;
        }
        const std::optional<MedVar> Viewed =
            Node.View == NdOp::COPY
                ? *Constant
                : viewOfConstant(Node.View, Node.ViewOffset, *Constant,
                                 Node.Output.Size);
        if (!Viewed || (State == Knowledge::Constant && Value != *Viewed)) {
          State = Knowledge::Varying;
          break;
        }
        State = Knowledge::Constant;
        Value = *Viewed;
      }
      if (Node.State == Knowledge::Constant && State == Knowledge::Constant &&
          Node.Value != Value)
        State = Knowledge::Varying;
      if (State == Node.State)
        continue;
      Node.State = State;
      Node.Value = Value;
      for (const auto &User : Node.Users)
        Enqueue(User);
    }
    return true;
  };
  if (!Drain())
    return false;
  // An unseeded cycle cannot contribute a defined value. Make every remaining
  // pending component opaque, then invalidate any provisional constants that
  // depended on it. No operand is changed before both fixed points converge.
  for (auto *Item : Ordered) {
    auto &Node = *Item;
    if (Node.State == Knowledge::Pending) {
      Node.State = Knowledge::Varying;
      for (const auto &User : Node.Users)
        Enqueue(User);
    }
  }
  if (!Drain())
    return false;

  // Which constants pass through a width view.  Such a constant reaches the
  // arithmetic that reads it, but not:
  // - a call, which reads its argument registers as its convention or source
  //   signature binds them (an Objective-C message's `W2`);
  // - a copy or another view, which keeps the register chain a later reader
  //   folds;
  // - a shift or a multiplication: a shifted word reads as an int literal in
  //   C (`(uint32_t)(0 >> 32)`), and a scaled index keeps the selector of the
  //   jump table it addresses.
  // The value solver has drained Work. Reuse its dependency graph to visit
  // each reachable node and use edge once, including cycles and shared tails.
  // Rescanning every input would take quadratic work on a reverse-ID chain.
  for (auto *Node : Ordered) {
    if (Node->View != NdOp::COPY) {
      Node->FromView = true;
      Work.push_back(Node);
    }
  }
  while (!Work.empty()) {
    if (!Budget--)
      return false;
    auto *Node = Work.front();
    Work.pop_front();
    for (auto *User : Node->Users) {
      if (!Budget--)
        return false;
      if (!User->FromView) {
        User->FromView = true;
        Work.push_back(User);
      }
    }
  }

  bool Changed = false;
  auto Replace = [&](MedVar &Input, bool Numeric, bool KeepsViews) {
    if (Input.isConst())
      return;
    const auto *Definition = Find(Input);
    if (!Definition || Definition->State != Knowledge::Constant ||
        Definition->Output.Size != Input.Size ||
        (KeepsViews && Definition->FromView))
      return;
    Input = Definition->Value;
    if (Numeric && Input.Provenance == ConstantAddressProvenance::Unknown)
      Input.Provenance = ConstantAddressProvenance::Scalar;
    Changed = true;
  };
  for (auto &Block : Func.Blocks) {
    for (auto &Phi : Block.Phis)
      for (auto &[Predecessor, Value] : Phi.Args)
        Replace(Value, false, false);
    for (auto &Op : Block.Ops) {
      const bool KeepsViews =
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
          Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::SUBBYTES ||
          Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
          Op.Opcode == NdOp::INT_ASHR || Op.Opcode == NdOp::INT_MULT;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        Replace(Op.Inputs[I], isNumericConstantOperand(Op.Opcode, I),
                KeepsViews);
    }
  }
  return Changed;
}
} // namespace neverd

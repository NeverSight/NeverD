//===- HighEntryStackOffsets.h - Shared SSA frame coordinates ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_ENTRYSTACKOFFSETS_H
#define NEVERD_IR_HIGH_ENTRYSTACKOFFSETS_H

#include "../../AffineFrameState.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace neverd::detail {

/// Entry-SP coordinates in one immutable MedFunc. Definition lookup and the
/// affine proof have the same lifetime; a converter creates a fresh instance
/// on each conversion, including a second layout attempt of the same MedFunc.
///
/// A PHI is a must-equality, not a choice of a convenient incoming path. The
/// shared solver checks all edges, balanced cycles and every arithmetic step.
/// No value obtained under a recursive PHI assumption is cached. The graph is
/// linear in the input IR; queries do not re-expand shared predecessors.
class HighEntryStackOffsets {
public:
  HighEntryStackOffsets(const MedFunc &Function, Arch Architecture)
      : Architecture(Architecture),
        StackPointer(getTargetRegInfo(Architecture).StackPointer),
        PointerSize(getTargetRegInfo(Architecture).PointerSize),
        Equations(Consume) {
    for (const MedBlock &Block : Function.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        if (auto [It, Added] = Phis.try_emplace(identity(Phi.Output), &Phi);
            !Added)
          It->second = nullptr;
      for (const MedOp &Op : Block.Ops)
        if (auto [It, Added] =
                Definitions.try_emplace(identity(Op.Output), &Op);
            !Added)
          It->second = nullptr;
    }
  }

  HighEntryStackOffsets(const HighEntryStackOffsets &) = delete;
  HighEntryStackOffsets &operator=(const HighEntryStackOffsets &) = delete;

  const MedOp *uniqueDefinition(const MedVar &Value) const {
    const auto Found = Definitions.find(identity(Value));
    return Found == Definitions.end() ? nullptr : Found->second;
  }

  std::optional<int64_t> offset(const MedVar &Value) {
    if (!carrier(Value))
      return std::nullopt;
    if (entryStack(Value))
      return 0;
    if (!Built)
      build();
    const auto Found = NodesByValue.find(view(Value));
    if (Found == NodesByValue.end())
      return std::nullopt;
    const Node &N = Nodes[Found->second];
    // Even a warm value needs its complete bounded dependency certificate.
    // Another query cannot turn a too-deep definition into a constant leaf.
    if (N.Depth > static_cast<size_t>(limits::kCallArgStoreAddressDepth))
      return std::nullopt;
    return N.Offset;
  }

private:
  using Identity = std::tuple<int, int, int>;
  using View = std::tuple<int, int, int, uint16_t, uint64_t>;
  using Frame = AffineFrameState;
  struct Node {
    MedVar Value;
    Frame::Result Equation;
    std::vector<size_t> Inputs;
    std::vector<size_t> Users;
    std::optional<int64_t> Offset;
    size_t Depth = 0;
  };

  static Identity identity(const MedVar &V) {
    return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
  }
  static View view(const MedVar &V) {
    const uint64_t Storage = V.Kind == MedVar::Reg     ? V.RegOff
                             : V.Kind == MedVar::Stack ? uint64_t(V.StackOff)
                                                       : 0;
    return {static_cast<int>(V.Kind), V.Id, V.SSAVer, V.Size, Storage};
  }
  bool carrier(const MedVar &V) const {
    return !V.isConst() && V.Kind != MedVar::Unspecified &&
           (V.TheArch == Arch::Unknown || V.TheArch == Architecture) &&
           (V.Size == PointerSize || (PointerSize == 4 && V.Size == 8));
  }
  bool entryStack(const MedVar &V) const {
    return V.Kind == MedVar::Reg && V.RegOff == StackPointer && V.SSAVer == 0 &&
           V.Size == PointerSize;
  }
  bool sameView(const MedVar &A, const MedVar &B) const {
    return carrier(A) && carrier(B) && view(A) == view(B);
  }

  static std::optional<int64_t> delta(const MedVar &V, uint16_t Width) {
    if (!V.isConst() || !V.Size || V.Size > 8 || !Width || Width > 8)
      return std::nullopt;
    const auto Mask = [](unsigned Bytes) {
      return Bytes == 8 ? ~uint64_t{0} : (uint64_t{1} << (Bytes * 8)) - 1;
    };
    // Coerce the literal to the operation's width before interpreting the
    // signed displacement: i8(0xf0) in an i64 add is +240, not -16.
    const uint64_t Bits = V.ConstVal & Mask(V.Size) & Mask(Width);
    const uint64_t Sign = uint64_t{1} << (Width * 8 - 1);
    return Bits & Sign ? -1 - static_cast<int64_t>((~Bits) & Mask(Width))
                       : static_cast<int64_t>(Bits);
  }

  size_t node(const MedVar &Value) {
    // All invalid inputs are the same rejected leaf. In particular a
    // constant's numeric value must never become a frame-coordinate anchor.
    if (!carrier(Value))
      return 0;
    const auto Key = view(Value);
    if (auto Found = NodesByValue.find(Key); Found != NodesByValue.end())
      return Found->second;
    const size_t Index = Nodes.size();
    NodesByValue.emplace(Key, Index);
    Nodes.push_back({Value, Equations.reference(), {}, {}, std::nullopt, 0});
    return Index;
  }

  void build() {
    Built = true;
    Nodes.push_back({{}, Equations.reference(), {}, {}, std::nullopt, 0});
    Equations.define(Nodes[0].Equation, Frame::invalid());
    for (const auto &[Key, Definition] : Definitions)
      if (Definition)
        node(Definition->Output);
    for (const auto &[Key, Phi] : Phis)
      if (Phi)
        node(Phi->Output);

    for (size_t I = 1; I < Nodes.size(); ++I) {
      // node() may grow Nodes; never retain a reference into its storage.
      const MedVar Value = Nodes[I].Value;
      const Frame::Result Target = Nodes[I].Equation;
      if (entryStack(Value)) {
        Equations.define(Target, Frame::constant(0));
        continue;
      }
      const auto PhiIt = Phis.find(identity(Value));
      const auto DefIt = Definitions.find(identity(Value));
      if (PhiIt != Phis.end()) {
        const PhiNode *Phi = PhiIt->second;
        if (!Phi || DefIt != Definitions.end() || Phi->ExceptionalEntry ||
            Phi->Args.empty() || !sameView(Value, Phi->Output)) {
          Equations.define(Target, Frame::invalid());
          continue;
        }
        std::vector<Frame::Result> Inputs;
        for (const auto &[Predecessor, Input] : Phi->Args) {
          const size_t Dependency = Input.Size == Value.Size ? node(Input) : 0;
          Nodes[I].Inputs.push_back(Dependency);
          Inputs.push_back(Nodes[Dependency].Equation);
        }
        Equations.define(Target, Equations.merge(Inputs));
        continue;
      }
      const MedOp *Def = DefIt == Definitions.end() ? nullptr : DefIt->second;
      if (!Def || !sameView(Value, Def->Output) ||
          Def->MemoryOrdering != NdMemoryOrdering::None ||
          Def->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          !Def->IntrinsicOutputs.empty()) {
        Equations.define(Target, Frame::invalid());
        continue;
      }
      const bool Copy = Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
                        Def->Inputs[0].Size == Value.Size;
      const bool Extend =
          Def->Opcode == NdOp::INT_ZEXT && Def->NumInputs == 1 &&
          (Def->Inputs[0].Size == Value.Size ||
           (PointerSize == 4 && Def->Inputs[0].Size == 4 && Value.Size == 8));
      const bool Arithmetic =
          (Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
          Def->NumInputs == 2 && Def->Inputs[0].Size == Value.Size;
      const auto Delta =
          Arithmetic ? delta(Def->Inputs[1], Value.Size) : std::nullopt;
      if (!Copy && !Extend && !Delta) {
        Equations.define(Target, Frame::invalid());
        continue;
      }
      const size_t Dependency = node(Def->Inputs[0]);
      Nodes[I].Inputs.push_back(Dependency);
      Frame::Result Input = Nodes[Dependency].Equation;
      if (Arithmetic)
        Input = Equations.adjust(Input, *Delta, Def->Opcode == NdOp::INT_SUB);
      Equations.define(Target, Input);
    }
    for (size_t I = 0; I < Nodes.size(); ++I)
      for (size_t Input : Nodes[I].Inputs)
        Nodes[Input].Users.push_back(I);
    for (Node &N : Nodes) {
      const auto Value = Equations.value(N.Equation);
      if (Value.K == Frame::Kind::Value)
        N.Offset = Value.Offset;
    }
    depths();
  }

  /// Bound a dependency expansion without enumerating all simple paths.
  /// Acyclic definitions use their exact longest path. Cyclic definitions
  /// use the smaller of two safe bounds: distinct SCC members, and one maximal
  /// unary segment per PHI. A non-PHI start also needs its prefix to the first
  /// PHI, which can be visited again before that PHI closes the cycle. All
  /// queries use the same bound, independent of order and previous cache hits.
  void depths() {
    std::vector<bool> Seen(Nodes.size());
    std::vector<size_t> Order;
    for (size_t Root = 0; Root < Nodes.size(); ++Root) {
      if (Seen[Root])
        continue;
      std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
      Seen[Root] = true;
      while (!Stack.empty()) {
        auto &[Current, Next] = Stack.back();
        if (Next == Nodes[Current].Inputs.size()) {
          Order.push_back(Current);
          Stack.pop_back();
          continue;
        }
        const size_t Input = Nodes[Current].Inputs[Next++];
        if (!Seen[Input]) {
          Seen[Input] = true;
          Stack.push_back({Input, 0});
        }
      }
    }
    const size_t Missing = Nodes.size();
    std::vector<size_t> Component(Nodes.size(), Missing), Sizes;
    for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
      if (Component[*It] != Missing)
        continue;
      const size_t ID = Sizes.size();
      Sizes.push_back(0);
      std::vector<size_t> Work{*It};
      Component[*It] = ID;
      for (size_t I = 0; I < Work.size(); ++I) {
        ++Sizes[ID];
        for (size_t User : Nodes[Work[I]].Users)
          if (Component[User] == Missing) {
            Component[User] = ID;
            Work.push_back(User);
          }
      }
    }
    const size_t TooDeep = limits::kCallArgStoreAddressDepth + 1;
    std::vector<size_t> Prefix(Nodes.size(), TooDeep);
    std::vector<std::vector<size_t>> Members(Sizes.size());
    for (size_t I = 0; I < Nodes.size(); ++I)
      Members[Component[I]].push_back(I);
    // Kosaraju numbers dependencies after their users.
    for (size_t C = Sizes.size(); C-- > 0;) {
      size_t External = 0;
      bool Cyclic = Sizes[C] > 1;
      for (size_t Member : Members[C])
        for (size_t Input : Nodes[Member].Inputs) {
          Cyclic |= Input == Member;
          if (Component[Input] != C)
            External = std::max(External, Nodes[Input].Depth + 1);
        }
      if (!Cyclic) {
        Nodes[Members[C].front()].Depth = std::min(TooDeep, External);
        continue;
      }
      // All non-PHI transfers have exactly one input. Propagating backwards
      // from PHIs therefore computes each member's exact prefix once, without
      // repeatedly walking long copy chains. A copy-only cycle has no proof.
      std::vector<size_t> Work;
      for (size_t Member : Members[C])
        if (Phis.count(identity(Nodes[Member].Value))) {
          Prefix[Member] = 0;
          Work.push_back(Member);
        }
      for (size_t I = 0; I < Work.size(); ++I)
        for (size_t User : Nodes[Work[I]].Users)
          if (Component[User] == C && Prefix[User] == TooDeep) {
            Prefix[User] = std::min(TooDeep, Prefix[Work[I]] + 1);
            // A capped prefix is already ineligible. Do not enqueue it: a
            // cycle without a PHI must not turn the cap into repeated work.
            if (Prefix[User] != TooDeep)
              Work.push_back(User);
          }
      // A recursive path stops when it revisits a PHI. Each distinct PHI
      // therefore emits at most one segment to the next PHI. Shared unary
      // chains may be counted in several maxima, which only raises the bound.
      // This avoids counting every parallel arm of a wide, shallow cycle.
      size_t SegmentSum = 0, ExitExtra = 0;
      for (size_t Member : Members[C]) {
        if (Prefix[Member] != 0)
          continue;
        size_t Segment = 0, Exit = 0;
        for (size_t Input : Nodes[Member].Inputs) {
          if (Component[Input] == C)
            Segment = std::max(Segment, Prefix[Input] + 1);
          else
            Exit = std::max(Exit, Nodes[Input].Depth + 1);
        }
        SegmentSum = std::min(TooDeep, SegmentSum + Segment);
        // Only PHIs can leave a cyclic SCC: a unary member whose input were
        // outside could never return. An exiting PHI replaces its internal
        // segment with Exit, rather than using both. Never subtract from the
        // saturated sum; a positive replacement cost is safe after capping.
        if (Exit > Segment)
          ExitExtra = std::max(ExitExtra, Exit - Segment);
      }
      const size_t MemberBound =
          std::min(TooDeep, std::max(Sizes[C], Sizes[C] - 1 + External));
      const size_t SegmentBound = std::min(TooDeep, SegmentSum + ExitExtra);
      const size_t PhiDepth = std::min(MemberBound, SegmentBound);
      for (size_t Member : Members[C])
        Nodes[Member].Depth = std::min(TooDeep, PhiDepth + Prefix[Member]);
    }
  }

  Arch Architecture;
  uint64_t StackPointer;
  uint16_t PointerSize;
  std::map<Identity, const MedOp *> Definitions;
  std::map<Identity, const PhiNode *> Phis;
  // There is no evidence allowance in this source projection. Its work is
  // bounded by the immutable SSA graph and the separately checked depth.
  const std::function<bool(size_t)> Consume = [](size_t) { return true; };
  Frame Equations;
  std::map<View, size_t> NodesByValue;
  std::vector<Node> Nodes;
  bool Built = false;
};

} // namespace neverd::detail

#endif

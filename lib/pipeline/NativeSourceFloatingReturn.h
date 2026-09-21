#ifndef NEVERD_PIPELINE_NATIVE_SOURCE_FLOATING_RETURN_H
#define NEVERD_PIPELINE_NATIVE_SOURCE_FLOATING_RETURN_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::detail {
namespace native_float_return {

using Key = std::tuple<MedVar::VarKind, int, int>;
inline Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

// This is a source-signature candidate, not an alternative machine ABI or a
// substitute for current-image call binding, state preservation or relifting.
// Query only the low eight bytes; the high half of a preserved Q register may
// be undefined. Integer operations and memory loads do not establish a type.
class Proof {
  struct Definition {
    MedVar Value;
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    const MedCallClobber *Clobber = nullptr;
    int Block = -1;
    int Position = -1;
  };
  struct Node {
    MedVar Value;
    std::vector<size_t> Inputs;
    bool Leaf = false;
    bool Typed = false;
  };

  const MedFunc &Function;
  const TargetRegInfo &TRI;
  std::map<int, const MedBlock *> Blocks;
  std::map<int, std::set<int>> Preds;
  std::set<int> Reachable;
  std::map<Key, Definition> Definitions;
  std::map<uint32_t, const MedOp *> Calls;
  std::map<uint32_t, std::pair<int, int>> CallPositions;
  std::map<std::pair<int, int>, bool> Dominance;
  std::map<int, std::set<int>> CanReach;
  std::map<Key, size_t> Indices;
  std::vector<Node> Nodes;
  std::vector<size_t> Roots;
  size_t Remaining = 262144;
  int Entry = -1;

  bool spend() {
    if (!Remaining)
      return false;
    --Remaining;
    return true;
  }

  bool call(const MedOp &Op) const {
    if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
        !Op.CallSiteId || !Op.SourceCallHint)
      return false;
    const auto &Hint = *Op.SourceCallHint;
    std::string Error;
    return Hint.Signature.Architecture == Arch::AArch64 &&
           validateSourceABI(Hint.Signature, Error) &&
           Op.NumInputs == sourceABIParameters(Hint.Signature).size() + 1 &&
           Op.DoesNotReturn == Hint.DoesNotReturn;
  }

  bool define(const MedVar &V, const MedOp *Op, const PhiNode *Phi,
              const MedCallClobber *Clobber, int Block, int Position) {
    if (!V.Size || V.Id < 0 || V.isConst())
      return false;
    return Definitions
        .emplace(key(V), Definition{V, Op, Phi, Clobber, Block, Position})
        .second;
  }

  bool graph() {
    if (Function.Blocks.empty() || Function.Blocks.size() > 4096)
      return false;
    for (const auto &B : Function.Blocks) {
      if (B.Id < 0 || !Blocks.emplace(B.Id, &B).second ||
          !B.ExceptionalSuccs.empty() || !B.ExceptionalPreds.empty())
        return false;
      if (B.StartAddr == Function.Entry) {
        if (Entry >= 0)
          return false;
        Entry = B.Id;
      }
    }
    if (Entry < 0)
      return false;
    for (const auto &[Id, B] : Blocks) {
      std::set<int> Seen;
      for (int S : B->Succs) {
        if (!spend() || !Blocks.count(S) || !Seen.insert(S).second)
          return false;
        Preds[S].insert(Id);
      }
    }
    for (const auto &[Id, B] : Blocks) {
      std::set<int> Declared;
      for (int P : B->Preds)
        if (!spend() || !Declared.insert(P).second)
          return false;
      if (Declared != Preds[Id])
        return false;
    }
    // A backedge PHI at the machine entry does not model the first invocation
    // from outside the function. Do not let its later typed seed stand in for
    // that missing incoming value; this first candidate excludes entry loops.
    if (!Preds[Entry].empty())
      return false;
    std::vector<int> Pending{Entry};
    Reachable.insert(Entry);
    for (size_t I = 0; I != Pending.size(); ++I)
      for (int S : Blocks.at(Pending[I])->Succs)
        if (Reachable.insert(S).second)
          Pending.push_back(S);

    for (const auto &[Id, B] : Blocks) {
      for (const auto &P : B->Phis) {
        if (!spend() || !define(P.Output, nullptr, &P, nullptr, Id, -1))
          return false;
        std::set<int> Seen;
        for (const auto &[Pred, V] : P.Args)
          if (!spend() || !Seen.insert(Pred).second || V.Size != P.Output.Size)
            return false;
        if (Seen.empty() || Seen != Preds[Id])
          return false;
      }
      if (B->Ops.size() > 65536)
        return false;
      for (size_t Position = 0; Position != B->Ops.size(); ++Position) {
        const auto &Op = B->Ops[Position];
        if (!spend())
          return false;
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          if (!call(Op) || !Calls.emplace(Op.CallSiteId, &Op).second)
            return false;
          CallPositions.emplace(Op.CallSiteId, std::make_pair(Id, Position));
        }
        const bool Seed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                          Op.Output == Op.Inputs[0] &&
                          Op.Output.Size == Op.Inputs[0].Size;
        if (Op.Output.Size && !Seed &&
            !define(Op.Output, &Op, nullptr, nullptr, Id, Position))
          return false;
      }
    }
    for (const auto &C : Function.CallClobbers) {
      const auto Owner = CallPositions.find(C.CallSiteId);
      if (!spend() || Owner == CallPositions.end() ||
          !define(C.Value, nullptr, nullptr, &C, Owner->second.first,
                  Owner->second.second))
        return false;
    }
    return true;
  }

  // Verify availability at the actual use, including each PHI predecessor and
  // the instant before a call's preserved-prefix definition. A reachable typed
  // call in a sibling block is not a definition for this path.
  bool available(const Definition &D, int UseBlock, int UsePosition) {
    if (!Reachable.count(D.Block) || !Reachable.count(UseBlock))
      return false;
    if (D.Block == UseBlock)
      return D.Position < UsePosition;
    const auto Query = std::make_pair(D.Block, UseBlock);
    if (const auto It = Dominance.find(Query); It != Dominance.end())
      return It->second;
    bool Dominates = true;
    std::set<int> Seen{D.Block};
    std::vector<int> Pending;
    if (Seen.insert(Entry).second)
      Pending.push_back(Entry);
    for (size_t I = 0; I != Pending.size(); ++I) {
      if (!spend())
        return false;
      if (Pending[I] == UseBlock) {
        Dominates = false;
        break;
      }
      for (int S : Blocks.at(Pending[I])->Succs)
        if (Seen.insert(S).second)
          Pending.push_back(S);
    }
    Dominance.emplace(Query, Dominates);
    return Dominates;
  }

  bool survivesCalls(const Definition &D, int UseBlock, int UsePosition) {
    if (D.Value.Kind != MedVar::Reg ||
        TRI.callPreservedPrefixSize(D.Value.RegOff, D.Value.Size) >= 8)
      return true;
    auto [Ancestors, Added] =
        CanReach.emplace(UseBlock, std::set<int>{UseBlock});
    if (Added) {
      std::vector<int> Pending{UseBlock};
      for (size_t I = 0; I != Pending.size(); ++I)
        for (int P : Preds.at(Pending[I])) {
          if (!spend())
            return false;
          if (Ancestors->second.insert(P).second)
            Pending.push_back(P);
        }
    }
    std::set<int> Seen{D.Block};
    std::vector<int> Pending{D.Block};
    for (size_t I = 0; I != Pending.size(); ++I) {
      const int Id = Pending[I];
      const auto &B = *Blocks.at(Id);
      const int End = Id == UseBlock ? UsePosition : B.Ops.size();
      const int Begin = Id == D.Block ? D.Position + 1 : 0;
      for (int Position = Begin; Position < End; ++Position) {
        if (!spend())
          return false;
        const auto &Op = B.Ops[Position];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          return false;
      }
      if (Id == UseBlock)
        continue;
      for (int S : B.Succs)
        if (Ancestors->second.count(S) && Seen.insert(S).second)
          Pending.push_back(S);
    }
    return true;
  }

  std::optional<size_t> need(const MedVar &V, int UseBlock, int UsePosition) {
    if (!spend() || V.Size < 8 || V.Size > 16)
      return std::nullopt;
    if (V.isConst()) {
      if (V.Size != 8 || Nodes.size() >= 4096)
        return std::nullopt;
      Nodes.push_back({V, {}, true, false});
      return Nodes.size() - 1;
    }
    if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
      return std::nullopt;
    auto D = Definitions.find(key(V));
    if (D == Definitions.end() || D->second.Value.Size != V.Size ||
        (V.Kind == MedVar::Reg && D->second.Value.RegOff != V.RegOff) ||
        D->second.Value.TheArch != V.TheArch ||
        !available(D->second, UseBlock, UsePosition) ||
        !survivesCalls(D->second, UseBlock, UsePosition))
      return std::nullopt;
    auto [I, Added] = Indices.emplace(key(V), Nodes.size());
    if (Added) {
      if (Nodes.size() >= 4096)
        return std::nullopt;
      Nodes.push_back({V, {}, false, false});
    }
    return I->second;
  }

  bool returns() {
    for (int Id : Reachable) {
      const auto &B = *Blocks.at(Id);
      std::optional<MedVar> Last;
      bool Returned = false;
      for (size_t I = 0; I != B.Ops.size(); ++I) {
        const auto &Op = B.Ops[I];
        if (!spend())
          return false;
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          Last.reset();
        const auto &V = Op.Output;
        if (V.Kind == MedVar::Reg && V.Size &&
            V.RegOff <= TRI.FPReturnReg + 7 &&
            (V.RegOff >= TRI.FPReturnReg ||
             TRI.FPReturnReg - V.RegOff < V.Size)) {
          Last.reset();
          const bool Seed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                            V == Op.Inputs[0] && V.Size == Op.Inputs[0].Size;
          if (!Seed && V.RegOff == TRI.FPReturnReg &&
              (V.Size == 8 || V.Size == 16))
            Last = V;
        }
        if (Op.Opcode != NdOp::RETURN)
          continue;
        if (!Last || I + 1 != B.Ops.size() || !B.Succs.empty())
          return false;
        auto Root = need(*Last, Id, I);
        if (!Root)
          return false;
        Roots.push_back(*Root);
        Returned = true;
      }
      if (B.Succs.empty() && !Returned) {
        if (B.Ops.empty())
          return false;
        const auto &LastOp = B.Ops.back();
        if (!call(LastOp) || !LastOp.DoesNotReturn ||
            LastOp.SourceCallHint->Signature.ReturnType->Kind !=
                NdTypeKind::Void)
          return false;
      }
    }
    return !Roots.empty();
  }

  bool values() {
    for (size_t I = 0; I != Nodes.size(); ++I) {
      if (Nodes[I].Leaf)
        continue;
      const auto D = Definitions.at(key(Nodes[I].Value));
      std::vector<std::tuple<MedVar, int, int>> Inputs;
      const auto Input = [&](const MedVar &V) {
        Inputs.emplace_back(V, D.Block, D.Position);
      };
      if (D.Phi) {
        for (const auto &[Pred, V] : D.Phi->Args) {
          if (!Reachable.count(Pred))
            return false;
          Inputs.emplace_back(V, Pred, Blocks.at(Pred)->Ops.size());
        }
      } else if (D.Clobber) {
        const auto &C = *D.Clobber;
        const auto &Owner = *Calls.at(C.CallSiteId);
        if (C.Value.Kind != MedVar::Reg || C.Value.Size != 16 ||
            C.Value.TheArch != Arch::AArch64 || C.PreservedPrefixSize != 8 ||
            TRI.callPreservedPrefixSize(C.Value.RegOff, C.Value.Size) != 8 ||
            C.PreservedInput.Kind != MedVar::Reg ||
            C.PreservedInput.Size != C.Value.Size ||
            C.PreservedInput.RegOff != C.Value.RegOff ||
            C.PreservedInput.TheArch != C.Value.TheArch ||
            C.PreservedInput.Id != C.Value.Id ||
            C.PreservedInput.SSAVer == C.Value.SSAVer || Owner.DoesNotReturn ||
            Owner.PreservesCallerSaved)
          return false;
        Input(C.PreservedInput);
      } else if (D.Op) {
        const auto &Op = *D.Op;
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          const auto &S = Op.SourceCallHint->Signature;
          if (Op.DoesNotReturn || !S.ReturnType ||
              S.ReturnType->Kind != NdTypeKind::Float ||
              S.ReturnType->Size != 8 || !S.ReturnComponents.empty() ||
              Op.Output.Kind != MedVar::Reg ||
              Op.Output.TheArch != Arch::AArch64 ||
              Op.Output.RegOff != TRI.FPReturnReg || Op.Output.Size != 8 ||
              S.ReturnLocation.Kind != SourceABICarrierKind::FloatingRegister ||
              S.ReturnLocation.RegisterOffset != TRI.FPReturnReg ||
              S.ReturnLocation.ValueBytes != 8)
            return false;
          Nodes[I].Leaf = Nodes[I].Typed = true;
          continue;
        }
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].Size == Op.Output.Size)
          Input(Op.Inputs[0]);
        else if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
                 Op.Inputs[0].Size == 8 && Op.Output.Size == 16)
          Input(Op.Inputs[0]);
        else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                 Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0 &&
                 Op.Output.Size == 8 && Op.Inputs[0].Size >= 8)
          Input(Op.Inputs[0]);
        else if (Op.Opcode == NdOp::CONCAT && Op.NumInputs == 2 &&
                 Op.Output.Size == 16 && Op.Inputs[0].Size == 8 &&
                 Op.Inputs[1].Size == 8)
          Input(Op.Inputs[1]);
        else
          return false;
      } else {
        return false;
      }
      for (const auto &[V, UseBlock, UsePosition] : Inputs) {
        auto Needed = need(V, UseBlock, UsePosition);
        if (!Needed)
          return false;
        Nodes[I].Inputs.push_back(*Needed);
      }
    }
    return true;
  }

  // Collapse identity cycles before judging their leaves. An SCC needs a real
  // outside seed; neither recursive DFS success nor an initially optimistic
  // value may turn an unseeded loop into evidence.
  bool grounded() {
    const size_t N = Nodes.size();
    std::vector<std::vector<size_t>> Reverse(N);
    for (size_t I = 0; I != N; ++I)
      for (size_t J : Nodes[I].Inputs)
        Reverse[J].push_back(I);
    std::vector<bool> Seen(N);
    std::vector<size_t> Order;
    for (size_t Start = 0; Start != N; ++Start) {
      if (Seen[Start])
        continue;
      Seen[Start] = true;
      std::vector<std::pair<size_t, size_t>> Stack{{Start, 0}};
      while (!Stack.empty()) {
        if (!spend())
          return false;
        auto &[V, Next] = Stack.back();
        if (Next == Nodes[V].Inputs.size()) {
          Order.push_back(V);
          Stack.pop_back();
          continue;
        }
        const size_t Input = Nodes[V].Inputs[Next++];
        if (!Seen[Input]) {
          Seen[Input] = true;
          Stack.emplace_back(Input, 0);
        }
      }
    }
    std::vector<size_t> Component(N, N);
    size_t Count = 0;
    for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
      if (Component[*It] != N)
        continue;
      std::vector<size_t> Pending{*It};
      Component[*It] = Count;
      for (size_t I = 0; I != Pending.size(); ++I)
        for (size_t J : Reverse[Pending[I]]) {
          if (!spend())
            return false;
          if (Component[J] == N) {
            Component[J] = Count;
            Pending.push_back(J);
          }
        }
      ++Count;
    }
    std::vector<std::set<size_t>> Inputs(Count);
    std::vector<bool> Grounded(Count), Typed(Count);
    for (size_t I = 0; I != N; ++I) {
      const auto C = Component[I];
      Grounded[C] = Grounded[C] || Nodes[I].Leaf;
      Typed[C] = Typed[C] || Nodes[I].Typed;
      for (size_t J : Nodes[I].Inputs)
        if (Component[J] != C)
          Inputs[C].insert(Component[J]);
    }
    // Kosaraju numbered parent users before their input components.
    for (size_t C = Count; C-- > 0;) {
      bool HasSeed = Grounded[C] || !Inputs[C].empty();
      for (size_t D : Inputs[C]) {
        if (!spend() || D <= C || !Grounded[D])
          return false;
        Typed[C] = Typed[C] || Typed[D];
      }
      Grounded[C] = HasSeed;
    }
    return std::all_of(Roots.begin(), Roots.end(),
                       [&](size_t R) { return Grounded[Component[R]]; }) &&
           std::any_of(Roots.begin(), Roots.end(),
                       [&](size_t R) { return Typed[Component[R]]; });
  }

public:
  explicit Proof(const MedFunc &F)
      : Function(F), TRI(getTargetRegInfo(Arch::AArch64)) {}
  bool run() { return graph() && returns() && values() && grounded(); }
};
} // namespace native_float_return

// Each normal return requires an explicit complete D0/Q0 write in its block;
// this first candidate does not infer a result solely from a nearby alias PHI.
inline bool hasProvenNativeSourceFloat64Return(const MedFunc &Function,
                                               Arch Architecture) {
  return Architecture == Arch::AArch64 &&
         native_float_return::Proof(Function).run();
}
} // namespace neverd::detail

#endif

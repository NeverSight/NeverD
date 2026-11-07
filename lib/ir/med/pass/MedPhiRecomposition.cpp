//===- MedPhiRecomposition.cpp - Correlated word slices ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover whole SSA words from edge-correlated low/high PHI inputs.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedPhiRecomposition.h"

#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<int, int, int, uint16_t>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer, V.Size}; }
bool sameValue(const MedVar &A, const MedVar &B) {
  return A == B && A.Size == B.Size;
}

struct Definitions {
  llvm::DenseMap<Key, const MedOp *> Ops;

  const MedOp *find(const MedVar &V) const {
    if (V.isConst())
      return nullptr;
    const auto I = Ops.find(key(V));
    return I == Ops.end() || I->second->RegistrationRoot !=
                                 MedOp::RegistrationRootKind::None
               ? nullptr
               : I->second;
  }

  MedVar strip(MedVar V) const {
    llvm::SmallDenseSet<Key, 8> Seen;
    while (Seen.insert(key(V)).second) {
      const auto *Op = find(V);
      if (!Op)
        break;
      if (Op->Opcode == NdOp::COPY && Op->NumInputs == 1 &&
          Op->Inputs[0].Size == V.Size) {
        V = Op->Inputs[0];
        continue;
      }
      if (Op->Opcode == NdOp::INT_OR && Op->NumInputs == 2 &&
          Op->Inputs[0].Size == V.Size && Op->Inputs[1].Size == V.Size) {
        auto Zero = [](const MedVar &Value) {
          return Value.isConst() && !Value.ConstVal &&
                 Value.AddressOwnerVA == InvalidVA &&
                 (Value.Provenance == ConstantAddressProvenance::Unknown ||
                  Value.Provenance == ConstantAddressProvenance::Scalar);
        };
        if (Zero(Op->Inputs[0])) {
          V = Op->Inputs[1];
          continue;
        }
        if (Zero(Op->Inputs[1])) {
          V = Op->Inputs[0];
          continue;
        }
      }
      break;
    }
    return V;
  }

  std::optional<MedVar> extended(MedVar V, uint16_t WholeSize) const {
    V = strip(V);
    const auto *Op = find(V);
    if (!Op || Op->Opcode != NdOp::INT_ZEXT || Op->NumInputs != 1 ||
        V.Size != WholeSize || !Op->Inputs[0].Size ||
        Op->Inputs[0].Size >= WholeSize)
      return std::nullopt;
    return strip(Op->Inputs[0]);
  }

  struct Slice {
    MedVar Whole;
    uint64_t Offset;
  };
  std::optional<Slice> slice(MedVar V, uint16_t WholeSize) const {
    V = strip(V);
    const auto *Op = find(V);
    if (!Op || Op->Opcode != NdOp::SUBBYTES || Op->NumInputs != 2 ||
        !Op->Inputs[1].isConst())
      return std::nullopt;
    uint64_t Offset = Op->Inputs[1].ConstVal;
    MedVar Whole = strip(Op->Inputs[0]);
    const auto *Shift = find(Whole);
    if (Shift && Shift->Opcode == NdOp::INT_RIGHT && Shift->NumInputs == 2 &&
        Shift->Inputs[1].isConst() && Shift->Inputs[0].Size == Whole.Size) {
      const uint64_t Bits = Shift->Inputs[1].ConstVal;
      if (Bits % 8 || Bits / 8 >= WholeSize || Offset >= WholeSize)
        return std::nullopt;
      Offset += Bits / 8;
      Whole = strip(Shift->Inputs[0]);
    }
    if (Whole.Kind == MedVar::Unspecified || Whole.Size != WholeSize ||
        Offset > WholeSize || V.Size > WholeSize - Offset)
      return std::nullopt;
    return Slice{Whole, Offset};
  }

  std::optional<std::pair<MedVar, MedVar>> pieces(const MedOp &Op) const {
    if (Op.NumInputs != 2)
      return std::nullopt;
    if (Op.Opcode == NdOp::CONCAT) {
      auto Low = strip(Op.Inputs[1]), High = strip(Op.Inputs[0]);
      if (Low.Size && High.Size && Low.Size + High.Size == Op.Output.Size)
        return std::pair{Low, High};
    }
    if (Op.Opcode != NdOp::INT_OR)
      return std::nullopt;
    for (unsigned I : {0u, 1u}) {
      auto Low = extended(Op.Inputs[I], Op.Output.Size);
      const auto *Shift = find(strip(Op.Inputs[1 - I]));
      if (!Low || !Shift || Shift->Opcode != NdOp::INT_LEFT ||
          Shift->NumInputs != 2 || Shift->Output.Size != Op.Output.Size ||
          !Shift->Inputs[1].isConst() ||
          Shift->Inputs[1].ConstVal != Low->Size * 8u)
        continue;
      auto High = extended(Shift->Inputs[0], Op.Output.Size);
      if (High && Low->Size + High->Size == Op.Output.Size)
        return std::pair{*Low, *High};
    }
    return std::nullopt;
  }
};

struct PhiLocation {
  MedBlock *Block;
  size_t Index;
};
} // namespace

bool recomposeSplitPhis(MedFunc &Func, int *NextVarId) {
  if (Func.SkippedSSA)
    return false;
  Definitions Defs;
  llvm::DenseMap<Key, PhiLocation> Phis;
  std::map<int, std::set<int>> Incoming;
  // The converter keeps allocating after this pass. Its counter also covers
  // ids no longer used in the function, which a register view can return to.
  uint64_t NextId = NextVarId ? uint64_t(std::max(*NextVarId, 0)) : 0;
  auto ObserveId = [&](const MedVar &V) {
    if (!V.isConst() && V.Id >= 0)
      NextId = std::max(NextId, uint64_t(V.Id) + 1);
  };
  for (const auto &V : Func.Params)
    ObserveId(V);
  for (const auto &V : Func.Locals)
    ObserveId(V);
  for (const auto &C : Func.CallClobbers) {
    ObserveId(C.Value);
    ObserveId(C.PreservedInput);
  }
  for (const auto &Info : Func.CallInfos)
    for (const auto &V : Info.Args)
      ObserveId(V);
  for (const auto &C : Func.StructReturnCandidates)
    for (const auto &V : C.Fields)
      ObserveId(V);
  for (auto &Block : Func.Blocks) {
    for (int Successor : Block.Succs)
      Incoming[Successor].insert(Block.Id);
    for (size_t I = 0; I < Block.Phis.size(); ++I) {
      const auto &Phi = Block.Phis[I];
      Phis[key(Phi.Output)] = {&Block, I};
      ObserveId(Phi.Output);
      for (const auto &[Pred, Value] : Phi.Args)
        ObserveId(Value);
      if (Phi.ExceptionalEntry)
        ObserveId(*Phi.ExceptionalEntry);
    }
    for (const auto &Op : Block.Ops) {
      if (!Op.Dead && Op.Output.Size && !Op.Output.isConst())
        Defs.Ops[key(Op.Output)] = &Op;
      ObserveId(Op.Output);
      for (const MedVar &Output : Op.IntrinsicOutputs)
        ObserveId(Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        ObserveId(Op.Inputs[I]);
    }
  }

  llvm::DenseMap<std::pair<Key, Key>, MedVar> Recovered;
  bool Changed = false;
  for (auto &Block : Func.Blocks) {
    for (auto &Op : Block.Ops) {
      if (Op.Dead || Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
        continue;
      auto Pieces = Defs.pieces(Op);
      if (!Pieces)
        continue;
      const auto Pair = std::pair{key(Pieces->first), key(Pieces->second)};
      auto Existing = Recovered.find(Pair);
      MedVar Whole;
      if (Existing != Recovered.end()) {
        Whole = Existing->second;
      } else {
        auto Low = Phis.find(Pair.first), High = Phis.find(Pair.second);
        if (Low == Phis.end() || High == Phis.end() ||
            Low->second.Block != High->second.Block)
          continue;
        auto &Owner = *Low->second.Block;
        const auto &LPhi = Owner.Phis[Low->second.Index];
        const auto &HPhi = Owner.Phis[High->second.Index];
        if (!Owner.ExceptionalPreds.empty() ||
            !hasCompleteOrdinaryPhiInputs(Owner, LPhi, Incoming[Owner.Id]) ||
            !hasCompleteOrdinaryPhiInputs(Owner, HPhi, Incoming[Owner.Id]))
          continue;

        PhiNode Joined;
        for (const auto &[Pred, Value] : LPhi.Args) {
          const auto HArg =
              std::find_if(HPhi.Args.begin(), HPhi.Args.end(),
                           [&](const auto &Arg) { return Arg.first == Pred; });
          auto L = Defs.slice(Value, Op.Output.Size);
          auto H = Defs.slice(HArg->second, Op.Output.Size);
          if (!L || !H || L->Offset || H->Offset != LPhi.Output.Size ||
              !sameValue(L->Whole, H->Whole))
            break;
          Joined.Args.emplace_back(Pred, L->Whole);
        }
        if (Joined.Args.size() != LPhi.Args.size())
          continue;

        // Reuse an existing whole-value join when available. Nothing is
        // evaluated or moved onto an edge: each input is an existing SSA value.
        const auto Match = std::find_if(
            Owner.Phis.begin(), Owner.Phis.end(), [&](const PhiNode &Phi) {
              return Phi.Output.Size == Op.Output.Size &&
                     hasCompleteOrdinaryPhiInputs(Owner, Phi,
                                                  Incoming[Owner.Id]) &&
                     std::all_of(Joined.Args.begin(), Joined.Args.end(),
                                 [&](const auto &A) {
                                   return std::any_of(
                                       Phi.Args.begin(), Phi.Args.end(),
                                       [&](const auto &B) {
                                         return A.first == B.first &&
                                                sameValue(A.second, B.second);
                                       });
                                 });
            });
        if (Match != Owner.Phis.end()) {
          Whole = Match->Output;
        } else {
          if (NextId >= uint64_t(std::numeric_limits<int>::max()))
            continue;
          Whole.Kind = MedVar::Temp;
          Whole.Id = static_cast<int>(NextId++);
          Whole.SSAVer = 1;
          Whole.Size = Op.Output.Size;
          Whole.TheArch = Op.Output.TheArch;
          Joined.Output = Whole;
          Owner.Phis.push_back(std::move(Joined));
        }
        Recovered.try_emplace(Pair, Whole);
      }
      Op.Opcode = NdOp::COPY;
      Op.Inputs.clear();
      Op.NumInputs = 0;
      Op.addInput(Whole);
      Changed = true;
    }
  }
  if (NextVarId)
    *NextVarId = static_cast<int>(std::max<uint64_t>(
        uint64_t(std::max(*NextVarId, 0)),
        std::min<uint64_t>(NextId, std::numeric_limits<int>::max())));
  return Changed;
}
} // namespace neverd

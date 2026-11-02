//===- ComputedBranchScalars.cpp - Numeric dispatch operands -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind numeric code-layout arithmetic only when all its uses are local
/// arithmetic or dispatches whose case keys are physical target addresses.
///
//===----------------------------------------------------------------------===//

#include "ComputedBranchScalars.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int, uint16_t>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer, V.Size}; }

bool scalarOperation(const MedOp &Op) {
  if (!Op.IntrinsicOutputs.empty() || Op.NumInputs > 2 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  LowOp Shape;
  Shape.Opcode = Op.Opcode;
  Shape.Output = NdVar::tmp(0, Op.Output.Size);
  for (unsigned I = 0; I < Op.NumInputs; ++I)
    Shape.addInput(NdVar::cst(1, Op.Inputs[I].Size));
  return bool(lowScalarConstantResult(
      Shape, [](const NdVar &V) { return std::optional<uint64_t>{V.Offset}; }));
}

bool physicalTargetSelector(const JumpTable &JT) {
  if (JT.HasBaseAddr || JT.EntrySize || JT.Targets.empty() ||
      JT.Targets.size() != JT.CaseLabels.size() ||
      JT.SelectorUseRefs.size() != 1)
    return false;
  const auto &Ref = JT.SelectorUseRefs.front();
  if (Ref.Addr != JT.InsnAddr || Ref.ExpectedOpcode != NdOp::INDIR_BR ||
      Ref.Role != JumpTableSelectorUseRef::ValueRole::Input || Ref.InputNo)
    return false;
  for (size_t I = 0; I < JT.Targets.size(); ++I)
    if (uint64_t(JT.CaseLabels[I]) != JT.Targets[I])
      return false;
  return true;
}
} // namespace

bool preserveComputedBranchScalars(MedFunc &F, const BinaryImage &Image) {
  std::map<va_t, std::pair<JumpTableSelectorUseRef, bool>> Selectors;
  for (const JumpTable &JT : F.JumpTables)
    if (physicalTargetSelector(JT))
      Selectors.emplace(JT.InsnAddr, std::pair{JT.SelectorUseRefs.front(),
                                               JT.OriginalAddressSelector});
  if (Selectors.empty())
    return true;
  if (F.SkippedSSA)
    return false;
  size_t Remaining = limits::kMaxScalarBranchEvidenceWork;
  const auto Spend = [&] {
    if (!Remaining)
      return false;
    --Remaining;
    return true;
  };
  struct Node {
    MedOp *Op = nullptr;
    PhiNode *Phi = nullptr;
  };
  std::map<Key, Node> Definitions;
  std::map<Key, std::vector<Node>> Uses;
  std::set<const MedOp *> Dispatches;
  std::vector<std::pair<MedVar, bool>> Backward;
  for (MedBlock &Block : F.Blocks) {
    for (PhiNode &Phi : Block.Phis) {
      if (!Spend() ||
          !Definitions.emplace(key(Phi.Output), Node{nullptr, &Phi}).second)
        return false;
      for (auto &[Pred, V] : Phi.Args) {
        if (!Spend())
          return false;
        if (!V.isConst())
          Uses[key(V)].push_back({nullptr, &Phi});
      }
    }
    for (MedOp &Op : Block.Ops) {
      if (!Spend())
        return false;
      if (Op.Dead)
        continue;
      if (Op.Output.Size && !Op.Output.isConst() &&
          !Definitions.emplace(key(Op.Output), Node{&Op, nullptr}).second)
        return false;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Uses[key(Op.Inputs[I])].push_back({&Op, nullptr});
      const auto At = Selectors.find(Op.Addr);
      if (At != Selectors.end() && Op.Opcode == NdOp::INDIR_BR &&
          Op.NumInputs == 1 && Op.OriginSeq == At->second.first.Seq &&
          Op.Inputs[0].Size == At->second.first.ExpectedSize) {
        Dispatches.insert(&Op);
        Backward.emplace_back(Op.Inputs[0], At->second.second);
      }
    }
  }
  if (Dispatches.size() != Selectors.size())
    return false;
  std::set<std::pair<Key, bool>> SeenBackward;
  std::set<std::tuple<Key, bool, bool>> SeenForward;
  std::vector<std::tuple<Key, bool, bool>> Forward;
  std::set<MedVar *> Constants;
  bool UnprovedFragment = false;
  const auto Collect = [&](MedVar &V, const MedVar &Output, bool Original,
                           bool Arithmetic) {
    if (!V.isConst()) {
      Backward.emplace_back(V, Original);
      return;
    }
    if (V.Provenance == ConstantAddressProvenance::AddressFragment) {
      UnprovedFragment = true;
      return;
    }
    if (V.Provenance != ConstantAddressProvenance::Scalar &&
        V.Size >= Image.getPointerSize() &&
        (isAddressProvenance(V.Provenance) ||
         Image.getSegmentFor(V.ConstVal))) {
      if (!Original)
        Constants.insert(&V);
      Forward.emplace_back(key(Output), Original, Arithmetic);
    }
  };
  while (!Backward.empty() || !Forward.empty()) {
    while (!Backward.empty()) {
      if (!Spend())
        return false;
      const auto [V, Original] = Backward.back();
      Backward.pop_back();
      if (V.isConst() || !SeenBackward.emplace(key(V), Original).second)
        continue;
      const auto At = Definitions.find(key(V));
      if (At == Definitions.end())
        continue;
      const Node N = At->second;
      if (N.Phi) {
        for (auto &[Pred, Input] : N.Phi->Args) {
          if (!Spend())
            return false;
          Collect(Input, N.Phi->Output, Original, false);
        }
      } else if (scalarOperation(*N.Op))
        for (unsigned I = 0; I < N.Op->NumInputs; ++I)
          Collect(N.Op->Inputs[I], N.Op->Output, Original,
                  N.Op->Opcode != NdOp::COPY);
    }
    if (Forward.empty())
      break;
    const auto [V, Original, Arithmetic] = Forward.back();
    Forward.pop_back();
    if (!Spend())
      return false;
    if (!SeenForward.emplace(V, Original, Arithmetic).second)
      continue;
    for (const Node &N : Uses[V]) {
      if (!Spend())
        return false;
      if (N.Op && Dispatches.count(N.Op))
        continue;
      if (N.Phi) {
        Forward.emplace_back(key(N.Phi->Output), Original, Arithmetic);
        Backward.emplace_back(N.Phi->Output, Original);
      } else {
        if (!scalarOperation(*N.Op)) {
          // The original-address projector can duplicate an exact pointer
          // COPY/PHI for the selector while preserving its observable identity.
          // Arithmetic in original code coordinates cannot escape that way:
          // relocated block labels need not retain their byte distances.
          if (!Original || Arithmetic)
            return false;
          continue;
        }
        Forward.emplace_back(key(N.Op->Output), Original,
                             Arithmetic || N.Op->Opcode != NdOp::COPY);
        Backward.emplace_back(N.Op->Output, Original);
      }
    }
  }
  // Publish atomically. A failed closure leaves every pointer occurrence with
  // its original identity and cannot authorize partially numeric arithmetic.
  if (UnprovedFragment)
    return false;
  for (MedVar *V : Constants)
    V->Provenance = ConstantAddressProvenance::Scalar;
  return true;
}
} // namespace neverd

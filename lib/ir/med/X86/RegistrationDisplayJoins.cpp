//===- RegistrationDisplayJoins.cpp - Frame joins of the view --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationDisplayJoins.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include <map>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

using Key = std::tuple<int, int, int, uint64_t>;

Key keyOf(const MedVar &V) {
  return {static_cast<int>(V.Kind), V.Id, V.SSAVer, V.RegOff};
}

/// Distances from the entry stack pointer: a name that is absent has no way in
/// yet, one that holds nullopt has no single distance.
class StackDistances {
public:
  StackDistances(const MedFunc &Func, uint64_t StackPointer)
      : Func(Func), StackPointer(StackPointer) {}

  std::optional<int64_t> of(const MedVar &V) const {
    const auto It = Known.find(keyOf(V));
    return It == Known.end() ? std::nullopt : It->second;
  }

  void solve() {
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const MedBlock &Block : Func.Blocks) {
        for (const PhiNode &Phi : Block.Phis)
          Changed |= note(Phi.Output, meetOf(Phi));
        for (const MedOp &Op : Block.Ops)
          if (Op.Output.Size)
            Changed |= note(Op.Output, transfer(Op));
      }
    }
  }

private:
  /// What a name holds so far: unset for no way in yet.
  using Value = std::optional<std::optional<int64_t>>;
  const MedFunc &Func;
  uint64_t StackPointer;
  std::map<Key, std::optional<int64_t>> Known;

  bool note(const MedVar &V, const Value &New) {
    if (!New)
      return false;
    const auto [It, Inserted] = Known.try_emplace(keyOf(V), *New);
    if (Inserted)
      return true;
    if (!It->second || It->second == *New)
      return false;
    It->second = std::nullopt;
    return true;
  }

  Value read(const MedVar &V) const {
    if (V.Kind == MedVar::Const)
      return std::optional<int64_t>{};
    if (V.Kind == MedVar::Reg && V.SSAVer == 0)
      return V.RegOff == StackPointer ? std::optional<int64_t>(0)
                                      : std::optional<int64_t>{};
    const auto It = Known.find(keyOf(V));
    return It == Known.end() ? Value{} : Value{It->second};
  }

  Value meetOf(const PhiNode &Phi) const {
    if (Phi.ExceptionalEntry)
      return std::optional<int64_t>{};
    Value Met;
    for (const auto &[Pred, Arg] : Phi.Args) {
      (void)Pred;
      const Value In = read(Arg);
      if (!In)
        continue;
      if (!*In || (Met && *Met != *In))
        return std::optional<int64_t>{};
      Met = In;
    }
    return Met;
  }

  Value transfer(const MedOp &Op) const {
    if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
      return registrationRootEntryStackOffset(Op);
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      return Op.NumInputs == 1 ? read(Op.Inputs[0]) : std::optional<int64_t>{};
    case NdOp::SUBBYTES:
      if (Op.NumInputs == 2 && Op.Inputs[1].Kind == MedVar::Const &&
          !Op.Inputs[1].ConstVal)
        return read(Op.Inputs[0]);
      return std::optional<int64_t>{};
    case NdOp::INT_ADD:
    case NdOp::INT_SUB: {
      if (Op.NumInputs != 2 || Op.Inputs[1].Kind != MedVar::Const)
        return std::optional<int64_t>{};
      const Value Base = read(Op.Inputs[0]);
      if (!Base || !*Base)
        return Base;
      const auto Step = static_cast<int64_t>(
          static_cast<int32_t>(static_cast<uint32_t>(Op.Inputs[1].ConstVal)));
      return std::optional<int64_t>(Op.Opcode == NdOp::INT_ADD ? **Base + Step
                                                               : **Base - Step);
    }
    default:
      return std::optional<int64_t>{};
    }
  }
};

} // namespace

void settleRegistrationDisplayJoins(MedFunc &Func) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);
  // The name the function reads the entry stack pointer by.
  std::optional<MedVar> Entry;
  for (const MedBlock &Block : Func.Blocks) {
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs && !Entry; ++I)
        if (Op.Inputs[I].Kind == MedVar::Reg &&
            Op.Inputs[I].RegOff == TRI.StackPointer &&
            Op.Inputs[I].Size == TRI.PointerSize && !Op.Inputs[I].SSAVer)
          Entry = Op.Inputs[I];
    if (Entry)
      break;
  }
  if (!Entry)
    return;
  StackDistances Distances(Func, TRI.StackPointer);
  Distances.solve();
  for (MedBlock &Block : Func.Blocks) {
    std::vector<MedOp> Settled;
    std::vector<PhiNode> Kept;
    Kept.reserve(Block.Phis.size());
    for (PhiNode &Phi : Block.Phis) {
      const std::optional<int64_t> Distance = Distances.of(Phi.Output);
      if (!Distance || Phi.Output.Kind != MedVar::Reg ||
          Phi.Output.Size != TRI.PointerSize ||
          (Phi.Output.RegOff != TRI.StackPointer &&
           Phi.Output.RegOff != TRI.FramePointer)) {
        Kept.push_back(std::move(Phi));
        continue;
      }
      MedOp Op;
      Op.Opcode = NdOp::INT_ADD;
      Op.Addr = Block.StartAddr;
      Op.Output = Phi.Output;
      Op.addInput(*Entry);
      Op.addInput(MedVar::makeConst(static_cast<uint64_t>(*Distance),
                                    static_cast<uint16_t>(TRI.PointerSize)));
      Settled.push_back(std::move(Op));
    }
    Block.Phis = std::move(Kept);
    Block.Ops.insert(Block.Ops.begin(),
                     std::make_move_iterator(Settled.begin()),
                     std::make_move_iterator(Settled.end()));
  }
}

} // namespace neverd

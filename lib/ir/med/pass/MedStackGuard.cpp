//===- MedStackGuard.cpp - Values of a stack protector check --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedStackGuard.h"

#include "llvm/ADT/STLExtras.h"

#include <optional>
#include <vector>

namespace neverd {

namespace {
/// Where a thread keeps its stack guard.
struct GuardSlot {
  NdMemoryAddressSpace Space;
  uint64_t Offset;
  uint16_t Bytes;
};

std::optional<GuardSlot> guardSlotOf(Arch Target) {
  switch (Target) {
  case Arch::X64:
    return GuardSlot{NdMemoryAddressSpace::X86FS, 0x28, 8};
  case Arch::X86:
    return GuardSlot{NdMemoryAddressSpace::X86GS, 0x14, 4};
  default:
    return std::nullopt;
  }
}

/// Whether \p Op makes of the values of \p Guard another of them: a copy, an
/// extension, a view, or the subtraction or exclusive OR of the check.
bool derives(const MedOp &Op, const MedValueMap<char> &Guard) {
  const auto Held = [&](const MedVar &Value) {
    return !Value.isConst() && Guard.count(medValueKey(Value)) != 0;
  };
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Op.NumInputs == 1 && Held(Op.Inputs[0]);
  case NdOp::SUBBYTES:
    return Op.NumInputs == 2 && Held(Op.Inputs[0]);
  case NdOp::INT_SUB:
  case NdOp::INT_XOR:
    return Op.NumInputs == 2 && (Held(Op.Inputs[0]) || Held(Op.Inputs[1]));
  default:
    return false;
  }
}

bool isValue(const MedVar &Value) { return Value.Size && !Value.isConst(); }
} // namespace

MedStackGuard::MedStackGuard(const MedFunc &Func, Arch Target) {
  const std::optional<GuardSlot> Slot = guardSlotOf(Target);
  if (!Slot)
    return;
  std::vector<const MedOp *> Loads;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.MemoryAddressSpace == Slot->Space &&
          Op.NumInputs >= 1 && Op.Output.Size == Slot->Bytes)
        Loads.push_back(&Op);
  if (Loads.empty())
    return;
  MedValueMap<const MedOp *> Defs;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (isValue(Op.Output))
        Defs.try_emplace(medValueKey(Op.Output), &Op);
  // The address a load reads: a constant, or a copy of one.
  const auto AddressOf = [&](MedVar Value) -> std::optional<uint64_t> {
    // A chain of copies is no longer than the function has values.
    for (size_t Step = 0; Step <= Defs.size(); ++Step) {
      if (Value.isConst())
        return Value.ConstVal;
      const auto Def = Defs.find(medValueKey(Value));
      if (Def == Defs.end() || Def->second->Opcode != NdOp::COPY ||
          Def->second->NumInputs != 1)
        return std::nullopt;
      Value = Def->second->Inputs[0];
    }
    return std::nullopt;
  };
  MedValueMap<char> Seeds;
  for (const MedOp *Load : Loads)
    if (AddressOf(Load->Inputs[0]) == Slot->Offset)
      Seeds.try_emplace(medValueKey(Load->Output), 1);
  if (Seeds.empty())
    return;
  // A merge of guard values is one: the slot that holds the copy merges
  // wherever paths join.  Take every PHI for one, then drop each that
  // merges anything else, until none is left to drop.
  std::vector<const PhiNode *> Phis;
  for (const MedBlock &Block : Func.Blocks)
    for (const PhiNode &Phi : Block.Phis)
      if (isValue(Phi.Output) && !Phi.Args.empty())
        Phis.push_back(&Phi);
  for (;;) {
    MedValueMap<char> Now = Seeds;
    for (const PhiNode *Phi : Phis)
      Now.try_emplace(medValueKey(Phi->Output), 1);
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const MedBlock &Block : Func.Blocks)
        for (const MedOp &Op : Block.Ops)
          if (isValue(Op.Output) && derives(Op, Now))
            Changed |= Now.try_emplace(medValueKey(Op.Output), 1).second;
    }
    const size_t Before = Phis.size();
    llvm::erase_if(Phis, [&](const PhiNode *Phi) {
      return llvm::any_of(Phi->Args, [&](const auto &Arm) {
        return Arm.second.isConst() || !Now.count(medValueKey(Arm.second));
      });
    });
    if (Phis.size() == Before) {
      Values = std::move(Now);
      break;
    }
  }
  // A value nothing but the check reads belongs to it: the copy the frame
  // kept, loaded to be subtracted from or compared with the guard.
  MedValueMap<char> ReadElsewhere, ReadByCheck;
  const auto Held = [&](const MedVar &Value) {
    return !Value.isConst() && Values.count(medValueKey(Value)) != 0;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &Arm : Phi.Args)
        if (isValue(Arm.second))
          ReadElsewhere.try_emplace(medValueKey(Arm.second), 1);
    for (const MedOp &Op : Block.Ops) {
      const bool Compares =
          (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
          Op.NumInputs == 2 && (Held(Op.Inputs[0]) || Held(Op.Inputs[1]));
      const bool Check =
          Compares ||
          (isValue(Op.Output) && Held(Op.Output) &&
           (Op.Opcode == NdOp::INT_SUB || Op.Opcode == NdOp::INT_XOR));
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (isValue(Op.Inputs[I]))
          (Check ? ReadByCheck : ReadElsewhere)
              .try_emplace(medValueKey(Op.Inputs[I]), 1);
    }
  }
  for (const auto &[Key, Read] : ReadByCheck) {
    (void)Read;
    if (!ReadElsewhere.count(Key))
      Values.try_emplace(Key, 1);
  }
}

} // namespace neverd

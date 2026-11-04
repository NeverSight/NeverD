//===- MedBoundCallStackArguments.cpp - Bound stack arguments -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedBoundCallStackArguments.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd {
namespace {

/// An address as a stack pointer value plus a constant.
struct FrameSlot {
  MedVar Base;
  int64_t Offset = 0;
};

/// The operation of \p Block before \p End that defines \p Value.
std::optional<size_t> definitionBefore(const MedBlock &Block, size_t End,
                                       const MedVar &Value) {
  if (Value.isConst())
    return std::nullopt;
  for (size_t I = End; I-- > 0;)
    if (Block.Ops[I].Output == Value)
      return I;
  return std::nullopt;
}

/// \p Address, which an operation of \p Block before \p End reads, as a
/// register plus a constant, where that block computes it so.
std::optional<FrameSlot> frameSlot(const MedBlock &Block, size_t End,
                                   const MedVar &Address) {
  if (Address.Kind == MedVar::Reg)
    return FrameSlot{Address, 0};
  const auto Def = definitionBefore(Block, End, Address);
  if (!Def)
    return std::nullopt;
  const MedOp &Op = Block.Ops[*Def];
  if (Op.NumInputs == 2 && Op.Inputs[0].Kind == MedVar::Reg &&
      Op.Inputs[1].isConst() &&
      (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB)) {
    const auto Constant = static_cast<int64_t>(Op.Inputs[1].ConstVal);
    return FrameSlot{Op.Inputs[0],
                     Op.Opcode == NdOp::INT_ADD ? Constant : -Constant};
  }
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
      Op.Inputs[0].Kind == MedVar::Reg)
    return FrameSlot{Op.Inputs[0], 0};
  return std::nullopt;
}

bool plainMemoryAccess(const MedOp &Op) {
  return Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

/// Whether \p Op may change memory a later load reads.
bool mayWriteMemory(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::STORE:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
    return true;
  case NdOp::INTRINSIC:
    return !Op.NumInputs || !Op.Inputs[0].isConst() ||
           !preservesCallSetup(static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
  default:
    return !plainMemoryAccess(Op);
  }
}

/// The value the block stored last in the slot that the LOAD at \p At of
/// \p Block reads, where nothing between them may change it.
std::optional<MedVar> storedValue(const MedBlock &Block, size_t At) {
  const MedOp &Load = Block.Ops[At];
  if (Load.Opcode != NdOp::LOAD || Load.NumInputs != 1 ||
      !plainMemoryAccess(Load) || !Load.Output.Size)
    return std::nullopt;
  const auto Slot = frameSlot(Block, At, Load.Inputs[0]);
  if (!Slot)
    return std::nullopt;
  const int64_t Bytes = Load.Output.Size;
  for (size_t I = At; I-- > 0;) {
    const MedOp &Op = Block.Ops[I];
    if (Op.Opcode == NdOp::STORE) {
      if (Op.NumInputs != 2 || !plainMemoryAccess(Op))
        return std::nullopt;
      const auto Stored = frameSlot(Block, I, Op.Inputs[0]);
      if (!Stored || !(Stored->Base == Slot->Base))
        return std::nullopt;
      const int64_t StoredBytes = Op.Inputs[1].Size;
      if (!StoredBytes)
        return std::nullopt;
      if (Stored->Offset == Slot->Offset && StoredBytes == Bytes)
        return Op.Inputs[1];
      // A store to a disjoint slot of the same stack pointer value leaves
      // this one as it was.
      if (Stored->Offset + StoredBytes <= Slot->Offset ||
          Slot->Offset + Bytes <= Stored->Offset)
        continue;
      return std::nullopt;
    }
    if (mayWriteMemory(Op))
      return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

bool forwardBoundCallStackArguments(MedFunc &Func) {
  bool Changed = false;
  for (MedBlock &Block : Func.Blocks)
    for (size_t C = 0; C < Block.Ops.size(); ++C) {
      MedOp &Call = Block.Ops[C];
      if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
          !Call.IntegerCallSignature)
        continue;
      const auto &Parameters = Call.IntegerCallSignature->Parameters;
      if (Call.NumInputs != Parameters.size() + 1 ||
          Call.Inputs.size() < Call.NumInputs)
        continue;
      for (size_t P = 0; P < Parameters.size(); ++P) {
        if (Parameters[P].Kind != SourceABICarrierKind::Stack)
          continue;
        MedVar &Operand = Call.Inputs[P + 1];
        const auto Load = definitionBefore(Block, C, Operand);
        if (!Load)
          continue;
        if (const auto Value = storedValue(Block, *Load)) {
          Operand = *Value;
          Changed = true;
        }
      }
    }
  return Changed;
}

} // namespace neverd

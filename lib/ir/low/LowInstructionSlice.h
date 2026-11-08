//===- LowInstructionSlice.h - Checked operation ownership ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWINSTRUCTIONSLICE_H
#define NEVERD_IR_LOW_LOWINSTRUCTIONSLICE_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>

namespace neverd::detail {

/// Recognize a guard that skips the remainder of an execution unit. Ordinary
/// ARM branches keep their existing CFG interpretation; delayed branches also
/// guard register-only slot effects and their final transfer.
inline bool hasInstructionGuard(llvm::ArrayRef<LowOp> Ops, va_t NextAddress,
                                bool Delayed) {
  std::optional<size_t> Guard;
  for (size_t I = 0; I < Ops.size(); ++I) {
    const auto &Op = Ops[I];
    if (Op.Opcode != NdOp::COND_BR)
      continue;
    if (Guard || Op.NumInputs < 2 || !Op.Inputs[0].isConst() ||
        Op.Inputs[0].Offset != NextAddress)
      return false;
    Guard = I;
  }
  if (!Guard)
    return false;
  bool HasEffect = false;
  for (size_t I = 0; I < Ops.size(); ++I) {
    bool Effect = false;
    switch (Ops[I].Opcode) {
    case NdOp::BRANCH:
      Effect = Delayed;
      break;
    case NdOp::LOAD:
    case NdOp::STORE:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
    case NdOp::INTRINSIC:
    case NdOp::INDIR_BR:
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::RETURN:
      Effect = true;
      break;
    default:
      break;
    }
    if (!Effect)
      continue;
    if (I < *Guard) {
      // A normal conditional link executes its slot before guarding the call.
      if (!Delayed)
        return false;
    } else {
      HasEffect = true;
    }
  }
  return HasEffect;
}

/// Check only the supplied operation slice. Complete block coverage, control
/// semantics and instruction modes require validateLowInstructionBoundaries.
inline std::optional<llvm::ArrayRef<LowOp>>
checkedInstructionOperations(const LowBlock &Block,
                             const LowInstructionBoundary &Instruction) {
  if (Instruction.FirstOp > Block.Ops.size() ||
      Instruction.OpCount > Block.Ops.size() - Instruction.FirstOp)
    return std::nullopt;
  const auto Ops = llvm::ArrayRef<LowOp>(Block.Ops).slice(Instruction.FirstOp,
                                                          Instruction.OpCount);
  if (const auto &Slot = Instruction.DelaySlot;
      Slot && (Slot->Offset != 4 || Slot->Size != 4 || Instruction.Size != 8 ||
               Slot->FirstOp > Ops.size() ||
               Slot->OpCount > Ops.size() - Slot->FirstOp))
    return std::nullopt;
  for (size_t I = 0; I < Ops.size(); ++I) {
    va_t Address = Instruction.Address;
    if (const auto &Slot = Instruction.DelaySlot;
        Slot && I >= Slot->FirstOp && I - Slot->FirstOp < Slot->OpCount)
      Address += Slot->Offset;
    if (Ops[I].Addr != Address)
      return std::nullopt;
  }
  return Ops;
}

} // namespace neverd::detail

#endif

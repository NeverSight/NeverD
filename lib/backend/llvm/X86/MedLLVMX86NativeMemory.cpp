//===- MedLLVMX86NativeMemory.cpp - Native memory entry state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain unknown-value evidence until native memory operand uses are resolved.
//===----------------------------------------------------------------------===//
#include "../emit/MedLLVMDeadSlots.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/intrinsics/X86/X86CompressExpand.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

#include <stdexcept>

namespace neverd {
bool MedLLVMEmitter::tryEmitX86NativeMemoryFunction(const MedFunc &Function) {
  bool Native = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      Native |=
          Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          (isX86CompactMemory(Intrinsic(Op.Inputs[0].ConstVal)) ||
           Intrinsic(Op.Inputs[0].ConstVal) == Intrinsic::X86FourSourceMemory);
  if (!Native)
    return false;
  const bool Executable = !PreserveUnknownSourceValues;
  // Use the existing source-state authority to retain missing entry values
  // while definitions and PHI edges are emitted. In particular, an unknown
  // mask must never become the legacy zero-initialized live-in value.
  llvm::SaveAndRestore<bool> Preserve(PreserveUnknownSourceValues, true);
  auto *Emitted = emitFunc(Function);
  if (Executable && Emitted) {
    llvm::SmallVector<llvm::AllocaInst *, 32> Slots;
    for (const auto &[Key, Slot] : VarAllocs)
      Slots.push_back(Slot);
    discardDeadMedLLVMSlots(Slots);
  }
  if (Executable && Emitted)
    for (auto &Block : *Emitted)
      for (auto &Instruction : llvm::make_early_inc_range(Block))
        if (llvm_source_unknown::isValue(&Instruction)) {
          if (!Instruction.use_empty())
            throw std::invalid_argument(
                "native x86 memory requires known entry values");
          Instruction.eraseFromParent();
        }
  return true;
}

} // namespace neverd

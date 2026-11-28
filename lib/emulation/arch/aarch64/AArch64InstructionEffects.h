//===- AArch64InstructionEffects.h - ARM64 admission and RAM effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_INSTRUCTION_EFFECTS_H
#define NEVERD_EMULATION_AARCH64_INSTRUCTION_EFFECTS_H
#include "AArch64Machine.h"

#include "llvm/ADT/SmallVector.h"

#include <capstone/capstone.h>

namespace neverd::emulation {
struct AArch64MemoryAccess {
  uint64_t Address;
  RegisterValue Value;
  unsigned Size, Permission;
  // Cache maintenance requires an accessible address, not a data load/store.
  // Its admission preflight must not manufacture memory observer events.
  bool CacheMaintenance = false;
};
/// Validate one checked instruction and describe its complete RAM footprint.
/// The ISA owns encodings and register views; this query has no guest effects.
llvm::Error getAArch64InstructionEffects(
    const cs_insn &Instruction, const AArch64MachineState &State,
    llvm::SmallVectorImpl<AArch64MemoryAccess> &Accesses);
} // namespace neverd::emulation
#endif

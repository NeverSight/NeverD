//===- MachOARM32Mode.h - Authenticated AArch32 code modes -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares strict parsing of the function-level ARM/Thumb mode carried by a
/// 32-bit ARM Mach-O symbol table.  A Mach-O CPU subtype selects a processor
/// ABI; it does not select the instruction set state of every function.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOARM32MODE_H
#define NEVERD_LOADER_MACHO_MACHOARM32MODE_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>

namespace neverd::macho_arm32 {

/// Positive Thumb-mode facts read directly from one validated ARM Mach-O
/// image.  An even nlist value without N_ARM_THUMB_DEF is not ARM-mode proof.
struct ModeInfo {
  uint32_t CPUSubtype = 0;
  std::map<va_t, InstructionMode> CodeSymbolModes;
  InstructionMode UniformMode = InstructionMode::Default;
};

/// Parse the exact CPU subtype and every Thumb-defined executable nlist symbol.
/// Capability bits, malformed command tables, conflicting definitions, and
/// non-ARM/64-bit inputs are rejected.
llvm::Expected<ModeInfo> parseModeInfo(llvm::ArrayRef<uint8_t> Binary);

/// Require every requested entry to have an exact nlist mode and require all
/// requested entries to use one code-generation mode.  The current registry
/// publishes only positive Thumb evidence; ARM and unknown entries fail closed.
llvm::Expected<InstructionMode>
requireUniformFunctionMode(const ModeInfo &Info,
                           llvm::ArrayRef<va_t> FunctionEntries);

/// Serialize one code pointer only when its target has exact symbol-table mode
/// provenance.  Data pointers are outside this helper's contract.
llvm::Expected<uint64_t> serializeCodePointer(const ModeInfo &Info,
                                              uint64_t Address);

} // namespace neverd::macho_arm32

#endif // NEVERD_LOADER_MACHO_MACHOARM32MODE_H

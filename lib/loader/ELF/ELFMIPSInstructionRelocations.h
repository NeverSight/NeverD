//===- ELFMIPSInstructionRelocations.h - Retained MIPS fields -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize linked ELF symbolic instruction fields without modifying bytes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_ELF_ELFMIPSINSTRUCTIONRELOCATIONS_H
#define NEVERD_LIB_LOADER_ELF_ELFMIPSINSTRUCTIONRELOCATIONS_H

#include "neverd/loader/ELF/MIPSInstructionRelocation.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd {
struct BinaryImage;
namespace elf_loader {
/// ELF64 packs the three operations on GP - (S + A) into one r_info.
inline std::optional<MIPSImmediatePart>
mipsPackedGPDisplacement(uint32_t Type) {
  using namespace llvm::ELF;
  constexpr uint32_t Prefix = R_MIPS_GPREL16 | (R_MIPS_SUB << 8);
  if (Type == (Prefix | (R_MIPS_HI16 << 16)))
    return MIPSImmediatePart::Hi16;
  if (Type == (Prefix | (R_MIPS_LO16 << 16)))
    return MIPSImmediatePart::Lo16;
  return std::nullopt;
}

void collectMIPSInstructionRelocations(BinaryImage &Image);
} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_ELF_ELFMIPSINSTRUCTIONRELOCATIONS_H

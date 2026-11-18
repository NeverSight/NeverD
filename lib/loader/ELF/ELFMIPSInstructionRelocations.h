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

namespace neverd {
struct BinaryImage;
namespace elf_loader {
void collectMIPSInstructionRelocations(BinaryImage &Image);
} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_ELF_ELFMIPSINSTRUCTIONRELOCATIONS_H

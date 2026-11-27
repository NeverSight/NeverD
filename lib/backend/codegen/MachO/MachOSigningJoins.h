//===- MachOSigningJoins.h - Joined Darwin signing addresses ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGJOINS_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGJOINS_H

#include "MachOSigningAddress.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace neverd {
struct LowFunc;

/// Each origin supplies the signing PC on its own incoming paths to AUT.
struct MachOSigningJoins {
  bool Unresolved = false;
  llvm::DenseMap<va_t, MachOSigningAddress> Addresses{};
  llvm::DenseMap<va_t, uint32_t> Consumers{};
  llvm::SmallDenseSet<uint32_t, 2> RetainedSigningWords{};
};

/// Prove ordinary incoming edges against their own reaching modifier and LR
/// definitions. An unresolved path cannot contribute a replacement.
MachOSigningJoins machOSigningJoins(const BinaryImage &Image, Decoder &Decode,
                                    const MachOLiftedSource &Source,
                                    const LowFunc &CFG);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGJOINS_H

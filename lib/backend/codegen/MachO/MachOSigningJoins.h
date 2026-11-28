//===- MachOSigningJoins.h - Darwin signing joins -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Prove signing-PC identities at ordinary control-flow joins.

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGJOINS_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGJOINS_H

#include "MachOSigningAddress.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <utility>

namespace neverd {
struct LowFunc;

struct MachOSigningIsland {
  va_t Consumer;
  unsigned Route;
  uint32_t Encoding;
};

/// Each origin supplies the signing PC on its own incoming paths to AUT.
struct MachOSigningJoins {
  bool Unresolved = false;
  llvm::DenseMap<va_t, MachOSigningAddress> Addresses{};
  llvm::DenseMap<va_t, uint32_t> Consumers{};
  // Only explicit incoming branches need a non-default label.
  llvm::DenseMap<va_t, std::pair<va_t, unsigned>> BranchRoutes{};
  llvm::DenseMap<va_t, MachOSigningConsumer> Dispatches{};
  llvm::DenseMap<va_t, llvm::SmallVector<MachOSigningIsland, 1>> Islands{};
  llvm::SmallDenseSet<va_t, 2> SigningPCs{};
  llvm::SmallDenseSet<uint32_t, 2> RetainedSigningWords{};
};

/// Prove ordinary incoming edges against their own reaching modifier and LR
/// definitions. An unresolved path cannot contribute a replacement.
MachOSigningJoins machOSigningJoins(const BinaryImage &Image, Decoder &Decode,
                                    const MachOLiftedSource &Source,
                                    const LowFunc &CFG);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGJOINS_H

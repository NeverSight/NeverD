//===- MachOEmbeddedDispatch.h - Relocatable switch dispatch ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_EMBEDDEDDISPATCH_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_EMBEDDEDDISPATCH_H

#include "neverd/loader/MachO/FunctionBody.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace neverd {
class Decoder;
struct BinaryImage;
struct LowFunc;

/// Borrowed targets from the source CFG. Only these table-address producers
/// and indirect transfers can move with the copied function.
struct MachOEmbeddedDispatch {
  llvm::DenseSet<va_t> TableAddresses;
  llvm::DenseMap<va_t, llvm::ArrayRef<va_t>> BranchTargets;
  std::vector<MachODataInCodeRange> Recovered;
};

std::optional<MachOEmbeddedDispatch>
machOEmbeddedDispatch(const BinaryImage &Image, Decoder &Decode,
                      const MachOLiftedSource &Source, const LowFunc &Function);
} // namespace neverd
#endif

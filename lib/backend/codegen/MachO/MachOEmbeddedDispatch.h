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
#include "llvm/ADT/SmallVector.h"

#include <utility>

namespace neverd {
class Decoder;
struct BinaryImage;
struct LowFunc;
struct JumpTable;

/// Borrowed targets from the source CFG. Only these table-address producers
/// and indirect transfers can move with the copied function.
struct MachOEmbeddedDispatch {
  llvm::DenseSet<va_t> TableAddresses;
  llvm::DenseMap<va_t, const JumpTable *> BranchTables;
  std::vector<MachODataInCodeRange> Recovered;
  /// Whole instruction intervals outside the entry CFG and the data records.
  llvm::SmallVector<std::pair<va_t, va_t>, 0> UnreachedCode;
};

/// Cover a machine read with same-kind records. If supplied, Missing receives
/// only the portions of the read omitted from the source table.
bool coverMachOEmbeddedRead(
    const MachOLiftedSource &Source, va_t Start, va_t Address, uint64_t Size,
    uint16_t Kind, std::vector<MachODataInCodeRange> *Missing = nullptr);

std::optional<MachOEmbeddedDispatch>
machOEmbeddedDispatch(const BinaryImage &Image, Decoder &Decode,
                      const MachOLiftedSource &Source, const LowFunc &Function);
} // namespace neverd
#endif

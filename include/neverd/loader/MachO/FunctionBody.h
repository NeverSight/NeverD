//===- FunctionBody.h - Darwin arm64e function extents ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_FUNCTIONBODY_H
#define NEVERD_LOADER_MACHO_FUNCTIONBODY_H

#include "neverd/loader/MachO/DataInCode.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// File-backed source extent shared by the register producer and publisher.
/// A forwarding entry consists solely of its first unconditional branch; the
/// old instructions after it are unreachable from that entry.
struct MachOLiftedSource {
  llvm::ArrayRef<uint8_t> Bytes;
  std::optional<va_t> ForwardTarget;
  llvm::ArrayRef<MachODataInCodeRange> Embedded;
};

/// Bound an authenticated entry by its instruction section or, for sectionless
/// installed code, its unwind range. NextEntry is the next mapped function, or
/// InvalidVA at the end of the mapping inventory.
std::optional<MachOLiftedSource> machOLiftedSource(const BinaryImage &Image,
                                                   va_t Start, va_t NextEntry);

/// Original function which owns an aligned interior address as storage.
/// After publication the old entry may forward while its bytes remain mapped.
std::optional<va_t> machOInteriorAddressOwner(const BinaryImage &Image,
                                              va_t VA);

/// Target of an integer literal load from a dyld-rebased, immutable slot.
std::optional<va_t> machORebasedLiteralAddress(const BinaryImage &Image,
                                               uint32_t Word, va_t PC);

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_FUNCTIONBODY_H

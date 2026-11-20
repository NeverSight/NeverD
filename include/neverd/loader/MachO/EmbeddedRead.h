//===- EmbeddedRead.h - Mach-O embedded data reads -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_EMBEDDEDREAD_H
#define NEVERD_LOADER_MACHO_EMBEDDEDREAD_H

#include "neverd/loader/MachO/DataInCode.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Match every intersecting record to the decoded read. When fresh evidence
/// supplies unmarked bytes, optionally return just those gaps for publication.
bool coverMachODataInCodeRead(
    llvm::ArrayRef<MachODataInCodeRange> Records, va_t Address, uint64_t Size,
    uint16_t Kind, bool AllowUnmarked,
    std::vector<MachODataInCodeRange> *Missing = nullptr);

struct MachOBoundedTableStorage {
  va_t Base, End;
  uint16_t Kind;
  bool AllowUnmarked;
  llvm::ArrayRef<MachODataInCodeRange> Records;

  bool contains(va_t Address, uint64_t Size) const;
};

/// Storage for a finite relative dispatch in immutable instruction bytes.
/// Its extent does not establish a runtime selector domain.
std::optional<MachOBoundedTableStorage>
getMachOBoundedTableStorage(const BinaryImage &Image, va_t Base,
                            uint16_t EntrySize);

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_EMBEDDEDREAD_H

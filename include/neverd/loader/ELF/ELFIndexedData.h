//===- ELFIndexedData.h - Bounds of linked ELF data reads ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_ELFINDEXEDDATA_H
#define NEVERD_LOADER_ELF_ELFINDEXEDDATA_H

#include "neverd/Common.h"

#include <mutex>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;

/// Layout facts for one analysis of an unchanged image. Parallel converters
/// share the symbol boundaries, built on the first qualifying indexed read.
class ELFIndexedData {
public:
  explicit ELFIndexedData(const BinaryImage &Image) : Image(Image) {}

  /// Identify the storage of a runtime load whose whole extent fits one
  /// read-only mapping and crosses no known data object boundary.
  std::optional<va_t> readOnlyOwner(va_t Address, uint64_t Size) const;

private:
  const BinaryImage &Image;
  mutable std::once_flag Init;
  mutable std::vector<va_t> Boundaries;
};
} // namespace neverd

#endif // NEVERD_LOADER_ELF_ELFINDEXEDDATA_H

//===- ReadOnlyBytesIndex.h - Budgeted storage queries --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound immutable storage queries and scoped PE32 instruction indexes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_READONLYBYTESINDEX_H
#define NEVERD_LIB_LOADER_READONLYBYTESINDEX_H

#include "neverd/Common.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
struct RelocatedAddressField;

/// PE32-only budgeted forms of the shared immutable storage validators. Ordered
/// fixup inventories pay for logarithmic lookup and only the bounded overlap
/// window; vector inventories and nested mapping ownership still pay their full
/// scans.
std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size,
                        size_t &Work, size_t Limit);
std::optional<va_t>
readImmutablePE32CodePointerStorage(const BinaryImage &Image, va_t Address,
                                    size_t &Work, size_t Limit);

/// Stronger PE32 instruction storage: every supplied absolute operand must
/// have exactly one HIGHLOW relocation. Uses the ordinary code validator and
/// bounds fresh ordered lookups without constructing or caching an index.
/// The caller still owns instruction meaning and the target's runtime role.
std::optional<std::vector<uint8_t>> readRelocatedPE32CodeBytes(
    const BinaryImage &Image, va_t Address, uint32_t Size,
    const std::vector<va_t> &AbsoluteOperands, size_t &Work, size_t Limit);

namespace read_only_detail {
struct PE32CodeFixup {
  enum class Kind { Forbidden, Base, Operand } Type;
  va_t Address;
  uint8_t Width;
  uint8_t BaseType = 0;
  const RelocatedAddressField *Field = nullptr;
};
} // namespace read_only_detail

/// A scoped index over an immutable BinaryImage. The image and its metadata
/// must remain unchanged for this object's lifetime. This private accelerator
/// shares the ordinary reader's validator; it grants no additional authority.
class IndexedPE32CodeBytes {
public:
  IndexedPE32CodeBytes(const BinaryImage &Image, size_t &Work, size_t Limit);
  std::optional<std::vector<uint8_t>>
  read(va_t Address, uint32_t Size, const std::vector<va_t> &AbsoluteOperands);

private:
  bool charge(size_t Amount);
  const BinaryImage &Image;
  size_t &Work;
  size_t Limit;
  bool Valid = false;
  uint8_t MaximumWidth = 8;
  std::vector<read_only_detail::PE32CodeFixup> Fixups;
};
} // namespace neverd

#endif

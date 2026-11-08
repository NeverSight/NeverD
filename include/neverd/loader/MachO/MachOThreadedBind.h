//===- MachOThreadedBind.h - Threaded dyld programs -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared bytecode plan for analysis and transactional authenticated storage.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOTHREADEDBIND_H
#define NEVERD_LOADER_MACHO_MACHOTHREADEDBIND_H

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::macho_loader::threaded {

struct StreamKind {
  bool Threaded = false;
  bool Malformed = false;
};

struct Import {
  std::string Name;
  int64_t Ordinal = 0;
  int64_t Addend = 0;
  uint8_t Flags = 0;
};

struct Root {
  unsigned SegmentIndex = 0;
  uint64_t Offset = 0;
};

struct Program {
  std::vector<Import> Imports;
  std::vector<Root> Roots;
  size_t CopiedBytes = 0;
};

/// Decode operands while selecting the format; payload bytes are not opcodes.
StreamKind classify(llvm::ArrayRef<uint8_t> Bytes, bool Lazy = false);

/// Validate one complete ordinal table/root program. Storage and referenced
/// library identities are validated by the image owner after this boundary.
std::optional<Program> decode(llvm::ArrayRef<uint8_t> Bytes,
                              size_t LibraryCount);

} // namespace neverd::macho_loader::threaded

#endif // NEVERD_LOADER_MACHO_MACHOTHREADEDBIND_H

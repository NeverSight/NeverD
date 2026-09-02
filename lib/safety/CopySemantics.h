//===- CopySemantics.h - Copy-family call semantics -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_COPYSEMANTICS_H
#define NEVERD_LIB_SAFETY_COPYSEMANTICS_H

#include "neverd/safety/CountedWriteSemantics.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace neverd::safety::detail {

inline std::string copySemanticName(llvm::StringRef Name) {
  return counted_write::canonicalSemanticName(Name);
}

inline bool usesWideElements(llvm::StringRef Name) {
  if (const std::optional<counted_write::Layout> Shape =
          counted_write::classifyLayout(Name))
    if (Shape->UsesWideElements)
      return true;
  const std::string Normalized = copySemanticName(Name);
  return Normalized == "wcscpy" || Normalized == "wcscat";
}

inline std::optional<uint64_t> countedWideElementBytes(llvm::StringRef Name,
                                                       BinaryFormat Format) {
  const std::optional<counted_write::Layout> Shape =
      counted_write::classifyLayout(Name);
  if (!Shape || !Shape->UsesWideElements)
    return std::nullopt;
  const std::optional<counted_write::Semantics> Semantics =
      counted_write::classify(Name, Format);
  if (!Semantics)
    return std::nullopt;
  return Semantics->ElementBytes;
}

inline bool requiresStringExtents(llvm::StringRef Name) {
  const std::string Normalized = copySemanticName(Name);
  return Normalized == "strcat" || Normalized == "strncat" ||
         Normalized == "strlcat" || Normalized == "strlcpy" ||
         Normalized == "strcat_chk" || Normalized == "strncat_chk";
}

inline bool usesTotalDestinationBound(llvm::StringRef Name) {
  const std::string Normalized = copySemanticName(Name);
  return Normalized == "strlcpy" || Normalized == "strlcat";
}

inline bool isExactCountedMemoryAccess(llvm::StringRef Name) {
  return counted_write::classifyLayout(Name).has_value();
}

inline bool copyAccessRequiresPositiveCount(llvm::StringRef Name,
                                            bool IsDestination) {
  const std::string Normalized = copySemanticName(Name);
  if (isExactCountedMemoryAccess(Normalized) || Normalized == "strncpy" ||
      Normalized == "strncpy_chk")
    return true;
  if (Normalized == "strncat" || Normalized == "strncat_chk")
    return !IsDestination;
  if (Normalized == "strlcpy" || Normalized == "strlcat")
    return IsDestination;
  return false;
}

inline std::optional<uint64_t> exactCountedMemoryBytes(llvm::StringRef Name,
                                                       BinaryFormat Format,
                                                       uint64_t Count) {
  const std::optional<counted_write::Semantics> Semantics =
      counted_write::classify(Name, Format);
  if (!Semantics)
    return std::nullopt;
  if (Count == 0)
    return 0;
  if (Count > std::numeric_limits<uint64_t>::max() / Semantics->ElementBytes)
    return std::nullopt;
  return Count * Semantics->ElementBytes;
}

inline bool fortifiedCountedAccessIsRejected(llvm::StringRef Name,
                                             BinaryFormat Format,
                                             uint64_t Count,
                                             uint64_t ObjectCapacity) {
  const std::string Normalized = copySemanticName(Name);
  if (Normalized == "strncpy_chk" || Normalized == "snprintf_chk")
    return Count > ObjectCapacity;
  std::optional<uint64_t> Bytes = exactCountedMemoryBytes(Name, Format, Count);
  return Bytes && *Bytes > ObjectCapacity;
}

inline bool isExactCopySourceRead(llvm::StringRef Name) {
  const std::optional<counted_write::Layout> Shape =
      counted_write::classifyLayout(Name);
  return Shape && (Shape->Kind == counted_write::SemanticKind::Memcpy ||
                   Shape->Kind == counted_write::SemanticKind::Memmove ||
                   Shape->Kind == counted_write::SemanticKind::Bcopy);
}

inline std::optional<uint64_t>
exactCopyReadBytes(llvm::StringRef Name, BinaryFormat Format, uint64_t Count) {
  const std::string Normalized = copySemanticName(Name);
  if (isExactCopySourceRead(Normalized))
    return exactCountedMemoryBytes(Normalized, Format, Count);
  return std::nullopt;
}

} // namespace neverd::safety::detail

#endif // NEVERD_LIB_SAFETY_COPYSEMANTICS_H

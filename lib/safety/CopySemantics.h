//===- CopySemantics.h - Copy-family call semantics -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_COPYSEMANTICS_H
#define NEVERD_LIB_SAFETY_COPYSEMANTICS_H

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/SinkCatalog.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace neverd::safety::detail {

inline std::string copySemanticName(llvm::StringRef Name) {
  std::string Normalized = SinkCatalog::normalize(Name);
  if (Normalized == "aeabi_memcpy" || Normalized == "aeabi_memcpy4" ||
      Normalized == "aeabi_memcpy8")
    return "memcpy";
  if (Normalized == "aeabi_memmove" || Normalized == "aeabi_memmove4" ||
      Normalized == "aeabi_memmove8")
    return "memmove";
  if (Normalized == "aeabi_memset" || Normalized == "aeabi_memset4" ||
      Normalized == "aeabi_memset8")
    return "memset";
  if (Normalized == "aeabi_memclr" || Normalized == "aeabi_memclr4" ||
      Normalized == "aeabi_memclr8")
    return "bzero";
  return Normalized;
}

inline bool usesWideElements(llvm::StringRef Name) {
  const std::string Normalized = copySemanticName(Name);
  return Normalized == "wmemcpy" || Normalized == "wmemmove" ||
         Normalized == "wcscpy" || Normalized == "wcscat";
}

inline std::optional<uint64_t>
countedWideElementBytes(llvm::StringRef Name, BinaryFormat Format) {
  const std::string Normalized = copySemanticName(Name);
  if (Normalized != "wmemcpy" && Normalized != "wmemmove")
    return std::nullopt;
  switch (Format) {
  case BinaryFormat::COFF:
    return 2;
  case BinaryFormat::ELF:
  case BinaryFormat::MachO:
    return 4;
  default:
    return std::nullopt;
  }
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
  const std::string Normalized = copySemanticName(Name);
  return Normalized == "memcpy" || Normalized == "memmove" ||
         Normalized == "wmemcpy" || Normalized == "wmemmove" ||
         Normalized == "bcopy" || Normalized == "memset" ||
         Normalized == "bzero" || Normalized == "memcpy_chk" ||
         Normalized == "memmove_chk" || Normalized == "memset_chk";
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
  const std::string Normalized = copySemanticName(Name);
  if (!isExactCountedMemoryAccess(Normalized))
    return std::nullopt;
  if (Count == 0)
    return 0;
  if (Normalized == "wmemcpy" || Normalized == "wmemmove") {
    std::optional<uint64_t> ElementBytes =
        countedWideElementBytes(Normalized, Format);
    if (!ElementBytes ||
        Count > std::numeric_limits<uint64_t>::max() / *ElementBytes)
      return std::nullopt;
    return Count * *ElementBytes;
  }
  return Count;
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
  const std::string Normalized = copySemanticName(Name);
  return Normalized == "memcpy" || Normalized == "memmove" ||
         Normalized == "wmemcpy" || Normalized == "wmemmove" ||
         Normalized == "bcopy" || Normalized == "memcpy_chk" ||
         Normalized == "memmove_chk";
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

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

inline bool usesWideElements(llvm::StringRef Name) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  return Normalized == "wmemcpy" || Normalized == "wmemmove" ||
         Normalized == "wcscpy" || Normalized == "wcscat";
}

inline std::optional<uint64_t>
countedWideElementBytes(llvm::StringRef Name, BinaryFormat Format) {
  const std::string Normalized = SinkCatalog::normalize(Name);
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
  const std::string Normalized = SinkCatalog::normalize(Name);
  return Normalized == "strcat" || Normalized == "strncat" ||
         Normalized == "strlcat" || Normalized == "strlcpy" ||
         Normalized == "strcat_chk" || Normalized == "strncat_chk";
}

inline bool usesTotalDestinationBound(llvm::StringRef Name) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  return Normalized == "strlcpy" || Normalized == "strlcat";
}

inline std::optional<uint64_t>
exactCopyReadBytes(llvm::StringRef Name, BinaryFormat Format, uint64_t Count) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  if (Normalized == "wmemcpy" || Normalized == "wmemmove") {
    std::optional<uint64_t> ElementBytes =
        countedWideElementBytes(Normalized, Format);
    if (!ElementBytes ||
        Count > std::numeric_limits<uint64_t>::max() / *ElementBytes)
      return std::nullopt;
    return Count * *ElementBytes;
  }
  if (Normalized == "memcpy" || Normalized == "memmove" ||
      Normalized == "bcopy" || Normalized == "memcpy_chk" ||
      Normalized == "memmove_chk")
    return Count;
  return std::nullopt;
}

} // namespace neverd::safety::detail

#endif // NEVERD_LIB_SAFETY_COPYSEMANTICS_H

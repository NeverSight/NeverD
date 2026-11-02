//===- CDisplayFieldPath.h - C member paths --------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared validation of display member names and constant array indices.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_CDISPLAYFIELDPATH_H
#define NEVERD_LIB_BACKEND_C_CDISPLAYFIELDPATH_H

#include "CIdentifier.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd {
/// Accept only member identifiers and decimal constant array indices. The
/// layout owner proves the access width, bounds and absence of ambiguity.
inline std::optional<std::string>
canonicalizeDisplayPath(llvm::StringRef Field) {
  std::string Path;
  llvm::StringRef Rest(Field);
  while (!Rest.empty()) {
    const auto Split = Rest.split('.');
    // A member, then the elements of an array it indexes: Reserved[1].
    llvm::StringRef Indexes = Split.first;
    const std::string Comp =
        Indexes.take_until([](char C) { return C == '['; }).str();
    Indexes = Indexes.drop_front(Comp.size());
    const std::string Name = canonicalizeCProjectionIdentifier(Comp, "");
    if (Name.empty() || Name != Comp)
      return std::nullopt;
    if (!Path.empty())
      Path += '.';
    Path += Name;
    while (!Indexes.empty()) {
      const size_t Close = Indexes.find(']');
      uint64_t Index = 0;
      if (!Indexes.starts_with("[") || Close == llvm::StringRef::npos ||
          Indexes.slice(1, Close).getAsInteger(10, Index))
        return std::nullopt;
      Path += "[" + std::to_string(Index) + "]";
      Indexes = Indexes.drop_front(Close + 1);
    }
    Rest = Split.second;
  }
  if (Path.empty())
    return std::nullopt;
  return Path;
}
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_CDISPLAYFIELDPATH_H

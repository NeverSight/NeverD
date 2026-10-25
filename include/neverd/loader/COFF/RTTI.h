//===- RTTI.h - Bounded MSVC vftable type evidence ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads a primary x64 MSVC vftable's type from its complete-object locator.
/// Missing, malformed and unsupported layouts supply no type evidence.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_RTTI_H
#define NEVERD_LOADER_COFF_RTTI_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct BinaryImage;

namespace coff_loader {

/// The exact mangled type body of a primary vftable linker name. Secondary
/// vftables with a base-class path are deliberately not interpreted here.
inline std::optional<llvm::StringRef>
primaryVTableTypeName(llvm::StringRef Name) {
  if (!Name.consume_front("??_7") || !Name.consume_back("6B@") ||
      !Name.ends_with("@@") || Name.size() <= 2)
    return std::nullopt;
  return Name;
}

/// Returns the mangled type body, borrowing image bytes. This verifies the
/// complete-object locator, class hierarchy and self base descriptor before
/// using TypeDescriptor::name. It does not identify a function or its ABI.
std::optional<llvm::StringRef> readPrimaryVTableTypeName(const BinaryImage &Img,
                                                         uint64_t VTable);

} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LOADER_COFF_RTTI_H

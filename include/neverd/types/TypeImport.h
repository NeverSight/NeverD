//===- TypeImport.h - Independent prototype providers ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Import missing prototypes from an independently parsed provider without
/// rebinding its same-named types to another provider's declarations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPEIMPORT_H
#define NEVERD_TYPES_TYPEIMPORT_H

#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::types {

struct PrototypeImport {
  std::vector<std::string> Types;
  std::vector<std::string> Prototypes;
  /// Provider type names in the complete dependency closure, including reused
  /// definitions. Renamed maps these original names to their imported names.
  std::vector<std::string> Dependencies;
  /// Original and imported names for conflicting types and their dependents.
  std::vector<std::pair<std::string, std::string>> Renamed;
};

/// Add prototypes absent from \p Library, with their complete type dependency
/// closure from \p Provider. Reuse identical definitions; give conflicting
/// types and affected existing dependents names beginning with \p Prefix.
/// Equality includes pointee types, qualifiers, conventions and enum values,
/// not just sizes. Existing prototypes and types retain their definitions.
/// If Names is present, import only those exact provider prototypes (an empty
/// selection imports nothing). Every selected name must exist in Provider.
/// The data model and record ABI must agree. Errors leave Library unchanged.
llvm::Expected<PrototypeImport> importPrototypes(
    TypeLibrary &Library, const TypeLibrary &Provider, llvm::StringRef Prefix,
    std::optional<llvm::ArrayRef<llvm::StringRef>> Names = std::nullopt);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPEIMPORT_H

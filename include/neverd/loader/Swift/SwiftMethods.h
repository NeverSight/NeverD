//===- SwiftMethods.h - Structured Swift source signatures -------*- C++
//-*-===//
#ifndef NEVERD_LOADER_SWIFT_SWIFTMETHODS_H
#define NEVERD_LOADER_SWIFT_SWIFTMETHODS_H

#include "neverd/Common.h"

#include <memory>
#include <string>
#include <vector>

namespace neverd {

/// Source declarations recovered from a demangling tree. These are source
/// hints, not authenticated ABI/debug information or ownership evidence.
struct SwiftSourceType {
  enum class Kind { Void, Boolean, Integer, Pointer, Floating };
  Kind TheKind = Kind::Void;
  std::string Name = "Void";
  unsigned Bits = 0;
  bool IsSigned = false;
  std::shared_ptr<SwiftSourceType> Pointee;
};

struct SwiftSourceParameter {
  std::string Name;
  SwiftSourceType Type;
};

struct SwiftStorageField {
  std::string Name;
  SwiftSourceType Type;
  uint64_t Offset = 0;
  bool IsMutable = false;
  /// Context-wide source storage spelling when a recovered accessor owns Name.
  /// Empty preserves the runtime field name; this never changes native layout.
  std::string BackingName;
};

struct SwiftSourceSignature {
  va_t Entry = 0;
  std::string MangledSymbol;
  std::string Module;
  /// "global", "class", or "struct"; nested/generic contexts are excluded.
  std::string ContextKind = "global";
  std::string ContextName;
  std::string Name;
  std::string DeclarationKind = "function";
  std::vector<std::string> Labels;
  std::vector<SwiftSourceParameter> Parameters;
  SwiftSourceType ReturnType;
  bool IsStatic = false;
  bool IsMutating = false;
  /// Mangling alone does not establish value-type self's physical convention.
  bool IsMutatingKnown = false;
  /// Established from the fixed ABI and native entry-value flow, not mangling.
  std::string SelfConvention;
  /// Native proof resolved a struct initializer's complete scalar result to
  /// the sole stored field. ReturnType then describes that ABI scalar.
  bool ReturnsContextValue = false;
  std::string UnsupportedReason;
  /// Set only from independently decoded native type metadata, never trusted
  /// merely because a submitted source-signature JSON asserts a layout.
  bool ContextLayoutKnown = false;
  std::vector<SwiftStorageField> ContextFields;
};

} // namespace neverd
#endif

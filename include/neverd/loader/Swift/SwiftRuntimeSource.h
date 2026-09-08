//===- SwiftRuntimeSource.h - Proven compiler source projections -*- C++
//-*-===//
#ifndef NEVERD_LOADER_SWIFT_SWIFTRUNTIMESOURCE_H
#define NEVERD_LOADER_SWIFT_SWIFTRUNTIMESOURCE_H

#include "neverd/loader/Swift/SwiftMethods.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;

enum class SwiftRuntimeSourceKind {
  AllocatingInitializer,
  TrivialDestructor,
  DeallocatingDestructor,
  TypeMetadataAccessor,
  ModifyAccessor,
  ModifyResume
};

struct SwiftRuntimeSourceIdentity {
  va_t Entry = 0;
  std::string MangledSymbol;
};

struct SwiftRuntimeSourceRequest {
  SwiftRuntimeSourceKind Kind = SwiftRuntimeSourceKind::TypeMetadataAccessor;
  SwiftSourceSignature Signature;
  /// Allocators require the exact initializing-constructor identity/signature;
  /// both native effect graphs are inspected, never inferred from field names.
  std::optional<SwiftSourceSignature> Initializer;
  /// Deallocators name their destructor; modifiers name their continuation;
  /// continuations name their modifier. Both native bodies are inspected.
  std::optional<SwiftRuntimeSourceIdentity> RelatedEntry;
};

struct SwiftRuntimeSourceDependency {
  /// "context", "method", "property", or "compiler_entry".
  std::string Kind;
  std::string Module;
  std::string ContextKind;
  std::string ContextName;
  std::string Name;
  SwiftRuntimeSourceIdentity Identity;
};

struct SwiftRuntimeSourceProof {
  bool Proven = false;
  std::string ProjectionKind;
  std::string Reason;
  std::vector<std::string> Evidence;
  std::vector<SwiftRuntimeSourceDependency> Dependencies;
  va_t Descriptor = 0;
  va_t Metadata = 0;
  uint64_t FieldOffset = 0;
  SwiftRuntimeSourceIdentity Continuation;
};

/// Establish a bounded native runtime-effect model for a compiler-generated
/// callable. This does not establish source recovery, authenticate an ABI, or
/// authorize executable rewriting. The source exporter must satisfy every
/// dependency using emitted source in the same context before claiming a
/// compiler source projection. Unknown effects remain explicitly unproven.
SwiftRuntimeSourceProof
recoverSwiftRuntimeSource(const SwiftRuntimeSourceRequest &Request,
                          const BinaryImage &Image,
                          const std::vector<LowFunc> &NativeFunctions);
} // namespace neverd
#endif

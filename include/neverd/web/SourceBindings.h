//===- SourceBindings.h - JavaScript lexical binding analysis ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript lexical binding analysis.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Source.h"

namespace neverd::web {

inline constexpr uint32_t NoSourceIndex = UINT32_MAX;
inline constexpr std::string_view JavaScriptBindingProfile =
    "javascript-lexical-bindings-v1";
inline constexpr uint64_t MaxJavaScriptBindingSteps = 2000000;

/// A scope is a static family of environments, not a runtime activation.
struct SourceScope {
  std::string ID;
  std::string Kind;
  uint32_t Parent = NoSourceIndex;
  uint32_t Node = NoSourceIndex;
  uint32_t VariableScope = NoSourceIndex;
  bool Strict = false;
  bool PerIteration = false;
  bool ObjectEnvironment = false;
  bool PossibleEvalDeclarations = false;
  bool PossibleDirectEval = false;
};

struct SourceBinding {
  std::string ID;
  std::string Kind;
  uint32_t Scope = NoSourceIndex;
  uint32_t Name = NoSourceIndex;
  bool Conflicting = false;
  bool Immutable = false;
  bool HasTemporalDeadZone = false;
  bool Implicit = false;
};

struct SourceDeclaration {
  uint32_t Node = NoSourceIndex;
  uint32_t Binding = NoSourceIndex;
  uint32_t OccurrenceScope = NoSourceIndex;
  std::string Kind;
};

struct SourceReference {
  std::string ID;
  uint32_t Node = NoSourceIndex;
  uint32_t Scope = NoSourceIndex;
  uint32_t Name = NoSourceIndex;
  uint32_t Binding = NoSourceIndex;
  std::string Access;
  // lexical_binding, external, dynamic_with, dynamic_eval,
  // annex_b_uncertain or conflicting_declaration. External means absent
  // from this source, never a proven intrinsic or even a resolvable name.
  std::string Resolution;
};

struct SourceBindingAnalysis {
  std::string ID;
  std::string SourceID;
  std::string Status;
  std::vector<SourceScope> Scopes;
  std::vector<SourceBinding> Bindings;
  std::vector<SourceDeclaration> Declarations;
  std::vector<SourceReference> References;
  // Private, decoded identifier names. Ordinary output exposes identities
  // and links only. Do not include these values in metadata or diagnostics.
  std::vector<std::u16string> Names;
  std::vector<uint32_t> NodeScopes;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t DiagnosticCount = 0;
  uint64_t Steps = 0;
};

/// Resolve declaration identities, not runtime values or definite execution.
/// A lexical result does not prove initialization, reachability, freedom from
/// exceptions, mutation, eval exposure or source-reflection safety.
SourceBindingAnalysis analyzeSourceBindings(const SourceAnalysis &Source);

} // namespace neverd::web

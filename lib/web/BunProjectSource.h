//===- BunProjectSource.h - Verified Bun source reference relocation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verified Bun source reference relocation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PRIVATE_BUNPROJECTSOURCE_H
#define NEVERD_WEB_PRIVATE_BUNPROJECTSOURCE_H

#include "neverd/web/Source.h"

#include <span>

namespace neverd::web {

class BunProjectRequireCall;

struct BunProjectSourceTarget {
  // Exact UTF-8 virtual identity. This is never interpreted as a host path.
  std::string VirtualName;
  // A fixed, flat, portable filename supplied by the project owner. Only ASCII
  // letters, digits, '.', '_' and '-' are admitted, avoiding URL
  // reinterpretation.
  std::string OutputFile;
};

struct BunProjectSourceEdit {
  // Byte offsets in the original source, including the literal's quotes.
  uint64_t Start = 0, End = 0;
  std::u16string OldTarget, NewTarget;
  std::string Kind;
};

struct BunProjectSourceReference {
  uint64_t Start = 0, End = 0;
  // Preserve JS code units, including lone surrogates. Serialize as units,
  // never as ordinary UTF-8 metadata.
  std::u16string Target;
};

struct BunProjectSource {
  std::string Status = "not_run", ParseStatus = "not_run", Text;
  std::vector<BunProjectSourceEdit> Edits;
  std::vector<SourceDiagnostic> Diagnostics;
  std::vector<BunProjectSourceReference> UnresolvedRuntimeReferences;
  // Admitted module-reference syntax whose literal is outside Bun's virtual
  // namespaces. These are unchanged requests, not proof of missing packages.
  std::vector<BunProjectSourceReference> ExternalModuleReferences;
};

inline constexpr uint64_t MaxBunProjectSourceTargets = 4096;
inline constexpr uint64_t MaxBunProjectTargetBytes = 8ULL * 1024 * 1024;
inline constexpr uint64_t MaxBunProjectSourceReferences = 100000;

/// Plan only AST-selected module source literals and exact noncomputed
/// import.meta.require(literal) calls. Imported aliases additionally require an
/// immutable proof from the private require owner, checked against this source.
/// All other virtual string literals remain unresolved. A successful candidate
/// reparses to the same tree except for the
/// listed literal values; it does not establish runtime or loader equivalence.
/// Failures publish neither a candidate nor partial edits. Source and mappings
/// are analyzed as data; no imports, target code, or subprocesses run.
BunProjectSource
planBunProjectSource(std::string_view ArtifactID, std::string_view Text,
                     std::string_view SourceType,
                     std::span<const BunProjectSourceTarget> Targets,
                     std::span<const BunProjectRequireCall> RequireCalls = {});

} // namespace neverd::web

#endif // NEVERD_WEB_PRIVATE_BUNPROJECTSOURCE_H

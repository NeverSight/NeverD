//===- SourcePaths.h - Captured source path candidates -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured source path candidates.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceOrigins.h"
#include "neverd/web/SourceValues.h"

#include <span>

namespace neverd::web {
inline constexpr std::string_view CapturedPathProfile =
    "javascript-captured-portable-path-candidates-v1";
inline constexpr uint64_t MaxCapturedPathSteps = 2000000;
inline constexpr uint64_t MaxCapturedPathUnits = 4096;
inline constexpr uint64_t MaxCapturedPathAllocatedUnits = 1048576;

enum class CapturedPathKind {
  Unknown,
  Text,
  Path,
  FileURLString,
  FileURLObject
};
struct CapturedPathValue {
  CapturedPathKind Kind = CapturedPathKind::Unknown;
  std::string Reason = "unsupported_path_expression";
  std::u16string Text; // Private; Path/URL use a virtual captured-root prefix.
  uint32_t RootNode = NoSourceIndex, OperationNode = NoSourceIndex;
};
struct CapturedPathContext {
  std::string NamespaceID, ManifestID;
  // Caller supplies exact admitted occurrence paths, never a host path.
  // ApplicationDirectory is empty at namespace root, otherwise canonical
  // relative components followed by exactly one '/'.
  std::string SourceMemberPath, ApplicationDirectory;
};
struct SourcePaths {
  std::string ID, SourceID, Status, Reason;
  std::vector<std::shared_ptr<const CapturedPathValue>> Nodes;
  uint64_t Steps = 0, AllocatedUnits = 0;
};

/// Only requested expressions and their dependencies are evaluated. Roots are
/// syntactic candidates under the captured-file context, never actual runtime
/// absolute paths. No filesystem, module loading, URL fetch or target call.
SourcePaths analyzeSourcePaths(const SourceAnalysis &Source,
                               const SourceBindingAnalysis &Bindings,
                               const SourceModuleAnalysis &Modules,
                               const SourceOrigins &Origins,
                               const SourceValueAnalysis &Values,
                               const CapturedPathContext &Context,
                               std::span<const uint32_t> RequestedNodes);

/// Resolve a renderer loadFile path using the selected app base; preload
/// paths must already be captured absolute candidates. Returns a private exact
/// namespace path or a fixed refusal reason, never a host pathname.
/// ApplicationDirectory follows CapturedPathContext's canonical contract.
struct CapturedPathTarget {
  std::string Path, Status;
};
CapturedPathTarget capturedEntryTarget(const CapturedPathValue &Value,
                                       std::string_view ApplicationDirectory,
                                       std::string_view EntryKind);
} // namespace neverd::web

#pragma once

#include "neverd/web/SourceBindings.h"

namespace neverd::web {

inline constexpr std::string_view JavaScriptBundleProfile =
    "webpack5-basic-loader-v1";
inline constexpr uint64_t MaxJavaScriptBundleSteps = 4000000;
inline constexpr uint64_t MaxJavaScriptBundles = 64;
inline constexpr uint64_t MaxJavaScriptBundleModules = 4096;
inline constexpr uint64_t MaxJavaScriptBundleDependencies = 50000;
inline constexpr uint64_t MaxJavaScriptBundleNameUnits = 1048576;

struct SourceBundle {
  std::string ID;
  uint32_t Node = NoSourceIndex;
  uint32_t TableNode = NoSourceIndex;
  uint32_t LoaderNode = NoSourceIndex;
  uint32_t TableBinding = NoSourceIndex;
  uint32_t LoaderBinding = NoSourceIndex;
  std::string TableKind;
  bool TableBindingWritten = false;
  bool LoaderBindingWritten = false;
};

struct SourceBundleModule {
  std::string ID;
  uint32_t Bundle = NoSourceIndex;
  uint32_t Node = NoSourceIndex;
  uint32_t Body = NoSourceIndex;
  uint32_t Key = NoSourceIndex;
  uint32_t RequireBinding = NoSourceIndex;
  // factory or removed_slot. These are original source partitions, not
  // reconstructed files or independently executable ECMAScript modules.
  std::string Kind;
  std::string WrapperHash;
  std::string BodyHash;
  bool RequireBindingWritten = false;
};

struct SourceBundleDependency {
  std::string ID;
  uint32_t Bundle = NoSourceIndex;
  uint32_t Node = NoSourceIndex;
  uint32_t CallerModule = NoSourceIndex;
  uint32_t CalleeBinding = NoSourceIndex;
  uint32_t TargetModule = NoSourceIndex;
  std::string Status;
};

struct SourceBundleRegion {
  std::string ID;
  uint32_t Bundle = NoSourceIndex;
  uint32_t Node = NoSourceIndex;
  // Bootstrap statements after the fixed loader prefix. Never promoted to
  // an original entry module merely because webpack often inlines entries.
  std::string Kind = "startup_or_runtime";
};

struct SourceBundleAnalysis {
  std::string ID;
  std::string SourceID;
  std::string BindingID;
  std::string BindingStatus;
  std::string Status;
  std::vector<SourceBundle> Bundles;
  std::vector<SourceBundleModule> Modules;
  std::vector<SourceBundleDependency> Dependencies;
  std::vector<SourceBundleRegion> Regions;
  std::vector<std::u16string> Keys; // private, never paths or output metadata
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t DiagnosticCount = 0;
  uint64_t Steps = 0;
};

/// Recover only a qualified structural loader/table profile from immutable
/// source. Recognized layout never authenticates the producer or runtime
/// values. No target execution, rewriting, unpacking eval strings or I/O.
SourceBundleAnalysis analyzeSourceBundles(const SourceAnalysis &Source,
                                          const SourceBindingAnalysis &Bindings,
                                          std::string_view SourceBytes);

} // namespace neverd::web

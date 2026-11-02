//===- BunSemanticIndex.h - Evidence-backed source navigation ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded source navigation for admitted Bun extractions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PRIVATE_BUNSEMANTICINDEX_H
#define NEVERD_WEB_PRIVATE_BUNSEMANTICINDEX_H

#include "neverd/web/Bun.h"

#include "llvm/Support/JSON.h"

#include <span>

namespace neverd::web {
struct BunSemanticLimits {
  static constexpr uint64_t HardRecords = 512ULL * 1024;
  static constexpr uint64_t HardUnits = 16ULL * 1024 * 1024;
  static constexpr uint64_t HardOutputBytes = 1024ULL * 1024 * 1024;
  uint64_t Records = HardRecords, Units = HardUnits;
  uint64_t OutputBytes = HardOutputBytes;
};

struct BunSemanticModuleName {
  uint32_t Index = 0;
  std::u16string Name;
};

struct BunSemanticSource {
  std::string Status = "not_run", ParseStatus = "not_run";
  std::string BindingStatus = "not_run", ModuleStatus = "not_run";
  std::string NavigationStatus = "not_run", FeatureStatus = "not_run";
  llvm::json::Array Records, Diagnostics;
  uint64_t RecordsUsed = 0, UnitsUsed = 0, ReservedBytes = 0;
};

// Explicit private disclosure: exact names and bounded source excerpts are
// emitted only into the local index. Semantic aliases never alter bindings.
// This uses the Large semantic profile, never a Recovery model.
BunSemanticSource
inspectBunSemanticSource(std::string_view ArtifactID, std::string_view Text,
                         std::string_view SourceType,
                         std::span<const BunSemanticModuleName> Names,
                         BunSemanticLimits Limits = {});

// Authority is an admitted extraction, not a deserialized project manifest.
// Writes exact original sources and selected same-tree readable views, a JSON
// evidence inventory, per-module navigation pages and a domain index. No code
// from the input is executed and no native/cache payload enters this index.
std::string exportBunSemanticIndex(const BunExtraction &Extraction,
                                   std::string_view OutputDirectory,
                                   BunSemanticLimits Limits = {});
} // namespace neverd::web

#endif // NEVERD_WEB_PRIVATE_BUNSEMANTICINDEX_H

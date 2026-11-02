//===- LineageJSON.h - Bounded version graph projections --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Revision-bound pages over retained comparison evidence. Query projections
/// share one optional trace cache without changing the source collection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_LINEAGEJSON_H
#define NEVERD_DIFF_LINEAGEJSON_H

#include "neverd/diff/Lineage.h"

#include "llvm/Support/JSON.h"

namespace neverd::diff {

struct LineageQuery {
  std::string Kind, Revision, Version, Report, Release, Search, Focus;
  uint64_t Address = 0, Offset = 0;
  uint32_t Index = 0, Limit = limits::diff::kDefaultPageSize;
  uint64_t MaxWork = limits::diff::kDefaultLineageWork, TimeoutMilliseconds = 0;
  bool Latest = true;
};

/// Throws invalid_argument for unknown fields, kinds, types or ranges.
LineageQuery readLineageQuery(const llvm::json::Object &Value);
LineageSettings readLineageSettings(const llvm::json::Object &Value);
llvm::json::Object lineageSummary(const Lineage &Value,
                                  const ComparisonControl &Control = {});

struct LineagePage {
  llvm::json::Object JSON;
  /// Set only for a trace query; callers publish this cache with its page.
  std::shared_ptr<const LineageTrace> Trace;
};

/// Includes complete original edge evidence and grouped conflicts; no implied
/// chronology, transitive patch verdict or newly authenticated native facts.
/// MaxWork counts explicit records/bytes, not ordered-index comparisons.
LineagePage lineagePage(const Lineage &Value, const LineageQuery &Query,
                        const ComparisonControl &Control = {},
                        std::shared_ptr<const LineageTrace> Cache = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_LINEAGEJSON_H

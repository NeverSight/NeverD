//===- ComparisonSource.h - Snapshot-bound source pages -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapts the ordinary source emitter to an exclusively owned comparison input.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_COMPARISONSOURCE_H
#define NEVERD_SDK_CAPI_COMPARISONSOURCE_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"
#include "neverd/diff/SourceAlignment.h"

#include "llvm/Support/JSON.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd::sdk {
struct Session;

struct ComparisonSourceOptions {
  std::string Representation = "c";
  uint64_t Offset = 0, Limit = limits::diff::kDefaultPageSize;
  /// Zero removes the additional native-instruction admission allowance.
  uint64_t MaxNativeInstructions =
      limits::diff::kDefaultSourceNativeInstructions;
  bool Refresh = false;
};

struct ComparisonSourcePage {
  diff::ComparisonState State = diff::ComparisonState::Complete;
  std::string Availability = "unavailable", Reason;
  llvm::json::Object Document;
};

ComparisonSourcePage
readComparisonSource(Session &Session, const diff::Program &Program,
                     uint32_t Index, const ComparisonSourceOptions &Options,
                     const diff::ComparisonControl &Control);

struct ComparisonSourceLine {
  /// Exact emitted spelling, including any line terminator.
  std::string Text;
  /// Unmodified emitter evidence plus snapshot-checked comparison_anchors.
  llvm::json::Object Evidence;
};

struct ComparisonSourceDocument {
  std::string Dialect;
  uint64_t FirstLine = 0, TotalLines = 0;
  std::vector<ComparisonSourceLine> Lines;
};

struct ComparisonSourceDifferenceOptions {
  ComparisonSourceOptions Source;
  diff::SourceAlignmentOptions Alignment;
  uint64_t MaxSourceBytes = limits::diff::kDefaultSourceAlignmentBytes;
  bool IncludePrelude = false;
};

struct ComparisonSourceDifference {
  diff::SourceAlignment Alignment;
  std::array<ComparisonSourceDocument, 2> Documents;
  std::string Availability = "unavailable", Reason;
  uint64_t SourceBytes = 0;
};

/// Collects complete selected-function renderings through ordinary pages.
/// The byte allowance covers both inputs' text and serialized row evidence.
/// Stop/limit/unavailable outcomes retain no source text or classified rows.
ComparisonSourceDifference
compareSources(Session &PrimarySession, const diff::Program &Primary,
               uint32_t PrimaryIndex, Session &SecondarySession,
               const diff::Program &Secondary, uint32_t SecondaryIndex,
               const ComparisonSourceDifferenceOptions &Options,
               const diff::ComparisonControl &Control);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_COMPARISONSOURCE_H

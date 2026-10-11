//===- SourceView.h - Reviewed source display projections --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reviewed source display projections.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceLocation.h"

#include <map>

namespace neverd::web {
inline constexpr std::string_view SourceViewProfile =
    "javascript-source-view-v1";
inline constexpr uint64_t MaxSourceViewBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxSourceViewSegments = 2 * MaxJavaScriptLexemes + 1;
inline constexpr uint64_t MaxSourceViewChunk = 65536;
inline constexpr uint64_t MaxPublishedSourceViews = 2;

struct SourceReviewRange {
  uint64_t Start = 0, End = 0;
};
struct SourceViewPolicy {
  std::string ID;
  std::vector<SourceReviewRange> Reviewed;
};
struct SourceViewSegment {
  uint64_t SourceStart = 0, SourceEnd = 0, Start = 0, End = 0;
  uint32_t Binding = NoSourceIndex;
  std::string Kind;
  bool Identity = false, Reviewed = false;
};
struct SourceView {
  std::string ID, SourceID, ArtifactID, SourceHash, PolicyID;
  std::string BindingAnalysisID, BindingStatus;
  std::string Text;
  std::vector<SourceViewSegment> Segments;
  std::vector<SourceReviewRange> Reviewed;
  uint64_t SourceSize = 0, ReviewedBytes = 0, HiddenRegions = 0;
  std::map<std::string, uint64_t> RegionCounts;
};

/// Ranges are an explicit local caller assertion, not an inferred permission.
/// Default output contains fixed syntax, layout, lexical aliases and labels.
SourceViewPolicy sourceViewPolicy(std::string_view Options);
/// A display projection only: never executable output or a semantic rewrite.
SourceView makeSourceView(const SourceAnalysis &Source,
                          const SourceBindingAnalysis &Bindings,
                          std::string_view Bytes,
                          const SourceViewPolicy &Policy);
struct SourceViewRange {
  uint64_t Start = 0, End = 0, FirstSegment = 0, LastSegment = 0;
  uint64_t SourceStart = 0, SourceEnd = 0;
  // byte_identity, exact_boundary or region_cover. A hidden interior cannot
  // be interpolated; a zero-width source point may select a whole label.
  std::string Mapping;
};
/// The caller validates requested boundaries against the original UTF-8 text.
SourceViewRange locateSourceView(const SourceView &View, uint64_t Start,
                                 uint64_t End);
} // namespace neverd::web

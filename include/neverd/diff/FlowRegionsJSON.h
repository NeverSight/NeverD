//===- FlowRegionsJSON.h - SSA region records and previews ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict bounded persistence and display records. Saved mappings require
/// recomputation from bound dependency observations before publication.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWREGIONSJSON_H
#define NEVERD_DIFF_FLOWREGIONSJSON_H

#include "neverd/diff/Control.h"
#include "neverd/diff/FlowRegions.h"

#include "llvm/Support/JSON.h"

namespace neverd::diff {
llvm::json::Object flowRegionSettingsJSON(const FlowRegionSettings &Settings);
FlowRegionSettings readFlowRegionSettings(const llvm::json::Value &Value);
llvm::json::Object flowRegionSummaryJSON(const FlowRegionAnalysis &Regions);
llvm::json::Object flowRegionAnchorJSON(const FlowRegionAnchor &Anchor,
                                        bool IncludeCoordinates = false);
llvm::json::Object
flowRegionHypothesisJSON(const FlowRegionHypothesis &Hypothesis,
                         bool Full = false);
void writeFlowRegionAnalysisFields(llvm::json::OStream &Out,
                                   const FlowRegionAnalysis &Regions,
                                   bool SummaryOnly);
FlowRegionAnalysis
readFlowRegionAnalysis(const llvm::json::Value &Value,
                       const FlowRegionSettings &Settings,
                       const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWREGIONSJSON_H

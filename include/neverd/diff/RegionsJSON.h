//===- RegionsJSON.h - Region evidence serialization ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared region settings, persistence and bounded display records. Parsing
/// refuses malformed data; binding additionally validates snapshot evidence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_REGIONSJSON_H
#define NEVERD_DIFF_REGIONSJSON_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Regions.h"

#include "llvm/Support/JSON.h"

namespace neverd::diff {
llvm::json::Object regionSettingsJSON(const RegionSettings &Settings);
/// Accepts a boolean or settings object. Throws invalid_argument on bad input.
RegionSettings readRegionSettings(const llvm::json::Value &Value);
/// Retain the profile and conditional call provenance on each evidence row.
void addRegionEvidenceFields(llvm::json::Object &Object, bool Calls);
llvm::json::Object regionAnalysisJSON(const RegionAnalysis &Regions,
                                      bool SummaryOnly = false);
/// Stream into an already open object without constructing the complete JSON
/// inventory in memory. Display pages and saved reports share record writers.
void writeRegionAnalysisFields(llvm::json::OStream &Out,
                               const RegionAnalysis &Regions, bool SummaryOnly);
RegionAnalysis readRegionAnalysis(const llvm::json::Value &Value,
                                  const RegionSettings &Settings,
                                  const ComparisonControl &Control = {});
/// Bounded hypothesis preview; anchor coordinates have their own page query.
llvm::json::Object regionHypothesisJSON(const RegionHypothesis &Hypothesis,
                                        const Program &Primary,
                                        const Program &Secondary);
llvm::json::Object regionAnchorJSON(const RegionAnchor &Anchor,
                                    const Program &Primary,
                                    const Program &Secondary);
} // namespace neverd::diff

#endif // NEVERD_DIFF_REGIONSJSON_H

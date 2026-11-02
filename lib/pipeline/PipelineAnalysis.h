//===- PipelineAnalysis.h - Display-only recovery policy -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private admission of decoded analysis bodies without native ABI claims.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINEANALYSIS_H
#define NEVERD_LIB_PIPELINE_PIPELINEANALYSIS_H

#include "neverd/Common.h"

#include <set>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct PipelineOptions;
} // namespace neverd

namespace neverd::pipeline_detail {

/// Checked out-of-line PE32 C++ callbacks needed by this analysis selection.
/// These entries belong to the run, never the image's ordinary ABI inventory.
std::set<va_t> analysisCallbackEntries(const BinaryImage &Image,
                                       const PipelineOptions &Options);

/// Every instruction has operations, but native state proof may be incomplete.
/// Decode/lift failures and ambiguous local returns still disqualify the body.
bool hasAnalysisInstructionBody(const LowFunc &Function);

/// Mark tentative bodies before any module evidence can be published.
void prepareAnalysisBody(const BinaryImage &Image,
                         const PipelineOptions &Options,
                         const std::set<va_t> &Callbacks, LowFunc &Function);

} // namespace neverd::pipeline_detail

#endif // NEVERD_LIB_PIPELINE_PIPELINEANALYSIS_H

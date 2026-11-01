//===- PipelineSourceInlineHigh.h - Source body helpers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// HighIR projection of call bodies admitted by the shared source proof.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_SOURCEINLINEHIGH_H
#define NEVERD_PIPELINE_SOURCEINLINEHIGH_H

#include "PipelineSourceInline.h"

#include <map>
#include <set>

namespace neverd {
struct SourceInlineBody {
  unsigned Parameters = 0;
  std::set<unsigned> Unused;
};

struct SourceInlineHighPreparation {
  std::shared_ptr<PipelineResult> Recovered;
  std::map<va_t, SourceInlineBody> Bodies;
};

using SourceInlineRequests = std::map<va_t, std::set<unsigned>>;
std::set<va_t> sourceInlineSupports(const PipelineResult &Original,
                                    const SourceInlineRequests &Requests,
                                    const std::set<va_t> &Roots);

std::shared_ptr<SourceInlineHighPreparation>
prepareSpecializedHighSourceCalls(PipelineResult &Recovered,
                                  std::map<va_t, SourceInlineBody> Bodies);

std::optional<SourceInlineHighView>
specializeHighSourceBodies(const BinaryImage &Image, PipelineResult &Recovered,
                           const PipelineOptions &Options, DebugContext *Debug,
                           const std::map<va_t, SourceInlineBody> &Bodies,
                           const SourceInlineHighView &Original);
} // namespace neverd

#endif // NEVERD_PIPELINE_SOURCEINLINEHIGH_H

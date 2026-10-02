#ifndef NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H
#define NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {
bool hasNativeScalarIntrinsicEvidence(const MedOp &Operation,
                                      Arch Architecture);
} // namespace neverd
#endif

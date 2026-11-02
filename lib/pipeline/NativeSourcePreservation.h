#ifndef NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H
#define NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {
bool hasNativeScalarIntrinsicEvidence(const MedOp &Operation,
                                      Arch Architecture);
/// A validated original load writes only its scalar result on normal
/// continuation. This grants no address, no-fault or frame-byte identity.
bool hasNativeReadOnlyMemoryIntrinsicEvidence(const MedOp &Operation,
                                              Arch Architecture);
} // namespace neverd
#endif

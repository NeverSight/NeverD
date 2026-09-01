//===- LowIRConcolicDetail.h - Concolic private test seams ------*- C++ -*-===//

#ifndef NEVERD_LIB_CONCOLIC_LOWIRCONCOLICDETAIL_H
#define NEVERD_LIB_CONCOLIC_LOWIRCONCOLICDETAIL_H

#include "neverd/concolic/LowIRConcolic.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::concolic::detail {

struct ModelProjection {
  ConcolicProjectionReason Reason = ConcolicProjectionReason::None;
  std::vector<symbolic::SymConcreteRegister> Seed;
};

/// Private seam: production uses this only after SAT. Tests can manufacture
/// malformed provenance/model pairs that a well-formed LowIR trace cannot.
ModelProjection
projectRegisterModel(const symbolic::SymContext &Ctx, symbolic::SymRef Query,
                     const solver::BitVectorModel &Model,
                     llvm::ArrayRef<symbolic::SymConcreteRegister> Baseline,
                     llvm::endianness Order);

/// Context-free replay comparison, separated so every rejection stays typed.
ConcolicReplayReason
compareReplayDecisions(llvm::ArrayRef<LowIRConcolicDecision> Replay,
                       llvm::ArrayRef<LowIRConcolicDecision> Original,
                       size_t TargetIndex);

struct CandidatePublication {
  ConcolicFlipStatus Status = ConcolicFlipStatus::CandidateBudgetExceeded;
  std::optional<size_t> CandidateIndex;
};

/// Publish a seed only after its caller has replay-verified it. Deduplication
/// precedes the capacity check so an existing witness consumes no new slot.
CandidatePublication
publishReplayVerifiedSeed(std::vector<LowIRConcolicCandidate> &Published,
                          std::vector<symbolic::SymConcreteRegister> Seed,
                          unsigned MaxCandidates);

ConcolicFlipStatus classifyPrefixEncodingFailure(solver::BlastError Error);

} // namespace neverd::concolic::detail

#endif // NEVERD_LIB_CONCOLIC_LOWIRCONCOLICDETAIL_H

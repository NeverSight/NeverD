//===- ReturningCallState.h - Bounded conditional call effect -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The machine-state portion of the production recovery call transition.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_CORE_RETURNINGCALLSTATE_H
#define NEVERD_ANALYSIS_CORE_RETURNINGCALLSTATE_H

#include "RecoveryArchitecture.h"

#include "neverd/symbolic/SymState.h"

#include <cstdint>

namespace neverd::analysis {

struct ReturningCallStateEffect {
  uint64_t MaxWork = 0;
  uint64_t MaxNodes = 0;
  uint64_t WorkUsed = 0;
  SpecializationStatus Status = SpecializationStatus::InvalidInput;
  const char *Diagnostic = "returning-call state effect was not applied";
};

/// Apply the already-admitted architecture policy to this evaluator's state.
/// The caller has validated frame containment and staged provenance/sidecars.
/// Work and node charging is shared with the actual specialization consumer.
/// Register/memory primitives preflight individually; failure of the composed
/// effect requires discarding this evaluator, not reusing a partial state.
/// Runtime-control/priority locals are outside this function; segment roots
/// inside State remain untouched. This is not a separate relation admission.
void applyReturningCallState(symbolic::SymState &State,
                             symbolic::SymRef FrameRoot,
                             symbolic::SymRef CallerStack,
                             int64_t PreservedBegin, int64_t FrameEnd,
                             va_t GuestContinuation,
                             const RecoveryCallTransition &Policy,
                             ReturningCallStateEffect &Effect);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_CORE_RETURNINGCALLSTATE_H

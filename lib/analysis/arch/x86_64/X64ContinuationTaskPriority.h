//===- X64ContinuationTaskPriority.h - CR8 instruction effects -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind scalar CR8 transfers to the continuation's privilege and fault domain.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONTASKPRIORITY_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONTASKPRIORITY_H

#include "X64TaskPriority.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include <cstddef>

namespace neverd::analysis::detail {

template <class Writer>
void emitX64ContinuationTaskPriority(Writer &W, const LowOp &Op) {
  const NdVar Priority = W.load(offsetof(X64ContinuationStateV2, CR8));
  const auto Transition = lowerX64TaskPriority(
      Op, Priority, [&](uint16_t Bytes) { return W.temporary(Bytes); });
  W.append(Transition->Ops);
  if (W.Options.PagedMemory) {
    // The checked body runs after fetch and stages all outputs. CS.RPL owns
    // CPL even for conforming code. Fault before publishing a GPR, CR8 or
    // any of the six arbitrary flags.
    const auto Selector = W.load(offsetof(X64ContinuationStateV5, CS) +
                                     offsetof(X64ContinuationSegment, Selector),
                                 2);
    const auto CPL = W.temporary(2), Unprivileged = W.temporary(1);
    W.emit(NdOp::INT_AND, CPL, {Selector, NdVar::scalar(3, 2)});
    W.emit(NdOp::INT_NOTEQUAL, Unprivileged, {CPL, NdVar::scalar(0, 2)});
    NdVar Bad = Unprivileged;
    if (Transition->Rejected) {
      Bad = W.temporary(1);
      W.emit(NdOp::BOOL_OR, Bad, {Unprivileged, *Transition->Rejected});
    }
    W.exitIf(Bad, [&] { W.fault(13); });
  } else if (Transition->Rejected) {
    W.rejectIf(*Transition->Rejected);
  }
  if (Transition->Rejected) {
    W.store(offsetof(X64ContinuationStateV2, CR8), Priority);
    W.WritesCr8 = true;
  }
}

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONTASKPRIORITY_H

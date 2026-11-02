//===- X64ContinuationSchedule.h - Bounded IRQ/NMI scheduling -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Instruction-boundary events and persistent interrupt inhibition.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64CONTINUATIONSCHEDULE_H
#define NEVERD_LIB_PIPELINE_X86_X64CONTINUATIONSCHEDULE_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class raw_ostream;
}
namespace neverd {
namespace analysis {
struct X64ContinuationInstructionSource;
}

void emitX64ContinuationSchedule(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64CONTINUATIONSCHEDULE_H

//===- X64ContinuationEnvironment.h - State boundary handoff -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit bounded environment resumption without inferring a native call ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X64CONTINUATIONENVIRONMENT_H
#define NEVERD_LIB_PIPELINE_X64CONTINUATIONENVIRONMENT_H

namespace llvm {
class raw_ostream;
}

namespace neverd {

/// The optional runtime provider receives the complete state at a stopping
/// boundary. A separate caller-owned budget bounds provider invocations,
/// including providers that leave PC unchanged. Invalid states and exhausted
/// translated-instruction budgets never invoke the provider.
void emitX64ContinuationEnvironment(llvm::raw_ostream &OS, unsigned Version);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X64CONTINUATIONENVIRONMENT_H

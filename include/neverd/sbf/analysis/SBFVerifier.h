//===- SBFVerifier.h - Layered official SBF verification -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFVERIFIER_H
#define NEVERD_SBF_ANALYSIS_SBFVERIFIER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/Support/Error.h"

namespace neverd::sbf {

/// Run the latest sbpf LocalVerifier policy after requisite verification.
/// Invalid LowIR is rejected as API misuse so callers cannot accidentally run
/// the permissive local scan before the requisite verifier.
llvm::Expected<VerificationReport>
verifyLocalPreflight(const LowIR &IR,
                     llvm::ArrayRef<uint32_t> RegisteredSyscallHashes);

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFVERIFIER_H

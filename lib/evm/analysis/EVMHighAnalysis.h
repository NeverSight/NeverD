//===- EVMHighAnalysis.h - Trusted HighIR recovery ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the private HighIR recovery boundary for canonical LowIR/MedIR
/// pairs produced inside the staged analyzer.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSIS_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSIS_H

#include "neverd/evm/analysis/EVMAnalyzer.h"

namespace neverd::evm::detail {

/// Recovers HighIR from an internally canonical pair. This skips only the
/// external-IR replay boundary; HighIR option validation and every HighIR
/// resource charge remain mandatory.
[[nodiscard]] llvm::Expected<EVMHighIR>
recoverCanonicalHighIR(const EVMLowIR &Low, const EVMMedIR &Med,
                       AnalyzeOptions Options);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSIS_H

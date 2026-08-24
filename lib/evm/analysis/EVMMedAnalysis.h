//===- EVMMedAnalysis.h - Trusted LowIR-to-MedIR lowering ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the private lowering boundary for LowIR that was produced by the
/// canonical decoder or already replay-validated by a public API boundary.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMMEDANALYSIS_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMMEDANALYSIS_H

#include "neverd/evm/analysis/EVMAnalyzer.h"

namespace neverd::evm::detail {

/// Validates the MedIR-stage options independently of any LowIR input.
[[nodiscard]] llvm::Error
validateMedAnalysisOptions(const AnalyzeOptions &Options);

/// Lowers canonical LowIR without replaying it. Callers must either own the
/// immediately preceding decodeLowIR() result or have passed the public LowIR
/// through verifyCanonicalLowIRForMedLowering().
[[nodiscard]] llvm::Expected<EVMMedIR>
lowerCanonicalLowToMedIR(const EVMLowIR &Low, AnalyzeOptions Options);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMMEDANALYSIS_H

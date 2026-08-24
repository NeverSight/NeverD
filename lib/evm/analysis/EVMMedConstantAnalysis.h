//===- EVMMedConstantAnalysis.h - Canonical MedIR SCCP --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the read-only constant evaluator shared by MedIR construction and
/// the public HighIR boundary verifier.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMMEDCONSTANTANALYSIS_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMMEDCONSTANTANALYSIS_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace neverd::evm::detail {

/// Recomputes the complete MedIR constant table without consulting derived
/// Instruction or Phi constants already attached to Med. Constant-kind
/// values are the verified PUSH seeds; every other result comes from SCCP.
llvm::Expected<std::vector<std::optional<llvm::APInt>>>
computeCanonicalMedIRConstants(const EVMMedIR &Med, size_t CodeSize,
                               size_t MaxWorklistUpdates);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMMEDCONSTANTANALYSIS_H

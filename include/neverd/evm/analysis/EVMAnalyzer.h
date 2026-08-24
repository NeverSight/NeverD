//===- EVMAnalyzer.h - Staged EVM bytecode analysis API -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the EVM bytecode decoder and the LowIR-to-MedIR-to-HighIR
/// analysis pipeline.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_ANALYSIS_EVMANALYZER_H
#define NEVERD_EVM_ANALYSIS_EVMANALYZER_H

#include "neverd/evm/EVMConstants.h"
#include "neverd/evm/bytecode/EVMDecoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::evm {

/// Extends linear-decoder options with higher-level recovery controls and
/// deterministic hostile-input bounds for whole-program abstract analysis.
struct AnalyzeOptions : DecodeOptions {
  bool RecoverHighLevel = true;
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)                   \
  size_t NAME = kDefault##NAME;
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)                      \
  size_t NAME = kDefault##NAME;
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)                        \
  size_t NAME = kDefault##NAME;
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
};

/// Decodes bytecode into lossless instructions, blocks, edges, and stack
/// heights. Strict mode rejects unknown and inactive bytes only when a proven
/// path reaches them, plus invalid stack flow; relaxed mode preserves those
/// paths as explicit fault nodes. A malformed conditional immediate is always
/// preserved so later boundaries remain exact.
llvm::Expected<EVMLowIR> decodeLowIR(llvm::ArrayRef<uint8_t> Code,
                                     AnalyzeOptions Options = {});

/// Lowers decoded stack operations to the EVM's 256-bit stack SSA form.
llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low,
                                      AnalyzeOptions Options = {});
/// Recovers selectors and best-effort ABI, storage, event, and error facts.
/// Deterministic HighIR resource exhaustion is returned as an error rather
/// than silently dropping facts.
llvm::Expected<EVMHighIR> recoverHighIR(const EVMLowIR &Low,
                                        const EVMMedIR &Med,
                                        AnalyzeOptions Options = {});

/// Runs all enabled EVM analysis stages.
llvm::Expected<EVMProgram> analyze(llvm::ArrayRef<uint8_t> Code,
                                   AnalyzeOptions Options = {});

/// Returns deterministic textual forms intended for diagnostics and tooling.
std::string dumpLowIR(const EVMLowIR &Low);
std::string dumpMedIR(const EVMMedIR &Med);
std::string dumpHighIR(const EVMHighIR &High);

} // namespace neverd::evm

#endif // NEVERD_EVM_ANALYSIS_EVMANALYZER_H

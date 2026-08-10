//===- Analyzer.h - Staged EVM bytecode analysis API ----------*- C++ -*-===//
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

#ifndef NEVERD_EVM_ANALYZER_H
#define NEVERD_EVM_ANALYZER_H

#include "neverd/evm/Decoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::evm {

/// Extends linear-decoder options with higher-level recovery controls.
struct AnalyzeOptions : DecodeOptions {
  bool RecoverHighLevel = true;
};

/// Decodes bytecode into lossless instructions, blocks, edges, and stack
/// heights. Strict mode rejects unknown and inactive bytes plus invalid stack
/// flow; relaxed mode preserves them as explicit fault nodes. A malformed
/// conditional immediate is always preserved so later boundaries remain exact.
llvm::Expected<EVMLowIR> decodeLowIR(llvm::ArrayRef<uint8_t> Code,
                                     AnalyzeOptions Options = {});

/// Lowers decoded stack operations to the EVM's 256-bit stack SSA form.
llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low);
/// Recovers selectors and best-effort ABI, storage, event, and error facts.
EVMHighIR recoverHighIR(const EVMLowIR &Low, const EVMMedIR &Med);

/// Runs all enabled EVM analysis stages.
llvm::Expected<EVMProgram> analyze(llvm::ArrayRef<uint8_t> Code,
                                   AnalyzeOptions Options = {});

/// Returns deterministic textual forms intended for diagnostics and tooling.
std::string dumpLowIR(const EVMLowIR &Low);
std::string dumpMedIR(const EVMMedIR &Med);
std::string dumpHighIR(const EVMHighIR &High);

} // namespace neverd::evm

#endif // NEVERD_EVM_ANALYZER_H

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

#include "neverd/evm/EVMIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::evm {

/// Controls hardfork activation, recovery strictness, and resource limits.
struct AnalyzeOptions {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  bool RecoverHighLevel = true;
  size_t MaxCodeSize = kMaxCodeSize;
};

/// Decodes bytecode into lossless instructions, blocks, edges, and stack
/// heights. Strict mode rejects unknown, inactive, or structurally invalid
/// input; relaxed mode preserves those bytes as explicit fault nodes.
llvm::Expected<EVMLowIR> decodeLowIR(llvm::ArrayRef<uint8_t> Code,
                                     AnalyzeOptions Options = {});

/// Lowers decoded stack operations to the EVM's 256-bit stack SSA form.
llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low);
/// Recovers selectors and best-effort ABI, storage, event, and error facts.
EVMHighIR recoverHighIR(const EVMLowIR &Low, const EVMMedIR &Med);

/// Runs all enabled EVM analysis stages.
llvm::Expected<EVMProgram> analyze(llvm::ArrayRef<uint8_t> Code,
                                   AnalyzeOptions Options = {});

/// Formats an instruction immediate at its encoded width, including leading
/// zeroes. Returns an empty string when the instruction has no immediate.
std::string formatImmediate(const LowInstruction &Instruction);
/// Returns deterministic textual forms intended for diagnostics and tooling.
std::string dumpLowIR(const EVMLowIR &Low);
std::string dumpMedIR(const EVMMedIR &Med);
std::string dumpHighIR(const EVMHighIR &High);

} // namespace neverd::evm

#endif // NEVERD_EVM_ANALYZER_H

//===- Analyzer.h - Staged EVM bytecode analysis API ----------*- C++ -*-===//

#ifndef NEVERD_EVM_ANALYZER_H
#define NEVERD_EVM_ANALYZER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace neverd::evm {

struct AnalyzeOptions {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  bool RecoverHighLevel = true;
  size_t MaxCodeSize = kMaxCodeSize;
};

llvm::Expected<EVMLowIR> decodeLowIR(std::span<const uint8_t> Code,
                                     AnalyzeOptions Options = {});

llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low);
EVMHighIR recoverHighIR(const EVMLowIR &Low, const EVMMedIR &Med);

llvm::Expected<EVMProgram> analyze(std::span<const uint8_t> Code,
                                   AnalyzeOptions Options = {});

std::string dumpLowIR(const EVMLowIR &Low);
std::string dumpMedIR(const EVMMedIR &Med);
std::string dumpHighIR(const EVMHighIR &High);

} // namespace neverd::evm

#endif // NEVERD_EVM_ANALYZER_H

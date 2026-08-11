//===- Analyzer.h - Solana SBF staged analysis ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYZER_H
#define NEVERD_SBF_ANALYZER_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/SBFIR.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

namespace neverd::sbf {

struct AnalyzeOptions {
  Version VersionOverride = Version::Auto;
  bool Strict = true;
  bool RecoverHighIR = true;
  SBFVMConfig VMConfig;
  /// An Anchor IDL supplied by the operator. Its names take precedence over the
  /// built-in dictionary when recovering instruction dispatch.
  const AnchorIdl *Idl = nullptr;
};

llvm::Expected<SBFProgram> analyze(const BinaryImage &Image,
                                   const AnalyzeOptions &Options = {});

/// Find a recovered function by name or hexadecimal virtual address. An empty
/// identifier selects the program entry function.
const Function *findFunction(const SBFProgram &Program,
                             llvm::StringRef Identifier = {});

std::string formatInstruction(const LowInstruction &Instruction);
std::string dumpLowIR(const LowIR &IR);
std::string dumpMedIR(const MedIR &IR);
std::string dumpHighIR(const HighIR &IR);

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYZER_H

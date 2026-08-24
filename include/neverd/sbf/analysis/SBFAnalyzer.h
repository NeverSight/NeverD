//===- SBFAnalyzer.h - Solana SBF staged analysis ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFANALYZER_H
#define NEVERD_SBF_ANALYSIS_SBFANALYZER_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/SBFIR.h"
#include "neverd/sbf/analysis/SBFVerifier.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <optional>
#include <string>

namespace neverd::sbf {

struct AnalyzeOptions {
  Version VersionOverride = Version::Auto;
  bool Strict = true;
  bool RecoverHighIR = true;
  VerificationPolicy Verification = VerificationPolicy::Requisite;
  /// The runtime the recovered description is about. The ISA version comes
  /// from the file and this does not: which gates are on, which loader owns
  /// the program, and whether the question is about running it or deploying it
  /// are all facts about the chain, and the file cannot state any of them.
  RuntimeProfile Profile;
  /// A complete custom VM and version policy. When absent, the environment is
  /// resolved from Profile using Agave's current policy. Keeping this as one
  /// explicit expert-only value prevents individual VM knobs from silently
  /// diverging from the named chain profile.
  std::optional<ExpertRuntimeEnvironmentOverride> ExpertEnvironment;
  /// Selects the authority for the accepted ISA-version range. When absent,
  /// auto-detection keeps the exact chain profile, while an explicit
  /// VersionOverride opts into every version supported by the pinned upstream
  /// sbpf toolchain. Set this value to force either interpretation.
  std::optional<RuntimeVersionPolicy> RuntimeVersions;
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

#endif // NEVERD_SBF_ANALYSIS_SBFANALYZER_H

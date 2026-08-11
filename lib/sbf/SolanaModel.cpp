//===- SolanaModel.cpp - Recovered Solana program facts -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/SolanaModel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"

#include <array>

namespace neverd::sbf {

llvm::StringRef recoveryEvidenceName(RecoveryEvidence Evidence) {
  switch (Evidence) {
  case RecoveryEvidence::ConstantDataflow:
    return "constant-dataflow";
  case RecoveryEvidence::KnownAddressTable:
    return "known-address-table";
  case RecoveryEvidence::AnchorDictionary:
    return "anchor-dictionary";
  case RecoveryEvidence::SuppliedIdl:
    return "supplied-idl";
  }
  return "unknown";
}

llvm::ArrayRef<LintInfo> lintInfos() {
  static const std::array Table = {
#define SBF_LINT(ID, NAME, SEVERITY, CONFIDENCE, SUMMARY)                      \
  LintInfo{Lint::ID, NAME, LintSeverity::SEVERITY, LintConfidence::CONFIDENCE, \
           SUMMARY},
#include "neverd/sbf/SBFLints.def"
  };
  return Table;
}

const LintInfo &getLintInfo(Lint ID) {
  return lintInfos()[static_cast<size_t>(ID)];
}

llvm::StringRef lintSeverityName(LintSeverity Severity) {
  switch (Severity) {
  case LintSeverity::Note:
    return "note";
  case LintSeverity::Warning:
    return "warning";
  }
  return "unknown";
}

llvm::StringRef lintConfidenceName(LintConfidence Confidence) {
  switch (Confidence) {
  case LintConfidence::Advisory:
    return "advisory";
  case LintConfidence::Likely:
    return "likely";
  case LintConfidence::Certain:
    return "certain";
  }
  return "unknown";
}

bool RecoveredSeed::isText() const {
  return !Bytes.empty() &&
         llvm::all_of(Bytes, [](uint8_t Byte) { return llvm::isPrint(Byte); });
}

bool PDADerivation::complete() const {
  return DeclaredSeedCount && Seeds.size() == *DeclaredSeedCount;
}

bool SolanaModel::empty() const {
  return !ProgramId && !IsAnchor && Pubkeys.empty() && Handlers.empty() &&
         Discriminators.empty() && CPISites.empty() && Derivations.empty() &&
         Errors.empty() && AccountAccesses.empty() && Findings.empty();
}

} // namespace neverd::sbf

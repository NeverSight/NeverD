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
namespace {

llvm::Error layoutError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: account layout: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

/// Report the first place where \p Fields stop tiling their span. A field of
/// size zero ends the fixed part, so nothing may follow it.
llvm::Error checkTiling(llvm::StringRef Table,
                        llvm::ArrayRef<LayoutFieldInfo> Fields) {
  uint64_t Expected = 0;
  for (auto [Index, Field] : llvm::enumerate(Fields)) {
    if (Field.Offset != Expected)
      return layoutError(Table + " field '" + Field.Name + "' starts at " +
                         llvm::Twine(Field.Offset) +
                         " but the previous field "
                         "ends at " +
                         llvm::Twine(Expected));
    if (Field.Size == 0 && Index + 1 != Fields.size())
      return layoutError(Table + " field '" + Field.Name +
                         "' has a data-determined length but is not last");
    Expected = Field.Offset + Field.Size;
  }
  return llvm::Error::success();
}

/// Offset of the first field whose length the data determines, or the end of
/// the table when every field is fixed.
uint64_t fixedSpan(llvm::ArrayRef<LayoutFieldInfo> Fields) {
  uint64_t End = 0;
  for (const LayoutFieldInfo &Field : Fields) {
    if (Field.Size == 0)
      return Field.Offset;
    End = Field.Offset + Field.Size;
  }
  return End;
}

} // namespace

llvm::ArrayRef<LayoutFieldInfo> inputFieldInfos() {
  static const std::array Table = {
#define SBF_INPUT_FIELD(ID, NAME, OFFSET, SIZE)                                \
  LayoutFieldInfo{NAME, OFFSET, SIZE},
#include "neverd/sbf/SBFAccountLayout.def"
  };
  return Table;
}

llvm::ArrayRef<LayoutFieldInfo> accountFieldInfos() {
  static const std::array Table = {
#define SBF_ACCOUNT_FIELD(ID, NAME, OFFSET, SIZE)                              \
  LayoutFieldInfo{NAME, OFFSET, SIZE},
#include "neverd/sbf/SBFAccountLayout.def"
  };
  return Table;
}

const LayoutFieldInfo &getInputFieldInfo(InputField Field) {
  return inputFieldInfos()[static_cast<size_t>(Field)];
}

const LayoutFieldInfo &getAccountFieldInfo(AccountField Field) {
  return accountFieldInfos()[static_cast<size_t>(Field)];
}

uint64_t firstAccountOffset() { return fixedSpan(inputFieldInfos()); }

uint64_t accountFixedSize() { return fixedSpan(accountFieldInfos()); }

llvm::Error validateAccountLayout() {
  if (llvm::Error E = checkTiling("input", inputFieldInfos()))
    return E;
  return checkTiling("account", accountFieldInfos());
}

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

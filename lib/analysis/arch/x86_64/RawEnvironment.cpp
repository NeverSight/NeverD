//===- RawEnvironment.cpp - x64 raw environment policy ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file x64 raw environment policy.

#include "../../core/RecoveryDigest.h"
#include "X64Recovery.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <utility>
#include <vector>

namespace neverd::analysis::x64 {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

std::string optionsDigest(const SpecializationOptions &O) {
  RecoveryDigest H("neverd-raw-environment-options-v2");
  H.number(O.ExplicitMachineState);
  H.number(O.NormalNonfaultingExecution);
  H.number(O.X64CetDisabled);
  if (O.UndefinedModel != SpecializationUndefinedModel::SelectedLowIR) {
    H.text("architectural-scalar-choices-v1");
    H.number(static_cast<unsigned>(O.UndefinedModel));
  }
  H.number(O.X64SegmentedReads);
  if (O.X64SegmentedWrites) {
    H.text("x64-segmented-writes-v1");
    H.number(O.X64SegmentedWrites);
  }
  H.number(O.X64CR8State);
  if (O.X64LegacyGSState) {
    H.text("x64-legacy-gs-state-v1");
    H.number(O.X64LegacyGSState);
  }
  if (O.X64MXCSRMask) {
    H.text("x64-mxcsr-mask-v1");
    H.number(*O.X64MXCSRMask);
  }
  H.number(O.X64MachineExits.size());
  for (const auto &E : O.X64MachineExits) {
    H.number(E.Version);
    H.number(E.Address);
    H.number(static_cast<unsigned>(E.Kind));
  }
  H.number(O.X64TerminalImports.size());
  for (const auto &B : O.X64TerminalImports) {
    H.number(B.Version);
    H.number(B.Slot);
    H.text(B.Module);
    H.text(B.Symbol);
  }
  H.number(O.X64CodeRegions.size());
  for (const auto &R : O.X64CodeRegions) {
    H.number(R.Version);
    H.number(R.Address);
    H.bytes(R.Bytes);
    H.text(R.Evidence);
  }
  H.number(O.X64ImportBindings.size());
  for (const auto &B : O.X64ImportBindings) {
    H.number(B.Version);
    H.number(B.Slot);
    H.text(B.Module);
    H.text(B.Symbol);
    H.number(B.Target);
    H.text(B.Evidence);
  }
  H.number(O.X64FlagsProfile.has_value());
  if (O.X64FlagsProfile)
    H.number(static_cast<unsigned>(*O.X64FlagsProfile));
  appendRawEnvironmentPolicy(H, O);
  return H.finish();
}

class X64MachineExitLookup final : public RecoveryMachineExitLookup {
  std::vector<SpecializationX64MachineExit> Declarations;

public:
  explicit X64MachineExitLookup(
      std::vector<SpecializationX64MachineExit> Entries)
      : Declarations(std::move(Entries)) {
    std::sort(
        Declarations.begin(), Declarations.end(),
        [](const auto &A, const auto &B) { return A.Address < B.Address; });
  }

  bool lookup(va_t Address, uint64_t &Work,
              std::optional<SpecializationExternalExit> &Exit) const override {
    Exit.reset();
    if (Declarations.empty())
      return true;
    // Bound the ordered search and final equality comparison before lookup.
    const uint64_t Cost = std::bit_width(Declarations.size()) + 1;
    if (Cost > Work)
      return false;
    Work -= Cost;
    const auto At =
        std::lower_bound(Declarations.begin(), Declarations.end(), Address,
                         [](const auto &Declaration, va_t Value) {
                           return Declaration.Address < Value;
                         });
    if (At != Declarations.end() && At->Address == Address) {
      Exit.emplace();
      Exit->MachineExit = *At;
    }
    return true;
  }
};

class X64RecoveryArchitecture final : public RecoveryArchitecture {
public:
  llvm::Expected<RawEnvironmentPreparation>
  validateRawDeclarations(const SpecializationOptions &Options) const override {
    return validateRawEnvironmentDeclarations(Options);
  }
  llvm::Expected<std::unique_ptr<RecoveryMachineExitLookup>>
  prepareMachineExitLookup(const SpecializationOptions &Options,
                           uint64_t &Work) const override {
    const uint64_t Count = Options.X64MachineExits.size();
    // Pay copying and ordered-index construction before allocation. The
    // original declaration order remains authoritative for domain receipts.
    const uint64_t Comparisons = 2 * std::bit_width(Count) + 2;
    if (Count > Work / Comparisons)
      return llvm::createStringError(std::errc::value_too_large,
                                     "machine-exit index budget exhausted");
    Work -= Count * Comparisons;
    return std::make_unique<X64MachineExitLookup>(Options.X64MachineExits);
  }
  std::string
  rawOptionsDigest(const SpecializationOptions &Options) const override {
    return optionsDigest(Options);
  }
  llvm::Error
  validateRawExtent(const SpecializationRawEnvironment &D) const override {
    for (const auto &R : D.Declaration.X64CodeRegions)
      if (R.Address < D.PrimaryImageEnd &&
          D.PrimaryImageBegin < R.Address + R.Bytes.size())
        return invalid("supplemental code overlaps the primary image extent");
    for (const auto &B : D.Declaration.X64ImportBindings)
      if (B.Slot < D.PrimaryImageBegin || B.Slot >= D.PrimaryImageEnd ||
          8 > D.PrimaryImageEnd - B.Slot)
        return invalid(
            "stable import slot is outside the primary image extent");
    for (const auto &E : D.Declaration.X64MachineExits)
      if (!(E.Address >= D.PrimaryImageBegin &&
            E.Address < D.PrimaryImageEnd) &&
          !llvm::any_of(D.Declaration.X64CodeRegions, [&](const auto &R) {
            return E.Address >= R.Address &&
                   E.Address - R.Address < R.Bytes.size();
          }))
        return invalid("machine exit is outside the declared code domain");
    return llvm::Error::success();
  }
  llvm::Error reserveExternalExitValidation(
      const SpecializationResult &R, const SpecializationOptions &Expected,
      RecoveryValidationBudget &Budget) const override {
    // Pay the native validator's ordered indexes and all lookup work before
    // it allocates, compares names or hashes native evidence. Generic graph
    // and payload walks have separate charges in the shared owner.
    const auto IndexWork = [&](uint64_t Count) {
      const uint64_t Comparisons = 2 * std::bit_width(Count) + 2;
      return Count <= Budget.Records / Comparisons &&
             Budget.records(Count * Comparisons, 8);
    };
    if (!IndexWork(R.Origins.size()) || !IndexWork(R.ExternalExits.size()))
      return RecoveryValidationBudget::exhausted();
    const uint64_t Exits = R.ExternalExits.size();
    const uint64_t OriginLookups =
        std::bit_width(uint64_t(R.Origins.size())) + 1;
    if (Exits > Budget.Records / OriginLookups ||
        !Budget.records(Exits * OriginLookups, 8))
      return RecoveryValidationBudget::exhausted();
    const uint64_t ExitLookups = std::bit_width(Exits) + 1;
    for (const auto &B : R.Residual.Blocks)
      if (B.InstructionBoundaries.size() > Budget.Records / ExitLookups ||
          !Budget.records(B.InstructionBoundaries.size() * ExitLookups, 8))
        return RecoveryValidationBudget::exhausted();
    const uint64_t Imports = Expected.X64TerminalImports.size();
    const uint64_t MachineExits = Expected.X64MachineExits.size();
    // Keep the v1 charge; check addition before multiplying by occurrences.
    if (MachineExits > UINT64_MAX - Imports)
      return RecoveryValidationBudget::exhausted();
    const uint64_t Terminals = Imports + MachineExits;
    if ((Exits && Terminals > Budget.Records / Exits) ||
        !Budget.records(Exits * Terminals, 32))
      return RecoveryValidationBudget::exhausted();
    if (Exits)
      for (const auto &Binding : Expected.X64TerminalImports)
        for (size_t Length : {Binding.Module.size(), Binding.Symbol.size()})
          if (Length > Budget.Bytes / Exits || !Budget.bytes(Length * Exits))
            return RecoveryValidationBudget::exhausted();
    for (const auto &Exit : R.ExternalExits)
      if (const auto &Import = Exit.Original.TerminalImport;
          Import && (!Budget.bytes(Import->Binding.Module.size()) ||
                     !Budget.bytes(Import->Binding.Symbol.size()) ||
                     !Budget.bytes(Import->FixedImageDigest.size())))
        return RecoveryValidationBudget::exhausted();
    return llvm::Error::success();
  }
  std::string
  externalExitDigest(const SpecializationExternalExit &Exit) const override {
    return x64::externalExitDigest(Exit);
  }
  llvm::Error
  validateExternalExits(const SpecializationResult &Result,
                        const SpecializationOptions &Options) const override {
    return x64::validateExternalExits(Result, Options);
  }
};
} // namespace

const RecoveryArchitecture &recoveryArchitecture() {
  static const X64RecoveryArchitecture Architecture;
  return Architecture;
}
} // namespace neverd::analysis::x64

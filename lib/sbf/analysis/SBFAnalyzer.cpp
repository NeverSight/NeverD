//===- SBFAnalyzer.cpp - Solana SBF staged analysis -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives the staged SBF analysis: build the runtime image, decode, resolve
/// control flow, build the CFG and MedIR, propagate registers, and recover
/// the HighIR and Solana views.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFAnalyzer.h"

#include "SBFAnalyzerDetail.h"

#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace neverd::sbf {

using namespace analyzer_detail;

llvm::Expected<SBFProgram> analyze(const BinaryImage &Image,
                                   const AnalyzeOptions &Options) {
  if (Image.Arch != Arch::SBF || !Image.SBF)
    return llvm::make_error<llvm::StringError>(
        "sbf: input image is not a loaded SBF ELF",
        llvm::inconvertibleErrorCode());
  SBFProgram Program;
  Program.Image = *Image.SBF;
  if (Options.VersionOverride != Version::Auto) {
    if (!isConcreteVersion(Options.VersionOverride))
      return llvm::make_error<llvm::StringError>(
          "sbf: invalid explicit version override",
          llvm::inconvertibleErrorCode());
    Program.Image.Version = Options.VersionOverride;
  }

  if (Options.ExpertEnvironment && Options.RuntimeVersions)
    return llvm::make_error<llvm::StringError>(
        "sbf: a complete expert environment cannot be combined with a "
        "separate runtime version policy",
        llvm::inconvertibleErrorCode());

  auto Environment = [&]() -> llvm::Expected<ResolvedRuntimeEnvironment> {
    if (Options.ExpertEnvironment)
      return resolveExpertRuntimeEnvironment(*Options.ExpertEnvironment);
    const RuntimeVersionPolicy VersionPolicy = Options.RuntimeVersions.value_or(
        Options.VersionOverride == Version::Auto
            ? RuntimeVersionPolicy::ChainProfile
            : RuntimeVersionPolicy::UpstreamToolchain);
    return resolveRuntimeEnvironment(Options.Profile, VersionPolicy);
  }();
  if (!Environment)
    return Environment.takeError();
  Program.Config = Environment->vmConfig();
  Program.EnvironmentOrigin = Environment->origin();
  Program.VersionPolicy = Environment->versionPolicy();
  Program.MinimumRuntimeVersion = Environment->minimumVersion();
  Program.MaximumRuntimeVersion = Environment->maximumVersion();
  Program.Profile = Environment->profile();
  Program.ActiveRuntimeFeatures = Environment->activeRuntimeFeatures();
  Program.RuntimeAccountABI = Environment->accountABI();
  Program.RegisteredSyscallHashes.assign(
      Environment->registeredSyscallHashes().begin(),
      Environment->registeredSyscallHashes().end());

  auto ExecutableImage = buildProgramImage(Image, Program.Image, *Environment);
  if (!ExecutableImage)
    return ExecutableImage.takeError();
  Program.ExecutableImage = std::move(*ExecutableImage);
  Program.Low.TheVersion = Program.ExecutableImage.version();
  Program.Low.TextAddress = Program.ExecutableImage.textAddress();
  Program.Low.EntrySlot = Program.ExecutableImage.entrySlot();

  DecodeContext Context{Image, Options, Program};
  if (llvm::Error Error = decodeInstructions(Context))
    return std::move(Error);
  if (Program.Low.EntrySlot >= Program.Low.Instructions.size())
    return analysisError(Program.Low.EntrySlot, Image.Entry,
                         "entry point is outside program text");
  if (llvm::Error Error = resolveControlFlow(Context))
    return std::move(Error);
  collectFunctionEntries(Context);
  if (Options.Verification == VerificationPolicy::RequisiteAndLocalPreflight) {
    const bool RequisiteFailed = std::any_of(
        Program.Low.Instructions.begin(), Program.Low.Instructions.end(),
        [](const LowInstruction &Instruction) {
          return Instruction.isInvalid();
        });
    if (RequisiteFailed) {
      Program.Verification.State = VerificationState::BlockedByRequisite;
    } else {
      auto Verification =
          verifyLocalPreflight(Program.Low, Program.RegisteredSyscallHashes);
      if (!Verification)
        return Verification.takeError();
      Program.Verification = std::move(*Verification);
      for (const VerificationIssue &Issue : Program.Verification.Issues) {
        Program.Low.Diagnostics.push_back(
            {DiagnosticSeverity::Error, Issue.Slot, Issue.Address,
             Issue.Message, ValidationRule::None});
        if (Options.Strict)
          return analysisError(Issue.Slot, Issue.Address, Issue.Message);
      }
    }
  }

  // Candidate CALLX targets enlarge FunctionEntrySlots monotonically. A new
  // function boundary contributes its own ABI predecessor and can therefore
  // invalidate the register proof that first suggested it. Recompute every
  // original CALLX after each rebuild, including sites previously classified
  // as Internal, until neither the bounded boundary set nor a classification
  // changes. Retaining discovered boundaries makes the process monotone and
  // avoids an Indirect/Internal oscillation without an arbitrary step cap.
  for (;;) {
    Program.Low.Blocks.clear();
    Program.Low.Edges.clear();
    Program.Med = {};
    buildCFG(Program.Low, Context.FunctionEntrySlots);
    buildMedIR(Program);
    runRegisterDataflow(Program, Context.FunctionEntrySlots);
    if (!refineCallXTargets(Context))
      break;
  }
  if (Options.RecoverHighIR) {
    recoverHighIR(Context);
    Program.High.Solana = recoverSolanaModel(Program, {Options.Idl});
  }
  return Program;
}

const Function *findFunction(const SBFProgram &Program,
                             llvm::StringRef Identifier) {
  if (Identifier.empty()) {
    for (const Function &Candidate : Program.High.Functions)
      if (Candidate.EntrySlot == Program.Low.EntrySlot)
        return &Candidate;
    return nullptr;
  }

  for (const Function &Candidate : Program.High.Functions)
    if (Candidate.Name == Identifier)
      return &Candidate;

  if (Identifier.front() == '-')
    return nullptr;
  llvm::StringRef AddressText = Identifier;
  if ((AddressText.consume_front("0x") || AddressText.consume_front("0X")) &&
      AddressText.empty())
    return nullptr;
  va_t Address = 0;
  if (AddressText.getAsInteger(16, Address))
    return nullptr;
  for (const Function &Candidate : Program.High.Functions)
    if (Candidate.Address == Address)
      return &Candidate;
  return nullptr;
}

} // namespace neverd::sbf

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
  Program.Config = Options.VMConfig;
  Program.Profile = Options.Profile;
  if (Options.VersionOverride != Version::Auto) {
    if (!isConcreteVersion(Options.VersionOverride))
      return llvm::make_error<llvm::StringError>(
          "sbf: invalid explicit version override",
          llvm::inconvertibleErrorCode());
    Program.Image.Version = Options.VersionOverride;
  }

  auto ExecutableImage =
      buildProgramImage(Image, Program.Image, Program.Config);
  if (!ExecutableImage)
    return ExecutableImage.takeError();
  Program.ExecutableImage = std::move(*ExecutableImage);
  Program.Low.TheVersion = Program.Image.Version;
  Program.Low.TextAddress = Program.Image.TextVM.Address;
  if (Image.Entry < Program.Low.TextAddress ||
      (Image.Entry - Program.Low.TextAddress) % kInstructionSize != 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: entry point is not instruction-aligned in text",
        llvm::inconvertibleErrorCode());
  Program.Low.EntrySlot = static_cast<size_t>(
      (Image.Entry - Program.Low.TextAddress) / kInstructionSize);

  DecodeContext Context{Image, Options, Program};
  if (llvm::Error Error = decodeInstructions(Context))
    return std::move(Error);
  if (Program.Low.EntrySlot >= Program.Low.Instructions.size() ||
      Program.Low.Instructions[Program.Low.EntrySlot].IsContinuation)
    return analysisError(Program.Low.EntrySlot, Image.Entry,
                         "entry point is not a complete instruction");
  if (llvm::Error Error = resolveControlFlow(Context))
    return std::move(Error);
  buildCFG(Program.Low);
  buildMedIR(Program);
  runRegisterDataflow(Program);
  if (Options.RecoverHighIR) {
    recoverHighIR(Image, Program);
    Program.High.Solana =
        recoverSolanaModel(Program, {Options.Idl, Options.Profile});
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

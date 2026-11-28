//===- NeverDCmdContinuation.cpp - Explicit x86 state source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read an explicit invocation declaration and export source with its manifest.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/X64ContinuationExport.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace neverd::cli {
namespace {
llvm::Expected<std::string> readContinuationInput(llvm::StringRef Path,
                                                  size_t Limit) {
  const std::filesystem::path File(std::u8string(Path.begin(), Path.end()));
  std::error_code EC;
  const auto Size = std::filesystem::file_size(File, EC);
  if (EC)
    return llvm::createFileError(Path, EC);
  if (!Size || Size > Limit)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "continuation input size is outside the request limit");
  std::ifstream Input(File, std::ios::binary);
  std::string Text(static_cast<size_t>(Size), '\0');
  if (!Input.read(Text.data(), static_cast<std::streamsize>(Size)) ||
      Input.peek() != std::char_traits<char>::eof())
    return llvm::createFileError(Path, llvm::errc::io_error);
  return Text;
}
} // namespace

int runContinuation(neverd_session_t Sess) {
  const bool Legacy = X86LegacySource.getNumOccurrences();
  const auto &Request = Legacy ? X86LegacySource : X64Continuation;
  const bool HasObservation = X64XbeginObservation.getNumOccurrences();
  if (HasObservation &&
      (Legacy || X64Continuation.empty() || X64XbeginObservation.empty())) {
    llvm::WithColor::error() << "--x64-xbegin-observation requires an "
                                "observation file and --x64-continuation\n";
    return 1;
  }
  if (Request.empty() || (Legacy && X64Continuation.getNumOccurrences()) ||
      (Legacy && LlvmRoute.getNumOccurrences()) || ContinuationReport.empty() ||
      OutputLanguage != NEVERD_OUTPUT_C || !ExportFunc.empty() || NoOpt ||
      Devirtualize || DecompileCallees || StackFrameInfo ||
      StackFrameFile.getNumOccurrences() ||
      SourceSymbolic.getNumOccurrences() ||
      OriginalCallState.getNumOccurrences() ||
      SourceLLVMControlFlow.getNumOccurrences() ||
      SourceLLVMLevel.getNumOccurrences() ||
      SourceLLVMPasses.getNumOccurrences() || MaxFunc.getNumOccurrences()) {
    llvm::WithColor::error()
        << "select --x64-continuation or --x86-legacy-source with a request "
           "JSON and "
           "--continuation-report; "
           "use C output (--llvm is only valid for --x64-continuation), "
           "without ordinary function, "
           "frame, devirtualization or source-optimization options\n";
    return 1;
  }
  const auto SameFile = [](llvm::StringRef A, llvm::StringRef B) {
    if (A.empty() || B.empty())
      return false;
    std::error_code ErrorA, ErrorB;
    const auto CanonicalA = std::filesystem::weakly_canonical(A.str(), ErrorA);
    const auto CanonicalB = std::filesystem::weakly_canonical(B.str(), ErrorB);
    return A == B || llvm::sys::fs::equivalent(A, B) ||
           (!ErrorA && !ErrorB && CanonicalA == CanonicalB);
  };
  if (SameFile(OutputFile, ContinuationReport) ||
      SameFile(OutputFile, Request) || SameFile(OutputFile, InputFile) ||
      SameFile(ContinuationReport, Request) ||
      SameFile(ContinuationReport, InputFile) ||
      (HasObservation &&
       (SameFile(OutputFile, X64XbeginObservation) ||
        SameFile(ContinuationReport, X64XbeginObservation)))) {
    llvm::WithColor::error()
        << "continuation source and manifest must use distinct output files, "
           "separate from the inputs\n";
    return 1;
  }
  const auto RequestLimit =
      Legacy ? limits::pipeline::kX86LegacySourceRequestBytes
             : limits::pipeline::kX64ContinuationRequestBytes;
  auto Text = readContinuationInput(Request, RequestLimit);
  if (!Text) {
    llvm::WithColor::error() << "cannot read continuation request: "
                             << llvm::toString(Text.takeError()) << "\n";
    return 1;
  }
  std::string Observation;
  if (HasObservation) {
    auto Saved = readContinuationInput(X64XbeginObservation, RequestLimit);
    if (!Saved) {
      llvm::WithColor::error() << "cannot read XBEGIN observation: "
                               << llvm::toString(Saved.takeError()) << "\n";
      return 1;
    }
    Observation = std::move(*Saved);
  }
  const char *Manifest = nullptr;
  const char *Source =
      Legacy ? neverd_x86_legacy_source_v1(Sess, Text->data(), Text->size(),
                                           &Manifest)
             : neverd_x64_continuation_source_with_observation_v1(
                   Sess, Text->data(), Text->size(),
                   HasObservation ? Observation.data() : nullptr,
                   Observation.size(), LlvmRoute, &Manifest);
  auto Free = llvm::scope_exit([&] {
    neverd_free_string(Source);
    neverd_free_string(Manifest);
  });
  if (!Source || !Manifest) {
    llvm::WithColor::error()
        << "continuation export failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  std::error_code Error;
  llvm::raw_fd_ostream Report(ContinuationReport, Error);
  if (Error) {
    llvm::WithColor::error()
        << "cannot write continuation manifest: " << Error.message() << "\n";
    return 1;
  }
  Report << Manifest;
  Report.close();
  if (Report.has_error()) {
    llvm::WithColor::error() << "continuation manifest write failed\n";
    Report.clear_error();
    return 1;
  }
  if (!OutputFile.empty()) {
    llvm::raw_fd_ostream Output(OutputFile, Error);
    if (Error) {
      llvm::WithColor::error()
          << "cannot write continuation source: " << Error.message() << "\n";
      return 1;
    }
    Output << Source;
    Output.close();
    if (Output.has_error()) {
      llvm::WithColor::error() << "continuation source write failed\n";
      Output.clear_error();
      return 1;
    }
    llvm::outs() << "State C written to " << OutputFile.getValue() << "\n";
  } else {
    llvm::outs() << Source;
  }
  return 0;
}
} // namespace neverd::cli

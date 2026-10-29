//===- NeverDCmdContinuation.cpp - Explicit x64 state source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read an explicit invocation declaration and export source with its manifest.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/pipeline/X64ContinuationExport.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace neverd::cli {
int runX64Continuation(neverd_session_t Sess) {
  if (X64Continuation.empty() || ContinuationReport.empty() ||
      OutputLanguage != NEVERD_OUTPUT_C || !ExportFunc.empty() || NoOpt ||
      Devirtualize || DecompileCallees || StackFrameInfo ||
      StackFrameFile.getNumOccurrences() ||
      SourceSymbolic.getNumOccurrences() ||
      SourceLLVMControlFlow.getNumOccurrences() ||
      SourceLLVMLevel.getNumOccurrences() ||
      SourceLLVMPasses.getNumOccurrences() || MaxFunc.getNumOccurrences()) {
    llvm::WithColor::error()
        << "--x64-continuation requires a request JSON and "
           "--continuation-report; "
           "use C output with optional --llvm, without ordinary function, "
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
      SameFile(OutputFile, X64Continuation) ||
      SameFile(OutputFile, InputFile) ||
      SameFile(ContinuationReport, X64Continuation) ||
      SameFile(ContinuationReport, InputFile)) {
    llvm::WithColor::error()
        << "continuation source and manifest must use distinct output files, "
           "separate from the inputs\n";
    return 1;
  }
  uint64_t Size = 0;
  if (llvm::sys::fs::file_size(X64Continuation, Size) || !Size ||
      Size > limits::pipeline::kX64ContinuationRequestBytes) {
    llvm::WithColor::error() << "cannot read a bounded continuation request\n";
    return 1;
  }
  auto Buffer = llvm::MemoryBuffer::getFile(X64Continuation);
  if (!Buffer) {
    llvm::WithColor::error()
        << "cannot read continuation request: " << Buffer.getError().message()
        << "\n";
    return 1;
  }
  const char *Manifest = nullptr;
  const char *Source = neverd_x64_continuation_source_v1(
      Sess, (*Buffer)->getBufferStart(), (*Buffer)->getBufferSize(), LlvmRoute,
      &Manifest);
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

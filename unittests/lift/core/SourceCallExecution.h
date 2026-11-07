#ifndef NEVERD_TEST_SOURCECALLEXECUTION_H
#define NEVERD_TEST_SOURCECALLEXECUTION_H

#include "../../HostCompiler.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

namespace source_call_execution_test {
inline void compileSource(const std::string &Source,
                          llvm::ArrayRef<llvm::StringRef> ExtraArguments,
                          bool Execute) {
  if (const auto Reason = neverd::test::lacksPosixHeaders(Source))
    GTEST_SKIP() << *Reason;
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  // Decompiled C reads and writes scalars through plain pointer casts and is
  // built as it documents: without strict aliasing.
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O1",
      "-fno-strict-aliasing",
      "-fno-inline",
      "-fblocks",
      "-Werror=implicit-function-declaration",
      "-Werror=return-type"};
  Arguments.append(ExtraArguments.begin(), ExtraArguments.end());
  // An oracle calls original instructions it has mapped. UBSan's function check
  // reads the eight bytes before a callee, which a fresh mapping need not have.
  if (llvm::is_contained(ExtraArguments,
                         llvm::StringRef("-fsanitize=undefined")))
    Arguments.push_back("-fno-sanitize=function");
  Arguments.append({SourcePath, "-o", BinaryPath});
  if (!Execute)
    Arguments.push_back("-fsyntax-only");
  std::string Error;
  const int Compiled = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
#ifdef _WIN32
  // C decompiled from an image defines the routines it carried, which the
  // static CRT also defines, and wide division needs a compiler runtime that
  // an MSVC link does not name.
  if (Compiled != 0 && Errors &&
      ((*Errors)->getBuffer().contains("LNK2005") ||
       (*Errors)->getBuffer().contains("LNK2019")))
    GTEST_SKIP() << "this host's C runtime does not link this source";
#endif
  ASSERT_EQ(Compiled, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << "\n"
                         << Source;
  if (!Execute)
    return;
  const int Ran = llvm::sys::ExecuteAndWait(
      BinaryPath, {BinaryPath}, std::nullopt, Redirects, 30, 0, &Error);
  const auto RuntimeErrors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Ran, 0) << Error
                    << (RuntimeErrors ? (*RuntimeErrors)->getBuffer().str()
                                      : "")
                    << "\n"
                    << Source;
}

inline void compileAndRun(const std::string &Source,
                          llvm::ArrayRef<llvm::StringRef> ExtraArguments = {}) {
  compileSource(Source, ExtraArguments, true);
}

inline void checkSyntax(const std::string &Source,
                        llvm::ArrayRef<llvm::StringRef> ExtraArguments = {}) {
  compileSource(Source, ExtraArguments, false);
}

} // namespace source_call_execution_test
#endif

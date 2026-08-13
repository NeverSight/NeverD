//===- SBFSourceDifferentialTests.cpp - SBF source backend execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compiles what the C and Rust backends emit and runs it against the same
/// programs the raw interpreter executed.
///
//===----------------------------------------------------------------------===//

#include "SBFSourceDifferentialCHarness.h"
#include "SBFSourceDifferentialDetail.h"
#include "SBFSourceDifferentialRustHarness.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

using test::analyzeProgram;
using test::compilerName;
using test::DifferentialCase;
using test::encode;
using test::encodeLDDW;
using test::initialMemory;
using test::makeCHarness;
using test::makeRustHarness;
using test::SourceBackend;
using test::TemporaryFile;

void compileAndRun(SourceBackend Backend, llvm::StringRef Source) {
  const llvm::StringRef CompilerName = compilerName(Backend);
  auto Compiler = llvm::sys::findProgramByName(CompilerName);
  ASSERT_TRUE(static_cast<bool>(Compiler))
      << CompilerName.str() << " disappeared after capability detection";
  if (!Compiler)
    return;

  TemporaryFile SourceFile(Backend == SourceBackend::C ? "c" : "rs");
  TemporaryFile Executable("out");
  ASSERT_FALSE(SourceFile.error()) << SourceFile.error().message();
  ASSERT_FALSE(Executable.error()) << Executable.error().message();
  {
    std::ofstream Output(SourceFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << Source.str();
  }

#ifdef _WIN32
  std::string LinkerArgument;
#endif
  llvm::SmallVector<llvm::StringRef, 16> Arguments;
  Arguments.push_back(*Compiler);
  if (Backend == SourceBackend::C) {
    Arguments.append({"-std=c11", "-Wall", "-Wextra", "-Werror"});
  } else {
    Arguments.append({"--edition=2021",
                      "--crate-name=neverd_sbf_source_differential", "-D",
                      "warnings"});
#ifdef _WIN32
    // Git for Windows puts its POSIX link.exe ahead of the MSVC linker.
    // Select the unambiguous COFF linker explicitly when invoking rustc.
    auto Linker = llvm::sys::findProgramByName("lld-link");
    ASSERT_TRUE(static_cast<bool>(Linker)) << "lld-link is not available";
    if (!Linker)
      return;
    LinkerArgument = "linker=" + *Linker;
    Arguments.append({"-C", LinkerArgument});
#endif
  }
  Arguments.append({SourceFile.str(), "-o", Executable.str()});
  std::string Error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Compiler, Arguments, std::nullopt, {}, 0,
                                      0, &Error),
            0)
      << Error;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

void runDifferential(SourceBackend Backend, const DifferentialCase &Case) {
  SCOPED_TRACE(Case.Name);
  ExecutionEnvironment OracleEnvironment = Case.Environment;
  OracleEnvironment.Syscall = [](uint32_t, const SyscallArguments &Arguments)
      -> std::optional<uint64_t> { return Arguments[0] + 1; };
  std::vector<MemoryRegion> Memory =
      initialMemory(Case.Program, OracleEnvironment);
  auto Expected = executeRaw(Case.Program, std::move(OracleEnvironment));
  ASSERT_TRUE(static_cast<bool>(Expected))
      << llvm::toString(Expected.takeError());
  ASSERT_EQ(Expected->Memory.size(), Memory.size());

  if (Backend == SourceBackend::C) {
    CEmitterOptions Options;
    Options.PreferStructuredControlFlow = false;
    auto Source = emitC(Case.Program, Options);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    *Source = "#include <string.h>\n" + *Source +
              makeCHarness(Case.Environment, *Expected, Memory);
    compileAndRun(Backend, *Source);
    return;
  }

  RustEmitterOptions Options;
  Options.PreferStructuredControlFlow = false;
  auto Source = emitRust(Case.Program, Options);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  *Source += makeRustHarness(Case.Environment, *Expected, Memory);
  compileAndRun(Backend, *Source);
}

void addSyntheticCases(std::vector<DifferentialCase> &Cases) {
  auto Add = [&](std::string Name, auto Program,
                 ExecutionEnvironment Environment = {}) {
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    Cases.push_back(
        {std::move(Name), std::move(*Program), std::move(Environment)});
  };

  Add("v0-sign-extension",
      analyzeProgram(Version::V0, {encode(Opcode::MOV64_IMM, 0, 0, 0,
                                          std::numeric_limits<int32_t>::max()),
                                   encode(Opcode::ADD32_IMM, 0, 0, 0, 1),
                                   encode(Opcode::EXIT)}));
  Add("v2-pqr",
      analyzeProgram(Version::V2, {encode(Opcode::MOV64_IMM, 0, 0, 0, -2),
                                   encode(Opcode::UDIV64_IMM, 0, 0, 0, -1),
                                   encode(Opcode::SUB64_IMM, 0, 0, 0, 3),
                                   encode(Opcode::EXIT)}));
  Add("v3-nested-calls",
      analyzeProgram(
          Version::V3,
          {encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
           encode(Opcode::CALL_IMM, 0, 1, 0, 3),
           encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
           encode(Opcode::MOV64_IMM, 0, 0, 0, 0),
           encode(Opcode::ADD64_IMM, 0, 0, 0, 40),
           encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
           encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT)}));

  ExecutionEnvironment InstructionDataEnvironment;
  InstructionDataEnvironment.InstructionData = UINT64_C(0x500000040);
  Add("instruction-data-register",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_REG, 0, 2), encode(Opcode::EXIT)}),
      std::move(InstructionDataEnvironment));

  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  ExecutionEnvironment MemoryEnvironment;
  MemoryEnvironment.Memory.push_back(
      {kInputStart, {9, 0, 0, 0, 0, 0, 0, 0}, false, "input"});
  Add("memory-and-syscall",
      analyzeProgram(
          Version::V3,
          {encode(Opcode::LD_DW_REG, 2, 1),
           encode(Opcode::ST_DW_REG, kFramePointerRegister, 2, -8),
           encode(Opcode::LD_DW_REG, 1, kFramePointerRegister, -8),
           encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
           encode(Opcode::EXIT)}),
      std::move(MemoryEnvironment));
  Add("memory-fault",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                      encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)}));
  Add("unresolved-legacy-call",
      analyzeProgram(Version::V0, {encode(Opcode::CALL_IMM, 0, 0, 0, -1),
                                   encode(Opcode::EXIT)}));

  const auto ContinuationTarget =
      encodeLDDW(2, kBytecodeStart + kInstructionSize);
  Add("callx-continuation",
      analyzeProgram(Version::V3,
                     {ContinuationTarget[0], ContinuationTarget[1],
                      encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT)}));
}

void addOfficialRelocationCase(std::vector<DifferentialCase> &Cases) {
  const char *Root = std::getenv("NEVERD_SBPF_ROOT");
  if (!Root)
    return;
  const std::filesystem::path Path = std::filesystem::path(Root) / "tests" /
                                     "elfs" /
                                     "reloc_64_relative_data_sbpfv0.so";
  if (!std::filesystem::exists(Path))
    return;
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  auto Program = analyze(*Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  Cases.push_back({"official-v0-relocated-data", std::move(*Program), {}});
}

void runAllSourceCases(SourceBackend Backend) {
  const llvm::StringRef CompilerName = compilerName(Backend);
  if (!llvm::sys::findProgramByName(CompilerName))
    GTEST_SKIP() << CompilerName.str() << " is not available";

  std::vector<DifferentialCase> Cases;
  addSyntheticCases(Cases);
  addOfficialRelocationCase(Cases);
  ASSERT_GE(Cases.size(), 6u);
  for (const DifferentialCase &Case : Cases)
    runDifferential(Backend, Case);
}

TEST(SBFSourceDifferential, GeneratedCMatchesTheRawOracle) {
  runAllSourceCases(SourceBackend::C);
}

TEST(SBFSourceDifferential, GeneratedRustMatchesTheRawOracle) {
  runAllSourceCases(SourceBackend::Rust);
}
} // namespace
} // namespace neverd::sbf

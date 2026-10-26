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
#include "neverd/sbf/emit/SBFRustEmitter.h"
#include "neverd/sbf/emit/SBFSourceLimits.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
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

void runDifferential(SourceBackend Backend, const DifferentialCase &Case,
                     bool Partitioned, std::string *RustModule = nullptr) {
  SCOPED_TRACE(Case.Name);
  SCOPED_TRACE(Partitioned ? "partitioned" : "monolithic");
  ExecutionEnvironment OracleEnvironment = Case.Environment;
  SyscallCallback CustomSyscall = OracleEnvironment.Syscall;
  OracleEnvironment.Syscall =
      [CustomSyscall = std::move(CustomSyscall)](
          uint32_t Hash,
          const SyscallArguments &Arguments) -> std::optional<uint64_t> {
    if (CustomSyscall)
      return CustomSyscall(Hash, Arguments);
    if (!getSyscallInfo(Hash))
      return std::nullopt;
    return Arguments[0] + 1;
  };
  std::vector<MemoryRegion> Memory =
      initialMemory(Case.Program, OracleEnvironment);
  auto Expected = executeRaw(Case.Program, std::move(OracleEnvironment));
  ASSERT_TRUE(static_cast<bool>(Expected))
      << llvm::toString(Expected.takeError());
  if (Case.LoadFault || Case.StoreFault) {
    ASSERT_EQ(Expected->Status, ExecutionStatus::Faulted);
    ASSERT_EQ(Expected->Fault, FaultCode::MemoryAccess);
    Expected->Fault = Case.LoadFault ? *Case.LoadFault : *Case.StoreFault;
  }
  ASSERT_EQ(Expected->Memory.size(), Memory.size());

  if (Backend == SourceBackend::C) {
    CEmitterOptions Options;
    Options.PreferStructuredControlFlow = false;
    Options.PartitionDispatcher = Partitioned;
    auto Source = emitC(Case.Program, Options);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    *Source = "#include <string.h>\n" + *Source +
              makeCHarness(Case.Environment, *Expected,
                           Case.Program.ActiveRuntimeFeatures, Memory,
                           Case.HostFaults, Case.LoadFault, Case.StoreFault);
    compileAndRun(Backend, *Source);
    return;
  }

  RustEmitterOptions Options;
  Options.PreferStructuredControlFlow = false;
  Options.PartitionDispatcher = Partitioned;
  auto Source = emitRust(Case.Program, Options);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  *Source += makeRustHarness(Case.Environment, *Expected,
                             Case.Program.ActiveRuntimeFeatures, Memory,
                             Case.HostFaults, Case.LoadFault, Case.StoreFault);
  if (RustModule)
    *RustModule = std::move(*Source);
  else
    compileAndRun(Backend, *Source);
}

void addSyntheticCases(std::vector<DifferentialCase> &Cases) {
  auto Add = [&](std::string Name, auto Program,
                 ExecutionEnvironment Environment = {},
                 std::vector<DifferentialCase::HostFault> HostFaults = {},
                 std::optional<FaultCode> LoadFault = std::nullopt,
                 std::optional<FaultCode> StoreFault = std::nullopt) {
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    Cases.push_back({std::move(Name), std::move(*Program),
                     std::move(Environment), std::move(HostFaults), LoadFault,
                     StoreFault});
  };

  // Real corpus programs include both reflexive comparisons and signed casts
  // immediately before '<'. Every predicate contributes a distinct result bit
  // so a wrong fold cannot be hidden by another wrong branch.
  std::vector<test::EncodedInstruction> Comparisons{
      encode(Opcode::MOV64_IMM, 4, 0, 0, -1),
      encode(Opcode::MOV64_IMM, 5, 0, 0, 0)};
  unsigned Predicate = 0;
  const auto AppendComparison = [&](Opcode Op, uint8_t Dst, uint8_t Src,
                                    int32_t Immediate = 0) {
    Comparisons.push_back(encode(Op, Dst, Src, 1, Immediate));
    Comparisons.push_back(
        encode(Opcode::ADD64_IMM, 0, 0, 0, int32_t{1} << Predicate++));
  };
  for (Opcode Op : {Opcode::JEQ64_REG,  Opcode::JNE64_REG,  Opcode::JGT64_REG,
                    Opcode::JGE64_REG,  Opcode::JLT64_REG,  Opcode::JLE64_REG,
                    Opcode::JSGT64_REG, Opcode::JSGE64_REG, Opcode::JSLT64_REG,
                    Opcode::JSLE64_REG, Opcode::JEQ32_REG,  Opcode::JNE32_REG,
                    Opcode::JGT32_REG,  Opcode::JGE32_REG,  Opcode::JLT32_REG,
                    Opcode::JLE32_REG,  Opcode::JSGT32_REG, Opcode::JSGE32_REG,
                    Opcode::JSLT32_REG, Opcode::JSLE32_REG})
    AppendComparison(Op, 4, 4);
  AppendComparison(Opcode::JSLT64_REG, 4, 5);
  AppendComparison(Opcode::JSLT32_REG, 4, 5);
  AppendComparison(Opcode::JSLT64_IMM, 4, 0);
  AppendComparison(Opcode::JSLT32_IMM, 4, 0);
  AppendComparison(Opcode::JSET64_REG, 4, 4);
  AppendComparison(Opcode::JSET32_REG, 5, 5);
  Comparisons.push_back(encode(Opcode::EXIT));
  Add("signed-and-reflexive-comparisons",
      analyzeProgram(Version::V3, Comparisons));
  Add("only-reflexive-signed-predicate",
      analyzeProgram(Version::V3, {encode(Opcode::JSGT64_REG, 1, 1, 1),
                                   encode(Opcode::MOV64_IMM, 0, 0, 0, 37),
                                   encode(Opcode::EXIT)}));

  // The source dialect must cast the complete expression. Rust's `as`
  // binds more tightly than bitwise operators; testing both legacy signed
  // and current zero extension catches a cast applied only to the RHS.
  for (Version SourceVersion : {Version::V0, Version::V3}) {
    std::vector<test::EncodedInstruction> Bitwise;
    const auto Wide = encodeLDDW(1, UINT64_C(0xabcdef0181234567));
    Bitwise.insert(Bitwise.end(), Wide.begin(), Wide.end());
    for (Opcode Op : {Opcode::OR32_REG, Opcode::AND32_REG, Opcode::XOR32_REG,
                      Opcode::OR32_IMM, Opcode::AND32_IMM, Opcode::XOR32_IMM}) {
      Bitwise.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, -1));
      Bitwise.push_back(encode(Op, 2, 1, 0, -2));
      Bitwise.push_back(encode(Opcode::XOR64_REG, 0, 2));
    }
    Bitwise.push_back(encode(Opcode::EXIT));
    Add(SourceVersion == Version::V0 ? "legacy-32-bit-expression-extension"
                                     : "current-32-bit-expression-extension",
        analyzeProgram(SourceVersion, Bitwise));
  }

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

  // Both caller and callee cross rendering regions. Callee-saved values must
  // survive the native region transitions and the guest software return;
  // caller-saved registers and the result must retain the callee's changes.
  std::vector<test::EncodedInstruction> RegionCalls;
  for (unsigned Register = kFirstCalleeSavedRegister;
       Register < kFirstCalleeSavedRegister + kCalleeSavedRegisterCount;
       ++Register)
    RegionCalls.push_back(encode(Opcode::MOV64_IMM, Register, 0, 0, Register));
  const size_t CallSlot = RegionCalls.size();
  RegionCalls.push_back({});
  for (size_t Index = 0; Index < kMaximumDispatcherRegionSlots * 2; ++Index)
    RegionCalls.push_back(encode(Opcode::ADD64_IMM, 0, 0, 0, 1));
  for (unsigned Register = kFirstCalleeSavedRegister;
       Register < kFirstCalleeSavedRegister + kCalleeSavedRegisterCount;
       ++Register) {
    RegionCalls.push_back(
        encode(Opcode::MUL64_IMM, Register, 0, 0, int32_t{1} << Register));
    RegionCalls.push_back(encode(Opcode::ADD64_REG, 0, Register));
  }
  RegionCalls.push_back(encode(Opcode::ADD64_REG, 0, kFramePointerRegister));
  RegionCalls.push_back(encode(Opcode::ADD64_REG, 0, kFirstArgumentRegister));
  RegionCalls.push_back(encode(Opcode::EXIT));
  const size_t CalleeSlot = RegionCalls.size();
  RegionCalls[CallSlot] =
      encode(Opcode::CALL_IMM, 0, 1, 0,
             static_cast<int32_t>(CalleeSlot - CallSlot - 1));
  for (size_t Index = 0; Index < kMaximumDispatcherRegionSlots * 2; ++Index)
    RegionCalls.push_back(encode(Opcode::ADD64_IMM, 0, 0, 0, 1));
  for (unsigned Register = kFirstCalleeSavedRegister;
       Register < kFirstCalleeSavedRegister + kCalleeSavedRegisterCount;
       ++Register)
    RegionCalls.push_back(encode(Opcode::MOV64_IMM, Register, 0, 0, -1));
  RegionCalls.push_back(
      encode(Opcode::MOV64_IMM, kFirstArgumentRegister, 0, 0, 7));
  RegionCalls.push_back(encode(Opcode::EXIT));
  Add("source-regions-retain-call-frame-registers",
      analyzeProgram(Version::V3, RegionCalls));

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
  auto WideFeatureSnapshot = analyzeProgram(
      Version::V3,
      {encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)});
  ASSERT_TRUE(static_cast<bool>(WideFeatureSnapshot))
      << llvm::toString(WideFeatureSnapshot.takeError());
  WideFeatureSnapshot->ActiveRuntimeFeatures =
      RuntimeFeature::SyscallParameterAddressRestrictions |
      RuntimeFeature::DisableAllocFreeDeployment;
  Add("wide-runtime-feature-snapshot", std::move(WideFeatureSnapshot));
  Add("memory-fault",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                      encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)}));
  Add("host-load-propagates-non-memory-fault",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                      encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)}),
      {}, {}, FaultCode::DivideByZero);
  Add("host-store-propagates-non-memory-fault",
      analyzeProgram(Version::V3, {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                                   encode(Opcode::ST_DW_IMM, 1, 0, 0, 42),
                                   encode(Opcode::EXIT)}),
      {}, {}, std::nullopt, FaultCode::DivideOverflow);
  Add("unresolved-legacy-call",
      analyzeProgram(Version::V0, {encode(Opcode::CALL_IMM, 0, 0, 0, -1),
                                   encode(Opcode::EXIT)}));
  ExecutionEnvironment CustomSyscallEnvironment;
  CustomSyscallEnvironment.RuntimeFeatures = RuntimeFeature::None;
  CustomSyscallEnvironment.Syscall =
      [](uint32_t Hash,
         const SyscallArguments &Arguments) -> std::optional<uint64_t> {
    if (Hash != std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    return Arguments[0] + 1;
  };
  Add("runtime-resolved-legacy-syscall",
      analyzeProgram(Version::V0, {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
                                   encode(Opcode::CALL_IMM, 0, 0, 0, -1),
                                   encode(Opcode::EXIT)}),
      std::move(CustomSyscallEnvironment));

  constexpr size_t CollisionTargetSlot = 4;
  const uint32_t CollisionKey = legacyFunctionKey(CollisionTargetSlot, {});
  ExecutionEnvironment CollisionEnvironment;
  CollisionEnvironment.Syscall =
      [CollisionKey](
          uint32_t Hash,
          const SyscallArguments &Arguments) -> std::optional<uint64_t> {
    if (Hash != CollisionKey)
      return std::nullopt;
    return Arguments[0] + 1;
  };
  Add("legacy-runtime-syscall-then-internal-call-key",
      analyzeProgram(Version::V0, {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
                                   encode(Opcode::CALL_IMM, 0, 0, 0, 2),
                                   encode(Opcode::ADD64_IMM, 0, 0, 0, 1),
                                   encode(Opcode::EXIT),
                                   encode(Opcode::MOV64_IMM, 0, 0, 0, 7),
                                   encode(Opcode::EXIT)}),
      std::move(CollisionEnvironment));

  ExecutionEnvironment DirectFaultEnvironment;
  DirectFaultEnvironment.HostSyscall =
      [Hash = Log64->Hash](uint32_t Candidate,
                           const SyscallArguments &) -> SyscallOutcome {
    return Candidate == Hash ? SyscallOutcome::fault(FaultCode::MemoryAccess)
                             : SyscallOutcome::unregistered();
  };
  Add("handled-direct-syscall-fault",
      analyzeProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 0, 0,
                                          static_cast<int32_t>(Log64->Hash)),
                                   encode(Opcode::EXIT)}),
      std::move(DirectFaultEnvironment),
      {{Log64->Hash, FaultCode::MemoryAccess}});

  ExecutionEnvironment CollisionFaultEnvironment;
  CollisionFaultEnvironment.HostSyscall =
      [CollisionKey](uint32_t Hash,
                     const SyscallArguments &) -> SyscallOutcome {
    return Hash == CollisionKey ? SyscallOutcome::fault(FaultCode::MemoryAccess)
                                : SyscallOutcome::unregistered();
  };
  Add("legacy-collision-propagates-handled-syscall-fault",
      analyzeProgram(Version::V0, {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
                                   encode(Opcode::CALL_IMM, 0, 0, 0, 2),
                                   encode(Opcode::ADD64_IMM, 0, 0, 0, 1),
                                   encode(Opcode::EXIT),
                                   encode(Opcode::MOV64_IMM, 0, 0, 0, 7),
                                   encode(Opcode::EXIT)}),
      std::move(CollisionFaultEnvironment),
      {{CollisionKey, FaultCode::MemoryAccess}});

  const auto ContinuationTarget =
      encodeLDDW(2, kBytecodeStart + kInstructionSize);
  Add("entrypoint-continuation",
      analyzeProgram(
          Version::V3,
          {ContinuationTarget[0], ContinuationTarget[1], encode(Opcode::EXIT)},
          {}, 1));
  Add("callx-continuation",
      analyzeProgram(Version::V3,
                     {ContinuationTarget[0], ContinuationTarget[1],
                      encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT)}));

  constexpr uint64_t NarrowedTargetSlot = kLDDWSlotCount + 1;
  constexpr uint64_t WideSlotAlias = uint64_t{1}
                                     << std::numeric_limits<uint32_t>::digits;
  const auto WideCallXTarget = encodeLDDW(
      kInstructionDataRegister,
      kBytecodeStart + (WideSlotAlias + NarrowedTargetSlot) * kInstructionSize);
  Add("callx-target-does-not-narrow-before-bounds-check",
      analyzeProgram(Version::V3,
                     {WideCallXTarget[0], WideCallXTarget[1],
                      encode(Opcode::CALL_REG, kInstructionDataRegister),
                      encode(Opcode::EXIT)}));

  const auto StaticContinuation = encodeLDDW(0, 0);
  Add("static-call-continuation",
      analyzeProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                                   StaticContinuation[0], StaticContinuation[1],
                                   encode(Opcode::EXIT)}));
  auto DepthFirst = analyzeProgram(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1), StaticContinuation[0],
                    StaticContinuation[1], encode(Opcode::EXIT)});
  ASSERT_TRUE(static_cast<bool>(DepthFirst))
      << llvm::toString(DepthFirst.takeError());
  DepthFirst->Config.MaxCallDepth = 1;
  Cases.push_back(
      {"static-call-depth-before-continuation", std::move(*DepthFirst), {}});
  Add("unsupported-static-call-discriminator",
      analyzeProgram(Version::V3,
                     {encode(Opcode::CALL_IMM, 0, 2), encode(Opcode::EXIT)}));
  Add("unsupported-static-call-target",
      analyzeProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0,
                                          std::numeric_limits<int32_t>::max()),
                                   encode(Opcode::EXIT)}));

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  Add("faulted-load-stops-before-following-host-call",
      analyzeProgram(Version::V3, {encode(Opcode::LD_DW_REG, 0, 1),
                                   encode(Opcode::CALL_IMM, 0, 0, 0,
                                          static_cast<int32_t>(Log64->Hash)),
                                   encode(Opcode::EXIT)}));
  // A shared post-step memory fault must win over the fallthrough overrun at
  // the final raw slot and must stop before any following host callback.
  Add("terminal-load-fault-precedes-overrun",
      analyzeProgram(Version::V3, {encode(Opcode::LD_DW_REG, 0, 1)}, Relaxed));
  Add("terminal-store-fault-precedes-overrun",
      analyzeProgram(Version::V3, {encode(Opcode::ST_DW_REG, 1, 0)}, Relaxed));
  ExecutionEnvironment TerminalMemory;
  TerminalMemory.Memory.push_back(
      {kInputStart, {9, 0, 0, 0, 0, 0, 0, 0}, false, "input"});
  Add("terminal-successful-load-overruns",
      analyzeProgram(Version::V3, {encode(Opcode::LD_DW_REG, 0, 1)}, Relaxed),
      std::move(TerminalMemory));
  test::EncodedInstruction Unknown{};
  Unknown[kOpcodeOffset] = std::numeric_limits<uint8_t>::max();
  Add("malformed-unknown-opcode",
      analyzeProgram(Version::V3, {Unknown, encode(Opcode::EXIT)}, Relaxed));
  Add("malformed-missing-lddw-continuation",
      analyzeProgram(Version::V3,
                     {encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                      encode(Opcode::EXIT), encode(Opcode::LDDW)},
                     Relaxed));
  Add("malformed-nonzero-lddw-continuation",
      analyzeProgram(
          Version::V0,
          {encode(Opcode::LDDW), encode(Opcode::EXIT), encode(Opcode::EXIT)},
          Relaxed));
  Add("malformed-invalid-lddw-destination",
      analyzeProgram(Version::V0,
                     {encode(Opcode::LDDW, 15), {}, encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-immediate-division-by-zero",
      analyzeProgram(
          Version::V2,
          {encode(Opcode::UDIV64_IMM, 0, 0, 0, 0), encode(Opcode::EXIT)},
          Relaxed));
  Add("malformed-immediate-shift-out-of-range",
      analyzeProgram(Version::V3,
                     {encode(Opcode::LSH64_IMM, 0, 0, 0, kDoubleWordBitWidth),
                      encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-invalid-endian-immediate",
      analyzeProgram(Version::V3,
                     {encode(Opcode::BE, 0, 0, 0, 24), encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-misaligned-frame-adjustment",
      analyzeProgram(Version::V2,
                     {encode(Opcode::ADD64_IMM, kFramePointerRegister, 0, 0, 1),
                      encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-invalid-source-register",
      analyzeProgram(
          Version::V3,
          {encode(Opcode::MOV64_REG, 0, std::numeric_limits<uint8_t>::max()),
           encode(Opcode::EXIT)},
          Relaxed));
  Add("malformed-frame-pointer-write",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_IMM, kFramePointerRegister),
                      encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-invalid-destination-register",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_IMM, 15), encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-branch-out-of-range",
      analyzeProgram(
          Version::V3,
          {encode(Opcode::JA, 0, 0, std::numeric_limits<int16_t>::max()),
           encode(Opcode::EXIT)},
          Relaxed));
  const auto InvalidBranchTarget = encodeLDDW(0, 0);
  Add("malformed-branch-to-lddw-continuation",
      analyzeProgram(Version::V0,
                     {InvalidBranchTarget[0], InvalidBranchTarget[1],
                      encode(Opcode::JA, 0, 0, -2), encode(Opcode::EXIT)},
                     Relaxed));
  Add("malformed-invalid-callx-register",
      analyzeProgram(
          Version::V0,
          {encode(Opcode::CALL_REG, 0, 0, 0, -1), encode(Opcode::EXIT)},
          Relaxed));
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
  Cases.push_back(
      {"official-v0-relocated-data", std::move(*Program), {}, {}, {}, {}});
}

void runAllSourceCases(SourceBackend Backend) {
  const llvm::StringRef CompilerName = compilerName(Backend);
  if (!llvm::sys::findProgramByName(CompilerName))
    GTEST_SKIP() << CompilerName.str() << " is not available";

  std::vector<DifferentialCase> Cases;
  addSyntheticCases(Cases);
  addOfficialRelocationCase(Cases);
  ASSERT_GE(Cases.size(), 6u);
  std::string RustModules;
  std::string RustDriver = "fn main() {\n";
  size_t ModuleIndex = 0;
  for (const DifferentialCase &Case : Cases) {
    for (bool Partitioned : {false, true}) {
      if (Backend != SourceBackend::Rust) {
        runDifferential(Backend, Case, Partitioned);
        continue;
      }
      // Compile every generated module and execute every oracle assertion,
      // while paying compiler/linker startup once for this synthetic suite.
      // Corpus native qualification continues to compile each complete SO's
      // source independently with its original resource budgets.
      std::string Source;
      runDifferential(Backend, Case, Partitioned, &Source);
      ASSERT_FALSE(Source.empty()) << Case.Name;
      const std::string Name = "nd_case_" + std::to_string(ModuleIndex++);
      RustModules += "pub mod " + Name + " {\n" + Source +
                     "\npub fn run() { main(); }\n}\n";
      RustDriver += "    eprintln!(\"" + Name + ": " + Case.Name +
                    (Partitioned ? " partitioned" : " monolithic") + "\"); " +
                    Name + "::run();\n";
    }
  }
  if (Backend == SourceBackend::Rust)
    compileAndRun(Backend, RustModules + RustDriver + "}\n");
}

TEST(SBFSourceDifferential, GeneratedCMatchesTheRawOracle) {
  runAllSourceCases(SourceBackend::C);
}

TEST(SBFSourceDifferential, GeneratedRustMatchesTheRawOracle) {
  runAllSourceCases(SourceBackend::Rust);
}

} // namespace
} // namespace neverd::sbf

//===- DevirtualizationSourceTests.cpp - Public VM execution checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <functional>
#include <map>

namespace {

using namespace neverd;

std::string readSource(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

va_t functionEntry(const BinaryImage &Image, const std::string &Name) {
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Name == Name && Symbol.IsFunc)
      return Symbol.Addr;
  return InvalidVA;
}

va_t dataAddress(const BinaryImage &Image, const std::string &Name) {
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Name == Name)
      return Symbol.Addr;
  return InvalidVA;
}

size_t count(const LowFunc &Function, NdOp Opcode) {
  size_t Result = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      Result += Op.Opcode == Opcode;
  return Result;
}

bool hasCycle(const LowFunc &Function) {
  std::map<int, const LowBlock *> Blocks;
  std::map<int, unsigned> Color;
  for (const auto &Block : Function.Blocks)
    Blocks[Block.Id] = &Block;
  std::function<bool(int)> Visit = [&](int Id) {
    if (Color[Id] == 1)
      return true;
    if (Color[Id] == 2)
      return false;
    Color[Id] = 1;
    const auto Found = Blocks.find(Id);
    if (Found != Blocks.end())
      for (int Succ : Found->second->Succs)
        if (Visit(Succ))
          return true;
    Color[Id] = 2;
    return false;
  };
  for (const auto &Block : Function.Blocks)
    if (Visit(Block.Id))
      return true;
  return false;
}

analysis::SpecializationOptions recoveryOptions(bool Finite = false) {
  analysis::SpecializationOptions Options;
  Options.ControlRegisters.push_back({x86reg::R10, 8});
  if (Finite)
    Options.ControlRegisters.push_back({x86reg::R9, 8});
  return Options;
}

class DevirtualizationSourceTest : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_SOURCE_DIR) / "fixtures" / Name;
  }

  RunResult buildFixture(const fs::path &Output, const char *Extra = nullptr) {
    std::vector<std::string> Args{"-target",
                                  "x86_64-linux-gnu",
                                  "-fuse-ld=lld",
                                  "-nostdlib",
                                  "-static",
                                  "-Wl,-e,generic_vm_register_arithmetic",
                                  fixture("generic_vm_register.S").string(),
                                  fixture("generic_vm_stack.S").string(),
                                  fixture("generic_vm_finite.S").string()};
    if (Extra)
      Args.push_back(fixture(Extra).string());
    Args.insert(Args.end(), {"-o", Output.string()});
    return exec(NEVERD_TEST_CLANG, Args);
  }
};

struct SourceCase {
  const char *Name;
  unsigned Kind;
  bool LLVM;
  bool Finite = false;
};

class DevirtualizationRoundTripTest
    : public DevirtualizationSourceTest,
      public ::testing::WithParamInterface<SourceCase> {};

TEST_F(DevirtualizationSourceTest,
       MultipleReturnsUseBackendSpecificWritebackLayouts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine-state source layout checks require clang";
  const auto Assembly = tmpFile("return-layout.S");
  const auto Binary = tmpFile("return-layout.elf");
  std::ofstream(Assembly) << R"(
.text
.globl return_layout
.type return_layout,@function
return_layout:
  cmpq $0, %rdi
  je .Lzero
  movl $17, %eax
  ret
.Lzero:
  movb $9, %ah
  ret
.size return_layout,.-return_layout
)";
  const auto Built =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-fuse-ld=lld",
                               "-nostdlib", "-static", "-Wl,-e,return_layout",
                               Assembly.string(), "-o", Binary.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Entry = functionEntry(*Image, "return_layout");
  ASSERT_NE(Entry, InvalidVA);
  for (unsigned Route = 0; Route != 3; ++Route) {
    SCOPED_TRACE(Route);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.InterpreterSpecialization.emplace();
    Options.InterpreterSpecialization->ExplicitMachineState = true;
    Options.OnlyFunctionEntries.insert(Entry);
    Options.LiftMode = Route == 1;
    Options.DumpLlvm = Route == 2;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.InterpreterRecovery.has_value());
    ASSERT_TRUE(Result.InterpreterRecovery->complete());
    EXPECT_EQ(count(Result.InterpreterRecovery->Residual, NdOp::RETURN), 2U);
    ASSERT_EQ(Result.LowFuncs.size(), 1U);
    EXPECT_EQ(count(Result.LowFuncs.front(), NdOp::RETURN), Route ? 2U : 1U);
    EXPECT_EQ(count(Result.LowFuncs.front(), NdOp::STORE), Route ? 34U : 18U);
  }
}

TEST_P(DevirtualizationRoundTripTest, RecoversMachineAndPreservesExecution) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM source recovery requires clang";
  const auto &[Name, Kind, LLVM, Finite] = GetParam();
  SCOPED_TRACE(Name);
  SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, Name);
  ASSERT_NE(Entry, InvalidVA);

  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.InterpreterSpecialization = recoveryOptions(Finite);
  Options.OnlyFunctionEntries.insert(Entry);
  Options.LiftMode = LLVM;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.MedIRVerifierFailures, 0u);
  ASSERT_FALSE(Result.LLVMVerifierFailed);
  ASSERT_EQ(Result.BackendUnhandledValueIntrinsics, 0u);
  ASSERT_TRUE(Result.InterpreterRecovery.has_value());
  const auto &Recovery = *Result.InterpreterRecovery;
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  EXPECT_FALSE(Recovery.Reads.empty());
  EXPECT_FALSE(Recovery.Origins.empty());
  if (Finite) {
    const va_t Bytecode = dataAddress(*Image, std::string(Name) + "_bytecode");
    ASSERT_NE(Bytecode, InvalidVA);
    // Every possible record must be backed by immutable evidence. This does
    // not prescribe whether joint controls use clones or one relational node.
    for (unsigned Lane = 0; Lane < (Kind == 0 ? 4u : 2u); ++Lane)
      EXPECT_TRUE(std::any_of(Recovery.Reads.begin(), Recovery.Reads.end(),
                              [&](const auto &Read) {
                                return Read.Address == Bytecode + Lane * 16 &&
                                       Read.Bytes.size() == 4;
                              }))
          << "Missing immutable witness for bytecode record " << Lane;
  }
  for (const auto &Block : Recovery.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::INDIR_BR);
      EXPECT_NE(Op.Opcode, NdOp::INDIR_CALL);
    }
  if (Kind == 2)
    EXPECT_TRUE(hasCycle(Recovery.Residual))
        << "A runtime-dependent loop must survive specialization";

  CEmitterOptions COptions;
  COptions.TheArch = Image->Arch;
  COptions.Format = Image->Format;
  COptions.Image = &*Image;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  if (LLVM) {
    ASSERT_NE(Result.LlvmModule, nullptr);
    ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions, nullptr,
                                    &*Image));
  } else {
    ASSERT_EQ(Result.HighFuncs.size(), 1u);
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
  }
  OS.flush();
  ASSERT_FALSE(Source.empty());
  const auto Harness = tmpFile("generic-vm-recovered.c");
  std::ofstream(Harness)
      << Source << "\n#define GENERIC_VM_SOURCE\n#define GENERIC_VM_FUNCTION "
      << Name << "\n#define GENERIC_VM_KIND " << Kind << "\n"
      << readSource(fixture(Finite ? "generic_vm_finite_reference.c"
                                   : "generic_vm_reference.c"));
  // The fixtures contain only scalar x64 operations.
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile(std::string("generic-vm-recovered") +
                                    neverd::test::executableSuffix());
    const auto Recompiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    PublicMachines, DevirtualizationRoundTripTest,
    ::testing::Values(SourceCase{"generic_vm_register_arithmetic", 0, false},
                      SourceCase{"generic_vm_register_arithmetic", 0, true},
                      SourceCase{"generic_vm_register_branch", 1, false},
                      SourceCase{"generic_vm_register_branch", 1, true},
                      SourceCase{"generic_vm_register_loop", 2, false},
                      SourceCase{"generic_vm_register_loop", 2, true},
                      SourceCase{"generic_vm_stack_arithmetic", 0, false},
                      SourceCase{"generic_vm_stack_arithmetic", 0, true},
                      SourceCase{"generic_vm_stack_branch", 1, false},
                      SourceCase{"generic_vm_stack_branch", 1, true},
                      SourceCase{"generic_vm_stack_loop", 2, false},
                      SourceCase{"generic_vm_stack_loop", 2, true},
                      SourceCase{"generic_vm_finite_arithmetic", 0, false,
                                 true},
                      SourceCase{"generic_vm_finite_arithmetic", 0, true, true},
                      SourceCase{"generic_vm_finite_branch", 1, false, true},
                      SourceCase{"generic_vm_finite_branch", 1, true, true},
                      SourceCase{"generic_vm_finite_loop", 2, false, true},
                      SourceCase{"generic_vm_finite_loop", 2, true, true}),
    [](const ::testing::TestParamInfo<SourceCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "_LLVMC" : "_HighC");
    });

TEST_F(DevirtualizationSourceTest,
       RuntimeFlagsRemainLiveAcrossRecoveredFiniteDispatch) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public x64 flag fixture requires clang";
  const auto Binary = tmpFile("generic-vm-flags.elf");
  const auto Compiled = buildFixture(Binary, "generic_vm_flags.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_flags");
  ASSERT_NE(Entry, InvalidVA);

  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.InterpreterSpecialization = recoveryOptions();
    Options.OnlyFunctionEntries.insert(Entry);
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.InterpreterRecovery.has_value());
    ASSERT_TRUE(Result.InterpreterRecovery->complete())
        << Result.InterpreterRecovery->Diagnostic;
    EXPECT_EQ(count(Result.InterpreterRecovery->Residual, NdOp::INTRINSIC), 2u);
    EXPECT_EQ(count(Result.InterpreterRecovery->Residual, NdOp::INDIR_BR), 0u);
    EXPECT_EQ(Result.BackendUnhandledValueIntrinsics, 0u);

    CEmitterOptions COptions;
    COptions.TheArch = Image->Arch;
    COptions.Format = Image->Format;
    COptions.Image = &*Image;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    if (LLVM) {
      ASSERT_NE(Result.LlvmModule, nullptr);
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions, nullptr,
                                      &*Image));
    } else {
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
    }
    OS.flush();
    const auto Harness = tmpFile("generic-vm-flags-recovered.c");
    {
      std::ofstream Output(Harness);
      Output << "#include <x86intrin.h>\n"
             << Source << R"(
#include <stdint.h>
int main(void) {
  const uint64_t values[] = {0, 1, 42, UINT64_MAX - 42, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if ((uint64_t)generic_vm_flags(values[i]) != values[i] + 42)
      return 1;
  return 0;
}
)";
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Executable =
          tmpFile(std::string("generic-vm-flags-recovered") +
                  neverd::test::executableSuffix());
      const auto Recompiled =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Optimization, "-Werror=return-type",
                "-Werror=implicit-function-declaration", "-fsanitize=undefined",
                "-fsanitize-trap=undefined", "-I", tmp().string(),
                Harness.string(), "-o", Executable.string()});
      ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
      const auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
    }
  }
}

TEST_F(DevirtualizationSourceTest, RefusesUncertifiedControl) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM negative fixtures require clang";
  const auto Binary = tmpFile("generic-vm-negative.elf");
  const auto Compiled = buildFixture(Binary, "generic_vm_negative.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  using Status = analysis::SpecializationStatus;
  const std::pair<const char *, Status> Cases[] = {
      {"generic_vm_unresolved_dispatch", Status::UnresolvedControl},
      {"generic_vm_writable_dispatch", Status::UnresolvedControl},
      {"generic_vm_return_dispatch", Status::Unsupported},
      {"generic_vm_callee_pop", Status::Unsupported},
      {"generic_vm_direct_call", Status::Unsupported},
      {"generic_vm_indirect_call", Status::Unsupported},
      {"generic_vm_unbound_flags", Status::Unsupported},
      {"generic_vm_far_return", Status::Unsupported},
      {"generic_vm_interrupt_return", Status::Unsupported},
      {"generic_vm_system_return", Status::Unsupported}};
  for (const auto &[Name, ExpectedStatus] : Cases) {
    SCOPED_TRACE(Name);
    const va_t Entry = functionEntry(*Image, Name);
    ASSERT_NE(Entry, InvalidVA);
    auto Recovery =
        analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
    EXPECT_FALSE(Recovery.complete());
    EXPECT_EQ(Recovery.Status, ExpectedStatus) << Recovery.Diagnostic;
    EXPECT_TRUE(Recovery.Residual.Blocks.empty());
    EXPECT_FALSE(Recovery.Diagnostic.empty());
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.InterpreterSpecialization = recoveryOptions();
    Options.OnlyFunctionEntries.insert(Entry);
    Options.EmitDumpOutput = false;
    const auto Result = Pipeline().run(*Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_TRUE(Result.LowFuncs.empty());
    EXPECT_TRUE(Result.MedFuncs.empty());
    EXPECT_TRUE(Result.HighFuncs.empty());
    EXPECT_EQ(Result.LlvmModule, nullptr);
  }
}

TEST_F(DevirtualizationSourceTest, RefusesNonNativeByteOrder) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM image contracts require clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_register_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  auto Options = recoveryOptions();
  Options.ByteOrder = llvm::endianness::big;
  const auto Result =
      analysis::specializeBinaryInterpreter(*Image, Entry, Options);
  EXPECT_EQ(Result.Status, analysis::SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_FALSE(Result.Diagnostic.empty());
}

TEST_F(DevirtualizationSourceTest,
       FiniteReadsRequireEveryCandidateCertificate) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public finite-read image contracts require clang";
  const auto Binary = tmpFile("generic-vm-finite.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool Writable : {false, true}) {
    SCOPED_TRACE(Writable ? "writable mapping" : "one missing candidate");
    auto Image = loadBinary(Binary);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const va_t Entry = functionEntry(*Image, "generic_vm_finite_arithmetic");
    const va_t Bytecode =
        dataAddress(*Image, "generic_vm_finite_arithmetic_bytecode");
    ASSERT_NE(Entry, InvalidVA);
    ASSERT_NE(Bytecode, InvalidVA);
    bool Altered = false;
    for (auto &Segment : Image->Segments)
      if (Segment.contains(Bytecode)) {
        ASSERT_FALSE(Segment.isWritable());
        if (Writable)
          Segment.Flags = Segment.Flags | SegmentFlags::Writable;
        else {
          // The first three records remain file-backed; the fourth does not.
          const auto RetainedBytes = Bytecode - Segment.VA + 3 * 16;
          ASSERT_LT(RetainedBytes, Segment.Data.size());
          Segment.Data.resize(RetainedBytes);
          Segment.FileSz = RetainedBytes;
        }
        Altered = true;
      }
    ASSERT_TRUE(Altered);
    const auto Result = analysis::specializeBinaryInterpreter(
        *Image, Entry, recoveryOptions(true));
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Reads.empty());
    EXPECT_FALSE(Result.Diagnostic.empty());
  }
}

TEST_F(DevirtualizationSourceTest, FiniteReadBudgetsNeverPublishPartialModels) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public finite-read budget contracts require clang";
  const auto Binary = tmpFile("generic-vm-finite.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_finite_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  for (bool QueryBudget : {false, true}) {
    SCOPED_TRACE(QueryBudget ? "solver query budget" : "read address budget");
    auto Options = recoveryOptions(true);
    if (QueryBudget)
      Options.MaxSolverQueries = 1;
    else
      Options.MaxImmutableReadAddresses = 1;
    const auto Result =
        analysis::specializeBinaryInterpreter(*Image, Entry, Options);
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Reads.empty());
    EXPECT_FALSE(Result.Diagnostic.empty());
    if (QueryBudget) {
      EXPECT_EQ(Result.Status, analysis::SpecializationStatus::BudgetExceeded);
      EXPECT_LE(Result.SolverQueries, Options.MaxSolverQueries);
    }
  }
}

TEST_F(DevirtualizationSourceTest, RefusesUndecodedCoveringExceptionHandler) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM image contracts require clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_register_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  ExceptionFunction Parent;
  Parent.CodeRange = {Entry, Entry + 0x1000};
  Parent.PersonalityVA = Entry + 0x800;
  ASSERT_FALSE(Parent.hasLanguageTable());
  ExceptionFunction Child;
  Child.CodeRange = {Entry, Entry + 0x400};
  // The most specific range contains no decoded table. Its parent still has
  // a handler, so finding only the smallest record is insufficient.
  Image->ExceptionMetadata.Functions = {Parent, Child};
  Image->ExceptionMetadata.rebuildIndex();
  ASSERT_EQ(Image->ExceptionMetadata.findFunction(Entry)->PersonalityVA, 0u);
  const auto Result =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_EQ(Result.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_FALSE(Result.Diagnostic.empty());

  // The first x64 prologue instruction spans multiple bytes. A malformed
  // runtime-function boundary inside it must not escape the overlap check.
  Parent.CodeRange = {Entry + 1, Entry + 2};
  Image->ExceptionMetadata.Functions = {Parent};
  Image->ExceptionMetadata.rebuildIndex();
  const auto Interior =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_EQ(Interior.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Interior.Residual.Blocks.empty());
}

TEST_F(DevirtualizationSourceTest, RefusesIncompleteExceptionMetadata) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM image contracts require clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_vm_register_arithmetic");
  ASSERT_NE(Entry, InvalidVA);
  for (const auto Status :
       {ExceptionParseStatus::Partial, ExceptionParseStatus::Malformed}) {
    Image->ExceptionMetadata.ParseStatus = Status;
    const auto Result =
        analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
    EXPECT_EQ(Result.Status, analysis::SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_FALSE(Result.Diagnostic.empty());
  }
}

TEST_F(DevirtualizationSourceTest, CLIEmitsSourceAndEvidenceOnlyOnSuccess) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM CLI recovery requires clang";
  const auto Binary = tmpFile("generic-vm.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  const auto Source = tmpFile("generic-vm.c");
  const auto Report = tmpFile("generic-vm.json");
  const auto Recovered =
      exec(ndBin(), {"decompile", "--func", "generic_vm_register_arithmetic",
                     "--devirtualize", "--vm-control=r10",
                     "--recovery-report=" + Report.string(), "-o",
                     Source.string(), Binary.string()});
  ASSERT_TRUE(Recovered.ok()) << Recovered.err;
  EXPECT_FALSE(readSource(Source).empty());
  auto JSON = llvm::json::parse(readSource(Report));
  ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getBoolean("complete"), true);
  EXPECT_EQ(Object->getBoolean("controlComplete"), true);
  EXPECT_EQ(Object->getInteger("maxControlRefinements"), 16);
  for (const char *Key :
       {"maxNodes", "maxContextsPerAddress", "maxOperations",
        "maxNodeEvaluations", "maxIndirectTargets", "maxImmutableReadAddresses",
        "maxControlTuples", "maxControlFields", "maxSolverQueries",
        "maxSolverGates", "maxSolverConflicts", "maxSolverPropagations",
        "maxSolverWatchVisits", "maxSymbolicNodes"}) {
    SCOPED_TRACE(Key);
    const auto Value = Object->getInteger(Key);
    ASSERT_TRUE(Value.has_value());
    EXPECT_GT(*Value, 0);
  }
  for (const char *Key : {"solverQueries", "relationalWidenings"}) {
    SCOPED_TRACE(Key);
    const auto Value = Object->getInteger(Key);
    ASSERT_TRUE(Value.has_value());
    EXPECT_GE(*Value, 0);
  }
  const auto *Controls = Object->getArray("controlRegisters");
  ASSERT_NE(Controls, nullptr);
  ASSERT_EQ(Controls->size(), 1u);
  const auto *Control = Controls->front().getAsObject();
  ASSERT_NE(Control, nullptr);
  EXPECT_EQ(Control->getInteger("offset"), x86reg::R10);
  EXPECT_EQ(Control->getInteger("bytes"), 8);

  const auto InvalidSource = tmpFile("invalid.c");
  const auto InvalidReport = tmpFile("invalid.json");
  const auto Refused =
      exec(ndBin(), {"decompile", "--func", "generic_vm_register_arithmetic",
                     "--devirtualize", "--vm-control=bogus",
                     "--recovery-report=" + InvalidReport.string(), "-o",
                     InvalidSource.string(), Binary.string()});
  EXPECT_FALSE(Refused.ok());
  EXPECT_FALSE(fs::exists(InvalidSource));
  auto InvalidJSON = llvm::json::parse(readSource(InvalidReport));
  ASSERT_TRUE(static_cast<bool>(InvalidJSON))
      << llvm::toString(InvalidJSON.takeError());
  ASSERT_NE(InvalidJSON->getAsObject(), nullptr);
  EXPECT_EQ(InvalidJSON->getAsObject()->getBoolean("complete"), false);
}

TEST_F(DevirtualizationSourceTest, CLIRefinementBudgetControlsBothSourceABIs) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery budget checks require clang";
  const auto Binary = tmpFile("generic-refinement-budget.elf");
  const auto Compiled = buildFixture(Binary, "generic_control_state.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool MachineState : {false, true})
    for (bool LLVM : {false, true})
      for (unsigned Limit : {1u, 2u}) {
        SCOPED_TRACE(MachineState ? "machine-state" : "ordinary-source");
        SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
        SCOPED_TRACE(Limit);
        const auto Stem = std::to_string(MachineState) + "-" +
                          std::to_string(LLVM) + "-" + std::to_string(Limit);
        const auto Source = tmpFile(Stem + ".c");
        const auto Report = tmpFile(Stem + ".json");
        std::vector<std::string> Args{"decompile",
                                      Binary.string(),
                                      "--func",
                                      "generic_control_state",
                                      "--devirtualize",
                                      "--vm-max-refinements=" +
                                          std::to_string(Limit),
                                      "--recovery-report=" + Report.string(),
                                      "-o",
                                      Source.string()};
        if (MachineState)
          Args.push_back("--vm-machine-state");
        if (LLVM)
          Args.push_back("--llvm");
        const auto Recovered = exec(ndBin(), Args);
        const bool Complete = Limit == 2;
        EXPECT_EQ(Recovered.ok(), Complete) << Recovered.err;
        EXPECT_EQ(fs::exists(Source), Complete);
        auto JSON = llvm::json::parse(readSource(Report));
        ASSERT_TRUE(static_cast<bool>(JSON))
            << llvm::toString(JSON.takeError());
        const auto *Object = JSON->getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getBoolean("complete"), Complete);
        EXPECT_EQ(Object->getBoolean("controlComplete"), Complete);
        EXPECT_EQ(Object->getInteger("maxControlRefinements"), Limit);
        EXPECT_EQ(Object->getInteger("controlRefinements"), Limit);
        for (const char *Key : {"controlRegisters", "controlFrameSlots"}) {
          const auto *Fields = Object->getArray(Key);
          ASSERT_NE(Fields, nullptr);
          EXPECT_TRUE(Fields->empty());
        }
        if (Complete)
          EXPECT_FALSE(readSource(Source).empty());
      }
}

TEST_F(DevirtualizationSourceTest, CLIFieldAndQueryBudgetsAreEnforced) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery budget checks require clang";
  const auto Binary = tmpFile("generic-field-query-budgets.elf");
  const auto Compiled = buildFixture(Binary, "generic_control_state.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool Machine : {false, true})
    for (bool LLVM : {false, true})
      for (unsigned Limited = 0; Limited < 3; ++Limited) {
        SCOPED_TRACE(Machine);
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Limited);
        const unsigned Fields = Limited == 1 ? 1 : 2;
        const unsigned Queries = Limited == 2 ? 1 : 4096;
        const auto Stem = std::to_string(Machine) + "-" + std::to_string(LLVM) +
                          "-" + std::to_string(Limited);
        const auto Source = tmpFile(Stem + ".c");
        const auto Report = tmpFile(Stem + ".json");
        std::vector<std::string> Args{
            "decompile",
            Binary.string(),
            "--func",
            "generic_control_state",
            "--devirtualize",
            "--vm-max-fields=" + std::to_string(Fields),
            "--vm-max-queries=" + std::to_string(Queries),
            "--recovery-report=" + Report.string(),
            "-o",
            Source.string()};
        if (Machine)
          Args.push_back("--vm-machine-state");
        if (LLVM)
          Args.push_back("--llvm");
        const auto Recovered = exec(ndBin(), Args);
        const bool Complete = Limited == 0;
        EXPECT_EQ(Recovered.ok(), Complete) << Recovered.err;
        EXPECT_EQ(fs::exists(Source), Complete);
        auto JSON = llvm::json::parse(readSource(Report));
        ASSERT_TRUE(static_cast<bool>(JSON))
            << llvm::toString(JSON.takeError());
        const auto *Object = JSON->getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getBoolean("complete"), Complete);
        EXPECT_EQ(Object->getBoolean("controlComplete"), Complete);
        EXPECT_EQ(Object->getInteger("maxControlFields"), Fields);
        EXPECT_EQ(Object->getInteger("maxSolverQueries"), Queries);
        EXPECT_EQ(Object->getInteger("maxControlRefinements"), 16);
        for (const char *Key : {"controlRegisters", "controlFrameSlots"}) {
          const auto *Controls = Object->getArray(Key);
          ASSERT_NE(Controls, nullptr);
          EXPECT_TRUE(Controls->empty());
        }
        if (!Complete) {
          EXPECT_EQ(Object->getString("status"), "budget-exceeded");
          EXPECT_EQ(Object->getInteger("residualBlocks"), 0);
        }
      }
}

TEST_F(DevirtualizationSourceTest, CLIWorkBudgetsAreEnforced) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery budget checks require clang";
  const auto Binary = tmpFile("generic-work-budgets.elf");
  const auto Compiled = buildFixture(Binary, "generic_control_state.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool Machine : {false, true})
    for (bool LLVM : {false, true})
      for (unsigned Case = 0; Case < 5; ++Case) {
        SCOPED_TRACE(Machine);
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Case);
        const uint32_t Evaluations = Case == 1   ? 1
                                     : Case == 3 ? UINT32_MAX
                                                 : 16384;
        const uint32_t Visits = Case == 2 ? 1 : Case == 3 ? UINT32_MAX : 65536;
        const auto Stem = std::to_string(Machine) + "-" + std::to_string(LLVM) +
                          "-" + std::to_string(Case);
        const auto Source = tmpFile("work-" + Stem + ".c");
        const auto Report = tmpFile("work-" + Stem + ".json");
        std::vector<std::string> Args{"decompile",
                                      Binary.string(),
                                      "--func",
                                      "generic_control_state",
                                      "--devirtualize",
                                      "--recovery-report=" + Report.string(),
                                      "-o",
                                      Source.string()};
        if (Case) {
          Args.push_back("--vm-max-evaluations=" + std::to_string(Evaluations));
          Args.push_back("--vm-max-discovery-visits=" + std::to_string(Visits));
        }
        if (Case >= 3)
          Args.push_back("--vm-chain-stop-at-repeat");
        if (Case == 3)
          Args.push_back("--vm-chain-transfers=8");
        if (Machine)
          Args.push_back("--vm-machine-state");
        if (LLVM)
          Args.push_back("--llvm");
        const auto Recovered = exec(ndBin(), Args);
        const bool Complete = Case == 0 || Case >= 3;
        EXPECT_EQ(Recovered.ok(), Complete) << Recovered.err;
        EXPECT_EQ(fs::exists(Source), Complete);
        auto JSON = llvm::json::parse(readSource(Report));
        ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
        const auto *Object = JSON->getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getBoolean("complete"), Complete);
        EXPECT_EQ(Object->getInteger("maxNodeEvaluations"),
                  int64_t(Evaluations));
        EXPECT_EQ(Object->getInteger("maxDiscoveryVisits"), int64_t(Visits));
        EXPECT_EQ(Object->getBoolean("stopChainingAtRepeatedDestination"),
                  Case >= 3);
        if (!Complete) {
          EXPECT_EQ(Object->getString("status"), "budget-exceeded");
          EXPECT_EQ(Object->getInteger("residualBlocks"), 0);
          for (const char *Key : {"origins", "immutableReads"}) {
            const auto *Evidence = Object->getArray(Key);
            ASSERT_NE(Evidence, nullptr);
            EXPECT_TRUE(Evidence->empty());
          }
        }
      }
  const auto Source = tmpFile("repeat-without-recovery.c");
  const auto Refused = exec(
      ndBin(), {"decompile", Binary.string(), "--func", "generic_control_state",
                "--vm-chain-stop-at-repeat", "-o", Source.string()});
  EXPECT_FALSE(Refused.ok());
  EXPECT_NE(Refused.err.find("require --devirtualize"), std::string::npos);
  EXPECT_FALSE(fs::exists(Source));
}

TEST_F(DevirtualizationSourceTest, CLIRejectsInvalidDiscoveryBudgets) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery budget checks require clang";
  const auto Binary = tmpFile("generic-refinement-options.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  const auto Source = tmpFile("invalid-refinement-options.c");
  for (const char *Option :
       {"--vm-max-refinements", "--vm-max-fields", "--vm-max-queries",
        "--vm-max-evaluations", "--vm-max-discovery-visits"}) {
    SCOPED_TRACE(Option);
    for (const char *Value : {"0", "-1", "4294967296", "1junk", "+1", "0x10"}) {
      SCOPED_TRACE(Value);
      const auto Refused = exec(
          ndBin(), {"decompile", Binary.string(), "--func",
                    "generic_vm_register_arithmetic", "--devirtualize",
                    std::string(Option) + "=" + Value, "-o", Source.string()});
      EXPECT_FALSE(Refused.ok());
      EXPECT_FALSE(fs::exists(Source));
    }
    const auto WithoutRecovery =
        exec(ndBin(), {"decompile", Binary.string(), "--func",
                       "generic_vm_register_arithmetic",
                       std::string(Option) + "=2", "-o", Source.string()});
    EXPECT_FALSE(WithoutRecovery.ok());
    EXPECT_FALSE(fs::exists(Source));
  }
}

TEST_F(DevirtualizationSourceTest,
       CLITransferChainAndEntryBoundsAffectRecovery) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery contract checks require clang";
  const auto Binary = tmpFile("generic-recovery-contract.elf");
  const auto Compiled = buildFixture(Binary, "generic_recovery_contract.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool Bounds : {false, true})
    for (bool Machine : {false, true}) {
      if (Bounds && !Machine)
        continue;
      for (bool LLVM : {false, true})
        for (bool Enabled : {false, true}) {
          SCOPED_TRACE(Bounds);
          SCOPED_TRACE(Machine);
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(Enabled);
          const std::string Name =
              Bounds ? "generic_recovery_bounds" : "generic_recovery_chain";
          const auto Stem = std::to_string(Bounds) + std::to_string(Machine) +
                            std::to_string(LLVM) + std::to_string(Enabled);
          const auto Source = tmpFile(Stem + ".c");
          const auto Report = tmpFile(Stem + ".json");
          std::vector<std::string> Args{"decompile",
                                        Binary.string(),
                                        "--func",
                                        Name,
                                        "--devirtualize",
                                        "--vm-no-control-discovery",
                                        std::string("--vm-chain-transfers=") +
                                            ((Bounds || Enabled) ? "4" : "0"),
                                        "--recovery-report=" + Report.string(),
                                        "-o",
                                        Source.string()};
          if (Bounds && Enabled)
            Args.push_back("--vm-entry-frame=-96:8");
          if (Machine)
            Args.push_back("--vm-machine-state");
          if (LLVM)
            Args.push_back("--llvm");
          const auto Recovered = exec(ndBin(), Args);
          EXPECT_EQ(Recovered.ok(), Enabled) << Recovered.err;
          EXPECT_EQ(fs::exists(Source), Enabled);
          auto Parsed = llvm::json::parse(readSource(Report));
          ASSERT_TRUE(static_cast<bool>(Parsed))
              << llvm::toString(Parsed.takeError());
          const auto *Object = Parsed->getAsObject();
          ASSERT_NE(Object, nullptr);
          EXPECT_EQ(Object->getBoolean("complete"), Enabled);
          EXPECT_EQ(Object->getInteger("maxChainedTransfers"),
                    (Bounds || Enabled) ? 4 : 0);
          EXPECT_EQ(Object->getBoolean("discoverControlState"), false);
          if (!Enabled)
            continue;
          const auto Text = readSource(Source);
          if (Bounds) {
            const auto *Range = Object->getObject("entryFrameBounds");
            ASSERT_NE(Range, nullptr);
            EXPECT_EQ(Range->getInteger("begin"), -96);
            EXPECT_EQ(Range->getInteger("end"), 8);
            EXPECT_NE(Text.find("[-96,8)"), std::string::npos);
            EXPECT_NE(Text.find("not checked at runtime"), std::string::npos);
          }
          const auto Harness = tmpFile(Stem + "-run.c");
          std::ofstream OS(Harness);
          OS << Text << R"C(
#include <stdint.h>
int main(void) {
  const uint64_t roots[] = {96, 159, 160, UINT64_C(1) << 63, UINT64_MAX - 7};
  const uint64_t inputs[] = {0, 1, UINT64_MAX - 16, UINT64_MAX};
  for (unsigned r = 0; r != 5; ++r)
    for (unsigned i = 0; i != 4; ++i) {
      uint64_t x = inputs[i];
)C";
          if (Machine)
            OS << "      uint64_t state[17] = {0};\n"
                  "      state[4] = roots[r]; state[7] = x; state[16] = 2;\n"
                  "      if ("
               << Name
               << "((uint8_t *)state) != 0 ||\n"
                  "          state[0] != x + 17 || state[4] != roots[r]) "
                  "return 1;\n";
          else
            OS << "      if ((uint64_t)" << Name
               << "(x) != x + 17) return 1;\n";
          OS << "    }\n  return 0;\n}\n";
          OS.close();
          std::ofstream(tmpFile("immintrin.h")).close();
          for (const char *Optimization : {"-O0", "-O2"}) {
            const auto Program =
                tmpFile(Stem + "-run" + neverd::test::executableSuffix());
            const auto Built = exec(
                NEVERD_TEST_CLANG,
                {"-std=c11", Optimization, "-Werror=return-type",
                 "-Werror=implicit-function-declaration",
                 "-fsanitize=undefined", "-fsanitize-trap=undefined", "-I",
                 tmp().string(), Harness.string(), "-o", Program.string()});
            ASSERT_TRUE(Built.ok()) << Built.err;
            const auto Ran = exec(Program.string(), {});
            EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
          }
        }
    }
}

TEST_F(DevirtualizationSourceTest, CLIDiscoveryCanBeExplicitlyDisabled) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public discovery option checks require clang";
  const auto Binary = tmpFile("generic-discovery-switch.elf");
  const auto Compiled = buildFixture(Binary, "generic_control_state.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool Machine : {false, true})
    for (bool LLVM : {false, true})
      for (bool Discovery : {false, true}) {
        const auto Stem = std::to_string(Machine) + std::to_string(LLVM) +
                          std::to_string(Discovery);
        const auto Source = tmpFile(Stem + ".c");
        const auto Report = tmpFile(Stem + ".json");
        std::vector<std::string> Args{"decompile",
                                      Binary.string(),
                                      "--func",
                                      "generic_control_state",
                                      "--devirtualize",
                                      "--recovery-report=" + Report.string(),
                                      "-o",
                                      Source.string()};
        if (!Discovery)
          Args.push_back("--vm-no-control-discovery");
        if (Machine)
          Args.push_back("--vm-machine-state");
        if (LLVM)
          Args.push_back("--llvm");
        const auto Recovered = exec(ndBin(), Args);
        EXPECT_EQ(Recovered.ok(), Discovery) << Recovered.err;
        EXPECT_EQ(fs::exists(Source), Discovery);
        auto Parsed = llvm::json::parse(readSource(Report));
        ASSERT_TRUE(static_cast<bool>(Parsed));
        const auto *Object = Parsed->getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getBoolean("complete"), Discovery);
        EXPECT_EQ(Object->getBoolean("discoverControlState"), Discovery);
        if (!Discovery) {
          EXPECT_EQ(Object->getString("status"), "unresolved-control");
          EXPECT_EQ(Object->getInteger("discoveryVisits"), 0);
        }
      }
}

TEST_F(DevirtualizationSourceTest, CLIRejectsMalformedEntryAndChainOptions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery option parsing requires clang";
  const auto Binary = tmpFile("generic-contract-options.elf");
  const auto Compiled = buildFixture(Binary);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  const auto Source = tmpFile("invalid-contract-options.c");
  const auto Reject = [&](const std::vector<std::string> &Options) {
    std::vector<std::string> Args{"decompile", Binary.string(),
                                  "--func",    "generic_vm_register_arithmetic",
                                  "-o",        Source.string()};
    Args.insert(Args.end(), Options.begin(), Options.end());
    const auto Result = exec(ndBin(), Args);
    EXPECT_FALSE(Result.ok());
    EXPECT_FALSE(fs::exists(Source));
  };
  for (const char *Value : {"", "-1", "4294967296", "1junk", "+1", "0x10"}) {
    Reject({"--devirtualize", std::string("--vm-chain-transfers=") + Value});
    Reject({"--devirtualize", std::string("--vm-chain-visits=") + Value});
  }
  for (const char *Value :
       {"", "-16", "-16:", ":8", "8:8", "8:7", "+1:8", "0x10:32", "1:8:9",
        "-9223372036854775809:8", "-16:9223372036854775808"})
    Reject({"--devirtualize", "--vm-machine-state",
            std::string("--vm-entry-frame=") + Value});
  for (const char *Option :
       {"--vm-chain-transfers=0", "--vm-chain-visits=0",
        "--vm-entry-frame=-16:8", "--vm-no-control-discovery",
        "--vm-no-control-discovery=false"})
    Reject({Option});
  Reject({"--devirtualize", "--vm-entry-frame=-16:8"});
  for (const char *Value : {"", "0:0", "3:0", "8:8", "8:-1", "8:1:2",
                            "4294967296:0", "16:", ":0", "+16:0", "0x10:0"})
    Reject({"--devirtualize", "--vm-machine-state",
            std::string("--vm-entry-alignment=") + Value});
  Reject({"--vm-entry-alignment=16:3"});
  Reject({"--devirtualize", "--vm-entry-alignment=16:3"});
  for (const char *Value : {"0", "-1", "+1", "0x10", "4294967296", "1junk"})
    Reject({"--devirtualize", std::string("--vm-max-symbolic-nodes=") + Value});
  Reject({"--vm-max-symbolic-nodes=262144"});
  Reject({"--vm-external-stores-disjoint-frame"});
  Reject({"--vm-external-stores-disjoint-frame=false"});
  Reject({"--devirtualize", "--vm-external-stores-disjoint-frame"});
  Reject({"--devirtualize", "--vm-machine-state",
          "--vm-external-stores-disjoint-frame"});
}

TEST_F(DevirtualizationSourceTest, CLIDestinationVisitsKeepRepeatedRelations) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery chain checks require clang";
  const auto Binary = tmpFile("generic-destination-visits.elf");
  ASSERT_TRUE(buildFixture(Binary, "generic_recovery_contract.S").ok());
  for (bool Machine : {false, true})
    for (bool LLVM : {false, true})
      for (unsigned Case = 0; Case != 6; ++Case) {
        const uint32_t Visits = Case == 0   ? 0
                                : Case == 1 ? 1
                                : Case == 3 ? UINT32_MAX
                                            : 2;
        const bool Legacy = Case == 4;
        const bool Chain = Case != 5;
        const bool Complete = Chain && !Legacy && Visits != 1;
        const auto Stem = std::to_string(Machine) + std::to_string(LLVM) +
                          std::to_string(Case);
        SCOPED_TRACE(Stem);
        const auto Source = tmpFile("visits-" + Stem + ".c");
        const auto Report = tmpFile("visits-" + Stem + ".json");
        std::vector<std::string> Args{
            "decompile",
            Binary.string(),
            "--func",
            "generic_recovery_repeat",
            "--devirtualize",
            "--vm-no-control-discovery",
            std::string("--vm-chain-transfers=") + (Chain ? "16" : "0"),
            "--vm-chain-visits=" + std::to_string(Visits),
            "--recovery-report=" + Report.string(),
            "-o",
            Source.string()};
        if (Legacy)
          Args.push_back("--vm-chain-stop-at-repeat");
        if (Machine)
          Args.push_back("--vm-machine-state");
        if (LLVM)
          Args.push_back("--llvm");
        const auto Recovered = exec(ndBin(), Args);
        EXPECT_EQ(Recovered.ok(), Complete) << Recovered.err;
        EXPECT_EQ(fs::exists(Source), Complete);
        auto JSON = llvm::json::parse(readSource(Report));
        ASSERT_TRUE(bool(JSON));
        const auto *Object = JSON->getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getBoolean("complete"), Complete);
        EXPECT_EQ(Object->getInteger("maxChainedVisitsPerDestination"), Visits);
        EXPECT_EQ(Object->getInteger("effectiveChainedVisitsPerDestination"),
                  Chain ? Legacy ? 1 : Visits : 0);
        if (!Complete) {
          EXPECT_EQ(Object->getInteger("residualBlocks"), 0);
          continue;
        }
        const auto Harness = tmpFile("visits-" + Stem + "-run.c");
        std::ofstream OS(Harness);
        OS << readSource(Source) << R"C(
#include <stdint.h>
int main(void) {
  uint64_t x = UINT64_MAX;
  for (unsigned i = 0; i != 1024; ++i) {
)C";
        if (Machine)
          OS << "    uint64_t state[17] = {0};\n"
                "    state[4] = 0x10000; state[7] = x; state[16] = 2;\n"
                "    if (generic_recovery_repeat((uint8_t *)state) != 0 ||\n"
                "        state[0] != x + 17 || state[4] != 0x10000) return "
                "1;\n";
        else
          OS << "    if ((uint64_t)generic_recovery_repeat(x) != x + 17) "
                "return 1;\n";
        OS << "    x = x * UINT64_C(6364136223846793005) + 1;\n"
              "  }\n  return 0;\n}\n";
        OS.close();
        std::ofstream(tmpFile("immintrin.h")).close();
        for (const char *Level : {"-O0", "-O2"}) {
          const auto Program = tmpFile("visits-" + Stem + "-run" +
                                       neverd::test::executableSuffix());
          const auto Built =
              exec(NEVERD_TEST_CLANG,
                   {"-std=c11", Level, "-Werror=return-type",
                    "-Werror=implicit-function-declaration",
                    "-fsanitize=undefined", "-fsanitize-trap=undefined", "-I",
                    tmp().string(), Harness.string(), "-o", Program.string()});
          ASSERT_TRUE(Built.ok()) << Built.err;
          const auto Ran = exec(Program.string(), {});
          EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
        }
      }
}

TEST_F(DevirtualizationSourceTest,
       CLIKeepsSignedMinBoundsAndLargestChainCount) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery option limits require clang";
  const auto Binary = tmpFile("generic-contract-extrema.elf");
  const auto Compiled = buildFixture(Binary, "generic_recovery_contract.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool LLVM : {false, true}) {
    const auto Source = tmpFile(LLVM ? "extrema-llvm.c" : "extrema-high.c");
    const auto Report =
        tmpFile(LLVM ? "extrema-llvm.json" : "extrema-high.json");
    std::vector<std::string> Args{"decompile",
                                  Binary.string(),
                                  "--func",
                                  "generic_recovery_bounds",
                                  "--devirtualize",
                                  "--vm-machine-state",
                                  "--vm-no-control-discovery",
                                  "--vm-chain-transfers=4294967295",
                                  "--vm-entry-frame=-9223372036854775808:8",
                                  "--recovery-report=" + Report.string(),
                                  "-o",
                                  Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Result = exec(ndBin(), Args);
    ASSERT_TRUE(Result.ok()) << Result.err;
    auto Parsed = llvm::json::parse(readSource(Report));
    ASSERT_TRUE(static_cast<bool>(Parsed));
    const auto *Object = Parsed->getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getInteger("maxChainedTransfers"), UINT32_MAX);
    const auto *Bounds = Object->getObject("entryFrameBounds");
    ASSERT_NE(Bounds, nullptr);
    EXPECT_EQ(Bounds->getInteger("begin"), INT64_MIN);
    EXPECT_NE(readSource(Source).find("[-9223372036854775808,8)"),
              std::string::npos);
  }
}

TEST_F(DevirtualizationSourceTest, CLIStoreSeparationKeepsEffectsAndReturn) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery execution requires clang";
  const auto Binary = tmpFile("generic-store-separation.elf");
  const auto Compiled = buildFixture(Binary, "generic_recovery_contract.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool LLVM : {false, true}) {
    const auto Source =
        tmpFile(LLVM ? "separation-llvm.c" : "separation-high.c");
    const auto Report = tmpFile("separation.json");
    std::vector<std::string> Args{"decompile",
                                  Binary.string(),
                                  "--func",
                                  "generic_recovery_external_store",
                                  "--devirtualize",
                                  "--vm-machine-state",
                                  "--vm-chain-transfers=64",
                                  "--vm-entry-frame=-8:8",
                                  "--recovery-report=" + Report.string(),
                                  "-o",
                                  Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    EXPECT_FALSE(exec(ndBin(), Args).ok());
    EXPECT_FALSE(fs::exists(Source));
    Args.push_back("--vm-external-stores-disjoint-frame");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    auto Parsed = llvm::json::parse(readSource(Report));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(
        Parsed->getAsObject()->getBoolean("externalStoresDisjointEntryFrame"),
        true);
    EXPECT_NE(readSource(Source).find("Unchecked external-STORE precondition"),
              std::string::npos);
    const auto Harness = tmpFile("separation-runtime.c");
    std::ofstream(Harness) << readSource(Source) << R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  _Alignas(32) uint8_t frames[2][256];
  uint64_t buffer[3], state[17], before[17];
  for (unsigned f = 0; f < 2; ++f) {
    for (unsigned test = 0; test < 32; ++test) {
      memset(frames, 0x5a, sizeof frames);
      buffer[0] = 31; buffer[1] = 0; buffer[2] = 71;
      for (unsigned i = 0; i < 17; ++i) state[i] = 300 + i + test;
      state[1] = (uint64_t)(uintptr_t)&buffer[1];
      state[2] = UINT64_MAX - test * UINT64_C(0x123456781);
      state[4] = (uint64_t)(uintptr_t)(frames[f] + 128);
      state[16] = 2;
      memcpy(before, state, sizeof state);
      if (generic_recovery_external_store((uint8_t *)state)) return 1;
      if (state[0] != before[2] || buffer[1] != before[2]) return 2;
      for (unsigned i = 1; i < 17; ++i) if (state[i] != before[i]) return 3;
      if (buffer[0] != 31 || buffer[2] != 71) return 4;
      for (unsigned frame = 0; frame < 2; ++frame)
        for (unsigned i = 0; i < 256; ++i)
          if ((frame != f || i < 120 || i >= 128) && frames[frame][i] != 0x5a)
            return 5;
    }
  }
  return 0;
}
)C";
    std::ofstream(tmpFile("immintrin.h")).close();
    for (const char *Optimization : {"-O0", "-O2"}) {
      const auto Program = tmpFile("separation-runtime");
      const auto Built =
          exec(NEVERD_TEST_CLANG, {"-std=c11", Optimization, Harness.string(),
                                   "-o", Program.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      const auto Ran = exec(Program.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
    }
  }
}

TEST_F(DevirtualizationSourceTest, CLIRegisterCasesCoverEveryMemoryEffect) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery execution requires clang";
  const auto Binary = tmpFile("generic-register-cases.elf");
  ASSERT_TRUE(buildFixture(Binary, "generic_recovery_contract.S").ok());
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto Source = tmpFile(LLVM ? "cases-llvm.c" : "cases-high.c");
    std::vector<std::string> Args{"decompile",
                                  Binary.string(),
                                  "--func",
                                  "generic_recovery_register_cases",
                                  "--devirtualize",
                                  "--vm-machine-state",
                                  "--vm-control=rax",
                                  "--vm-chain-transfers=64",
                                  "--vm-entry-frame=-64:8",
                                  "-o",
                                  Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    const auto Harness = tmpFile("cases-runtime.c");
    std::ofstream(Harness) << readSource(Source) << R"C(
#include <stdint.h>
#include <string.h>
#include <stdio.h>
int main(void) {
  _Alignas(32) uint8_t frames[2][256];
  uint64_t state[17], before[17];
  for (unsigned f = 0; f < 2; ++f) {
    for (unsigned test = 0; test < 64; ++test) {
      memset(frames, 0x65, sizeof frames);
      for (unsigned i = 0; i < 17; ++i)
        state[i] = UINT64_MAX - (test + i) * UINT64_C(0x102030405);
      state[1] = (test & 7) | ((uint64_t)test << 32);
      state[4] = (uint64_t)(uintptr_t)(frames[f] + 128);
      state[16] = 2;
      memcpy(before, state, sizeof state);
      unsigned offset = (test & 4 ? 0 : 32) + (test & 1 ? 16 : 32);
      if (generic_recovery_register_cases((uint8_t *)state)) return 1;
      if (state[0] != offset || state[2] != (test & 4 ? 32 : 64) ||
          state[8] != before[4] - offset) {
        fprintf(stderr, "case %u: result %llu, scratch %llu, offset %llu; expected %u\n",
                test, (unsigned long long)state[0], (unsigned long long)state[2],
                (unsigned long long)(before[4] - state[8]), offset);
        return 2;
      }
      for (unsigned i = 1; i < 16; ++i)
        if (i != 2 && i != 8 && state[i] != before[i]) return 3;
      for (unsigned frame = 0; frame < 2; ++frame)
        for (unsigned i = 0; i < 256; ++i)
          if (frames[frame][i] !=
              (frame == f && i == 128 - offset ? 90 : 0x65)) return 4;
    }
  }
  return 0;
}
)C";
    std::ofstream(tmpFile("immintrin.h")).close();
    for (const char *Optimization : {"-O0", "-O2"}) {
      const auto Program = tmpFile("cases-runtime");
      const auto Built =
          exec(NEVERD_TEST_CLANG, {"-std=c11", Optimization, Harness.string(),
                                   "-o", Program.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      const auto Ran = exec(Program.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
    }
  }
}

TEST_F(DevirtualizationSourceTest, CLIChecksEntryAlignmentBeforeStateCommit) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public recovery execution requires clang";
  const auto Binary = tmpFile("generic-entry-alignment.elf");
  const auto Compiled = buildFixture(Binary, "generic_recovery_contract.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  for (bool LLVM : {false, true}) {
    const auto Source = tmpFile(LLVM ? "alignment-llvm.c" : "alignment-high.c");
    const auto Report = tmpFile("alignment.json");
    std::vector<std::string> Args{"decompile",
                                  Binary.string(),
                                  "--func",
                                  "generic_recovery_chain",
                                  "--devirtualize",
                                  "--vm-machine-state",
                                  "--vm-chain-transfers=64",
                                  "--vm-entry-alignment=16:3",
                                  "--vm-max-symbolic-nodes=1048576",
                                  "--recovery-report=" + Report.string(),
                                  "-o",
                                  Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    auto Parsed = llvm::json::parse(readSource(Report));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getBoolean("complete"), true);
    EXPECT_EQ(Parsed->getAsObject()->getInteger("maxSymbolicNodes"), 1048576);
    const auto *Domain =
        Parsed->getAsObject()->getObject("entryFrameAlignment");
    ASSERT_NE(Domain, nullptr);
    EXPECT_EQ(Domain->getInteger("alignment"), 16);
    EXPECT_EQ(Domain->getInteger("residue"), 3);
    const auto Harness = tmpFile("alignment-runtime.c");
    std::ofstream(Harness) << readSource(Source) << R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  _Alignas(32) uint8_t frame[64] = {0};
  uint64_t state[17], before[17];
  for (unsigned residue = 0; residue < 16; ++residue) {
    for (unsigned i = 0; i < 17; ++i) state[i] = 300 + i;
    state[4] = residue == 3 ? (uint64_t)(uintptr_t)(frame + 19) : residue;
    state[16] = 2;
    memcpy(before, state, sizeof state);
    uint64_t status = generic_recovery_chain((uint8_t *)state);
    if (residue == 3) {
      if (status || state[0] != before[7] + 17 || state[4] != before[4]) return 1;
    } else if (status != 2 || memcmp(before, state, sizeof state)) return 2;
    for (unsigned i = 0; i < sizeof frame; ++i) if (frame[i]) return 3;
  }
  return 0;
}
)C";
    std::ofstream(tmpFile("immintrin.h")).close();
    for (const char *Optimization : {"-O0", "-O2"}) {
      const auto Program = tmpFile("alignment-runtime");
      const auto Built =
          exec(NEVERD_TEST_CLANG, {"-std=c11", Optimization, Harness.string(),
                                   "-o", Program.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      const auto Ran = exec(Program.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
    }
  }
}

TEST_F(DevirtualizationSourceTest,
       CLIDiscoversSpilledControlWithoutManualHints) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "automatic control-state recovery requires clang";
  const auto Binary = tmpFile("generic-control-state.elf");
  const auto Compiled = buildFixture(Binary, "generic_control_state.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_control_state");
  ASSERT_NE(Entry, InvalidVA);
  const auto WithoutDiscovery =
      analysis::specializeBinaryInterpreter(*Image, Entry, {});
  EXPECT_EQ(WithoutDiscovery.Status,
            analysis::SpecializationStatus::UnresolvedControl)
      << WithoutDiscovery.Diagnostic;
  EXPECT_TRUE(WithoutDiscovery.Residual.Blocks.empty());

  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto Source = tmpFile("generic-control-state.c");
    const auto Report = tmpFile("generic-control-state.json");
    std::vector<std::string> Args{"decompile",
                                  Binary.string(),
                                  "--func",
                                  "generic_control_state",
                                  "--devirtualize",
                                  "--recovery-report=" + Report.string(),
                                  "-o",
                                  Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    auto JSON = llvm::json::parse(readSource(Report));
    ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
    const auto *Object = JSON->getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getBoolean("complete"), true);
    EXPECT_EQ(Object->getBoolean("controlComplete"), true);
    EXPECT_EQ(Object->getBoolean("discoverControlState"), true);
    for (const char *Key : {"controlRegisters", "controlFrameSlots"}) {
      const auto *Fields = Object->getArray(Key);
      ASSERT_NE(Fields, nullptr) << Key;
      EXPECT_TRUE(Fields->empty()) << Key;
    }
    for (const auto &[Counter, Budget] :
         {std::pair{"discoveredControlFields", "maxControlFields"},
          std::pair{"controlRefinements", "maxControlRefinements"},
          std::pair{"discoveryVisits", "maxDiscoveryVisits"}}) {
      SCOPED_TRACE(Counter);
      const auto Work = Object->getInteger(Counter);
      const auto Limit = Object->getInteger(Budget);
      ASSERT_TRUE(Work.has_value());
      ASSERT_TRUE(Limit.has_value());
      EXPECT_GT(*Work, 0);
      EXPECT_LE(*Work, *Limit);
    }

    const auto Harness = tmpFile("generic-control-state-test.c");
    std::ofstream(Harness) << readSource(Source) << R"C(
#include <stdint.h>
int main(void) {
  uint64_t random = UINT64_C(0x4b1d23f506789ace);
  for (unsigned i = 0; i < 1024; ++i) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    uint64_t input = i < 256 ? i : random;
    if (i == 1023) input = UINT64_MAX;
    uint64_t expected = (input ^ 85) + 9 + 13 * (input & 3);
    if ((uint64_t)generic_control_state(input) != expected) return 1;
  }
  return 0;
}
)C";
    std::ofstream(tmpFile("immintrin.h")).close();
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Executable =
          tmpFile(std::string("generic-control-state-test") +
                  neverd::test::executableSuffix());
      const auto Recompiled =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Optimization, "-Werror=return-type",
                "-Werror=implicit-function-declaration", "-fsanitize=undefined",
                "-fsanitize-trap=undefined", "-I", tmp().string(),
                Harness.string(), "-o", Executable.string()});
      ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << readSource(Source);
      const auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err << readSource(Source);
    }
  }
}

TEST_F(DevirtualizationSourceTest, PERecoveryRequiresFullMetadataAndFileHash) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public PE recovery requires clang and lld";
  const auto Binary = tmpFile("generic-vm.exe");
  const auto Compiled =
      exec(NEVERD_TEST_CLANG,
           {"-target", "x86_64-pc-windows-msvc", "-fuse-ld=lld", "-nostdlib",
            "-Wl,/entry:generic_vm_register_arithmetic,/subsystem:console,/"
            "nodefaultlib",
            fixture("generic_vm_register.S").string(), "-o", Binary.string()});
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Format, BinaryFormat::COFF);
  const auto Entry = Image->Entry;
  ASSERT_NE(Entry, 0u);

  const ExceptionInfo OriginalExceptions = Image->ExceptionMetadata;
  ExceptionFunction Unrelated;
  Unrelated.CodeRange = {Entry + 0x100000, Entry + 0x100010};
  Unrelated.ParseStatus = ExceptionParseStatus::Partial;
  Image->ExceptionMetadata.Functions.push_back(Unrelated);
  Image->ExceptionMetadata.ParseStatus = ExceptionParseStatus::Partial;
  Image->ExceptionMetadata.rebuildIndex();
  const auto Localized =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_TRUE(Localized.complete()) << Localized.Diagnostic;
  // A partial record covering the reached function is still an unsupported
  // exceptional edge, even when the PE directory itself is well formed.
  Image->ExceptionMetadata.Functions.back().CodeRange = {Entry, Entry + 16};
  const auto Covering =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_EQ(Covering.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Covering.Residual.Blocks.empty());
  Image->ExceptionMetadata.Functions.back().CodeRange = Unrelated.CodeRange;
  Image->ExceptionMetadata.Diagnostics.push_back("incomplete PE directory");
  const auto Unattributed =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_EQ(Unattributed.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Unattributed.Residual.Blocks.empty());
  Image->ExceptionMetadata.Diagnostics.clear();
  Image->ExceptionMetadata.Functions.back().CodeRange = {};
  const auto Unbounded =
      analysis::specializeBinaryInterpreter(*Image, Entry, recoveryOptions());
  EXPECT_EQ(Unbounded.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Unbounded.Residual.Blocks.empty());
  Image->ExceptionMetadata = OriginalExceptions;

  BinaryLoadOptions RestrictedOptions;
  RestrictedOptions.OnlyFunctionEntries.insert(Entry);
  auto Restricted = loadBinary(Binary, RestrictedOptions);
  ASSERT_TRUE(static_cast<bool>(Restricted))
      << llvm::toString(Restricted.takeError());
  ASSERT_FALSE(Restricted->LoadOnlyFunctionEntries.empty());
  const auto Refused = analysis::specializeBinaryInterpreter(*Restricted, Entry,
                                                             recoveryOptions());
  EXPECT_EQ(Refused.Status, analysis::SpecializationStatus::Unsupported);
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
  EXPECT_TRUE(Refused.Reads.empty());
  EXPECT_NE(Refused.Diagnostic.find("full PE metadata"), std::string::npos);

  // A numeric --func must select the entry after loading all PE metadata.
  const auto Source = tmpFile("generic-vm-pe.c");
  const auto Report = tmpFile("generic-vm-pe.json");
  const auto Recovered =
      exec(ndBin(), {"decompile", "--func", "0x" + llvm::utohexstr(Entry),
                     "--devirtualize", "--vm-control=r10",
                     "--recovery-report=" + Report.string(), "-o",
                     Source.string(), Binary.string()});
  ASSERT_TRUE(Recovered.ok()) << Recovered.err;
  EXPECT_FALSE(readSource(Source).empty());
  auto JSON = llvm::json::parse(readSource(Report));
  ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getBoolean("complete"), true);
  const std::string Bytes = readSource(Binary);
  ASSERT_FALSE(Bytes.empty());
  const auto Hash = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()));
  EXPECT_EQ(Object->getString("imageSha256"), llvm::toHex(Hash));
}

TEST_F(DevirtualizationSourceTest, LLVMCMixedFrameViewsPreserveUntouchedBytes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "mixed frame source execution requires clang";
  llvm::LLVMContext Context;
  llvm::Module Module("mixed-frame-views", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> Builder(Context);
  constexpr uint64_t Initial = UINT64_C(0xaabbccddee112233);
  for (unsigned Offset : {0u, 3u}) {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt64Ty(), {Builder.getInt64Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage,
        "generic_vm_mixed_frame_" + std::to_string(Offset), Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Frame = Builder.CreateAlloca(
        llvm::ArrayType::get(Builder.getInt8Ty(), 64), nullptr, "frame");
    Builder.CreateGEP(Builder.getInt8Ty(), Frame, Builder.getInt64(32),
                      "frame_end");
    auto *Whole =
        Builder.CreateGEP(Builder.getInt8Ty(), Frame, Builder.getInt64(16));
    auto *Narrow = Builder.CreateGEP(Builder.getInt8Ty(), Frame,
                                     Builder.getInt64(16 + Offset));
    Builder.CreateStore(Builder.getInt64(Initial), Whole);
    Builder.CreateStore(
        Builder.CreateTrunc(Function->getArg(0), Builder.getInt8Ty()), Narrow);
    Builder.CreateRet(Builder.CreateLoad(Builder.getInt64Ty(), Whole));
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS));
  OS.flush();
  const auto Harness = tmpFile("mixed-frame.c");
  std::ofstream(Harness) << Source << R"(
int main(void) {
  static const uint64_t values[] = {0, 1, 0xab, UINT64_MAX};
  for (unsigned i = 0; i != 4; ++i) {
    const uint64_t initial = UINT64_C(0xaabbccddee112233);
    const uint64_t low = values[i] & UINT64_C(0xff);
    if (generic_vm_mixed_frame_0(values[i]) !=
        ((initial & ~UINT64_C(0xff)) | low))
      return 1;
    if (generic_vm_mixed_frame_3(values[i]) !=
        ((initial & ~(UINT64_C(0xff) << 24)) | (low << 24)))
      return 2;
  }
  return 0;
}
)";
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable =
        tmpFile(std::string("mixed-frame") + neverd::test::executableSuffix());
    const auto Recompiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", Optimization, "-fsanitize=undefined",
                            "-fsanitize-trap=undefined", "-I", tmp().string(),
                            Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

TEST_F(DevirtualizationSourceTest, OriginalMachinesMatchIndependentOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM fixtures require clang";
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native x64 ELF execution requires an x64 Linux host";
#else
  for (bool Finite : {false, true})
    for (bool MicrosoftABI : {false, true})
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(MicrosoftABI ? "Win64 ABI" : "SysV ABI");
        SCOPED_TRACE(Optimization);
        const auto Executable = tmpFile(std::string("generic-vm-native") +
                                        neverd::test::executableSuffix());
        std::vector<std::string> Args{"-std=c11",
                                      Optimization,
                                      "-no-pie",
                                      "-fsanitize=undefined",
                                      "-fsanitize-trap=undefined",
                                      "-o",
                                      Executable.string()};
        if (Finite) {
          Args.push_back(fixture("generic_vm_finite.S").string());
          Args.push_back(fixture("generic_vm_finite_reference.c").string());
        } else {
          Args.push_back(fixture("generic_vm_register.S").string());
          Args.push_back(fixture("generic_vm_stack.S").string());
          Args.push_back(fixture("generic_vm_reference.c").string());
        }
        SCOPED_TRACE(Finite ? "finite address VM" : "register/stack VMs");
        if (MicrosoftABI)
          Args.push_back("-DGENERIC_VM_MS_ABI");
        const auto Compiled = exec(NEVERD_TEST_CLANG, Args);
        ASSERT_TRUE(Compiled.ok()) << Compiled.err;
        const auto Ran = exec(Executable.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err;
      }
#endif
}

TEST_F(DevirtualizationSourceTest, PublicMachinesAssembleForBothNativeABIs) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target public VM fixtures require clang";
  for (const char *Triple : {"x86_64-linux-gnu", "x86_64-pc-windows-msvc"}) {
    SCOPED_TRACE(Triple);
    for (const char *Name :
         {"generic_vm_register.S", "generic_vm_stack.S", "generic_vm_finite.S",
          "generic_vm_negative.S", "generic_control_state.S"}) {
      SCOPED_TRACE(Name);
      const auto Compiled = exec(
          NEVERD_TEST_CLANG, {"-target", Triple, "-c", fixture(Name).string(),
                              "-o", tmpFile("generic-vm.o").string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err;
    }
  }
}

} // namespace

TEST_F(DevirtualizationSourceTest, MachineStateCLIRecoversPhysicalState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine-state recovery requires clang";
  const auto Binary = tmpFile("generic-state.elf");
  const auto Compiled = buildFixture(Binary, "generic_native_state.S");
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  struct Case {
    const char *Name;
    const char *Expected;
  };
  for (const auto &Test : {Case{"generic_state_flags", "flags"},
                           Case{"generic_state_call", "3 * words[7] + 19"},
                           Case{"generic_state_spill", "37"},
                           Case{"generic_state_rdssp", "123"}}) {
    SCOPED_TRACE(Test.Name);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const auto Source = tmpFile("generic-state.c");
      const auto Report = tmpFile("generic-state.json");
      std::vector<std::string> Args{"decompile",
                                    Binary.string(),
                                    "--func",
                                    Test.Name,
                                    "--devirtualize",
                                    "--vm-machine-state",
                                    "--recovery-report=" + Report.string(),
                                    "-o",
                                    Source.string()};
      if (LLVM)
        Args.push_back("--llvm");
      const auto Recovered = exec(ndBin(), Args);
      ASSERT_TRUE(Recovered.ok()) << Recovered.err;
      auto JSON = llvm::json::parse(readSource(Report));
      ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
      const auto *Object = JSON->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), true);
      EXPECT_EQ(Object->getString("sourceABI"), "x64-machine-state-v1");
      EXPECT_TRUE(Object->getString("executionProfile").has_value());
      const auto Harness = tmpFile("generic-state-test.c");
      std::ofstream Out(Harness);
      Out << readSource(Source) << "\n#include <stdint.h>\nint main(void) {\n"
          << "for (unsigned i = 0; i < 128; ++i) {\n"
          << "uint64_t stack[256] = {0}; uint64_t words[17];\n"
          << "for (unsigned j = 0; j < 17; ++j) words[j] = 1009 + 17*j;\n"
          << "const unsigned bits[7] = {0,2,4,6,7,10,11};\n"
          << "uint64_t flags = 0x202;\n"
          << "for (unsigned j = 0; j < 7; ++j) flags |= (uint64_t)((i>>j)&1) "
             "<< bits[j];\n"
          << "words[16] = flags; words[4] = (uintptr_t)&stack[128];\n"
          << "uint64_t expected = " << Test.Expected << ";\n"
          << "uint64_t status = " << Test.Name << "("
          << "(void*)words"
          << ");\n"
          << "if (status || words[0] != expected || words[4] != "
             "(uintptr_t)&stack[128]) return 1;\n"
          << "if (words[3] != 1009 + 17*3 || words[5] != 1009 + 17*5) return "
             "2;\n"
          << "} return 0; }\n";
      Out.close();
      std::ofstream(tmpFile("immintrin.h")).close();
      for (const char *Opt : {"-O0", "-O2"}) {
        SCOPED_TRACE(Opt);
        const auto Executable = tmpFile(std::string("generic-state-test") +
                                        neverd::test::executableSuffix());
        const auto Built =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c11", Opt, "-fsanitize=undefined",
                  "-fsanitize-trap=undefined", "-Werror=return-type",
                  "-Werror=implicit-function-declaration", "-I", tmp().string(),
                  Harness.string(), "-o", Executable.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << readSource(Source);
        const auto Ran = exec(Executable.string(), {});
        ASSERT_TRUE(Ran.ok()) << Ran.err << readSource(Source);
      }
    }
  }
}

TEST_F(DevirtualizationSourceTest, MachineExecutionProfilesRemainExplicit) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine-state profiles require clang";
  const auto Binary = tmpFile("generic-state.elf");
  ASSERT_TRUE(buildFixture(Binary, "generic_native_state.S").ok());
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = functionEntry(*Image, "generic_state_rdssp");
  analysis::SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  const va_t CallEntry = functionEntry(*Image, "generic_state_call");
  EXPECT_FALSE(analysis::specializeBinaryInterpreter(*Image, CallEntry, Options)
                   .complete());
  EXPECT_FALSE(
      analysis::specializeBinaryInterpreter(*Image, Entry, Options).complete());
  Options.X64CetDisabled = true;
  EXPECT_TRUE(analysis::specializeBinaryInterpreter(*Image, CallEntry, Options)
                  .complete());
  EXPECT_TRUE(
      analysis::specializeBinaryInterpreter(*Image, Entry, Options).complete());
  EXPECT_FALSE(
      analysis::specializeBinaryInterpreter(
          *Image, functionEntry(*Image, "generic_state_incssp"), Options)
          .complete());
  Options.ExplicitMachineState = false;
  EXPECT_EQ(
      analysis::specializeBinaryInterpreter(*Image, Entry, Options).Status,
      analysis::SpecializationStatus::InvalidInput);

  Options.ExplicitMachineState = true;
  ExceptionFunction Handler;
  Handler.CodeRange = {Entry, Entry + 16};
  Handler.PersonalityVA = Entry + 64;
  Image->ExceptionMetadata.Functions = {Handler};
  Image->ExceptionMetadata.rebuildIndex();
  Options.NormalNonfaultingExecution = false;
  EXPECT_FALSE(
      analysis::specializeBinaryInterpreter(*Image, Entry, Options).complete());
  Options.NormalNonfaultingExecution = true;
  EXPECT_TRUE(
      analysis::specializeBinaryInterpreter(*Image, Entry, Options).complete());
}

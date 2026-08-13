//===- SBFUpstreamConformanceTests.cpp - Anza sbpf corpus tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Support/BinaryLoading.h"
#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/Interpreter.h"
#include "neverd/sbf/Syscalls.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace neverd::sbf {
namespace {

enum class LoadExpectation : uint8_t { Accept, Reject };
enum class VMProfile : uint8_t {
#define SBF_UPSTREAM_VM_PROFILE(ID, OPTIMIZE_RODATA) ID,
#include "SBFUpstreamProfiles.def"
};

struct VMProfileDefinition {
  bool OptimizeRodata;
};

constexpr std::array VMProfiles = {
#define SBF_UPSTREAM_VM_PROFILE(ID, OPTIMIZE_RODATA)                           \
  VMProfileDefinition{OPTIMIZE_RODATA},
#include "SBFUpstreamProfiles.def"
};

SBFVMConfig makeVMConfig(VMProfile Profile) {
  const size_t Index = static_cast<size_t>(Profile);
  EXPECT_LT(Index, VMProfiles.size());
  SBFVMConfig Config;
  if (Index < VMProfiles.size())
    Config.OptimizeRodata = VMProfiles[Index].OptimizeRodata;
  return Config;
}
enum class InputProfile : uint8_t {
#define SBF_UPSTREAM_INPUT_PROFILE(ID, FIRST_BYTE) ID,
#include "SBFUpstreamProfiles.def"
};
enum class SyscallProfile : uint8_t {
#define SBF_UPSTREAM_SYSCALL_PROFILE(ID, NAME) ID,
#include "SBFUpstreamProfiles.def"
};
enum class ExecutionExpectation : uint8_t {
  NotRun,
  Return,
  FaultCallDepth,
};

struct FixtureExpectation {
  const char *File;
  Version TheVersion;
  LoadExpectation Load;
  VMProfile VM;
  InputProfile Input;
  SyscallProfile Syscalls;
  uint64_t Budget;
  ExecutionExpectation Execution;
  uint64_t Result;
};

constexpr FixtureExpectation Fixtures[] = {
#define SBF_UPSTREAM_ELF(FILE, VERSION, LOAD, VM_PROFILE, INPUT_PROFILE,       \
                         SYSCALL_PROFILE, BUDGET, EXECUTION, RESULT)           \
  {FILE,                                                                       \
   Version::VERSION,                                                           \
   LoadExpectation::LOAD,                                                      \
   VMProfile::VM_PROFILE,                                                      \
   InputProfile::INPUT_PROFILE,                                                \
   SyscallProfile::SYSCALL_PROFILE,                                            \
   BUDGET,                                                                     \
   ExecutionExpectation::EXECUTION,                                            \
   RESULT},
#include "SBFUpstreamManifest.def"
};

std::filesystem::path corpusRoot() {
  if (const char *Root = std::getenv("NEVERD_SBPF_ROOT"))
    return Root;
  return {};
}

ExecutionEnvironment makeEnvironment(const FixtureExpectation &Fixture) {
  ExecutionEnvironment Environment;
  if (Fixture.Input == InputProfile::ByteOne)
    Environment.Memory.push_back({kInputStart, {1}, false, "upstream.input"});
  if (Fixture.Syscalls == SyscallProfile::Log) {
    const uint32_t LogHash = hashSymbolName("log");
    Environment.Syscall =
        [LogHash](uint32_t Hash,
                  const SyscallArguments &) -> std::optional<uint64_t> {
      if (Hash != LogHash)
        return std::nullopt;
      return 0;
    };
  }
  return Environment;
}

class SBFUpstreamConformanceTest
    : public ::testing::TestWithParam<FixtureExpectation> {};

TEST_P(SBFUpstreamConformanceTest, MatchesPublishedLoadAndExecutionContract) {
  const FixtureExpectation &Fixture = GetParam();
  const std::filesystem::path Root = corpusRoot();
  const std::filesystem::path Path = Root / "tests" / "elfs" / Fixture.File;
  if (Root.empty() || !std::filesystem::exists(Path))
    GTEST_SKIP() << "set NEVERD_SBPF_ROOT to an Anza sbpf checkout";

  auto Image = loadBinary(Path);
  if (Fixture.Load == LoadExpectation::Reject) {
    EXPECT_FALSE(static_cast<bool>(Image));
    if (Image)
      llvm::consumeError(llvm::Error::success());
    else
      llvm::consumeError(Image.takeError());
    return;
  }

  ASSERT_TRUE(static_cast<bool>(Image))
      << (Image ? std::string() : llvm::toString(Image.takeError()));
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->Version, Fixture.TheVersion);

  AnalyzeOptions Analyze;
  Analyze.VMConfig = makeVMConfig(Fixture.VM);
  auto Program = analyze(*Image, Analyze);
  ASSERT_TRUE(static_cast<bool>(Program))
      << (Program ? std::string() : llvm::toString(Program.takeError()));
  if (Fixture.Execution == ExecutionExpectation::NotRun)
    return;

  InterpreterOptions Options;
  // Upstream's TestContextObject budget counts compute units rather than raw
  // steps. Keep the published value in the manifest, but use it only as a
  // sanity lower bound until NeverD models the instruction meter.
  Options.MaxSteps = std::max<uint64_t>(Fixture.Budget, 1'024);
  auto Result = executeRaw(*Program, makeEnvironment(Fixture), Options);
  ASSERT_TRUE(static_cast<bool>(Result))
      << (Result ? std::string() : llvm::toString(Result.takeError()));

  if (Fixture.Execution == ExecutionExpectation::FaultCallDepth) {
    EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
    EXPECT_EQ(Result->Fault, FaultCode::CallDepth);
    return;
  }
  ASSERT_EQ(Result->Status, ExecutionStatus::Returned) << Result->Error;
  EXPECT_EQ(Result->ReturnValue, Fixture.Result);
}

std::string
fixtureName(const ::testing::TestParamInfo<FixtureExpectation> &Info) {
  std::string Name = Info.param.File;
  for (char &Character : Name)
    if (Character == '.' || Character == '-')
      Character = '_';
  return Name;
}

INSTANTIATE_TEST_SUITE_P(AnzaSBPF, SBFUpstreamConformanceTest,
                         ::testing::ValuesIn(Fixtures), fixtureName);

} // namespace
} // namespace neverd::sbf

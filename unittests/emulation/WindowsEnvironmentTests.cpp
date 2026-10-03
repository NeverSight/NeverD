//===- WindowsEnvironmentTests.cpp - Win32 environment observations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <initializer_list>

namespace neverd::emulation {
namespace {
#define NEVERD_ENV_WIDE(Name, Text) constexpr char16_t Name[] = Text;
#define NEVERD_ENV_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ENV_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsEnvironmentCases.def"
#undef NEVERD_ENV_WIDE
#undef NEVERD_ENV_TEXT
#undef NEVERD_ENV_VALUE
struct Case {
  const char *Name, *Argument;
};
constexpr Case Cases[] = {
#define NEVERD_ENV_CASE(Name, Argument) {#Name, Argument},
#include "fixtures/WindowsEnvironmentCases.def"
#undef NEVERD_ENV_CASE
};
llvm::StringRef expected(llvm::StringRef Argument) {
#define NEVERD_ENV_EXPECTED(Mode, Hex)                                         \
  if (Argument == Mode)                                                        \
    return Hex;
#include "fixtures/WindowsEnvironmentCases.def"
#undef NEVERD_ENV_EXPECTED
  return {};
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsEnvironment : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.ISA == GuestArchitecture::X64
                          ? ExecutionContract::CheckedUserX64
                          : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Path = std::filesystem::path(NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Environment = {InitialVariable};
    Options.Limits.Instructions = InstructionLimit;
#endif
  }
};
TEST_P(WindowsEnvironment, ExecutesOriginalEnvironmentScenarios) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    ASSERT_FALSE(expected(C.Argument).empty());
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(C.Argument));
  }
}
TEST_P(WindowsEnvironment, RejectsInvalidStatePointersAndUnsupportedSemantics) {
  for (const char *Argument : {BadPointer, BadBlock, BadOutput, DoubleFree,
                               NonASCII, Overlap, NullSet}) {
    SCOPED_TRACE(Argument);
    Options.Arguments = {ProgramFile, Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_TRUE(R->Stop == ProcessStopReason::RuntimeFailure ||
                R->Stop == ProcessStopReason::UnsupportedService)
        << R->Diagnostic;
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardOutput.empty());
    ASSERT_FALSE(R->NativeCalls.empty());
    EXPECT_FALSE(R->NativeCalls.back().Result);
  }
}
TEST_P(WindowsEnvironment, SnapshotReleaseReclaimsLimitedGuestMemory) {
  Options.MemoryLimit = LimitedMemory;
  Options.StackSize = LimitedStack;
  Options.OutputLimit = PageSize;
  Options.Arguments = {ProgramFile, ReclaimArgument};
  auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
  EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
  EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(ReclaimArgument));
}
TEST_P(WindowsEnvironment, ValidatesUpdatesBeforePublishingGuestBytes) {
  namespace win = windows_process;
  const auto &P = GetParam();
  auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  win::VirtualMemory Virtual(*Space, Options);
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  auto Program =
      llvm::cantFail(win::loadProgram(Path, Options, *Budget, Virtual));
  auto Env = llvm::cantFail(win::prepareEnvironment(*Space, Program, Options));
  llvm::cantFail(Space->map(win::value::GateBase, ScratchSize,
                            Read | Write | UserAccessible));
  ExecutionConfiguration Config;
  Config.Backend = P.Backend;
  Config.Architecture = P.ISA;
  Config.Contract = P.ISA == GuestArchitecture::X64
                        ? ExecutionContract::CheckedUserX64
                        : ExecutionContract::CheckedUserAArch64;
  auto Backend = llvm::cantFail(createExecutionBackend(Config, Space));
  ProcessResult Result{};
  win::Services OS(*Backend.CPU, *Space, Program.Modules.front().Loaded, Env,
                   Options, Result, Virtual, Program, *Budget);
  auto Store = [&](uint64_t Address, const std::u16string &Text) {
    std::vector<uint8_t> Bytes((Text.size() + 1) * sizeof(char16_t));
    for (size_t I = 0; I < Text.size(); ++I)
      llvm::support::endian::write16le(Bytes.data() + I * sizeof(char16_t),
                                       Text[I]);
    llvm::cantFail(Space->write(Address, Bytes));
  };
  auto Call = [&](win::API API, std::initializer_list<uint64_t> Arguments) {
    NativeCallEvent Event{};
    std::copy(Arguments.begin(), Arguments.end(), Event.Arguments.begin());
    const auto *S = llvm::find_if(win::services(),
                                  [&](const auto &S) { return S.Kind == API; });
    return OS.invoke(*S, Event);
  };
  auto Bytes = [&] {
    std::vector<uint8_t> Data(win::value::EnvironmentCapacity);
    llvm::cantFail(Space->read(Env.Variables, Data));
    return Data;
  };
  const uint64_t Name = win::value::GateBase;
  const uint64_t Value = Name + PageSize;
  Store(Name, DirectName);
  Store(Value, std::u16string(DirectGrowthUnits, u'x'));
  const auto Original = Bytes();
  const uint64_t Protected = (Env.Variables + PageSize) & ~(PageSize - 1);
  llvm::cantFail(Space->protect(Protected, PageSize, Read | UserAccessible));
  auto Failed = Call(win::API::SetEnvironmentVariableW, {Name, Value});
  ASSERT_FALSE(bool(Failed));
  llvm::consumeError(Failed.takeError());
  EXPECT_EQ(Bytes(), Original);
  llvm::cantFail(
      Space->protect(Protected, PageSize, Read | Write | UserAccessible));
  Store(Value, std::u16string(DirectOversizeUnits, u'x'));
  Failed = Call(win::API::SetEnvironmentVariableW, {Name, Value});
  ASSERT_FALSE(bool(Failed));
  llvm::consumeError(Failed.takeError());
  EXPECT_EQ(Bytes(), Original);

  Store(Value, std::u16string(DirectValueUnits, u'x'));
  auto Set =
      llvm::cantFail(Call(win::API::SetEnvironmentVariableW, {Name, Value}));
  EXPECT_EQ(Set.Value, 1u);
  Store(Name, std::u16string(DirectExpansion) +
                  std::u16string(DirectTailUnits, u'x'));
  const auto Changed = Bytes();
  Failed = Call(win::API::ExpandEnvironmentStringsW, {Name, 0, 0});
  ASSERT_FALSE(bool(Failed));
  llvm::consumeError(Failed.takeError());
  EXPECT_EQ(Bytes(), Changed);

  const uint64_t Mapped = Space->mappedBytes();
  Options.MemoryLimit = Mapped;
  auto Snapshot = llvm::cantFail(Call(win::API::GetEnvironmentStringsW, {}));
  EXPECT_EQ(Snapshot.Value, 0u);
  EXPECT_EQ(Space->mappedBytes(), Mapped);
  EXPECT_EQ(Bytes(), Changed);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(
                win::value::TEB + win::value::TebLastError, sizeof(uint32_t))),
            win::value::ErrorNotEnoughMemory);
  Options.MemoryLimit = process_defaults::Memory;
  Store(Name, DirectName);
  EXPECT_EQ(
      llvm::cantFail(Call(win::API::SetEnvironmentVariableW, {Name, 0})).Value,
      1u);
  Store(Name, Initial);
  EXPECT_EQ(
      llvm::cantFail(Call(win::API::SetEnvironmentVariableW, {Name, 0})).Value,
      1u);
  Snapshot = llvm::cantFail(Call(win::API::GetEnvironmentStringsW, {}));
  ASSERT_TRUE(Snapshot.Value && *Snapshot.Value);
  EXPECT_EQ(
      llvm::cantFail(Space->readInteger(*Snapshot.Value, sizeof(uint32_t))),
      0u);
  EXPECT_EQ(
      llvm::cantFail(Call(win::API::FreeEnvironmentStringsW, {*Snapshot.Value}))
          .Value,
      1u);
  EXPECT_EQ(Space->mappedBytes(), Mapped);
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsEnvironment,
                         testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsEnvironmentNative, RunsOriginalEnvironmentExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR) / X64Dir /
       ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  const llvm::StringRef Environment[] = {InitialVariable};
  auto Run = [&](const char *Argument, uint32_t ExpectedStatus,
                 llvm::StringRef ExpectedOutput, const char *Label) {
    SCOPED_TRACE(Argument);
    std::string Diagnostic;
    bool Failed = false;
    const int Status = llvm::sys::ExecuteAndWait(
        Program, {Program, Argument}, llvm::ArrayRef(Environment), Redirects,
        NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
    ASSERT_FALSE(Failed) << Diagnostic;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << Label << Argument << ' ' << uint32_t(Status) << ' '
                 << llvm::toHex((*Out)->getBuffer()) << '\n';
    EXPECT_EQ(uint32_t(Status), ExpectedStatus)
        << llvm::toHex((*Err)->getBuffer());
    EXPECT_TRUE((*Err)->getBuffer().empty());
    EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), ExpectedOutput);
  };
  for (const auto &C : Cases) {
    ASSERT_FALSE(expected(C.Argument).empty());
    Run(C.Argument, ExitStatus, expected(C.Argument), ObservationLabel);
  }
  Run(NullSet, AccessViolationStatus, {}, ExceptionLabel);
#endif
}
} // namespace
} // namespace neverd::emulation

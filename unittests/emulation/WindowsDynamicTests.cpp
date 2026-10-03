//===- WindowsDynamicTests.cpp - Original runtime loader observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "WindowsNativeTestSupport.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
#define NEVERD_DYNAMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DYNAMIC_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_TEXT
#undef NEVERD_DYNAMIC_VALUE
struct Case {
  const char *Name, *Argument;
};
constexpr Case Cases[] = {
#define NEVERD_DYNAMIC_CASE(Name, Argument) {#Name, Argument},
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_CASE
};
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
class WindowsDynamic : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR
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
    Directory =
        std::filesystem::path(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) / P.Directory;
    Options.Backend = P.Backend;
#endif
  }
};
llvm::StringRef expected(bool NoEntry, llvm::StringRef File, char Mode) {
#define NEVERD_DYNAMIC_EXPECTED(Variant, Program, Selector, Hex)               \
  if (NoEntry == bool(Variant) && File == Program && Mode == Selector)         \
    return Hex;
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_EXPECTED
  return {};
}
bool selected(bool NoEntry, llvm::StringRef File, const Case &C) {
  const char Mode = C.Argument[1];
  return (File != StaticProgramFile || Mode == ReferencesMode) &&
         (!NoEntry ||
          (Mode != FailedMiddleMode && Mode != LeafRole &&
           Mode != NestedFailureMode && Mode != ForwardFailedMiddleMode &&
           Mode != ForwardFailedLeafMode));
}
TEST_P(WindowsDynamic, ExecutesOriginalRuntimeLoaderScenarios) {
  uint64_t Scenarios = 0;
  for (bool NoEntry : {false, true}) {
    const auto Inputs = NoEntry ? Directory / NoEntryDirectory : Directory;
    Options.Windows = WindowsProcessOptions{{{LeafFile, Inputs / LeafFile},
                                             {MiddleFile, Inputs / MiddleFile},
                                             {TopFile, Directory / TopFile}}};
    for (const char *File : {ProgramFile, StaticProgramFile})
      for (const auto &C : Cases) {
        if (!selected(NoEntry, File, C))
          continue;
        const auto Expected = expected(NoEntry, File, C.Argument[1]);
        ASSERT_FALSE(Expected.empty()) << C.Name;
        ++Scenarios;
        SCOPED_TRACE(C.Name);
        SCOPED_TRACE(NoEntry);
        SCOPED_TRACE(File);
        Options.Arguments = {File, C.Argument};
        Options.Limits.TimeoutMicroseconds =
            C.Argument[1] == RepeatMode ? ReloadTimeoutMicroseconds
                                        : process_defaults::TimeoutMicroseconds;
        auto R = emulateProcess(Directory / File, ProcessProfile::WindowsPE64,
                                Options);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
        EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
        EXPECT_TRUE(R->StandardError.empty());
        EXPECT_EQ(llvm::toHex(R->StandardOutput), Expected);
      }
  }
  EXPECT_EQ(Scenarios, ExpectedScenarios);
}
TEST_P(WindowsDynamic, RejectsInvalidLoaderStateAndRetiredCode) {
  Options.Windows = WindowsProcessOptions{
      {{LeafFile, Directory / LeafFile}, {MiddleFile, Directory / MiddleFile}}};
  for (const char *Argument :
       {ChangedLoaderArgument, ChangedTLSArgument, ChangedPEBArgument,
        ChangedLdrArgument, StaleCodeArgument}) {
    SCOPED_TRACE(Argument);
    Options.Arguments = {ProgramFile, Argument};
    auto R = emulateProcess(Directory / ProgramFile,
                            ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    if (Argument == StaleCodeArgument)
      EXPECT_EQ(R->Stop, ProcessStopReason::CPUFailure) << R->Diagnostic;
    else {
      EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
      EXPECT_NE(R->Diagnostic.find(windows_process::text::LoaderChanged),
                std::string::npos);
    }
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardError.empty());
  }
  for (const char *File : {ProgramFile, StaticProgramFile}) {
    SCOPED_TRACE(File);
    Options.Arguments = {File, ReentrantArgument};
    auto R =
        emulateProcess(Directory / File, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_NE(R->Diagnostic.find(windows_process::text::LoaderReentrant),
              std::string::npos);
    EXPECT_TRUE(R->StandardError.empty());
  }
}
TEST_P(WindowsDynamic, SharedBudgetsDoNotCompleteSuspendedCalls) {
  Options.Windows = WindowsProcessOptions{{{LeafFile, Directory / LeafFile},
                                           {MiddleFile, Directory / MiddleFile},
                                           {TopFile, Directory / TopFile}}};
  Options.Arguments = {ProgramFile, NestedArgument};
  for (bool Events : {false, true}) {
    Options.Limits.Instructions =
        Events ? process_defaults::Instructions : ShortInstructions;
    Options.Limits.Events = Events ? ShortEvents : process_defaults::Events;
    auto R = emulateProcess(Directory / ProgramFile,
                            ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, Events ? ProcessStopReason::EventLimit
                              : ProcessStopReason::InstructionLimit)
        << R->Diagnostic;
    EXPECT_FALSE(R->ExitStatus);
    if (Events) {
      auto Call = llvm::find_if(R->NativeCalls, [](const auto &C) {
        return C.Name == LoadLibraryName;
      });
      ASSERT_NE(Call, R->NativeCalls.end());
      EXPECT_FALSE(Call->Result);
    }
  }
}
class WindowsDynamicLink : public testing::Test {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<windows_process::VirtualMemory> Virtual;
  std::optional<windows_process::Program> Program;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Directory =
        std::filesystem::path(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) / X64Dir;
    Options.Windows =
        WindowsProcessOptions{{{LeafFile, Directory / LeafFile},
                               {MiddleFile, Directory / MiddleFile},
                               {TopFile, Directory / TopFile}}};
    auto Physical = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
    Space = llvm::cantFail(AddressSpace::create(Physical, Options.MemoryLimit));
    Virtual = std::make_unique<windows_process::VirtualMemory>(*Space, Options);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    auto P = windows_process::loadProgram(Directory / ProgramFile, Options,
                                          *Budget, *Virtual);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    Program = std::move(*P);
#endif
  }
};
TEST_F(WindowsDynamicLink,
       FailedPreparationReleasesReservationsWithoutRefundingWork) {
  namespace win = windows_process;
  auto &P = *Program;
  auto LeafPath = P.Catalogue.at(LeafFile);
  P.Catalogue.erase(LeafFile);
  const auto Before = P.Reads;
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  auto InputBudget = P.Reads;
  auto DLL = llvm::cantFail(
      win::loadProgramImage(Directory / MiddleFile, InputBudget, true));
  for (unsigned I = 0; I < RepeatCount; ++I) {
    auto Linked = win::linkModule(P, MiddleFile, *Virtual, *Budget);
    ASSERT_FALSE(bool(Linked));
    auto E = Linked.takeError();
    EXPECT_TRUE(E.isA<win::ModuleLoadError>());
    llvm::consumeError(std::move(E));
    EXPECT_FALSE(win::findModule(P, MiddleFile));
    auto Info = llvm::cantFail(Virtual->query(DLL.Base));
    ASSERT_TRUE(Info);
    EXPECT_EQ(Info->State, win::value::MemFree);
  }
  EXPECT_EQ(P.Modules.size(), 2u);
  EXPECT_LT(P.Reads.FileBytes, Before.FileBytes);
  EXPECT_LT(P.Reads.MappedBytes, Before.MappedBytes);
  EXPECT_LT(P.Reads.Records, Before.Records);
  P.Catalogue.emplace(LeafFile, LeafPath);
  auto Linked = win::linkModule(P, MiddleFile, *Virtual, *Budget);
  ASSERT_TRUE(bool(Linked)) << llvm::toString(Linked.takeError());
  auto Old = win::moduleRef(P, Linked->Root);
  const size_t Count = P.Modules.size();
  for (auto Ref : Linked->Added)
    ASSERT_FALSE(bool(win::retireModule(P, Ref, *Virtual)));
  auto Reload = win::linkModule(P, MiddleFile, *Virtual, *Budget);
  ASSERT_TRUE(bool(Reload)) << llvm::toString(Reload.takeError());
  EXPECT_EQ(P.Modules.size(), Count);
  EXPECT_EQ(Reload->Root, Old.Index);
  EXPECT_FALSE(win::current(P, Old));
  EXPECT_NE(win::moduleRef(P, Reload->Root).Generation, Old.Generation);
  for (auto Ref : Reload->Added)
    ASSERT_FALSE(bool(win::retireModule(P, Ref, *Virtual)));
  auto Leaf = win::linkModule(P, LeafFile, *Virtual, *Budget);
  ASSERT_TRUE(bool(Leaf)) << llvm::toString(Leaf.takeError());
  const auto LeafRef = win::moduleRef(P, Leaf->Root);
  for (auto State :
       {win::ModuleState::Initializing, win::ModuleState::Detaching}) {
    P.Modules[LeafRef.Index].State = State;
    auto Reentrant = win::linkModule(P, MiddleFile, *Virtual, *Budget);
    ASSERT_FALSE(bool(Reentrant));
    EXPECT_NE(
        llvm::toString(Reentrant.takeError()).find(win::text::LoaderReentrant),
        std::string::npos);
    EXPECT_TRUE(win::current(P, LeafRef));
    EXPECT_EQ(P.Modules[LeafRef.Index].State, State);
    EXPECT_FALSE(win::findModule(P, MiddleFile));
  }
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, WindowsDynamic,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
TEST(WindowsDynamicOracle, NativeWindowsLoadsAndUnloadsOriginalImages) {
#if !defined(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) || !defined(_WIN32) ||        \
    !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) / X64Dir;
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  uint64_t Scenarios = 0;
  for (bool NoEntry : {false, true}) {
    for (const char *File :
         {ProgramFile, StaticProgramFile, LeafFile, MiddleFile, TopFile})
      ASSERT_FALSE(llvm::sys::fs::copy_file(
          ((NoEntry && (File == LeafFile || File == MiddleFile)
                ? Directory / NoEntryDirectory
                : Directory) /
           File)
              .string(),
          (Root / File).string()));
    for (const char *File : {ProgramFile, StaticProgramFile}) {
      const auto Program = (Root / File).string();
      const auto Output = (Root / StdoutFile).string(),
                 Error = (Root / StderrFile).string();
      const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                          Error};
      for (const auto &C : Cases) {
        if (!selected(NoEntry, File, C))
          continue;
        ++Scenarios;
        SCOPED_TRACE(C.Name);
        const auto Expected = expected(NoEntry, File, C.Argument[1]);
        ASSERT_FALSE(Expected.empty());
        const unsigned Repetitions =
            C.Argument[1] == ReturnMode ? NativeReturnRepetitions : 1;
        for (unsigned I = 0; I < Repetitions; ++I) {
          SCOPED_TRACE(I);
          int Status;
          if (C.Argument[1] == ReturnMode) {
            auto Thread = native_test::observeNativeThread(
                Program, C.Argument, Output, Error, NativeTimeoutSeconds);
            ASSERT_TRUE(bool(Thread)) << llvm::toString(Thread.takeError());
            Status = int(*Thread);
          } else {
            std::string Diagnostic;
            bool Failed = false;
            Status = llvm::sys::ExecuteAndWait(
                Program, {Program, C.Argument}, std::nullopt, Redirects,
                NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
            ASSERT_FALSE(Failed) << Diagnostic;
          }
          auto Out = llvm::MemoryBuffer::getFile(Output),
               Err = llvm::MemoryBuffer::getFile(Error);
          ASSERT_TRUE(bool(Out));
          ASSERT_TRUE(bool(Err));
          if (!I)
            llvm::outs() << ObservationLabel << NoEntry << ' ' << File << ' '
                         << C.Argument << ' ' << Status << ' '
                         << llvm::toHex((*Out)->getBuffer()) << '\n';
          EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), Expected);
          EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
          EXPECT_TRUE((*Err)->getBuffer().empty());
        }
      }
    }
  }
  EXPECT_EQ(Scenarios, ExpectedScenarios);
#endif
}
} // namespace
} // namespace neverd::emulation

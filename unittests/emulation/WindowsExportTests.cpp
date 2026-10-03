//===- WindowsExportTests.cpp - Runtime and forwarded PE exports --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_EXPORT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_EXPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsExportCases.def"
#undef NEVERD_EXPORT_TEXT
#undef NEVERD_EXPORT_VALUE
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
std::string observedTrace(bool Direct, bool NoExports, bool Observe) {
  if (!Observe)
    return Direct ? DirectTrace : NormalTrace;
  std::string Result(AttachTrace);
  const char *Order = Direct ? DirectLoaderOrder : LoaderOrder;
  Result += Order;
  auto AppendError = [&](uint32_t Error) {
    char Bytes[sizeof(Error)];
    llvm::support::endian::write32le(Bytes, Error);
    Result.append(Bytes, sizeof(Bytes));
  };
#define NEVERD_EXPORT_MISSING(Module, Name, Error) AppendError(Error);
#define NEVERD_EXPORT_NO_DIRECTORY_MISSING(Module, Name, Error)                \
  if (NoExports)                                                               \
    AppendError(Error);
#include "fixtures/WindowsExportCases.def"
#undef NEVERD_EXPORT_NO_DIRECTORY_MISSING
#undef NEVERD_EXPORT_MISSING
  Result += Order;
  Result += Message;
  Result += Direct ? DirectDetachTrace : DetachTrace;
  return Result;
}
ProcessOptions options(const std::filesystem::path &Directory,
                       bool Direct = false) {
  ProcessOptions O;
  O.Windows.emplace();
  for (const char *Name : {LeafFile, BridgeFile, TopFile})
    O.Windows->Modules.push_back(
        {Name, (Direct && Name == BridgeFile ? Directory / DirectDirectory
                                             : Directory) /
                   Name});
  return O;
}
class WindowsExports : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_EXPORT_FIXTURE_DIR
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
        std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / P.Directory;
    Options = options(Directory);
    Options.Backend = P.Backend;
#endif
  }
  llvm::Expected<ProcessResult> run(const char *Argument,
                                    const char *File = ProgramFile,
                                    bool Direct = false) {
    Options.Windows = options(Directory, Direct).Windows;
    Options.Arguments = {File, Argument};
    return emulateProcess(Directory / File, ProcessProfile::WindowsPE64,
                          Options);
  }
};
TEST_P(WindowsExports, QueriesCodeDataOrdinalsAliasesAndForwarders) {
  for (bool Direct : {false, true})
    for (const char *File : {ProgramFile, NoExportsFile})
      for (const char *Argument : {NormalArgument, ObserveArgument}) {
        SCOPED_TRACE(Direct);
        SCOPED_TRACE(File);
        SCOPED_TRACE(Argument);
        auto R = run(Argument, File, Direct);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
        EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
        EXPECT_EQ(R->StandardOutput,
                  observedTrace(Direct, File == NoExportsFile,
                                Argument == ObserveArgument));
        EXPECT_TRUE(R->StandardError.empty());
        EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      }
}
TEST_P(WindowsExports, RejectsMissingModulesCyclesAndInvalidOrdinalForwarders) {
  auto Missing = run(UnusedArgument);
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::Exited) << Missing->Diagnostic;
  EXPECT_EQ(Missing->ExitStatus, ExitStatus);
  EXPECT_EQ(Missing->StandardOutput, std::string(AttachTrace) + DetachTrace);
  EXPECT_TRUE(Missing->StandardError.empty());
  const std::pair<const char *, const char *> Cases[] = {
      {CycleArgument, win::text::ForwarderCycle},
      {BadOrdinalArgument, win::text::ModuleForwarder}};
  for (const auto &[Argument, Diagnostic] : Cases) {
    SCOPED_TRACE(Argument);
    auto R = run(Argument);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
    EXPECT_NE(R->Diagnostic.find(Diagnostic), std::string::npos);
    EXPECT_EQ(R->StandardOutput, AttachTrace);
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardError.empty());
  }
}
TEST_P(WindowsExports,
       DetectsChangedAndUnreadableMetadataAfterSuccessfulQuery) {
  for (const char *Argument : {ChangedEATArgument, ChangedHeaderArgument,
                               ChangedDirectoryArgument, UnreadableArgument}) {
    SCOPED_TRACE(Argument);
    auto R = run(Argument);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
    EXPECT_NE(R->Diagnostic.find(Argument == UnreadableArgument
                                     ? win::text::Access
                                     : win::text::ExportChanged),
              std::string::npos);
    EXPECT_EQ(R->StandardOutput, AttachTrace);
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardError.empty());
  }
}
TEST_P(WindowsExports, BoundsGuestNamesAndRejectsInvalidPointers) {
  for (const char *Argument : {InvalidNameArgument, LongNameArgument}) {
    SCOPED_TRACE(Argument);
    auto R = run(Argument);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, Argument == LongNameArgument
                           ? ProcessStopReason::UnsupportedService
                           : ProcessStopReason::RuntimeFailure)
        << R->Diagnostic;
    EXPECT_EQ(R->StandardOutput, AttachTrace);
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardError.empty());
  }
}
TEST_P(WindowsExports, ChecksHeadersEvenWhenTheImageHasNoExports) {
#ifdef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
  const auto File = std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) /
                    (std::string(GetParam().Directory) + ExeExtension);
  auto Physical = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto Space =
      llvm::cantFail(AddressSpace::create(Physical, Options.MemoryLimit));
  win::VirtualMemory Memory(*Space, Options);
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  auto Program = win::loadProgram(File, Options, *Budget, Memory);
  ASSERT_TRUE(bool(Program)) << llvm::toString(Program.takeError());
  ASSERT_TRUE(Program->Modules.front().Loaded.Exports.Entries.empty());
  for (const auto &R : Program->Modules.front().Loaded.Regions) {
    ASSERT_FALSE(bool(
        Space->map(R.Address, R.Bytes.size(), Read | Write | UserAccessible)));
    ASSERT_FALSE(bool(Space->write(R.Address, R.Bytes)));
  }
  auto Backend =
      createExecutionBackend(GetParam().Backend,
                             GetParam().ISA == GuestArchitecture::X64
                                 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64,
                             Space, GetParam().ISA);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto Lookup = [&] {
    return win::resolveExport(*Program, 0, MissingName, std::nullopt, *Budget,
                              Backend->CPU.get());
  };
  auto Missing = Lookup();
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_FALSE(Missing->Address);
  const auto Address = Program->Modules.front().Loaded.Base + PEOffset;
  const auto Original =
      llvm::cantFail(Space->readInteger(Address, sizeof(uint32_t)));
  llvm::cantFail(Space->writeInteger(Address, Original + 1, sizeof(uint32_t)));
  auto Changed = Lookup();
  EXPECT_FALSE(bool(Changed));
  if (!Changed)
    EXPECT_NE(
        llvm::toString(Changed.takeError()).find(win::text::ExportChanged),
        std::string::npos);
  llvm::cantFail(Space->writeInteger(Address, Original, sizeof(uint32_t)));
  const uint64_t HeaderBytes =
      Program->Modules.front().ExportMetadata.front().Size;
  Program->Reads.MetadataBytes = HeaderBytes * 2;
  for (unsigned I = 0; I < 2; ++I) {
    auto R = Lookup();
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_FALSE(R->Address);
  }
  auto Exhausted = Lookup();
  EXPECT_FALSE(bool(Exhausted));
  if (!Exhausted)
    EXPECT_NE(
        llvm::toString(Exhausted.takeError()).find(win::text::ExportBudget),
        std::string::npos);
#endif
}
TEST(WindowsExportResolution, QueriesConsumeTheSharedBudgetAndDeadline) {
#ifndef NEVERD_WINDOWS_EXPORT_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / X64Dir;
  auto O = options(Directory);
  auto Physical = llvm::cantFail(PhysicalMemory::create(O.MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(Physical, O.MemoryLimit));
  win::VirtualMemory Memory(*Space, O);
  auto Budget = llvm::cantFail(ExecutionBudget::create(O.Limits));
  auto P = win::loadProgram(Directory / ProgramFile, O, *Budget, Memory);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  ASSERT_EQ(P->Modules.size(), 4u);
  ASSERT_EQ(P->AttachOrder.size(), 3u);
  EXPECT_EQ(P->Identities[P->AttachOrder.front()].Name, LeafFile);
  ASSERT_EQ(P->LoaderInitializationOrder.size(), 3u);
  EXPECT_EQ(P->Identities[P->LoaderInitializationOrder.front()].Name, TopFile);
  // Misses also consume work; a later call cannot create a fresh allowance.
  P->Reads.Records = 2;
  for (unsigned I = 0; I < 2; ++I) {
    auto R = win::resolveExport(*P, 0, MissingName, std::nullopt, *Budget);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_FALSE(R->Address);
  }
  auto Exhausted =
      win::resolveExport(*P, 0, MissingName, std::nullopt, *Budget);
  EXPECT_FALSE(bool(Exhausted));
  if (!Exhausted)
    EXPECT_NE(
        llvm::toString(Exhausted.takeError()).find(win::text::ExportBudget),
        std::string::npos);
  const auto Past = ExecutionBudget::Clock::now() -
                    std::chrono::microseconds(O.Limits.TimeoutMicroseconds + 1);
  auto Expired = llvm::cantFail(ExecutionBudget::create(O.Limits, Past));
  auto TimedOut =
      win::resolveExport(*P, 0, MissingName, std::nullopt, *Expired);
  EXPECT_FALSE(bool(TimedOut));
  if (!TimedOut)
    EXPECT_NE(
        llvm::toString(TimedOut.takeError()).find(win::text::ModuleTimeout),
        std::string::npos);
#endif
}
TEST(WindowsExportResolution,
     BoundsAcyclicChainsWithoutConfusingSelfForwarding) {
  win::Program P;
  P.Identities.push_back({LeafFile, 0, 0, 0});
  P.Slots.emplace(LeafFile, 0);
  win::Module M;
  M.State = win::ModuleState::Ready;
  M.Loaded.Base = PageSize;
  for (uint32_t I = 1; I <= ForwarderLimit + 1; ++I) {
    M.Ordinals.emplace(I, M.Loaded.Exports.Entries.size());
    M.Loaded.Exports.Entries.push_back(
        {I,
         uint32_t(PageSize),
         PEExportKind::Forwarder,
         {},
         std::string(ChainForwarder) + std::to_string(I + 1)});
  }
  P.Modules.push_back(std::move(M));
  auto &Entries = P.Modules.front().Loaded.Exports.Entries;
  auto Budget =
      llvm::cantFail(ExecutionBudget::create(ProcessOptions{}.Limits));
  Entries[ForwarderLimit - 1].Kind = PEExportKind::Address;
  auto Exact = win::resolveExport(P, 0, {}, 1, *Budget);
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  ASSERT_TRUE(Exact->Address);
  EXPECT_EQ(*Exact->Address, PageSize * 2);
  Entries[ForwarderLimit - 1].Kind = PEExportKind::Forwarder;
  Entries[ForwarderLimit].Kind = PEExportKind::Address;
  auto TooLong = win::resolveExport(P, 0, {}, 1, *Budget);
  EXPECT_FALSE(bool(TooLong));
  if (!TooLong)
    EXPECT_NE(
        llvm::toString(TooLong.takeError()).find(win::text::ForwarderDepth),
        std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, WindowsExports,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
TEST(WindowsExportOracle, NativeWindowsQueriesOriginalImages) {
#if !defined(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) || !defined(_WIN32) ||         \
    !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / X64Dir;
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  for (bool Direct : {false, true}) {
    for (const char *File :
         {ProgramFile, NoExportsFile, LeafFile, BridgeFile, TopFile})
      ASSERT_FALSE(llvm::sys::fs::copy_file(
          ((Direct && File == BridgeFile ? Directory / DirectDirectory
                                         : Directory) /
           File)
              .string(),
          (Root / File).string()));
    for (const char *File : {ProgramFile, NoExportsFile}) {
      const auto Program = (Root / File).string();
      const auto Output = (Root / StdoutFile).string(),
                 Error = (Root / StderrFile).string();
      const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                          Error};
      for (const char *Argument : {ObserveArgument, NormalArgument}) {
        std::string Diagnostic;
        bool Failed = false;
        const int Status = llvm::sys::ExecuteAndWait(
            Program, {Program, Argument}, std::nullopt, Redirects,
            NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
        ASSERT_FALSE(Failed) << Diagnostic;
        auto Out = llvm::MemoryBuffer::getFile(Output),
             Err = llvm::MemoryBuffer::getFile(Error);
        ASSERT_TRUE(bool(Out));
        ASSERT_TRUE(bool(Err));
        EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
        llvm::outs() << ObservationLabel << Direct << ' ' << File << ' '
                     << Argument << ' ' << Status << ' '
                     << llvm::toHex((*Out)->getBuffer()) << '\n';
        EXPECT_EQ((*Out)->getBuffer(),
                  observedTrace(Direct, File == NoExportsFile,
                                Argument == ObserveArgument));
        EXPECT_TRUE((*Err)->getBuffer().empty());
      }
    }
  }
#endif
}
} // namespace
} // namespace neverd::emulation

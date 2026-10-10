//===- UnpackDriverTests.cpp - Recovery through the driver environment ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "UnpackTestSupport.h"

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/object/PEChecksum.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#ifdef NEVERD_UNPACK_DRIVER_CLI
#include "neverd/sdk/NeverDCAPI.h"
#endif
#ifdef _WIN32
#include <windows.h>
// ImageHlp declarations require the Windows types above.
#include <imagehlp.h>
#endif

namespace neverd::unpack {
namespace {
using namespace emulation;
using llvm::support::endian::read32le;
using llvm::support::endian::write32le;

struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
};
void PrintTo(const Backend &B, std::ostream *Out) { *Out << B.Name; }

class UnpackDriver : public testing::TestWithParam<Backend> {
protected:
  std::filesystem::path Scratch;
  test::Image Original;
  UnpackOptions Options;
  void SetUp() override {
    ExecutionConfiguration Config;
    Config.Backend = GetParam().Kind;
    Config.Architecture = GuestArchitecture::X64;
    Config.Contract = ExecutionContract::CheckedX64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-unpack-driver",
                                                      Directory));
    Scratch = Directory.str().str();
    Original = test::readImage(NEVERD_UNPACK_DRIVER_FIXTURE);
    ASSERT_FALSE(HasFailure());
    Options.Process.Backend = GetParam().Kind;
  }
  void TearDown() override {
    if (!Scratch.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Scratch, Ignored);
    }
  }
  std::filesystem::path packed(unsigned Mode = 0) {
    auto Bytes = Original.File;
    const auto *Program = Original.section(".prog");
    const auto *Record = Original.section(".pack");
    const auto Entry = Original.Exports.find("packed_entry");
    if (!Program || !Record || Entry == Original.Exports.end() ||
        Original.Entry < Program->RVA ||
        Original.Entry - Program->RVA >= Program->VirtualSize ||
        std::count_if(
            Original.Sections.begin(), Original.Sections.end(),
            [](const auto &S) { return S.Name == ".prog"; }) != 1 ||
        Program->VirtualSize > 65536 ||
        Program->VirtualSize > Program->FileSize || Record->FileSize < 65552) {
      ADD_FAILURE() << "independent driver pack record is incomplete";
      return {};
    }
    uint8_t *Data = Bytes.data() + Record->FileOffset;
    write32le(Data, Mode);
    write32le(Data + 4, 0xa5);
    write32le(Data + 8, Program->VirtualSize);
    for (uint32_t I = 0; I < Program->VirtualSize; ++I)
      Data[16 + I] = uint8_t(Bytes[Program->FileOffset + I] ^ 0xa5);
    std::fill_n(Bytes.begin() + Program->FileOffset, Program->FileSize, 0);
    write32le(Bytes.data() + Original.EntryOffset, Entry->second);
    const auto Path = Scratch / "packed.sys";
    test::writeFile(Path, Bytes);
    return Path;
  }
  DriverOptions scenario() const {
    DriverOptions D;
    D.Backend = GetParam().Kind;
    D.Contract = ExecutionContract::CheckedX64;
    D.Unload = true;
    if (Options.Driver)
      D.CPUID = Options.Driver->CPUID;
    D.Requests.push_back({DriverRequestKind::Create});
    DriverRequest IO;
    IO.ControlCode = 0x222000;
    IO.Input = {0x10, 0xe1, 0, 0xff};
    IO.OutputSize = IO.Input.size();
    D.Requests.push_back(IO);
    D.Requests.push_back({DriverRequestKind::Cleanup});
    D.Requests.push_back({DriverRequestKind::Close});
    return D;
  }
  void checkLifecycle(const std::filesystem::path &Path) {
    auto Run = emulateDriver(Path, scenario());
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    ASSERT_EQ(Run->Requests.size(), 4u);
    for (const auto &Q : Run->Requests) {
      EXPECT_TRUE(Q.Completed);
      EXPECT_EQ(Q.IOStatus, 0u);
    }
    EXPECT_EQ(Run->Requests[1].Information, 4u);
    EXPECT_EQ(Run->Requests[1].Output,
              (std::vector<uint8_t>{0x4a, 0xbb, 0x5a, 0xa5}));
    EXPECT_TRUE(Run->UnloadCompleted);
    EXPECT_TRUE(Run->Devices.empty());
  }
};

TEST_P(UnpackDriver, RecoversDriverEntryImportsAndLifecycle) {
  checkLifecycle(NEVERD_UNPACK_DRIVER_FIXTURE);
  ASSERT_FALSE(HasFatalFailure());
  const auto Input = packed();
  checkLifecycle(Input);
  ASSERT_FALSE(HasFatalFailure());
  auto Result = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked)
      << unpackResultJSON(*Result);
  EXPECT_EQ(Result->Profile, profile::ReportProfile);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  EXPECT_EQ(Result->ProcessStop, "observer");
  ASSERT_FALSE(Result->Transfers.empty());
  EXPECT_TRUE(Result->Transfers.back().ProgramInvocation);
  EXPECT_TRUE(Result->RuntimeState.AdditionalStateInventoryKnown);
  EXPECT_FALSE(Result->RuntimeState.HasAdditionalDependencies);
  const auto Rebuilt = test::readImage(Result->Image);
  EXPECT_EQ(Rebuilt.Entry, Original.Entry);
  for (const auto &I : Rebuilt.Imports)
    EXPECT_EQ(I.Module, "ntoskrnl.exe");
  const auto Output = Scratch / "recovered.sys";
  test::writeFile(Output, Result->Image);
  checkLifecycle(Output);
}

TEST_P(UnpackDriver, RetainedKernelObjectsAndBorrowedPointersAreNotRecovery) {
  for (unsigned Mode : {1, 2, 3, 10, 12, 13, 14, 15, 16, 17, 18, 19}) {
    SCOPED_TRACE(Mode);
    if (Mode == 19) {
      Options.Driver.emplace();
      Options.Driver->CPUID = {{0, std::nullopt, {1, 2, 3, 4}}};
    }
    const auto Input = packed(Mode);
    if (Mode >= 15) {
      checkLifecycle(Input);
      ASSERT_FALSE(HasFatalFailure());
    }
    auto Result = unpackFile(Input, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_TRUE(Result->Image.empty());
    EXPECT_FALSE(Result->Diagnostic.empty());
    if (Mode == 3 || Mode == 10)
      EXPECT_GT(Result->RuntimeState.PossibleHeapReferences, 0u);
    else
      EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    auto Snapshot = Options;
    Snapshot.SnapshotOnly = true;
    auto Bytes = unpackFile(Input, Snapshot);
    ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
    EXPECT_EQ(Bytes->Outcome, UnpackOutcome::Snapshot) << Bytes->Diagnostic;
    EXPECT_FALSE(Bytes->Image.empty());
    EXPECT_FALSE(Bytes->Diagnostic.empty());
    if (GetParam().Kind == ExecutionBackendKind::Unicorn && Mode >= 12) {
      auto Strict = Options;
      Strict.Process.Contract = ExecutionContract::Legacy;
      auto Legacy = unpackFile(Input, Strict);
      ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
      EXPECT_EQ(Legacy->Outcome, UnpackOutcome::UnsupportedState)
          << Legacy->Diagnostic;
      EXPECT_TRUE(Legacy->RuntimeState.HasAdditionalDependencies);
    }
  }
}

TEST_P(UnpackDriver, RecoversAfterReleasingTransientImageMDLs) {
  const auto Input = packed(11);
  checkLifecycle(Input);
  ASSERT_FALSE(HasFatalFailure());
  auto Result = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked)
      << unpackResultJSON(*Result);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  EXPECT_FALSE(Result->RuntimeState.HasAdditionalDependencies);
  const auto Output = Scratch / "released-mdl.sys";
  test::writeFile(Output, Result->Image);
  checkLifecycle(Output);
  if (GetParam().Kind == ExecutionBackendKind::Unicorn) {
    auto Strict = Options;
    Strict.Process.Contract = ExecutionContract::Legacy;
    auto Legacy = unpackFile(Input, Strict);
    ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
    ASSERT_EQ(Legacy->Outcome, UnpackOutcome::Unpacked) << Legacy->Diagnostic;
    EXPECT_EQ(Legacy->EntryRVA, Result->EntryRVA);
    EXPECT_EQ(Legacy->Image, Result->Image);
  }
}

TEST_P(UnpackDriver,
       EntryArgumentsRegistersAndShadowSpaceRequireTheirOwnProof) {
  for (unsigned Mode : {5, 6, 7, 8, 9}) {
    SCOPED_TRACE(Mode);
    auto Result = unpackFile(packed(Mode), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    EXPECT_TRUE(Result->Image.empty());
  }
}

TEST_P(UnpackDriver, DynamicKernelExportIdentitySurvivesRebinding) {
  auto Result = unpackFile(packed(4), Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked)
      << unpackResultJSON(*Result);
  EXPECT_TRUE(std::any_of(
      Result->Imports.begin(), Result->Imports.end(), [](const auto &I) {
        return I.Module == "ntoskrnl.exe" && I.Name == "IofCompleteRequest" &&
               I.Origin == ImportOrigin::Runtime;
      }));
  const auto Output = Scratch / "dynamic.sys";
  test::writeFile(Output, Result->Image);
  checkLifecycle(Output);
}

TEST_P(UnpackDriver, UserRuntimeInputsAndRestorationAreRejected) {
  const auto Input = packed();
  auto Restore = Options;
  Restore.RestoreRuntime = true;
  auto Result = unpackFile(Input, Restore);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState);
  EXPECT_TRUE(Result->Image.empty());
  auto UserInputs = Options;
  UserInputs.Process.Windows->PEBVersion = WindowsPEBVersion{10, 0, 19043, 2};
  auto Invalid = unpackFile(Input, UserInputs);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("user-process inputs"),
            std::string::npos);
}

TEST_P(UnpackDriver, MalformedExportMetadataFailsBeforeExecution) {
  const auto Directory = Original.directory(llvm::COFF::EXPORT_TABLE);
  auto Offset = [&](uint32_t RVA) -> uint64_t {
    for (const auto &S : Original.Sections)
      if (RVA >= S.RVA && RVA - S.RVA < S.FileSize)
        return S.FileOffset + RVA - S.RVA;
    return UINT64_MAX;
  };
  const uint64_t Table = Offset(Directory.RelativeVirtualAddress);
  ASSERT_LT(Table, Original.File.size());
  const auto *Data = Original.File.data() + Table;
  const uint64_t Names = Offset(read32le(Data + 32));
  const uint64_t Ordinals = Offset(read32le(Data + 36));
  const uint64_t Addresses = Offset(read32le(Data + 28));
  ASSERT_LT(Names + 4, Original.File.size());
  ASSERT_LT(Ordinals + 2, Original.File.size());
  ASSERT_LT(Addresses + 4, Original.File.size());
  for (unsigned Case = 0; Case < 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Bytes = Original.File;
    if (Case == 0)
      llvm::support::endian::write16le(Bytes.data() + Ordinals, 0xffff);
    else if (Case == 1)
      write32le(Bytes.data() + Addresses,
                0x800); // Unmapped header/section gap.
    else
      write32le(Bytes.data() + Names, 0xfffffff0);
    const auto Path = Scratch / "invalid-export.sys";
    test::writeFile(Path, Bytes);
    auto Run = emulateDriver(Path, scenario());
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError()).find("export"),
              std::string::npos);
  }
}

class DriverObserver final : public ProcessObserver {
public:
  std::string Fail;
  std::vector<bool> Entries;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    if (Fail == "started")
      return llvm::createStringError("driver observer failure");
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base, M->Size}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  invoking(ProcessView &P) override {
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base, M->Size}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    if (Fail == "watched")
      return llvm::createStringError("driver observer failure");
    Entries.push_back(P.programInvocation());
    return std::vector<ExecutionWatch>{};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  resuming(ProcessView &) override {
    if (Fail == "resuming")
      return llvm::createStringError("driver observer failure");
    return std::nullopt;
  }
  llvm::Error exporting(ProcessView &, const ProcessExportView &,
                        std::optional<uint64_t>) override {
    if (Fail == "exporting")
      return llvm::createStringError("driver observer failure");
    return llvm::Error::success();
  }
};

TEST_P(UnpackDriver, ObservationPreservesLifecycleAndCallbackProvenance) {
  DriverObserver Observer;
  auto Run = observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  EXPECT_TRUE(Run->UnloadCompleted);
  ASSERT_EQ(Observer.Entries.size(), 6u);
  EXPECT_TRUE(Observer.Entries.front());
  EXPECT_TRUE(std::none_of(Observer.Entries.begin() + 1, Observer.Entries.end(),
                           [](bool E) { return E; }));
}

TEST_P(UnpackDriver, SchedulingSlicesPreserveInvocationAndTransferIdentity) {
  auto D = scenario();
  D.Scheduling = DriverScheduling{1, 1};
  DriverObserver Observer;
  auto Run = observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, D, Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  ASSERT_EQ(Observer.Entries.size(), 6u);
  EXPECT_TRUE(Observer.Entries.front());
  EXPECT_TRUE(std::none_of(Observer.Entries.begin() + 1, Observer.Entries.end(),
                           [](bool E) { return E; }));
  const auto Input = packed();
  auto Unscheduled = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Unscheduled)) << llvm::toString(Unscheduled.takeError());
  auto Scheduled = Options;
  Scheduled.Driver.emplace();
  Scheduled.Driver->Scheduling = D.Scheduling;
  auto Result = unpackFile(Input, Scheduled);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked)
      << unpackResultJSON(*Result);
  EXPECT_EQ(Result->EntryRVA, Unscheduled->EntryRVA);
  EXPECT_EQ(Result->Transfers.size(), Unscheduled->Transfers.size());
  EXPECT_EQ(Result->Image, Unscheduled->Image);
  // Raw write logs are not an input to recovery. A small API-event budget must
  // still observe the many decoder writes that produce the recovered entry.
  auto FewEvents = Options;
  FewEvents.Process.Limits.Events = 1;
  auto WithoutWriteLog = unpackFile(Input, FewEvents);
  ASSERT_TRUE(bool(WithoutWriteLog))
      << llvm::toString(WithoutWriteLog.takeError());
  ASSERT_EQ(WithoutWriteLog->Outcome, UnpackOutcome::Unpacked)
      << WithoutWriteLog->Diagnostic;
  EXPECT_EQ(WithoutWriteLog->EntryRVA, Unscheduled->EntryRVA);
  EXPECT_EQ(WithoutWriteLog->Image, Unscheduled->Image);
  FewEvents.Process.Contract = ExecutionContract::Legacy;
  auto Legacy = unpackFile(Input, FewEvents);
  ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
  ASSERT_EQ(Legacy->Outcome, UnpackOutcome::Unpacked) << Legacy->Diagnostic;
  EXPECT_EQ(Legacy->EntryRVA, Unscheduled->EntryRVA);
  EXPECT_EQ(Legacy->Image, Unscheduled->Image);
}

TEST_P(UnpackDriver, ObserverFailuresAreAPIErrors) {
  for (const char *Where : {"started", "watched", "resuming", "exporting"}) {
    SCOPED_TRACE(Where);
    DriverObserver Observer;
    Observer.Fail = Where;
    auto Run =
        observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, scenario(), Observer);
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError()).find("driver observer failure"),
              std::string::npos);
  }
}

TEST_P(UnpackDriver, CAPIAndCLIUseTheDriverEnvironment) {
#ifndef NEVERD_UNPACK_DRIVER_CLI
  GTEST_SKIP() << "shared SDK and CLI are not built";
#else
  const auto Input = packed().string();
  const auto APIPath = (Scratch / "api.sys").string();
  const auto CLIPath = (Scratch / "cli.sys").string();
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Release = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  std::string JSON = "{\"backend\":\"";
  JSON += executionBackendName(GetParam().Kind);
  JSON += "\",\"driver\":{\"service_name\":\"UnpackOracle\"}}";
  const char *Raw =
      neverd_unpack_json(Session, Input.c_str(), APIPath.c_str(), JSON.c_str());
  ASSERT_NE(Raw, nullptr) << neverd_last_error(Session);
  auto Free = llvm::scope_exit([&] { neverd_free_string(Raw); });
  auto API = llvm::json::parse(Raw);
  ASSERT_TRUE(bool(API)) << llvm::toString(API.takeError());
  ASSERT_EQ(API->getAsObject()->getString("outcome"), "unpacked");
  const auto Report = (Scratch / "cli.json").string();
  const auto Command = neverd::test::shellQuote(NEVERD_UNPACK_DRIVER_CLI) +
                       " unpack " + neverd::test::shellQuote(Input) + " -o " +
                       neverd::test::shellQuote(CLIPath) +
                       " --options=" + neverd::test::shellQuote(JSON) +
                       neverd::test::redirectStdout(Report) +
                       neverd::test::silenceStderr();
  ASSERT_EQ(
      neverd::test::systemExitCode(neverd::test::runShellCommand(Command)), 0);
  EXPECT_EQ(test::readFile(APIPath), test::readFile(CLIPath));
  auto Bytes = test::readFile(Report);
  auto CLI = llvm::json::parse(llvm::StringRef(
      reinterpret_cast<const char *>(Bytes.data()), Bytes.size()));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  API->getAsObject()->getObject("output")->erase("path");
  CLI->getAsObject()->getObject("output")->erase("path");
  EXPECT_EQ(*API, *CLI);
#endif
}

TEST(DriverChecksum, AgreesWithTheIndependentLinker) {
  const auto Bytes = test::readFile(NEVERD_UNPACK_DRIVER_FIXTURE);
  ASSERT_GT(Bytes.size(), 0x40u);
  const uint64_t Offset = uint64_t(read32le(Bytes.data() + 0x3c)) + 24 +
                          offsetof(llvm::object::pe32plus_header, CheckSum);
  ASSERT_LE(Offset + 4, Bytes.size());
  const uint32_t Linked = read32le(Bytes.data() + Offset);
  ASSERT_NE(Linked, 0u);
  EXPECT_EQ(computePEChecksum(Bytes, Offset), Linked);
  EXPECT_FALSE(computePEChecksum(Bytes, Bytes.size()));
}

TEST(DriverChecksum, NativeImageHlpValidatesOddAndEvenFileExtents) {
#ifdef _WIN32
  HMODULE Module = LoadLibraryW(L"imagehlp.dll");
  ASSERT_NE(Module, nullptr);
  auto Release = llvm::scope_exit([&] { FreeLibrary(Module); });
  auto Check = reinterpret_cast<decltype(&CheckSumMappedFile)>(
      GetProcAddress(Module, "CheckSumMappedFile"));
  ASSERT_NE(Check, nullptr);
  auto Bytes = test::readFile(NEVERD_UNPACK_DRIVER_FIXTURE);
  const uint64_t Offset = uint64_t(read32le(Bytes.data() + 0x3c)) + 24 +
                          offsetof(llvm::object::pe32plus_header, CheckSum);
  for (unsigned I = 0; I < 5; ++I) {
    DWORD Header = 0, Expected = 0;
    ASSERT_NE(Check(Bytes.data(), DWORD(Bytes.size()), &Header, &Expected),
              nullptr);
    EXPECT_EQ(computePEChecksum(Bytes, Offset), Expected);
    Bytes.push_back(uint8_t(0xef - I));
  }
#else
  GTEST_SKIP() << "native ImageHlp requires Windows";
#endif
}

INSTANTIATE_TEST_SUITE_P(
    Backends, UnpackDriver,
    testing::Values(Backend{"Unicorn", ExecutionBackendKind::Unicorn},
                    Backend{"Kvm", ExecutionBackendKind::KVM},
                    Backend{"Whp", ExecutionBackendKind::WHP}),
    [](const auto &I) { return I.param.Name; });
} // namespace
} // namespace neverd::unpack

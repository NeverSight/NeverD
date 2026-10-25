//===- LinuxProcessTests.cpp - Executable ELF userspace contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "LinuxDeviceTestData.h"
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/ELF/ELFLoader.h"

namespace neverd::emulation {
namespace {
#define NEVERD_LINUX_FIXTURE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_LINUX_FIXTURE_BYTES(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_LINUX_FIXTURE_MODE(Name, Character, Text)                       \
  constexpr char Name[] = Text;
#include "fixtures/LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_MODE
#undef NEVERD_LINUX_FIXTURE_BYTES
#undef NEVERD_LINUX_FIXTURE_TEXT
#undef NEVERD_LINUX_FIXTURE_VALUE
namespace tls_fixture {
#define NEVERD_TLS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_TLS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxTLSCases.def"
#undef NEVERD_TLS_VALUE
#undef NEVERD_TLS_TEXT
} // namespace tls_fixture
namespace pie_fixture {
#define NEVERD_PIE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PIE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxPIECases.def"
#undef NEVERD_PIE_VALUE
#undef NEVERD_PIE_TEXT
} // namespace pie_fixture
namespace memory_fixture {
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_MEMORY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
#undef NEVERD_LINUX_MEMORY_TEXT
} // namespace memory_fixture
namespace output_fixture {
#define NEVERD_LINUX_OUTPUT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_OUTPUT_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_LINUX_OUTPUT_CASE(Name, Mode, Out, Err)                         \
  constexpr char Name = Mode;
#include "fixtures/LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_CASE
#undef NEVERD_LINUX_OUTPUT_TEXT
#undef NEVERD_LINUX_OUTPUT_VALUE
} // namespace output_fixture
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Fixture;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, #ISA ".elf"},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class LinuxProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    Path =
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) / GetParam().Fixture;
    ExecutionConfiguration Configuration;
    Configuration.Backend = GetParam().Backend;
    Configuration.Architecture = GetParam().ISA;
    Configuration.Contract = GetParam().ISA == GuestArchitecture::X64
                                 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(GetParam().Backend, GetParam().ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Options.Backend = GetParam().Backend;
    Options.Arguments = {ExecutableName, Normal};
    Options.Environment = {Environment};
#endif
  }
  ProcessResult run() {
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::LinuxELF64, Options));
  }
  ProcessResult runOutput(char Mode) {
    Path.replace_filename(Path.stem().string() + output_fixture::Suffix);
    Options.Arguments = {output_fixture::ExecutableName, std::string(1, Mode)};
    return run();
  }
  ProcessResult runTime(char Mode, const char *Optimization) {
    auto TimePath = Path.parent_path() /
                    (Path.stem().string() + "-time-" + Optimization + ".elf");
    Options.Arguments = {"clock", std::string(1, Mode)};
    return llvm::cantFail(
        emulateProcess(TimePath, ProcessProfile::LinuxELF64, Options));
  }
};

TEST_P(LinuxProcess, RelativeSleepUsesSharedClockAndErrorPolicyOnBothISAs) {
  Options.LinuxTime.emplace();
  Options.LinuxTime->AdvanceOnIdle = true;
  Options.LinuxTime->Clocks = {{0, {4294967297, 999999998}}, {1, {12, 3}}};
  for (const char *Opt : {"O0", "O2"}) {
    auto R = runTime('s', Opt);
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    std::string Bytes;
    for (uint64_t Value : {4294967299ULL, 5ULL, 13ULL, 10ULL})
      for (unsigned I = 0; I < 8; ++I)
        Bytes.push_back(static_cast<char>(Value >> (I * 8)));
    EXPECT_EQ(R.StandardOutput, Bytes);
    ASSERT_GE(R.Services.size(), 4u);
    EXPECT_EQ(R.Services[0].Number,
              GetParam().ISA == GuestArchitecture::X64 ? 35u : 101u);
    EXPECT_EQ(R.Services[0].Result, uint64_t(0) - 22);
    EXPECT_EQ(R.Services[1].Result, uint64_t(0) - 14);
    EXPECT_EQ(R.Services[2].Result, 0u);
    EXPECT_EQ(R.Services[3].Result, 0u);
  }
}

TEST_P(LinuxProcess, RawMonotonicAdvancesFromItsOwnEpochOnBothISAs) {
  Options.LinuxTime.emplace();
  Options.LinuxTime->AdvanceOnIdle = true;
  Options.LinuxTime->Clocks = {
      {0, {4294967297, 999999998}}, {1, {12, 3}}, {4, {4294967301, 999999996}}};
  for (const char *Opt : {"O0", "O2"}) {
    SCOPED_TRACE(Opt);
    auto R = runTime('r', Opt);
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    std::string Expected;
    for (uint64_t Word :
         {4294967297ULL, 999999998ULL, 12ULL, 3ULL, 4294967301ULL, 999999996ULL,
          4294967299ULL, 5ULL, 13ULL, 10ULL, 4294967303ULL, 3ULL, 0ULL, 0ULL})
      for (unsigned I = 0; I < 8; ++I)
        Expected.push_back(static_cast<char>(Word >> (I * 8)));
    EXPECT_EQ(R.StandardOutput, Expected);
    ASSERT_GE(R.Services.size(), 7u);
    EXPECT_EQ(R.Services[2].Number,
              GetParam().ISA == GuestArchitecture::X64 ? 228u : 113u);
    EXPECT_EQ(R.Services[2].Arguments[0], 0x1234567800000004ULL);
    EXPECT_EQ(R.Services[3].Number,
              GetParam().ISA == GuestArchitecture::X64 ? 35u : 101u);
    for (unsigned I = 0; I < 7; ++I)
      EXPECT_EQ(R.Services[I].Result, 0u);
  }
}

TEST_P(LinuxProcess, ExplicitClocksPreserve64BitWireLayoutsOnBothISAs) {
  Options.LinuxTime.emplace();
  Options.LinuxTime->Timezone = LinuxTimezone{-60, 2};
  Options.LinuxTime->Clocks[1] = {123, 456789};
  Options.InstructionQuantum = 3;
  for (const char *Opt : {"O0", "O2"}) {
    for (int64_t Seconds : {int64_t(4294967297), int64_t(-1)}) {
      Options.LinuxTime->Clocks[0] = {Seconds, 987654321};
      auto R = runTime('n', Opt);
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
      std::string Bytes;
      auto Append = [&](uint64_t Word, unsigned Size) {
        for (unsigned I = 0; I < Size; ++I)
          Bytes.push_back(static_cast<char>(Word >> (I * 8)));
      };
      Append(static_cast<uint64_t>(Seconds), 8);
      Append(987654, 8);
      Append(uint32_t(0) - 60, 4);
      Append(2, 4);
      Append(static_cast<uint64_t>(Seconds), 8);
      Append(987654321, 8);
      Append(123, 8);
      Append(456789, 8);
      if (GetParam().ISA == GuestArchitecture::X64)
        Append(static_cast<uint64_t>(Seconds), 8);
      EXPECT_EQ(R.StandardOutput, Bytes);
      EXPECT_EQ(R.Services[0].Number,
                GetParam().ISA == GuestArchitecture::X64 ? 96u : 169u);
      EXPECT_EQ(R.Services[1].Number,
                GetParam().ISA == GuestArchitecture::X64 ? 228u : 113u);
    }
  }
}

TEST_P(LinuxProcess, ResidencyErrorsPreserveTheVectorAndMappedQueriesStop) {
  for (const char *Opt : {"O0", "O2"}) {
    const auto Fixture = Path.parent_path() /
                         (Path.stem().string() + "-residency-" + Opt + ".elf");
    Options.Arguments = {"residency", "n"};
    auto R = llvm::cantFail(
        emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    EXPECT_EQ(R.StandardOutput, std::string(32, char(0xa5)));
    Options.Arguments[1] = "m";
    R = llvm::cantFail(
        emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("page residency"), std::string::npos);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number,
              GetParam().ISA == GuestArchitecture::X64 ? 27u : 232u);
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(LinuxProcess, ResidencyObservationsExecuteOnBothLP64ABIs) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{0x60000000, {0, 1, 0}},
                                             {0x60000000, {1, 0, 1}}};
  for (const char *Opt : {"O0", "O2"}) {
    const auto Fixture = Path.parent_path() /
                         (Path.stem().string() + "-residency-" + Opt + ".elf");
    Options.Arguments = {"residency", "o"};
    auto R = llvm::cantFail(
        emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    std::string Expected(32, char(0xa5));
    Expected[3] = 0;
    Expected[4] = 1;
    Expected[5] = 0;
    Expected[8] = 1;
    Expected[9] = 0;
    Expected[10] = 1;
    EXPECT_EQ(R.StandardOutput, Expected);
    unsigned Calls = 0;
    for (const auto &Call : R.Services)
      if (Call.Number ==
          (GetParam().ISA == GuestArchitecture::X64 ? 27u : 232u)) {
        ASSERT_TRUE(Call.Result);
        EXPECT_EQ(*Call.Result, Calls++ == 0 ? uint64_t(-22) : 0u);
      }
    EXPECT_EQ(Calls, 4u);
  }
}

TEST_P(LinuxProcess, NegativePrctlOptionsUseIntWidthOnBothLP64ABIs) {
  for (const char *Opt : {"O0", "O2"}) {
    SCOPED_TRACE(Opt);
    const auto Fixture = Path.parent_path() /
                         (Path.stem().string() + "-residency-" + Opt + ".elf");
    Options.Arguments = {"prctl", "p"};
    auto Run = emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, ProcessStopReason::Exited) << Run->Diagnostic;
    EXPECT_EQ(Run->ExitStatus, 0u);
    std::string Expected;
    for (unsigned I = 0; I != 4; ++I) {
      Expected.push_back(char(0xea));
      Expected.append(7, char(0xff));
    }
    Expected.append(32, char(0xa5));
    EXPECT_EQ(Run->StandardOutput, Expected);
    unsigned Calls = 0;
    for (const auto &Call : Run->Services)
      if (Call.Number ==
          (GetParam().ISA == GuestArchitecture::X64 ? 157u : 167u)) {
        ASSERT_TRUE(Call.Result);
        EXPECT_EQ(*Call.Result, uint64_t(-22));
        ++Calls;
      }
    EXPECT_EQ(Calls, 4u);
    Options.Arguments[1] = "P";
    Run = emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Run->Diagnostic, "prctl operation is unmodeled");
    EXPECT_TRUE(Run->StandardOutput.empty());
    ASSERT_FALSE(Run->Services.empty());
    EXPECT_EQ(Run->Services.back().Arguments[0], 0x800000000000000fULL);
    EXPECT_FALSE(Run->Services.back().Result);
  }
}

TEST_P(LinuxProcess, DiscardPreservesOuterPagesThenClearsBothSidesOfAHole) {
  Options.Limits.Instructions = 500000;
  Options.Limits.TimeoutMicroseconds = 30000000;
  for (const char *Opt : {"O0", "O2"}) {
    const auto Fixture = Path.parent_path() /
                         (Path.stem().string() + "-residency-" + Opt + ".elf");
    Options.Arguments = {"discard", "d"};
    auto Run = emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    const auto &R = *Run;
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    std::string Expected(4096, '1');
    Expected.append(4096, '\0');
    Expected.append(4096, '3');
    Expected.append(8192, '\0');
    EXPECT_EQ(R.StandardOutput, Expected);
    unsigned Calls = 0;
    for (const auto &Call : R.Services)
      if (Call.Number ==
          (GetParam().ISA == GuestArchitecture::X64 ? 28u : 233u)) {
        ASSERT_TRUE(Call.Result);
        EXPECT_EQ(*Call.Result, Calls++ == 0 ? 0u : uint64_t(-12));
      }
    EXPECT_EQ(Calls, 2u);
  }
}

TEST_P(LinuxProcess, SignalActionsShareExactLP64StateAndCopyFailureOrdering) {
  for (const char *Opt : {"O0", "O2"}) {
    SCOPED_TRACE(Opt);
    const auto SignalPath = Path.parent_path() /
                            (Path.stem().string() + "-signals-" + Opt + ".elf");
    Options.Arguments = {"signals", "n"};
    Options.LinuxSignals.emplace();
    Options.LinuxSignals->Actions[11] = {0x8877665544332211, 0x10000004,
                                         0x123456789abcdef0, 1};
    auto R = llvm::cantFail(
        emulateProcess(SignalPath, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    std::string Expected;
    const uint64_t Words[] = {0x8877665544332211,
                              0x10000004,
                              0x123456789abcdef0,
                              1,
                              1,
                              0x10000000,
                              0x123456789abcdef0,
                              0xfffffffffffbfeff,
                              1,
                              0x10000000,
                              0x123456789abcdef0,
                              0xfffffffffffbfeff};
    for (uint64_t Word : Words)
      for (unsigned Byte = 0; Byte != 8; ++Byte)
        Expected.push_back(char(Word >> (8 * Byte)));
    EXPECT_EQ(R.StandardOutput, Expected);

    Options.Arguments = {"signals", "m"};
    Options.LinuxSignals.reset();
    auto Missing = llvm::cantFail(
        emulateProcess(SignalPath, ProcessProfile::LinuxELF64, Options));
    EXPECT_EQ(Missing.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Missing.Diagnostic.find("linux_signals"), std::string::npos);
    EXPECT_FALSE(Missing.Services.back().Result);
  }
}

TEST_P(LinuxProcess, KillSharesProcessTerminationAndUnknownDeliveryBoundaries) {
  for (const char *Opt : {"O0", "O2"}) {
    const auto SignalPath = Path.parent_path() /
                            (Path.stem().string() + "-signals-" + Opt + ".elf");
    for (const char *Mode : {"q", "k", "u", "h"}) {
      SCOPED_TRACE(Opt);
      SCOPED_TRACE(Mode);
      Options.Arguments = {"signals", Mode};
      const auto R = llvm::cantFail(
          emulateProcess(SignalPath, ProcessProfile::LinuxELF64, Options));
      if (*Mode == 'q') {
        EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
        EXPECT_EQ(R.ExitStatus, 0u);
        EXPECT_FALSE(R.TerminationSignal);
        EXPECT_TRUE(R.StandardOutput.empty());
      } else {
        EXPECT_EQ(R.Stop, *Mode == 'k' ? ProcessStopReason::Signaled
                                       : ProcessStopReason::UnsupportedService)
            << R.Diagnostic;
        EXPECT_EQ(R.TerminationSignal,
                  *Mode == 'k' ? std::optional<uint32_t>(9) : std::nullopt);
        EXPECT_FALSE(R.ExitStatus);
        EXPECT_EQ(R.StandardOutput, "before");
        ASSERT_FALSE(R.Services.empty());
        EXPECT_EQ(R.Services.back().Number,
                  GetParam().ISA == GuestArchitecture::X64 ? 62u : 129u);
        EXPECT_FALSE(R.Services.back().Result);
      }
    }
  }
}

TEST_P(LinuxProcess, SignalDeliverySharesMasksPendingAndAArch64Frames) {
  for (const char *Opt : {"O0", "O2"}) {
    const auto SignalPath = Path.parent_path() /
                            (Path.stem().string() + "-signals-" + Opt + ".elf");
    Options.LinuxSignals.emplace();
    Options.LinuxSignals->InitialMask = 0;
    Options.LinuxSignals->AlternateStackDisabled = true;
    for (const char *Mode : {"v", "d", "s"}) {
      SCOPED_TRACE(Opt);
      SCOPED_TRACE(Mode);
      Options.Arguments = {"signals", Mode};
      const auto R = llvm::cantFail(
          emulateProcess(SignalPath, ProcessProfile::LinuxELF64, Options));
      if (*Mode == 'd') {
        EXPECT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
        EXPECT_EQ(R.TerminationSignal, 15u);
        EXPECT_FALSE(R.ExitStatus);
      } else if (*Mode == 's' && GetParam().ISA == GuestArchitecture::X64) {
        EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
        EXPECT_NE(R.Diagnostic.find("signal frame"), std::string::npos);
      } else {
        ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
        EXPECT_EQ(R.ExitStatus, 0u);
        uint64_t Expected[8] = {};
        Expected[0] = *Mode == 's' ? 10 : 0;
        Expected[1] = 512;
        Expected[2] = *Mode == 's' ? 0 : 512;
        Expected[4] = *Mode == 's' ? 0 : 4096;
        std::string Bytes;
        for (uint64_t Word : Expected)
          for (unsigned Byte = 0; Byte < 8; ++Byte)
            Bytes.push_back(char(Word >> (8 * Byte)));
        EXPECT_EQ(R.StandardOutput, Bytes);
      }
    }
  }
}

TEST_P(LinuxProcess, MemoryFilesPreserveBinaryBytesCursorsAndFaultPrefixes) {
  // O0 link checks fill a complete page one guest instruction at a time.
  // Allow contended runners while retaining instruction/event limits.
  Options.Limits.TimeoutMicroseconds = 30000000;
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->Files["/fixture/data"] = {0,    0xff, 0x41,
                                                0x0a, 0x80, 0x5a};
  Options.LinuxFiles->SymbolicLinks["/links/raw"] = {'/', 'x', 0xff, 'y'};
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  for (const char *Optimization : {"O0", "O2"}) {
    auto FilePath = Path.parent_path() /
                    (Path.stem().string() + "-files-" + Optimization + ".elf");
    for (char Mode : {'s', 'f', 'c', 't', 'n', 'a', 'p', 'd', 'q', 'v', 'l'}) {
      SCOPED_TRACE(testing::Message() << Optimization << ':' << Mode);
      Options.Arguments = {"files", std::string(1, Mode)};
      Options.LinuxFiles->DescriptorLimit =
          Mode == 'c' || Mode == 'a' || Mode == 'd' || Mode == 'q' ? 4 : 256;
      auto R = emulateProcess(FilePath, ProcessProfile::LinuxELF64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 0u);
    }
  }
}

TEST_P(LinuxProcess, RenameMovesNamesWithoutReplacingOpenFileIdentity) {
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  for (const char *opt : {"O0", "O2"}) {
    auto file =
        Path.parent_path() / (Path.stem().string() + "-files-" + opt + ".elf");
    for (char mode : {'Q', 'T', '1'}) {
      Options.Arguments = {"files", std::string(1, mode)};
      Options.LinuxFiles = fileRenameTestInput();
      if (mode == '1')
        Options.LinuxFiles->DynamicProc = LinuxProcOptions{"fixture", 12};
      auto r = emulateProcess(file, ProcessProfile::LinuxELF64, Options);
      ASSERT_TRUE(bool(r)) << llvm::toString(r.takeError());
      EXPECT_EQ(r->Stop, ProcessStopReason::Exited) << r->Diagnostic;
      EXPECT_EQ(r->ExitStatus, 0u);
      Options.LinuxFiles->FileRenames.reset();
      auto refused = emulateProcess(file, ProcessProfile::LinuxELF64, Options);
      ASSERT_TRUE(bool(refused)) << llvm::toString(refused.takeError());
      EXPECT_EQ(refused->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(refused->Diagnostic.find("explicit provider"),
                std::string::npos);
    }
  }
}

TEST_P(LinuxProcess,
       UnlinkKnownErrorsKeepOriginalFilesAndUnsupportedBoundaries) {
  Options.Limits.TimeoutMicroseconds = 30000000;
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->Files["/fixture/data"] = {0,    0xff, 0x41,
                                                0x0a, 0x80, 0x5a};
  Options.LinuxFiles->SymbolicLinks["/links/raw"] = {'/', 'x'};
  for (const char *opt : {"O0", "O2"}) {
    auto file =
        Path.parent_path() / (Path.stem().string() + "-files-" + opt + ".elf");
    for (char mode : {'e', 'E', 'F', 'G', 'H', 'I', 'K'}) {
      SCOPED_TRACE(testing::Message() << opt << ":" << mode);
      Options.Arguments = {"files", std::string(1, mode)};
      auto r = llvm::cantFail(
          emulateProcess(file, ProcessProfile::LinuxELF64, Options));
      if (mode == 'e') {
        EXPECT_EQ(r.Stop, ProcessStopReason::Exited) << r.Diagnostic;
        EXPECT_EQ(r.ExitStatus, 0u);
      } else {
        EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService)
            << r.Diagnostic;
        ASSERT_FALSE(r.Services.empty());
        EXPECT_FALSE(r.Services.back().Result);
      }
    }
  }
}

TEST_P(LinuxProcess, UnlinkClosedFileRequiresItsSeparateProvider) {
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  for (const char *Opt : {"O0", "O2"}) {
    auto File =
        Path.parent_path() / (Path.stem().string() + "-files-" + Opt + ".elf");
    Options.Arguments = {"files", "E"};
    Options.LinuxFiles = fileUnlinkTestInput();
    auto R = llvm::cantFail(
        emulateProcess(File, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    Options.LinuxFiles->FileUnlinks.reset();
    auto Refused = llvm::cantFail(
        emulateProcess(File, ProcessProfile::LinuxELF64, Options));
    EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Refused.Diagnostic.find("explicit provider"), std::string::npos);
    ASSERT_FALSE(Refused.Services.empty());
    EXPECT_FALSE(Refused.Services.back().Result);
  }
}

TEST_P(LinuxProcess,
       FileCreationExecutesOriginalCallersWithoutAliasingMovedFiles) {
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  for (const char *Opt : {"O0", "O2"}) {
    auto File =
        Path.parent_path() / (Path.stem().string() + "-files-" + Opt + ".elf");
    Options.Arguments = {"files", "N"};
    Options.LinuxFiles = fileCreationTestInput();
    auto R = llvm::cantFail(
        emulateProcess(File, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    Options.LinuxFiles->FileCreations.reset();
    auto Refused = llvm::cantFail(
        emulateProcess(File, ProcessProfile::LinuxELF64, Options));
    EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Refused.Diagnostic.find("explicit object reservation"),
              std::string::npos);
    ASSERT_FALSE(Refused.Services.empty());
    EXPECT_FALSE(Refused.Services.back().Result);
  }
}

TEST_P(LinuxProcess, FileCreationCountsExecuteIndependentRawLifecycles) {
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  for (const char *Opt : {"O0", "O2"}) {
    auto File =
        Path.parent_path() / (Path.stem().string() + "-files-" + Opt + ".elf");
    Options.Arguments = {"files", "M"};
    Options.LinuxFiles = fileCreationCountTestInput();
    auto R = llvm::cantFail(
        emulateProcess(File, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    Options.LinuxFiles->FileCreations->Files.at("/fixture/data").Count = 1;
    auto Refused = llvm::cantFail(
        emulateProcess(File, ProcessProfile::LinuxELF64, Options));
    EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Refused.Diagnostic.find("already consumed"), std::string::npos);
    ASSERT_FALSE(Refused.Services.empty());
    EXPECT_FALSE(Refused.Services.back().Result);
  }
}

TEST_P(LinuxProcess, ExplicitDeviceTransactionsShareDescriptorOwnership) {
  Options.LinuxFiles.emplace().Devices["/fixture/control"] = deviceTestInput();
  for (const char *Optimization : {"O0", "O2"}) {
    auto DevicePath = Path.parent_path() / (Path.stem().string() + "-devices-" +
                                            Optimization + ".elf");
    auto R = emulateProcess(DevicePath, ProcessProfile::LinuxELF64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 0u);
  }
}
TEST_P(LinuxProcess, StatefulDescriptorsPagesAndContextsShareOneKernelOwner) {
  Options.LinuxFiles.emplace().Files["/fixture/bytes"] = {'a', 'b', 'c'};
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  for (const char *Optimization : {"O0", "O2"}) {
    auto DevicePath = Path.parent_path() / (Path.stem().string() + "-devices-" +
                                            Optimization + ".elf");
    for (char Mode : {'d', 'u', 'k', 'b', 'g'}) {
      SCOPED_TRACE(testing::Message() << Optimization << ':' << Mode);
      Options.Arguments = {"devices", std::string(1, Mode)};
      Options.LinuxFiles->Devices["/fixture/control"] =
          Mode == 'k' || Mode == 'g' ? contextDeviceTestInput()
                                     : updatingDeviceTestInput();
      if (Mode == 'g') {
        auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
        D.ReadOnlyMappings[0].Updates.clear();
        D.KGSL->SynchronousA3xx = true;
      }
      Options.LinuxTime->NanosecondsPerInstruction = Mode == 'b' ? 100 : 0;
      Options.InstructionQuantum = 1024;
      if (Mode == 'b')
        Options.LinuxFiles->Devices["/fixture/control"]
            .ReadOnlyMappings[0]
            .Updates = {{1000000, 0, {0x9a}}};
      auto R = emulateProcess(DevicePath, ProcessProfile::LinuxELF64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 0u);
      EXPECT_EQ(R->StandardOutput, Mode == 'd' ? "out" : "");
      EXPECT_EQ(R->StandardError, Mode == 'd' ? "err" : "");
    }
  }
}
TEST_P(LinuxProcess, GPUObjectStorageChecksCompletePagesAndLifetime) {
  // Original O0 code checks every word of three pages. Keep the guest
  // instruction/event limits, while allowing a contended software CPU.
  Options.Limits.TimeoutMicroseconds = 30000000;
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  D.KGSL->MemoryObjects = true;
  Options.LinuxFiles.emplace().Devices["/fixture/control"] = D;
  Options.Arguments = {"devices", "o"};
  Options.InstructionQuantum = 1024;
  for (const char *Optimization : {"O0", "O2"}) {
    SCOPED_TRACE(Optimization);
    auto DevicePath = Path.parent_path() / (Path.stem().string() + "-devices-" +
                                            Optimization + ".elf");
    auto R = emulateProcess(DevicePath, ProcessProfile::LinuxELF64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 0u);
  }
}

TEST_P(LinuxProcess, LegacyGPUObjectsShareModernState) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  D.KGSL->MemoryObjects = true;
  Options.LinuxFiles.emplace().Devices["/fixture/control"] = D;
  Options.Arguments = {"devices", "l"};
  Options.InstructionQuantum = 1024;
  for (const char *Optimization : {"O0", "O2"}) {
    SCOPED_TRACE(Optimization);
    auto DevicePath = Path.parent_path() / (Path.stem().string() + "-devices-" +
                                            Optimization + ".elf");
    auto R = emulateProcess(DevicePath, ProcessProfile::LinuxELF64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 0u);
  }
}

TEST_P(LinuxProcess, MemoryFileOpenFlagsRetainObservedPathErrors) {
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->Files["/fixture/data"] = {0, 0xff, 0x41};
  Options.LinuxFiles->DescriptorLimit = 4;
  constexpr AndroidGKIKernel Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  AndroidGKIKernel::Name,
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (auto Kernel : Kernels) {
    SCOPED_TRACE(unsigned(Kernel));
    Options.LinuxKernel.emplace().GKI = Kernel;
    for (const char *Opt : {"O0", "O2"}) {
      auto FilePath = Path.parent_path() /
                      (Path.stem().string() + "-files-" + Opt + ".elf");
      Options.Arguments = {"files", "o"};
      auto R = llvm::cantFail(
          emulateProcess(FilePath, ProcessProfile::LinuxELF64, Options));
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxProcess, MemoryFileOpenFlagsKeepUnobservedOpenedFiles) {
  Options.LinuxFiles.emplace().Files["/fixture/data"] = {0};
  for (const char *Opt : {"O0", "O2"}) {
    auto FilePath =
        Path.parent_path() / (Path.stem().string() + "-files-" + Opt + ".elf");
    for (char Mode : {'R', 'D', 'B'}) {
      Options.Arguments = {"files", std::string(1, Mode)};
      auto R = llvm::cantFail(
          emulateProcess(FilePath, ProcessProfile::LinuxELF64, Options));
      if (Mode == 'B') {
        EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
        EXPECT_EQ(R.ExitStatus, 0u);
        ASSERT_GE(R.Services.size(), 5u);
        EXPECT_EQ(R.Services[0].Result, 3u);
        EXPECT_EQ(R.Services[1].Result, uint64_t(-21));
        EXPECT_EQ(R.Services[2].Result, 0u);
        EXPECT_EQ(R.Services[3].Result, 0u);
      } else {
        EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService)
            << R.Diagnostic;
        EXPECT_FALSE(R.Services.back().Result);
        EXPECT_NE(R.Diagnostic.find("direct I/O"), std::string::npos);
      }
    }
  }
}
TEST_P(LinuxProcess, ReleasedGKIXAttrsPreserveNameAndTargetErrorOrder) {
  Options.LinuxFiles.emplace().Files["/fixture/data"] = {0, 0xff};
  struct Case {
    AndroidGKIKernel Kernel;
    bool NameFirst;
  };
  constexpr Case Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  {AndroidGKIKernel::Name, NameFirst},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (auto K : Kernels) {
    SCOPED_TRACE(unsigned(K.Kernel));
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    for (const char *Opt : {"O0", "O2"}) {
      auto FilePath = Path.parent_path() /
                      (Path.stem().string() + "-files-" + Opt + ".elf");
      Options.Arguments = {"files", K.NameFirst ? "Y" : "X"};
      auto R = llvm::cantFail(
          emulateProcess(FilePath, ProcessProfile::LinuxELF64, Options));
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxProcess, XAttrsKeepUnknownKernelAndExistingObjectBoundaries) {
  Options.LinuxFiles.emplace().Files["/fixture/data"] = {0};
  for (const char *Opt : {"O0", "O2"}) {
    auto FilePath =
        Path.parent_path() / (Path.stem().string() + "-files-" + Opt + ".elf");
    Options.Arguments = {"files", "X"};
    auto R = llvm::cantFail(
        emulateProcess(FilePath, ProcessProfile::LinuxELF64, Options));
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("released GKI"), std::string::npos);
    Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android17_6_18;
    for (char Mode : {'U', 'V', 'W', 'J'}) {
      Options.Arguments = {"files", std::string(1, Mode)};
      auto E = llvm::cantFail(
          emulateProcess(FilePath, ProcessProfile::LinuxELF64, Options));
      EXPECT_EQ(E.Stop, ProcessStopReason::UnsupportedService) << E.Diagnostic;
      EXPECT_FALSE(E.Services.back().Result);
      EXPECT_NE(E.Diagnostic.find("extended attributes"), std::string::npos);
    }
    Options.LinuxKernel.reset();
  }
}
struct XAttrImportCase {
  LinuxXAttrQueryImport Import;
  const char *Mode;
};
// Independent fixture modes retain their literal errno and buffer assertions.
constexpr XAttrImportCase XAttrImports[] = {
    {LinuxXAttrQueryImport::TargetBeforeName, "X"},
    {LinuxXAttrQueryImport::NameBeforeTarget, "Y"}};

TEST_P(LinuxProcess, XAttrQueryImportExecutesIndependentRawErrorOrders) {
  Options.LinuxFiles.emplace().Files["/fixture/data"] = {0, 0xff};
  for (const auto &C : XAttrImports) {
    SCOPED_TRACE(C.Mode);
    Options.LinuxKernel.emplace().XAttrQueryImport = C.Import;
    for (const char *Opt : {"O0", "O2"}) {
      SCOPED_TRACE(Opt);
      const auto Fixture = Path.parent_path() /
                           (Path.stem().string() + "-files-" + Opt + ".elf");
      Options.Arguments = {"files", C.Mode};
      auto R = llvm::cantFail(
          emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
    }
  }
}

TEST_P(LinuxProcess, XAttrQueryImportKeepsObjectAndSymbolicLinkBoundaries) {
  for (const auto &C : XAttrImports) {
    SCOPED_TRACE(C.Mode);
    Options.LinuxKernel.emplace().XAttrQueryImport = C.Import;
    for (const char *Opt : {"O0", "O2"}) {
      SCOPED_TRACE(Opt);
      const auto Fixture = Path.parent_path() /
                           (Path.stem().string() + "-files-" + Opt + ".elf");
      Options.LinuxFiles.emplace().Files["/fixture/data"] = {0};
      for (char Mode : {'U', 'V', 'W', 'J'}) {
        SCOPED_TRACE(Mode);
        Options.Arguments = {"files", std::string(1, Mode)};
        auto R = llvm::cantFail(
            emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
        EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService)
            << R.Diagnostic;
        EXPECT_NE(R.Diagnostic.find("extended attributes"), std::string::npos);
        ASSERT_FALSE(R.Services.empty());
        EXPECT_FALSE(R.Services.back().Result);
      }
      Options.LinuxFiles->Files.clear();
      for (bool Directory : {false, true}) {
        SCOPED_TRACE(Directory);
        Options.LinuxFiles->SymbolicLinks.clear();
        const char *Link = Directory ? "/fixture" : "/fixture/data";
        Options.LinuxFiles->SymbolicLinks[Link] = {'/', 't'};
        Options.Arguments = {"files", Directory ? "V" : "U"};
        auto R = llvm::cantFail(
            emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
        EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService)
            << R.Diagnostic;
        EXPECT_NE(R.Diagnostic.find("symbolic-link"), std::string::npos);
        ASSERT_FALSE(R.Services.empty());
        EXPECT_FALSE(R.Services.back().Result);
      }
    }
  }
}

TEST_P(LinuxProcess, XAttrQueryImportKeepsPidFDAbsenceIndependent) {
  auto &Kernel = Options.LinuxKernel.emplace();
  for (const auto &C : XAttrImports) {
    Kernel.XAttrQueryImport = C.Import;
    for (const char *Opt : {"O0", "O2"}) {
      const auto Fixture = Path.parent_path() /
                           (Path.stem().string() + "-pidfd-" + Opt + ".elf");
      Options.Arguments = {"pidfd", "m", "0"};
      Kernel.UnavailableSyscalls.clear();
      auto Unknown = llvm::cantFail(
          emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
      EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService)
          << Unknown.Diagnostic;
      ASSERT_FALSE(Unknown.Services.empty());
      EXPECT_EQ(Unknown.Services.back().Number, 434u);
      EXPECT_FALSE(Unknown.Services.back().Result);
      Kernel.UnavailableSyscalls.insert(LinuxUnavailableSyscall::PidFDOpen);
      auto Absent = llvm::cantFail(
          emulateProcess(Fixture, ProcessProfile::LinuxELF64, Options));
      ASSERT_EQ(Absent.Stop, ProcessStopReason::Exited) << Absent.Diagnostic;
      EXPECT_EQ(Absent.ExitStatus, 91u);
      ASSERT_EQ(Absent.Services.size(), 3u);
      EXPECT_EQ(Absent.Services[1].Number, 434u);
      EXPECT_EQ(Absent.Services[1].Result, uint64_t(-38));
    }
  }
}

TEST_P(LinuxProcess, TimeFaultsPreserveKernelErrnosAndOrderedWrites) {
  Options.LinuxTime.emplace();
  Options.LinuxTime->Clocks[0] = {4294967297, 987654321};
  Options.LinuxTime->Timezone = LinuxTimezone{0, 0};
  for (const char *Opt : {"O0", "O2"}) {
    auto R = runTime('f', Opt);
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    std::string Expected;
    for (uint64_t V :
         {uint64_t(4294967297), uint64_t(987654), uint64_t(4294967297)})
      for (unsigned I = 0; I < 8; ++I)
        Expected.push_back(static_cast<char>(V >> (I * 8)));
    EXPECT_EQ(R.StandardOutput, Expected);
  }
}
TEST_P(LinuxProcess, MissingDynamicAndPartialClockOperationsHaveNoReturn) {
  for (const char *Opt : {"O0", "O2"}) {
    for (char Mode : {'m', 'd', 'p'}) {
      Options.LinuxTime.emplace();
      if (Mode != 'm')
        Options.LinuxTime->Clocks[0] = {1, 2};
      auto R = runTime(Mode, Opt);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_FALSE(R.ExitStatus);
      ASSERT_FALSE(R.Services.empty());
      EXPECT_FALSE(R.Services.back().Result);
    }
  }
}

TEST_P(LinuxProcess, LoadsDataBSSAndInitialStackThenHandlesErrorsAndExits) {
  // A tiny quantum forces many real CPU resumptions through the same process
  // image, registers, stack, service return state and resource account.
  Options.InstructionQuantum = 3;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, ExitStatus);
  EXPECT_EQ(Result.Architecture, GetParam().ISA);
  EXPECT_EQ(Result.SelectedBackend, GetParam().Backend);
  EXPECT_EQ(Result.StandardOutput, Message);
  EXPECT_EQ(Result.StandardError,
            std::string(reinterpret_cast<const char *>(BinaryOutput),
                        sizeof(BinaryOutput)));
  EXPECT_GT(Result.Instructions, Options.InstructionQuantum);
  EXPECT_EQ(Result.Events, Result.Services.size());
  ASSERT_FALSE(Result.Services.empty());
  EXPECT_FALSE(Result.Services.back().Result);
}
TEST_P(LinuxProcess, InstructionCreditsSurviveQuantumResumptions) {
  Options.Arguments[1] = Loop;
  Options.InstructionQuantum = 3;
  Options.Limits.Instructions = 1000;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::InstructionLimit)
      << Result.Diagnostic;
  EXPECT_EQ(Result.Instructions, Options.Limits.Instructions);
  EXPECT_FALSE(Result.ExitStatus);
}
TEST_P(LinuxProcess, IdentityQueriesAgreeWithStartupAuxiliaryVector) {
  Options.Arguments[1] = IdentityQueries;
  Options.InstructionQuantum = 3;
  auto Result = run();
  ASSERT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, ExitStatus);
  ASSERT_EQ(Result.Services.size(), 5u);
  const std::vector<uint64_t> Numbers =
      GetParam().ISA == GuestArchitecture::X64
          ? std::vector<uint64_t>{102, 107, 104, 108}
          : std::vector<uint64_t>{174, 175, 176, 177};
  for (size_t I = 0; I < Numbers.size(); ++I) {
    EXPECT_EQ(Result.Services[I].Number, Numbers[I]);
    EXPECT_EQ(Result.Services[I].Result, Identity);
  }
}
TEST_P(LinuxProcess, IdentityMutationRemainsAnUnsupportedService) {
  Options.Arguments[1] = SetIdentity;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_EQ(Result.Services.size(), 1u);
  EXPECT_EQ(Result.Services[0].Number,
            GetParam().ISA == GuestArchitecture::X64 ? 105u : 146u);
  EXPECT_FALSE(Result.Services[0].Result);
}
TEST_P(LinuxProcess,
       AnonymousMappingsHeapAndCodeSurviveRealServiceContinuations) {
  Path.replace_filename(Path.stem().string() + memory_fixture::Suffix);
  Options.Arguments = {memory_fixture::ExecutableName, memory_fixture::Normal};
  Options.InstructionQuantum = 3;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, memory_fixture::ExitStatus);
  EXPECT_EQ(Result.StandardOutput, memory_fixture::Message);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_EQ(Result.Events, Result.Services.size());
}
TEST_P(LinuxProcess,
       GuestStoresObserveBothExplicitAndPartialProtectionChanges) {
  Path.replace_filename(Path.stem().string() + memory_fixture::Suffix);
  for (const char *Mode :
       {memory_fixture::ProtectionFault, memory_fixture::HoleFault}) {
    SCOPED_TRACE(Mode);
    Options.Arguments = {memory_fixture::ExecutableName, Mode};
    const auto Result = run();
    EXPECT_EQ(Result.Stop, ProcessStopReason::CPUFailure) << Result.Diagnostic;
    EXPECT_FALSE(Result.ExitStatus);
    ASSERT_TRUE(Result.LastCPUExit);
    ASSERT_TRUE(Result.LastCPUExit->Fault);
    EXPECT_EQ(Result.LastCPUExit->Fault->Kind, BackendFaultKind::Protection);
    EXPECT_TRUE(Result.StandardOutput.empty());
  }
}
TEST_P(LinuxProcess, FixedReplacementIsRejectedWithoutInventingAMappingResult) {
  Path.replace_filename(Path.stem().string() + memory_fixture::Suffix);
  Options.Arguments = {memory_fixture::ExecutableName,
                       memory_fixture::Unsupported};
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_EQ(Result.Services.size(), 1u);
  EXPECT_FALSE(Result.Services.front().Result);
}
TEST_P(LinuxProcess, CompilerTLSBlocksRemainIndependentAcrossProcessQuanta) {
  Path.replace_filename(Path.stem().string() + tls_fixture::Suffix);
  Options.Arguments = {tls_fixture::ExecutableName, tls_fixture::Normal};
  Options.InstructionQuantum = 3;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, tls_fixture::ExitStatus);
  EXPECT_EQ(Result.StandardOutput, tls_fixture::Message);
  EXPECT_EQ(Result.SelectedBackend, GetParam().Backend);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_GT(Result.Instructions, Options.InstructionQuantum);
}
TEST_P(LinuxProcess, UnimplementedArchPrctlDoesNotInventAServiceReturn) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << tls_fixture::RequiresX64;
  Path.replace_filename(Path.stem().string() + tls_fixture::Suffix);
  Options.Arguments = {tls_fixture::ExecutableName, tls_fixture::Unknown};
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService)
      << Result.Diagnostic;
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_FALSE(Result.Services.empty());
  EXPECT_EQ(Result.Services.back().Number, tls_fixture::ArchPrctl);
  EXPECT_EQ(Result.Services.back().Arguments[0],
            tls_fixture::UnsupportedOperation);
  EXPECT_FALSE(Result.Services.back().Result);
  EXPECT_TRUE(Result.StandardOutput.empty());
}
TEST_P(LinuxProcess, StaticPIEReceivesRawSlotsAndRelocatesAtItsActualBias) {
  Path.replace_filename(Path.stem().string() + pie_fixture::Suffix);
  Options.Arguments = {pie_fixture::ExecutableName};
  Options.InstructionQuantum = 3;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, pie_fixture::ExitStatus);
  EXPECT_EQ(Result.StandardOutput, pie_fixture::Message);
  EXPECT_EQ(Result.SelectedBackend, GetParam().Backend);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_GT(Result.Instructions, Options.InstructionQuantum);
}
TEST_P(LinuxProcess, ServiceEventLimitStopsBeforeAnotherGuestReturn) {
  Options.Limits.Events = 1;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::EventLimit) << Result.Diagnostic;
  EXPECT_EQ(Result.Events, Options.Limits.Events);
  EXPECT_EQ(Result.Services.size(), Options.Limits.Events);
  EXPECT_TRUE(Result.StandardOutput.empty());
  EXPECT_FALSE(Result.ExitStatus);
}
TEST_P(LinuxProcess, OutputLimitDoesNotPublishAnOverBudgetWrite) {
  Options.OutputLimit = sizeof(Message) - 2;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit) << Result.Diagnostic;
  EXPECT_TRUE(Result.StandardOutput.empty());
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_FALSE(Result.ExitStatus);
}
TEST_P(LinuxProcess, UnknownServiceDoesNotBecomeSuccessOrHostExecution) {
  Options.Arguments[1] = Unknown;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService)
      << Result.Diagnostic;
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_EQ(Result.Services.size(), 1u);
  EXPECT_EQ(Result.Services[0].Number, UnknownService);
  EXPECT_FALSE(Result.Services[0].Result);
}
TEST_P(LinuxProcess, UnmappedAndReadOnlyStoresRemainGuestCPUFailures) {
  for (const auto *Mode : {Fault, ReadOnly}) {
    Options.Arguments[1] = Mode;
    auto Result = run();
    EXPECT_EQ(Result.Stop, ProcessStopReason::CPUFailure) << Result.Diagnostic;
    EXPECT_FALSE(Result.ExitStatus);
    ASSERT_TRUE(Result.LastCPUExit);
    EXPECT_EQ(Result.LastCPUExit->Kind, ExecutionExitKind::GuestFault);
    EXPECT_TRUE(Result.StandardOutput.empty());
  }
}
TEST_P(LinuxProcess, AWriteCanReturnItsReadablePrefixAndContinue) {
  Options.Arguments[1] = Partial;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, ExitStatus);
  EXPECT_EQ(Result.StandardOutput, Tail);
  ASSERT_FALSE(Result.Services.empty());
  EXPECT_EQ(Result.Services[0].Result, sizeof(Tail) - 1);
}
TEST_P(LinuxProcess, InvalidRequestAndStackStringsFailBeforeExecution) {
  Options.Arguments.push_back(std::string(sizeof(Message), '\0'));
  auto Result = emulateProcess(Path, ProcessProfile::LinuxELF64, Options);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  Options.Arguments = {ExecutableName, Normal};
  Options.StackSize = PageSize - 1;
  Result = emulateProcess(Path, ProcessProfile::LinuxELF64, Options);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
}

#define NEVERD_LINUX_OUTPUT_CASE(Name, Mode, Out, Err)                         \
  TEST_P(LinuxProcess, Vectored##Name) {                                       \
    auto Result = runOutput(Mode);                                             \
    EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;    \
    EXPECT_EQ(Result.ExitStatus, output_fixture::ExitStatus);                  \
    EXPECT_EQ(Result.StandardOutput, std::string(Out, sizeof(Out) - 1));       \
    EXPECT_EQ(Result.StandardError, std::string(Err, sizeof(Err) - 1));        \
  }
#include "fixtures/LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_CASE

TEST_P(LinuxProcess, VectoredBudgetChecksAllBuffersBeforePublishing) {
  Options.OutputLimit = output_fixture::MessageSplit;
  auto Result = runOutput(output_fixture::Gather);
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit) << Result.Diagnostic;
  EXPECT_FALSE(Result.ExitStatus);
  EXPECT_TRUE(Result.StandardOutput.empty());
  EXPECT_TRUE(Result.StandardError.empty());
  ASSERT_EQ(Result.Services.size(), 1u);
  EXPECT_FALSE(Result.Services.front().Result);
}
TEST_P(LinuxProcess, VectoredBudgetRetainsOnlyEarlierCompletedCalls) {
  Options.OutputLimit = sizeof(output_fixture::Message) - 1;
  auto Result = runOutput(output_fixture::Gather);
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit) << Result.Diagnostic;
  EXPECT_FALSE(Result.ExitStatus);
  EXPECT_EQ(Result.StandardOutput,
            std::string(output_fixture::Message,
                        sizeof(output_fixture::Message) - 1));
  EXPECT_TRUE(Result.StandardError.empty());
  ASSERT_EQ(Result.Services.size(), 2u);
  EXPECT_EQ(Result.Services.front().Result,
            sizeof(output_fixture::Message) - 1);
  EXPECT_FALSE(Result.Services.back().Result);
}

INSTANTIATE_TEST_SUITE_P(Backends, LinuxProcess, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation

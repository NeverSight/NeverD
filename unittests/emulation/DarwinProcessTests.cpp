//===- DarwinProcessTests.cpp - Real macOS/iOS Mach-O workloads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinAdvisoryLockTestData.h"
#include "DarwinCallbackTestData.h"
#include "DarwinCreatedAdvisoryLockTestData.h"
#include "DarwinCreatedAttributeTestData.h"
#include "DarwinCreatedSynchronizationTestData.h"
#include "DarwinDataSynchronizationTestData.h"
#include "DarwinDirectoryModeTestData.h"
#include "DarwinDirectoryOwnershipTestData.h"
#include "DarwinDirectoryTimestampTestData.h"
#include "DarwinEntropyTestData.h"
#include "DarwinFileTestData.h"
#include "DarwinFullSynchronizationTestData.h"
#include "DarwinLibSystemFileTestData.h"
#include "DarwinLibSystemMemoryTestData.h"
#include "DarwinModeTestData.h"
#include "DarwinOwnershipTestData.h"
#include "DarwinSymbolicLinkFileSystemTestData.h"
#include "DarwinSymbolicOwnershipTestData.h"
#include "DarwinSynchronizationTestData.h"
#include "DarwinSystemTestData.h"
#include "DarwinTestImage.h"
#include "DarwinTimeTestData.h"
#include "DarwinTimestampMutationTestData.h"
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
struct Profile {
  const char *Name, *File;
  ProcessProfile OS;
  GuestArchitecture ISA;
  ExecutionBackendKind Backend;
};
std::vector<Profile> profiles() {
  std::vector<Profile> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::HVF,
                       ExecutionBackendKind::KVM, ExecutionBackendKind::WHP})
    for (auto P :
         {Profile{"MacOSX64", "macos-x86_64", ProcessProfile::MacOSMachO64,
                  GuestArchitecture::X64, Backend},
          Profile{"MacOSARM64", "macos-arm64", ProcessProfile::MacOSMachO64,
                  GuestArchitecture::AArch64, Backend},
          Profile{"IOSARM64", "ios-arm64", ProcessProfile::IOSMachO64,
                  GuestArchitecture::AArch64, Backend},
          Profile{"SimulatorX64", "ios-simulator-x86_64",
                  ProcessProfile::IOSSimulatorMachO64, GuestArchitecture::X64,
                  Backend},
          Profile{"SimulatorARM64", "ios-simulator-arm64",
                  ProcessProfile::IOSSimulatorMachO64,
                  GuestArchitecture::AArch64, Backend}})
      Result.push_back(P);
  return Result;
}
class DarwinProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
    const auto &P = GetParam();
#ifndef NEVERD_DARWIN_FIXTURE_DIR
    if (requireHvf(P.Backend, P.ISA))
      FAIL() << "Clang and ld64.lld Darwin fixtures are required";
    GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures are unavailable";
#else
    Path = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / P.File;
    ExecutionConfiguration C;
    C.Backend = P.Backend;
    C.Architecture = P.ISA;
    C.Contract = P.ISA == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Options.Backend = P.Backend;
    Options.Arguments = {"guest", "normal", "argument"};
    Options.Environment = {"MODE=test"};
    Options.InstructionQuantum = 7;
#endif
  }
  llvm::Expected<ProcessResult> run(llvm::StringRef Mode) {
    Options.Arguments[1] = Mode.str();
    return emulateProcess(Path, GetParam().OS, Options);
  }
};
// These semantic batches make repeated bounded guest runs. Their host allowance
// is independent of the native cancellation tests' explicit short deadlines.
constexpr uint64_t BatchedProcessTimeoutMicroseconds = 20'000'000;
TEST_P(DarwinProcess, MachSelfPortsPreserveExplicitBitsAndIndependentRuns) {
  Options.InstructionQuantum = 1024;
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const auto check = [&](const ProcessResult &R,
                         const std::array<uint64_t, 3> &Carriers) {
    EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 37);
    EXPECT_TRUE(R.StandardError.empty());
    EXPECT_EQ(R.SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R.Services.size(), 22u);
    for (unsigned Q = 0; Q != 3; ++Q) {
      const uint32_t Low = X64 ? 0x1000000 | (27 + Q) : 0u - (27 + Q);
      for (unsigned I = 0; I != 7; ++I) {
        const auto &E = R.Services[Q * 7 + I];
        const uint64_t Prefix = !I      ? (X64 ? 0 : 0xffffffff00000000ULL)
                                : I < 3 ? 0
                                : I < 5 ? 0x1234567800000000ULL
                                        : 0xffffffff00000000ULL;
        EXPECT_EQ(E.Number, Prefix | Low);
        EXPECT_EQ(E.Result, Carriers[Q]);
        EXPECT_FALSE(E.Error);
        EXPECT_FALSE(E.ThreadID);
        EXPECT_EQ(E.Arguments[0], UINT64_MAX);
        EXPECT_EQ(E.Arguments[X64 ? 2 : 1], 0x1122334455667788ULL);
        EXPECT_EQ(E.Arguments[X64 ? 1 : 2], 0x8877665544332211ULL);
      }
    }
    const auto &Write = R.Services.back();
    EXPECT_EQ(Write.Number, X64 ? 0x2000004u : 4u);
    EXPECT_EQ(Write.Arguments[0], 1u);
    EXPECT_EQ(Write.Result, Write.Arguments[2]);
    EXPECT_EQ(Write.Error, false);
    EXPECT_FALSE(Write.ThreadID);
  };
  Options.DarwinSystem = darwin_test::machSelfPortOptions();
  const std::array<uint64_t, 3> Mixed = {0xffffffff80000001ULL, 0, UINT64_MAX};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("mach-self-ports");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    check(*R, Mixed);
    EXPECT_EQ(R->StandardOutput, "J");
  }
  struct Sample {
    uint32_t Name;
    uint64_t Carrier;
    const char *Hex;
  };
  for (const auto &S :
       {Sample{0, 0, "0000000000000000"}, Sample{1, 1, "0100000000000000"},
        Sample{0x7fffffff, 0x7fffffff, "ffffff7f00000000"},
        Sample{0x80000000, 0xffffffff80000000ULL, "00000080ffffffff"},
        Sample{0x80000001, 0xffffffff80000001ULL, "01000080ffffffff"},
        Sample{UINT32_MAX, UINT64_MAX, "ffffffffffffffff"}}) {
    SCOPED_TRACE(S.Name);
    Options.DarwinSystem->ThreadSelfPort = S.Name;
    Options.DarwinSystem->TaskSelfPort = S.Name;
    Options.DarwinSystem->HostSelfPort = S.Name;
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto R = run("mach-self-port-values");
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      check(*R, {S.Carrier, S.Carrier, S.Carrier});
      EXPECT_EQ(R->StandardOutput,
                llvm::fromHex(std::string(S.Hex) + S.Hex + S.Hex));
      EXPECT_EQ(Options.DarwinSystem->ThreadSelfPort, S.Name);
      EXPECT_EQ(Options.DarwinSystem->TaskSelfPort, S.Name);
      EXPECT_EQ(Options.DarwinSystem->HostSelfPort, S.Name);
    }
  }
  auto Independent = Options;
  Independent.DarwinSystem = darwin_test::machSelfPortOptions();
  auto R = emulateProcess(Path, GetParam().OS, Independent);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  check(*R, Mixed);
  EXPECT_EQ(R->StandardOutput, llvm::fromHex(darwin_test::MachSelfPortsHex));
  EXPECT_EQ(Options.DarwinSystem->ThreadSelfPort, UINT32_MAX);
  const std::array<std::optional<uint32_t> DarwinSystemOptions::*, 3> Members =
      {&DarwinSystemOptions::ThreadSelfPort, &DarwinSystemOptions::TaskSelfPort,
       &DarwinSystemOptions::HostSelfPort};
  const char *Selections[] = {"thread", "task", "host"};
  for (unsigned Q = 0; Q != 3; ++Q) {
    Options.Arguments[2] = Selections[Q];
    for (unsigned State = 0; State != 3; ++State) {
      Options.DarwinSystem.reset();
      if (State)
        Options.DarwinSystem.emplace();
      if (State == 2) {
        Options.DarwinSystem = darwin_test::machSelfPortOptions();
        Options.DarwinSystem->ThreadID = UINT64_MAX;
        ((*Options.DarwinSystem).*Members[Q]).reset();
      }
      auto Missing = run("mach-self-port-missing");
      ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
      EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Missing->Diagnostic,
                std::string("Darwin current-") + Selections[Q] +
                    " Mach port observation is not configured");
      EXPECT_EQ(Missing->StandardOutput, "!");
      ASSERT_EQ(Missing->Services.size(), 2u);
      EXPECT_EQ(Missing->Services.back().Number,
                X64 ? uint64_t(0x1000000 | (27 + Q))
                    : uint64_t(-int64_t(27 + Q)));
      EXPECT_FALSE(Missing->Services.back().Result);
      EXPECT_FALSE(Missing->Services.back().Error);
      EXPECT_FALSE(Missing->Services.back().ThreadID);
    }
  }
}

TEST_P(DarwinProcess, OwnerQueriesKeepPermissionAndUnknownBoundaries) {
  Options.InstructionQuantum = 1024;
  Options.DarwinFiles = darwin_test::ownerQueryOptions();
  Options.DarwinSystem.emplace().Credentials =
      DarwinCredentials{501, 501, 20, 20, {}};
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("owner-queries");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    EXPECT_EQ(R->StandardOutput, "P");
    EXPECT_TRUE(R->StandardError.empty());
    ASSERT_EQ(R->Services.size(), 51u);
    constexpr uint64_t FileErrors[] = {0, 13, 13, 13, 0, 13, 13, 13};
    for (unsigned API = 0; API != 3; ++API) {
      for (unsigned Request = 0; Request != 8; ++Request) {
        const auto &E = R->Services[API * 8 + Request];
        EXPECT_EQ(E.Number, Class + (API ? 466 : 33));
        EXPECT_EQ(E.Result, FileErrors[Request]);
        EXPECT_EQ(E.Error, FileErrors[Request] != 0);
        EXPECT_EQ(E.Arguments[API ? 2 : 1], Request);
      }
    }
    const auto &Raw = R->Services[47];
    EXPECT_EQ(Raw.Number, 0x1234567800000021ULL | Class);
    EXPECT_EQ(Raw.Result, 0u);
    EXPECT_EQ(Raw.Error, false);
    EXPECT_EQ(R->Services[48].Number, Class + 197);
    EXPECT_EQ(R->Services[48].Error, false);
    EXPECT_EQ(R->Services[49].Number, Class + 73);
    EXPECT_EQ(R->Services[49].Result, 0u);
    EXPECT_EQ(R->Services[50].Number, Class + 4);
    EXPECT_EQ(R->Services[50].Result, 1u);
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").Mode, 0100400);
    EXPECT_EQ(Options.DarwinSystem->Credentials->RealUID, 501u);
  }
  const auto Good = Options;
  for (const char *Mode : {"owner-query-open", "owner-query-map"}) {
    Options = Good;
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(
        R->Diagnostic,
        "Darwin static owner queries do not authorize other vnode operations");
    EXPECT_EQ(R->StandardOutput, "!");
    ASSERT_EQ(R->Services.size(), 2u);
    EXPECT_EQ(R->Services.back().Number,
              Class + (llvm::StringRef(Mode).ends_with("map") ? 197 : 5));
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
  for (unsigned Missing = 0; Missing != 6; ++Missing) {
    SCOPED_TRACE(Missing);
    Options = Good;
    Options.Arguments[2] = "/data";
    const char *Reason;
    if (Missing == 0) {
      Options.DarwinSystem.reset();
      Reason = "Darwin owner authorization requires explicit credentials";
    } else if (Missing < 3) {
      Options.DarwinFiles->Metadata.erase(Missing == 1 ? "/" : "/data");
      Reason = "Darwin owner authorization requires explicit object metadata";
    } else {
      Options.DarwinSystem->Credentials->RealUID = Missing == 3 ? 0 : 502;
      if (Missing == 5) {
        Options.DarwinSystem->Credentials->RealUID = 501;
        Options.DarwinFiles->Metadata["/data"].UID = 502;
      }
      Reason =
          "Darwin owner authorization requires a matching nonzero owner UID";
    }
    auto R = run("owner-query-stop");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, Reason);
    EXPECT_EQ(R->StandardOutput, "!");
    ASSERT_EQ(R->Services.size(), 2u);
    EXPECT_EQ(R->Services.back().Number, Class + 33);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
  Options = Good;
  auto Independent = run("owner-queries");
  ASSERT_TRUE(bool(Independent)) << llvm::toString(Independent.takeError());
  EXPECT_EQ(Independent->Stop, ProcessStopReason::Exited)
      << Independent->Diagnostic;
  EXPECT_EQ(Independent->StandardOutput, "P");
}

TEST_P(DarwinProcess, OrdinaryQueriesPreserveGroupKnowledgeAndSelectedSearch) {
  Options.InstructionQuantum = 1024;
  Options.DarwinFiles = darwin_test::ordinaryQueryOptions();
  Options.DarwinSystem.emplace().Credentials =
      DarwinCredentials{501, 502, 30, 20, std::vector<uint32_t>{20, 40}};
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-queries");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    EXPECT_EQ(R->StandardOutput, "G");
    EXPECT_TRUE(R->StandardError.empty());
    ASSERT_EQ(R->Services.size(), 146u);
    constexpr uint64_t Read[] = {0, 13, 13, 13, 0, 13, 13, 13};
    constexpr uint64_t None[] = {0, 13, 13, 13, 13, 13, 13, 13};
    constexpr uint64_t RW[] = {0, 13, 0, 13, 0, 13, 0, 13};
    constexpr uint64_t X[] = {0, 0, 13, 13, 13, 13, 13, 13};
    for (unsigned API = 0; API != 3; ++API)
      for (unsigned Request = 0; Request != 8; ++Request) {
        const uint64_t Expected[] = {
            Read[Request], API == 2 ? RW[Request] : None[Request], X[Request],
            API == 2 ? None[Request] : Read[Request]};
        for (unsigned File = 0; File != 4; ++File) {
          const auto &E = R->Services[API * 32 + Request * 4 + File];
          EXPECT_EQ(E.Number, Class + (API ? 466 : 33));
          EXPECT_EQ(E.Result, Expected[File]);
          EXPECT_EQ(E.Error, Expected[File] != 0);
          EXPECT_EQ(E.Arguments[API ? 2 : 1], Request);
          if (API)
            EXPECT_EQ(E.Arguments[3], API == 2 ? 16u : 0u);
        }
      }
    constexpr uint64_t Remaining[] = {
        0,  13, 13, 13, 0,  13, 13, 13, 0,  13, 13, 13, 0, 13, 13, 13,
        0,  13, 13, 0,  13, 13, 0,  13, 13, 13, 13, 13, 0, 2,  0,  13,
        13, 13, 13, 13, 0,  2,  0,  2,  0,  0,  0,  0,  0, 2};
    for (unsigned I = 0; I != std::size(Remaining); ++I) {
      const auto &E = R->Services[96 + I];
      EXPECT_EQ(E.Result, Remaining[I]);
      EXPECT_EQ(E.Error, Remaining[I] != 0);
    }
    EXPECT_EQ(R->Services[142].Number, 0x1234567800000021ULL | Class);
    EXPECT_EQ(R->Services[142].Result, 0u);
    EXPECT_EQ(R->Services[143].Number, Class + 197);
    EXPECT_EQ(R->Services[143].Error, false);
    EXPECT_EQ(R->Services[144].Number, Class + 73);
    EXPECT_EQ(R->Services[144].Result, 0u);
    EXPECT_EQ(R->Services[145].Number, Class + 4);
    EXPECT_EQ(R->Services[145].Result, 1u);
    EXPECT_EQ(
        Options.DarwinSystem->Credentials->GroupAccessList,
        std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{20, 40}));
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/group").Mode, 0100060);
  }
  const auto Good = Options;
  for (const char *Mode : {"ordinary-query-open", "ordinary-query-map"}) {
    Options = Good;
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, "Darwin static ordinary queries do not authorize "
                             "other vnode operations");
    EXPECT_EQ(R->StandardOutput, "!");
    ASSERT_EQ(R->Services.size(), 2u);
    EXPECT_EQ(R->Services.back().Number,
              Class + (llvm::StringRef(Mode).ends_with("map") ? 197 : 5));
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
  for (unsigned Missing = 0; Missing != 5; ++Missing) {
    SCOPED_TRACE(Missing);
    Options = Good;
    Options.Arguments[2] = Missing == 0 ? "/unknown" : "/agreement";
    const char *Reason =
        "Darwin ordinary authorization requires known group membership";
    if (Missing == 1) {
      Options.DarwinSystem.reset();
      Reason = "Darwin ordinary authorization requires explicit credentials";
    } else if (Missing == 2 || Missing == 3) {
      Options.DarwinFiles->Metadata.erase(Missing == 2 ? "/" : "/agreement");
      Reason =
          "Darwin ordinary authorization requires explicit object metadata";
    } else if (Missing == 4) {
      Options.DarwinSystem->Credentials->EffectiveUID = 0;
      Reason = "Darwin ordinary authorization requires a nonzero selected UID";
    }
    auto R = run("ordinary-query-unknown");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, Reason);
    EXPECT_EQ(R->StandardOutput, "!");
    ASSERT_EQ(R->Services.size(), 2u);
    EXPECT_EQ(R->Services.back().Number, Class + 466);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
  Options = Good;
  auto Independent = run("ordinary-queries");
  ASSERT_TRUE(bool(Independent)) << llvm::toString(Independent.takeError());
  EXPECT_EQ(Independent->Stop, ProcessStopReason::Exited)
      << Independent->Diagnostic;
  EXPECT_EQ(Independent->StandardOutput, "G");
}

TEST_P(DarwinProcess, OrdinaryQueriesUseExplicitMembershipUIDWithoutResolver) {
  Options.InstructionQuantum = 1024;
  Options.DarwinFiles = darwin_test::closedGroupQueryOptions();
  Options.DarwinSystem.emplace().Credentials = DarwinCredentials{
      501, 502, 30, 20, std::vector<uint32_t>{20, 40}, 4294967195u};
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr uint64_t Read[] = {0, 13, 13, 13, 0, 13, 13, 13};
  constexpr uint64_t None[] = {0, 13, 13, 13, 13, 13, 13, 13};
  constexpr uint64_t RW[] = {0, 13, 0, 13, 0, 13, 0, 13};
  constexpr uint64_t X[] = {0, 0, 13, 13, 13, 13, 13, 13};
  constexpr uint64_t Remaining[] = {
      0,  13, 13, 13, 0,  13, 13, 13, 0,  13, 13, 13, 0, 13, 13, 13,
      0,  13, 13, 0,  13, 13, 0,  13, 13, 13, 13, 13, 0, 2,  0,  13,
      13, 13, 13, 13, 0,  2,  0,  2,  0,  0,  0,  0,  0, 2};
  constexpr uint64_t Search[] = {2, 0, 0, 0, 13, 13, 0, 0};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-queries-closed-groups");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    EXPECT_EQ(R->StandardOutput, "GN");
    EXPECT_TRUE(R->StandardError.empty());
    ASSERT_EQ(R->Services.size(), 173u);
    for (unsigned API = 0; API != 3; ++API)
      for (unsigned Request = 0; Request != 8; ++Request) {
        const uint64_t Expected[] = {
            Read[Request], API == 2 ? RW[Request] : None[Request], X[Request],
            API == 2 ? None[Request] : Read[Request]};
        for (unsigned File = 0; File != 4; ++File) {
          const auto &E = R->Services[API * 32 + Request * 4 + File];
          EXPECT_EQ(E.Number, Class + (API ? 466 : 33));
          EXPECT_EQ(E.Result, Expected[File]);
          EXPECT_EQ(E.Error, Expected[File] != 0);
          EXPECT_EQ(E.Arguments[API ? 2 : 1], Request);
          if (API)
            EXPECT_EQ(E.Arguments[3], API == 2 ? 16u : 0u);
        }
      }
    for (unsigned I = 0; I != std::size(Remaining); ++I) {
      const auto &E = R->Services[96 + I];
      EXPECT_EQ(E.Result, Remaining[I]);
      EXPECT_EQ(E.Error, Remaining[I] != 0);
    }
    EXPECT_EQ(R->Services[142].Number, 0x1234567800000021ULL | Class);
    EXPECT_EQ(R->Services[142].Result, 0u);
    EXPECT_EQ(R->Services[143].Number, Class + 197);
    EXPECT_EQ(R->Services[143].Error, false);
    EXPECT_EQ(R->Services[144].Number, Class + 73);
    EXPECT_EQ(R->Services[144].Result, 0u);
    EXPECT_EQ(R->Services[145].Number, Class + 4);
    EXPECT_EQ(R->Services[145].Result, 1u);
    for (unsigned I = 0; I != 2; ++I) {
      const auto &E = R->Services[146 + I];
      EXPECT_EQ(E.Number, Class + 466);
      EXPECT_EQ(E.Result, 13u);
      EXPECT_EQ(E.Error, true);
      EXPECT_EQ(E.Arguments[2], I ? 6u : 2u);
      EXPECT_EQ(E.Arguments[3], 16u);
    }
    for (unsigned API = 0; API != 3; ++API)
      for (unsigned I = 0; I != 8; ++I) {
        const auto &E = R->Services[148 + API * 8 + I];
        EXPECT_EQ(E.Number, Class + (API ? 466 : 33));
        EXPECT_EQ(E.Result, Search[I]);
        EXPECT_EQ(E.Error, Search[I] != 0);
        EXPECT_EQ(E.Arguments[API ? 2 : 1], I == 1 || I == 6 ? 4u : 0u);
        if (API)
          EXPECT_EQ(E.Arguments[3], API == 2 ? 16u : 0u);
      }
    EXPECT_EQ(R->Services[172].Number, Class + 4);
    EXPECT_EQ(R->Services[172].Result, 1u);
    EXPECT_EQ(Options.DarwinSystem->Credentials->GroupMembershipUID,
              4294967195u);
    EXPECT_EQ(
        Options.DarwinSystem->Credentials->GroupAccessList,
        std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{20, 40}));
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/external").Mode, 040001);
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/blocked").Mode, 040010);
  }
  const auto Good = Options;
  const std::optional<uint32_t> UnknownUIDs[] = {std::nullopt, 0u,
                                                 uint32_t(INT32_MAX)};
  for (auto MembershipUID : UnknownUIDs) {
    Options = Good;
    Options.DarwinSystem->Credentials->GroupMembershipUID = MembershipUID;
    auto R = run("ordinary-queries-closed-groups");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic,
              "Darwin ordinary authorization requires known group membership");
    EXPECT_EQ(R->StandardOutput, "G");
    ASSERT_EQ(R->Services.size(), 147u);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
  for (unsigned Missing = 0; Missing != 3; ++Missing) {
    Options = Good;
    Options.Arguments[2] = "/unknown";
    const char *Reason =
        "Darwin ordinary authorization requires known group membership";
    if (Missing == 0)
      Options.DarwinSystem->Credentials->GroupAccessList.reset();
    else if (Missing == 1) {
      Options.DarwinSystem->Credentials->EffectiveUID = 0;
      Reason = "Darwin ordinary authorization requires a nonzero selected UID";
    } else {
      Options.DarwinFiles->Metadata.erase("/unknown");
      Reason =
          "Darwin ordinary authorization requires explicit object metadata";
    }
    auto R = run("ordinary-query-unknown");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, Reason);
    EXPECT_EQ(R->StandardOutput, "!");
    ASSERT_EQ(R->Services.size(), 2u);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
  for (const char *Mode : {"ordinary-query-open", "ordinary-query-map"}) {
    Options = Good;
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, "Darwin static ordinary queries do not authorize "
                             "other vnode operations");
    EXPECT_EQ(R->StandardOutput, "!");
    ASSERT_EQ(R->Services.size(), 2u);
  }
  Options = Good;
  auto Again = run("ordinary-queries-closed-groups");
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  EXPECT_EQ(Again->Stop, ProcessStopReason::Exited) << Again->Diagnostic;
  EXPECT_EQ(Again->StandardOutput, "GN");
  EXPECT_EQ(Options.DarwinSystem->Credentials->GroupMembershipUID, 4294967195u);
}

TEST_P(DarwinProcess, ThreadIdentityPreservesExplicitBitsAndIndependentRuns) {
  Options.InstructionQuantum = 1024;
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  const auto check = [&](const ProcessResult &R, uint64_t ID) {
    EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 37);
    EXPECT_TRUE(R.StandardError.empty());
    EXPECT_EQ(R.SelectedBackend, GetParam().Backend);
    std::vector<const ProcessServiceEvent *> Calls;
    for (const auto &E : R.Services)
      if (uint32_t(E.Number) == Class + 372)
        Calls.push_back(&E);
    ASSERT_EQ(Calls.size(), 10u);
    for (unsigned I = 0; I != 10; ++I) {
      EXPECT_EQ(Calls[I]->Result, ID);
      EXPECT_EQ(Calls[I]->Error, false);
      EXPECT_FALSE(Calls[I]->ThreadID);
      const uint64_t Prefix = I < 4   ? 0
                              : I < 7 ? 0x1234567800000000ULL
                                      : 0xffffffff00000000ULL;
      EXPECT_EQ(Calls[I]->Number, Prefix | (Class + 372));
    }
    EXPECT_EQ(Calls.front()->Arguments[0], UINT64_MAX);
    EXPECT_EQ(Calls.front()->Arguments[1], 0x8000000000000000ULL);
    EXPECT_EQ(Calls.front()->Arguments[2], 0x1122334455667788ULL);
    EXPECT_EQ(Calls.front()->Arguments[3], 1u);
    EXPECT_EQ(Calls.front()->Arguments[4], UINT64_MAX);
    EXPECT_EQ(Calls.front()->Arguments[5], 0x123456789abcdef0ULL);
  };
  Options.DarwinSystem = darwin_test::threadIdentityOptions();
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("thread-identity");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    check(*R, 0xfedcba9876543210ULL);
    EXPECT_EQ(R->StandardOutput, "T");
  }
  struct Sample {
    uint64_t ID;
    const char *Hex;
  };
  for (const auto &S :
       {Sample{0, "0000000000000000"}, Sample{1, "0100000000000000"},
        Sample{0x100000001ULL, "0100000001000000"},
        Sample{0x8000000000000000ULL, "0000000000000080"},
        Sample{UINT64_MAX, "ffffffffffffffff"}}) {
    SCOPED_TRACE(S.ID);
    Options.DarwinSystem->ThreadID = S.ID;
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto R = run("thread-identity-value");
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      check(*R, S.ID);
      EXPECT_EQ(R->StandardOutput, llvm::fromHex(S.Hex));
      EXPECT_EQ(Options.DarwinSystem->ThreadID, S.ID);
    }
  }
  auto Independent = Options;
  Independent.DarwinSystem->ThreadID = 0xfedcba9876543210ULL;
  auto R = emulateProcess(Path, GetParam().OS, Independent);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  check(*R, 0xfedcba9876543210ULL);
  EXPECT_EQ(R->StandardOutput, llvm::fromHex(darwin_test::ThreadIdentityHex));
  EXPECT_EQ(Options.DarwinSystem->ThreadID, UINT64_MAX);
  for (bool Present : {false, true}) {
    Options.DarwinSystem.reset();
    if (Present)
      Options.DarwinSystem.emplace();
    auto Missing = run("thread-identity-missing");
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic,
              "Darwin current-thread identity observation is not configured");
    EXPECT_EQ(Missing->StandardOutput, "!");
    ASSERT_EQ(Missing->Services.size(), 2u);
    EXPECT_EQ(Missing->Services.back().Number, Class + 372);
    EXPECT_FALSE(Missing->Services.back().Result);
    EXPECT_FALSE(Missing->Services.back().Error);
    EXPECT_FALSE(Missing->Services.back().ThreadID);
  }
}

TEST_P(DarwinProcess, EntropyReplayKeepsBytesFaultOrderAndFreshRunLifetime) {
  Options.InstructionQuantum = 1024;
  Options.DarwinSystem = darwin_test::entropyReplayOptions();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("entropy-replay");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, "R");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    std::vector<const ProcessServiceEvent *> Calls;
    for (const auto &E : R->Services)
      if (E.Number == Class + 500)
        Calls.push_back(&E);
    ASSERT_EQ(Calls.size(), 28u);
    for (unsigned I = 0; I != 24; ++I) {
      const bool Zero = I % 6 == 0;
      EXPECT_EQ(Calls[I]->Arguments[1] == 0, Zero);
      EXPECT_EQ(Calls[I]->Result, Zero ? 0 : 22);
      EXPECT_EQ(Calls[I]->Error, !Zero);
    }
    EXPECT_EQ(Calls[24]->Arguments[0], 0u);
    EXPECT_EQ(Calls[24]->Result, 14u);
    EXPECT_EQ(Calls[24]->Error, true);
    for (unsigned I = 25; I != 28; ++I) {
      EXPECT_EQ(Calls[I]->Result, 0u);
      EXPECT_EQ(Calls[I]->Error, false);
    }
  }
  for (const auto &[Mode, Diagnostic] :
       {std::pair{"entropy-missing",
                  "Darwin entropy observations are not configured"},
        std::pair{"entropy-exhausted",
                  "Darwin entropy observations are exhausted"},
        std::pair{
            "entropy-mismatch",
            "Darwin entropy observation length does not match the request"},
        std::pair{"entropy-partial",
                  "Darwin partial entropy output is unsupported"}}) {
    Options.DarwinSystem = darwin_test::entropyReplayOptions();
    if (llvm::StringRef(Mode) == "entropy-missing")
      Options.DarwinSystem->EntropyReads.reset();
    if (llvm::StringRef(Mode) == "entropy-exhausted")
      Options.DarwinSystem->EntropyReads->resize(1);
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, Diagnostic);
    EXPECT_EQ(R->StandardOutput, "!");
    EXPECT_EQ(R->Services.back().Number, Class + 500);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
}

TEST_P(DarwinProcess, StartupDataBSSCarryAndBinaryOutput) {
  auto Result = run("normal");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, std::string("darwin\0\xff\n", 9));
  ASSERT_EQ(Result->Services.size(), 4u);
  EXPECT_EQ(Result->Services[0].Result, 9);
  EXPECT_EQ(Result->Services[0].Error, true);
  EXPECT_EQ(Result->Services[1].Result, 1000);
  EXPECT_EQ(Result->Services[1].Error, false);
  EXPECT_EQ(Result->Services[2].Result, 14);
  EXPECT_EQ(Result->Services[2].Error, true);
  EXPECT_EQ(Result->Services[3].Result, 9);
  EXPECT_EQ(Result->Services[3].Error, false);
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  EXPECT_EQ(Result->Profile, GetParam().OS);
}
TEST_P(DarwinProcess, UnixThreadReceivesArgcAtTheInitialStackPointer) {
  using namespace llvm::MachO;
  const auto &P = GetParam();
  const auto Platform = P.OS == ProcessProfile::MacOSMachO64 ? PLATFORM_MACOS
                        : P.OS == ProcessProfile::IOSMachO64
                            ? PLATFORM_IOS
                            : PLATFORM_IOSSIMULATOR;
  darwin_test::Image I(P.ISA == GuestArchitecture::X64, Platform);
  darwin_test::TemporaryImage File(I);
  auto Result = emulateProcess(File.path(), P.OS, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, Options.Arguments.size());
  EXPECT_EQ(Result->Services.size(), 1u);
  EXPECT_EQ(Result->SelectedBackend, P.Backend);
  EXPECT_EQ(Result->Profile, P.OS);
}
TEST_P(DarwinProcess, MainReturnAndBSDExitHaveRealStatus) {
  for (auto Mode : {"return", "exit", "identity"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
  }
}
TEST_P(DarwinProcess,
       FilesShareOffsetsAcrossDupAndKeepPositionedReadsIndependent) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  for (auto Mode : {"files", "files-nocancel"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput, "f");
    ASSERT_FALSE(Result->Services.empty());
    EXPECT_EQ(Result->Services.front().Number,
              (GetParam().ISA == GuestArchitecture::X64 ? 0x2000000u : 0u) +
                  (Mode == llvm::StringRef("files") ? 5u : 398u));
    EXPECT_EQ(Result->Services.front().Error, false);
  }
}
TEST_P(DarwinProcess,
       WritableFilesPreserveSharedContentsOffsetsAndNativeErrorOrder) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.Arguments[2] = "/data";
  for (auto Mode : {"writable-files", "writable-files-nocancel"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput, "00006e");
    EXPECT_TRUE(Result->StandardError.empty());
    const uint64_t Class =
        GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
    const uint64_t Pwrite =
        Class + (llvm::StringRef(Mode).ends_with("nocancel") ? 415 : 154);
    unsigned Positioned = 0;
    for (const auto &Event : Result->Services)
      if (Event.Number == Pwrite)
        ++Positioned;
    EXPECT_EQ(Positioned, 5u);
  }
}
TEST_P(DarwinProcess,
       VirtualFileMetadataTracksConfiguredAllocationAndSharedLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->MutationPolicies["/data"] = darwin_test::MutationPolicy;
  Options.Arguments[2] = "/data";
  auto Result = run("virtual-file-metadata");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
            darwin_test::MutationMetadataHex);
  EXPECT_TRUE(Result->StandardError.empty());
}
TEST_P(DarwinProcess,
       SparseSeekPreservesNativeBoundariesAndDescriptionLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->MutationPolicies["/data"] = darwin_test::MutationPolicy;
  Options.Arguments[2] = "/data";
  auto Result = run("sparse-file-seek");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "s");
  EXPECT_TRUE(Result->StandardError.empty());
}
TEST_P(DarwinProcess,
       RenamePreservesReplacementIdentityPathsAndMappedLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->MutationPolicies["/data"] = darwin_test::MutationPolicy;
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->Directories.insert("/");
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->SwapRenameDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
  Options.Arguments[2] = "/data";
  auto Result = run("renamed-file");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "r");
  EXPECT_TRUE(Result->StandardError.empty());
}

TEST_P(DarwinProcess,
       DirectoryRenamePreservesSubtreesAndRetainedObjectParents) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
  Options.Arguments[2] = "/data";
  auto Result = run("renamed-directory");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "d");
  EXPECT_TRUE(Result->StandardError.empty());
}

TEST_P(DarwinProcess, DirectorySwapPreservesBothSubtreesAndMixedObjectState) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Directories.insert("/");
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->SwapRenameDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
  Options.Arguments[2] = "/data";
  auto Result = run("swapped-directory");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "s");
  EXPECT_TRUE(Result->StandardError.empty());
}

TEST_P(DarwinProcess,
       InitialDirectorySwapPreservesRootsSubtreesAndMixedObjects) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Directories = {"/", "/empty"};
  Options.DarwinFiles->MutableDirectories = {"/", "/empty"};
  Options.DarwinFiles->SwapRenameDirectories = {"/", "/empty"};
  Options.DarwinFiles->ExchangeableDirectories.insert("/empty");
  Options.Arguments[2] = "/data";
  auto Result = run("initial-directory-swap");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "q");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}

TEST_P(DarwinProcess, InitialDirectoryMovePreservesObjectsGrantsAndNameReuse) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Directories.insert("/empty");
  Options.DarwinFiles->MutableDirectories = {"/", "/empty"};
  Options.DarwinFiles->MovableDirectories.insert("/empty");
  Options.Arguments[2] = "/data";
  auto Result = run("initial-directory-move");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "p");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}

TEST_P(DarwinProcess, CreationMetadataUsesExplicitIdentityUmaskAndParentGroup) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
  Options.Arguments[2] = "/data";
  for (const char *Mode :
       {"created-file-metadata", "virtual-created-metadata"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "created-file-metadata"
                  ? "71"
                  : darwin_test::CreationMetadataHex);
    EXPECT_TRUE(Result->StandardError.empty());
  }
}

TEST_P(DarwinProcess,
       NamespaceCreationMetadataPreservesObjectIdentityAndVirtualRecords) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->Directories.insert("/");
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->SwapRenameDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::NamespaceCreationPolicy;
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"created-namespace-metadata", "virtual-created-namespace-metadata"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "created-namespace-metadata"
                  ? "4e"
                  : darwin_test::NamespaceMetadataHex);
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_TRUE(llvm::any_of(Result->Services, [](const auto &E) {
      return (uint32_t(E.Number) & 0x00ffffff) == 474 && E.Result == 17 &&
             E.Error == true;
    }));
    EXPECT_TRUE(llvm::any_of(Result->Services, [](const auto &E) {
      return (uint32_t(E.Number) & 0x00ffffff) == 488 && E.Arguments[4] == 18 &&
             E.Result == 0 && E.Error == false;
    }));
  }
}

TEST_P(DarwinProcess,
       VirtualEnumerationTracksNamespaceChangesAndRetainedDirectories) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->Directories.insert("/");
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->SwapRenameDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::NamespaceCreationPolicy;
  Options.DarwinFiles->DirectoryEnumerationPolicies["/"] =
      darwin_test::EnumerationPolicy;
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"directory-enumeration-mutations", "virtual-directory-enumeration"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "directory-enumeration-mutations"
                  ? "45"
                  : darwin_test::EnumerationMetadataHex);
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_TRUE(llvm::any_of(Result->Services, [](const auto &E) {
      return (uint32_t(E.Number) & 0x00ffffff) == 344 && E.Result == 0 &&
             E.Error == false;
    }));
    EXPECT_TRUE(llvm::any_of(Result->Services, [](const auto &E) {
      return (uint32_t(E.Number) & 0x00ffffff) == 488 && E.Arguments[4] == 2 &&
             E.Result == 0 && E.Error == false;
    }));
  }
}

TEST_P(DarwinProcess,
       InitialDirectoryMutationKeepsFullStatAndIndependentCreationRules) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->Directories = {"/", "/empty"};
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::NamespaceCreationPolicy;
  Options.DarwinFiles->DirectoryMutationPolicies["/"] =
      darwin_test::InitialDirectoryMutationPolicy;
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"initial-directory-metadata", "virtual-initial-directory-metadata"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "initial-directory-metadata"
                  ? "49"
                  : darwin_test::InitialDirectoryMetadataHex);
    EXPECT_TRUE(Result->StandardError.empty());
  }
}

TEST_P(DarwinProcess, MutableInitialLinksKeepIdentityAndReferentLifetime) {
  Options.DarwinFiles = darwin_test::mutableInitialSymbolicLinkOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"mutable-initial-links", "virtual-mutable-initial-links"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "mutable-initial-links"
                  ? "4d"
                  : darwin_test::InitialSymbolicLinkMetadataHex);
    EXPECT_TRUE(Result->StandardError.empty());
  }
}

TEST_P(DarwinProcess, DirectoryLinkRootsKeepIdentityAndReferentLifetime) {
  Options.DarwinFiles = darwin_test::directoryLinkRootOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"directory-link-roots", "virtual-directory-link-roots"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "directory-link-roots"
                  ? "52"
                  : darwin_test::DirectoryLinkRootMetadataHex);
    EXPECT_TRUE(Result->StandardError.empty());
  }
}

TEST_P(DarwinProcess, CommonAttributesPreserveRecordAndDescriptorState) {
  Options.DarwinFiles = darwin_test::commonAttributesOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode : {"common-attributes", "common-attributes-values",
                           "common-attributes-unsupported"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic;
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "common-attributes-values"
                  ? darwin_test::CommonAttributesHex
                  : "41");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin selected file attributes are not modeled");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 220u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}
TEST_P(DarwinProcess, NonblockingDescriptorsKeepNativeControlState) {
  Options.DarwinFiles = darwin_test::nonblockingDescriptorOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"nonblocking-descriptors", "nonblocking-flags-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic;
    EXPECT_EQ(Result->StandardOutput, "N");
    EXPECT_TRUE(Result->StandardError.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic, "unsupported Darwin fcntl command");
      ASSERT_FALSE(Result->Services.empty());
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 92u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else
      EXPECT_EQ(Result->ExitStatus, 37);
  }
}

TEST_P(DarwinProcess, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder) {
  Options.DarwinFiles = darwin_test::symbolicDescriptorOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"symbolic-descriptors", "symbolic-descriptors-values",
        "symbolic-descriptors-name-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic << "; instructions=" << Result->Instructions
        << "; services=" << Result->Services.size();
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "symbolic-descriptors-values"
                  ? darwin_test::SymbolicDescriptorHex
                  : "53");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin multiple-name vnode observations are unsupported");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 92u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, HardLinksShareObjectsAndRetainExplicitNameBoundary) {
  Options.DarwinFiles = darwin_test::hardLinksOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode :
       {"hard-links", "hard-links-values", "hard-links-name-unsupported",
        "hard-links-attributes-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic << "; instructions=" << Result->Instructions
        << "; services=" << Result->Services.size();
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "hard-links-values"
                  ? darwin_test::HardLinksHex
                  : "48");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin multiple-name vnode observations are unsupported");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff,
                llvm::StringRef(Mode) == "hard-links-name-unsupported" ? 92u
                                                                       : 228u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, XattrMutationsPreserveInputAuthorityAndObjectLifetime) {
  Options.DarwinFiles = darwin_test::xattrMutationsOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode : {"xattr-mutations", "xattr-mutations-values",
                           "xattr-mutations-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic << "; instructions=" << Result->Instructions
        << "; services=" << Result->Services.size();
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "xattr-mutations-values"
                  ? darwin_test::XattrMutationsHex
                  : "56");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin extended-attribute mutation is not authorized");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 236u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, BulkAttributesPreserveGroupsAndSharedDirectoryState) {
  Options.DarwinFiles = darwin_test::bulkAttributesOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode : {"bulk-attributes", "bulk-attributes-values",
                           "bulk-attributes-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic << "; instructions=" << Result->Instructions
        << "; services=" << Result->Services.size();
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "bulk-attributes-values"
                  ? darwin_test::BulkAttributesHex
                  : "42");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin selected file attributes are not modeled");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 461u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, AttributeNamesPreserveReferencesAndRetainedIdentity) {
  Options.DarwinFiles = darwin_test::attributeNamesOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode : {"attribute-names", "attribute-names-values",
                           "attribute-names-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic << "; instructions=" << Result->Instructions
        << "; services=" << Result->Services.size();
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "attribute-names-values"
                  ? darwin_test::AttributeNamesHex
                  : "4e");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(
          Result->Diagnostic,
          "Darwin object name is outside the bounded UTF-8 catalogue contract");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 220u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, ExtendedAttributesPreserveValuesNamesAndObjectLifetime) {
  Options.DarwinFiles = darwin_test::extendedAttributesOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode : {"extended-attributes", "extended-attributes-values",
                           "extended-attributes-unsupported"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic << "; instructions=" << Result->Instructions
        << "; services=" << Result->Services.size();
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "extended-attributes-values"
                  ? darwin_test::ExtendedAttributesHex
                  : "58");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin extended-attribute observations are unknown");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 234u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, KernelPathConfPreservesLookupAndDescriptorState) {
  Options.DarwinFiles = darwin_test::kernelPathConfOptions();
  Options.Arguments[2] = "/data";
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  for (const char *Mode : {"kernel-pathconf", "kernel-pathconf-values",
                           "kernel-pathconf-unsupported"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const bool Unknown = llvm::StringRef(Mode).ends_with("-unsupported");
    ASSERT_EQ(Result->Stop, Unknown ? ProcessStopReason::UnsupportedService
                                    : ProcessStopReason::Exited)
        << Result->Diagnostic;
    EXPECT_EQ(llvm::toHex(Result->StandardOutput, true),
              llvm::StringRef(Mode) == "kernel-pathconf-values"
                  ? darwin_test::KernelPathConfHex
                  : "43");
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    if (Unknown) {
      EXPECT_EQ(Result->Diagnostic,
                "Darwin filesystem-dependent pathconf selector is not modeled");
      const auto &Last = Result->Services.back();
      EXPECT_EQ(uint32_t(Last.Number) & 0x00ffffff, 191u);
      EXPECT_EQ(Last.Arguments[1], 4u);
      EXPECT_FALSE(Last.Result);
      EXPECT_FALSE(Last.Error);
    } else {
      EXPECT_EQ(Result->ExitStatus, 37);
    }
  }
}

TEST_P(DarwinProcess, CreatePreservesExclusiveChecksAndReusedNameLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->MutationPolicies["/data"] = darwin_test::MutationPolicy;
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.Arguments[2] = "/data";
  auto Result = run("created-file");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "c");
  EXPECT_TRUE(Result->StandardError.empty());
}

TEST_P(DarwinProcess, UnlinkPreservesOpenObjectsAndNativeNameLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->MutationPolicies["/data"] = darwin_test::MutationPolicy;
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.Arguments[2] = "/data";
  auto Result = run("unlinked-file");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "u");
  EXPECT_TRUE(Result->StandardError.empty());
}
TEST_P(DarwinProcess,
       PrivateFileMappingsRetainCopiesAfterCloseAndPreserveOffsets) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  auto Result = run("file-mapping");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "m");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}
TEST_P(DarwinProcess, DirectoryRelativePathsAndCWDMatchNativeLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::metadata();
  Options.DarwinFiles->Directories.insert("/empty");
  Options.DarwinFiles->WorkingDirectory = "/empty";
  Options.Arguments[2] = "/data";
  auto Result = run("directories");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "d");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}
TEST_P(DarwinProcess, DirectoryEnumerationPreservesRecordsCookiesAndCopyOrder) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::metadata();
  Options.DarwinFiles->Directories.insert("/empty");
  Options.DarwinFiles->DirectoryContents["/"] =
      darwin_test::directoryContents();
  Options.Arguments[2] = "/data";
  auto Result = run("directory-entries");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "e");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}
TEST_P(DarwinProcess, InitialDirectoryRemovalRetainsObjectsAfterNameReuse) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->Directories.insert("/empty");
  Options.DarwinFiles->RemovableDirectories.insert("/empty");
  Options.Arguments[2] = "/data";
  auto Result = run("initial-directory-removal");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "j");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {137u, 475u, 472u, 13u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Result == 0 &&
             Event.Error == false;
    })) << Number;
}
TEST_P(DarwinProcess,
       DeletedDirectoriesRetainObjectsParentsAndWorkingDirectory) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.Arguments[2] = "/data";
  auto Result = run("deleted-directories");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "h");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {475u, 472u, 13u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Result == 0 &&
             Event.Error == false;
    })) << Number;
}
TEST_P(DarwinProcess, DirectoryMutationsPreserveNamespaceAndOrphanFiles) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.Arguments[2] = "/data";
  auto Result = run("directory-mutations");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "m");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {136u, 475u, 137u, 472u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Result == 0 &&
             Event.Error == false;
    })) << Number;
}
TEST_P(DarwinProcess,
       SymbolicLinksPreserveRawTargetsMetadataAndNoFollowPolicies) {
  Options.DarwinFiles = darwin_test::symbolicLinkOptions();
  Options.Arguments[2] = "/data";
  auto Result = run("symbolic-links");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "y");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {58u, 473u, 470u, 466u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Error == false;
    })) << Number;
}

TEST_P(DarwinProcess, FixedLinksObserveMutableTargetsAndRetainOldObjects) {
  Options.DarwinFiles = darwin_test::mixedSymbolicLinkOptions();
  Options.Arguments[2] = "/work/data";
  auto Result = run("symbolic-link-mutations");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "z");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {463u, 465u, 472u, 473u, 475u, 197u, 73u, 13u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Error == false;
    })) << Number;
}

TEST_P(DarwinProcess, FileExistenceUsesNativeModeBitsAndPathErrorOrder) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  auto Result = run("file-access");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "a");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {33u, 466u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Result == 0 &&
             Event.Error == false;
    })) << Number;
  for (auto Number : {463u, 464u})
    for (uint32_t Flags : {0x100u, 0x20000000u})
      EXPECT_TRUE(llvm::any_of(Result->Services,
                               [&](const auto &Event) {
                                 return Event.Number == Class + Number &&
                                        uint32_t(Event.Arguments[2]) ==
                                            (Flags | 0x1000000) &&
                                        Event.Result && *Event.Result >= 3 &&
                                        Event.Error == false;
                               }))
          << Number << ":" << Flags;
  EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
    return Event.Number == Class + 92 && Event.Arguments[1] == 3 &&
           Event.Result == 0 && Event.Error == false;
  }));
}
TEST_P(DarwinProcess,
       VectorIOPreservesCopyOrderFaultPrefixesAndAggregateOffsets) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.Arguments[2] = "/data";
  auto Result = run("vectored-io");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "v!");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {120u, 121u, 411u, 412u, 540u, 541u, 542u, 543u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Result &&
             Event.Error == false;
    })) << Number;
}
TEST_P(DarwinProcess, CredentialsKeepGroupQueriesAndCreationOwnershipCoherent) {
  for (unsigned Missing = 0; Missing != 3; ++Missing) {
    Options.DarwinSystem.reset();
    if (Missing)
      Options.DarwinSystem.emplace();
    if (Missing == 2) {
      Options.DarwinSystem = darwin_test::credentialOptions();
      Options.DarwinSystem->Credentials->GroupAccessList.reset();
    }
    auto R = run("credentials");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, "Darwin group access list is not configured");
    EXPECT_TRUE(R->StandardOutput.empty());
  }
  Options.DarwinSystem = darwin_test::credentialOptions();
  for (const std::optional<uint32_t> MembershipUID :
       {std::optional<uint32_t>{}, std::optional<uint32_t>{4294967195u}}) {
    Options.DarwinSystem->Credentials->GroupMembershipUID = MembershipUID;
    for (const char *Mode : {"credentials", "virtual-credentials"}) {
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 37);
      EXPECT_EQ(R->StandardOutput,
                llvm::StringRef(Mode) == "credentials"
                    ? "k"
                    : llvm::fromHex(darwin_test::CredentialsHex));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      EXPECT_TRUE(llvm::any_of(R->Services, [](const auto &E) {
        return (uint32_t(E.Number) & 0x00ffffff) == 79 &&
               E.Arguments[0] == 0x1234567800001000ULL && E.Result == 5 &&
               E.Error == false;
      }));
      EXPECT_TRUE(llvm::any_of(R->Services, [](const auto &E) {
        return (uint32_t(E.Number) & 0x00ffffff) == 79 &&
               E.Arguments[0] == 0x12345678ffffffffULL && E.Result == 22 &&
               E.Error == true;
      }));
    }
  }
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::mutationMetadata();
  Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
  Options.DarwinFiles->MutableDirectories.insert("/");
  Options.DarwinFiles->InitialUmask = 0027;
  Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
  Options.Arguments[2] = "/data";
  for (uint32_t EffectiveUID : {202u, 0u}) {
    Options.DarwinSystem->Credentials->EffectiveUID = EffectiveUID;
    auto R = run("created-file-metadata");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, "q");
    EXPECT_TRUE(llvm::any_of(R->Services, [&](const auto &E) {
      return (uint32_t(E.Number) & 0x00ffffff) == 25 &&
             E.Result == EffectiveUID && E.Error == false;
    }));
  }
  EXPECT_EQ(Options.DarwinSystem->Credentials->RealUID, 101u);
  EXPECT_EQ(*Options.DarwinSystem->Credentials->GroupAccessList,
            (std::vector<uint32_t>{404, 0, INT32_MAX, 7, 7}));
  EXPECT_EQ(Options.DarwinFiles->Metadata.at("/").GID, 0xfedcba98u);
}

TEST_P(DarwinProcess,
       ResourceUsagePreservesIndependentSnapshotsSignedLayoutAndCopyOrder) {
  for (bool EmptySystem : {false, true}) {
    if (EmptySystem)
      Options.DarwinSystem = DarwinSystemOptions{};
    auto Missing = run("resource-usage");
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic,
              "Darwin resource usage observation is not configured");
    EXPECT_TRUE(Missing->StandardOutput.empty());
  }
  for (bool SelfOnly : {true, false}) {
    Options.DarwinSystem = darwin_test::resourceUsageOptions();
    if (SelfOnly)
      Options.DarwinSystem->ResourceUsageChildren.reset();
    else
      Options.DarwinSystem->ResourceUsageSelf.reset();
    auto MissingPeer = run("virtual-resource-usage");
    ASSERT_TRUE(bool(MissingPeer)) << llvm::toString(MissingPeer.takeError());
    EXPECT_EQ(MissingPeer->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(MissingPeer->Diagnostic,
              "Darwin resource usage observation is not configured");
    EXPECT_EQ(MissingPeer->StandardOutput,
              SelfOnly
                  ? llvm::fromHex(darwin_test::ResourceUsageHex).substr(0, 144)
                  : "");
  }
  Options.DarwinSystem = darwin_test::resourceUsageOptions();
  for (auto Mode : {"resource-usage", "virtual-resource-usage"}) {
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("resource-usage")
                  ? "g"
                  : llvm::fromHex(darwin_test::ResourceUsageHex));
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_TRUE(llvm::any_of(Result->Services, [](const auto &Event) {
      return (uint32_t(Event.Number) & 0x00ffffff) == 117 &&
             Event.Arguments[0] == 0x12345678ffffffffULL && Event.Result == 0 &&
             Event.Error == false;
    }));
  }
  EXPECT_EQ(Options.DarwinSystem->ResourceUsageSelf->UserSeconds, INT64_MIN);
  EXPECT_EQ(Options.DarwinSystem->ResourceUsageChildren->Counters[13],
            INT64_MAX);
}

TEST_P(DarwinProcess,
       ResourceLimitsPreserveExplicitPairsSelectorsAndCopyOrder) {
  for (bool EmptySystem : {false, true}) {
    if (EmptySystem)
      Options.DarwinSystem = DarwinSystemOptions{};
    auto Missing = run("resource-limits");
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic,
              "Darwin resource limit observation is not configured");
    EXPECT_TRUE(Missing->StandardOutput.empty());
  }
  Options.DarwinSystem = darwin_test::resourceLimitOptions();
  Options.DarwinSystem->MaxFilesPerProcess.reset();
  for (auto Mode : {"resource-limits", "virtual-resource-limits"}) {
    auto MissingCap = run(Mode);
    ASSERT_TRUE(bool(MissingCap)) << llvm::toString(MissingCap.takeError());
    EXPECT_EQ(MissingCap->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(MissingCap->Diagnostic,
              "Darwin sysctl observation is not configured");
    EXPECT_EQ(MissingCap->StandardOutput,
              Mode == llvm::StringRef("resource-limits")
                  ? ""
                  : llvm::fromHex(darwin_test::ResourceLimitsHex));
  }
  Options.DarwinSystem = darwin_test::resourceLimitOptions();
  for (auto Mode : {"resource-limits", "virtual-resource-limits"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("resource-limits")
                  ? "l"
                  : llvm::fromHex(darwin_test::ResourceLimitsHex));
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_TRUE(llvm::any_of(Result->Services, [](const auto &Event) {
      return (uint32_t(Event.Number) & 0x00ffffff) == 194 &&
             Event.Arguments[0] == 0x100000008ULL && Event.Result == 0 &&
             Event.Error == false;
    }));
    unsigned Queries = 0;
    bool HighNumber = false;
    for (const auto &Event : Result->Services)
      if ((uint32_t(Event.Number) & 0x00ffffff) == 89) {
        ++Queries;
        EXPECT_EQ(Event.Result, 64);
        EXPECT_EQ(Event.Error, false);
        HighNumber |= (Event.Number >> 32) == 0x12345678;
        EXPECT_EQ(Event.Arguments[0], UINT64_MAX);
        EXPECT_EQ(Event.Arguments[2], 0x1122334455667788ULL);
      }
    EXPECT_EQ(Queries, 2u);
    EXPECT_TRUE(HighNumber);
  }
  EXPECT_EQ(Options.DarwinSystem->ResourceLimits.at(0).Current, 0u);
  EXPECT_EQ(Options.DarwinSystem->ResourceLimits.at(0).Maximum, 0u);
  EXPECT_EQ(Options.DarwinSystem->ResourceLimits.at(8).Current, 256u);
  EXPECT_EQ(Options.DarwinSystem->MaxFilesPerProcess, 64u);
}

TEST_P(DarwinProcess, DescriptorTableRequiresExplicitPeersAndKeepsBudgets) {
  Options.DarwinFiles.emplace().DescriptorLimit = 3;
  for (unsigned Configuration = 0; Configuration != 6; ++Configuration) {
    Options.DarwinSystem.reset();
    if (Configuration) {
      Options.DarwinSystem.emplace();
      if (Configuration == 2)
        Options.DarwinSystem->ResourceLimits[8] = {0, uint64_t(INT64_MAX)};
      if (Configuration == 3 || Configuration == 4)
        Options.DarwinSystem->MaxFilesPerProcess =
            Configuration == 3 ? 0u : 64u;
      if (Configuration == 4)
        Options.DarwinSystem->ResourceLimits[7] = {0, 0};
      if (Configuration == 5)
        Options.DarwinSystem->ResourceLimits[8] = {256, 1024};
    }
    auto Missing = run("descriptor-table-query");
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic,
              "Darwin descriptor table observations are not configured");
    EXPECT_TRUE(Missing->StandardOutput.empty());
    ASSERT_EQ(Missing->Services.size(), 1u);
    EXPECT_FALSE(Missing->Services.front().Result);
    EXPECT_EQ(Missing->Services.front().Arguments[0], UINT64_MAX);
  }
  struct Sample {
    uint64_t Current;
    uint32_t Cap;
    const char *Hex;
  };
  for (const Sample &S :
       {Sample{0, 64, "00000000"}, Sample{1, 64, "01000000"},
        Sample{256, 64, "40000000"}, Sample{0x100000001ULL, 64, "40000000"},
        Sample{uint64_t(INT64_MAX), INT32_MAX, "ffffff7f"},
        Sample{256, 0, "00000000"}}) {
    SCOPED_TRACE(S.Current);
    Options.DarwinSystem.emplace();
    Options.DarwinSystem->ResourceLimits[8] = {S.Current, uint64_t(INT64_MAX)};
    Options.DarwinSystem->MaxFilesPerProcess = S.Cap;
    auto Result = run("descriptor-table-query");
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput, llvm::fromHex(S.Hex));
    EXPECT_TRUE(Result->StandardError.empty());
    ASSERT_FALSE(Result->Services.empty());
    const auto &Event = Result->Services.front();
    EXPECT_EQ(Event.Number,
              0x1234567800000059ULL |
                  (GetParam().ISA == GuestArchitecture::X64 ? 0x02000000 : 0));
    EXPECT_EQ(Event.Error, false);
    EXPECT_EQ(Event.Arguments[2], 0x1122334455667788ULL);
    EXPECT_EQ(Options.DarwinFiles->DescriptorLimit, 3u);
    EXPECT_EQ(Options.DarwinSystem->ResourceLimits.at(8).Current, S.Current);
    EXPECT_EQ(Options.DarwinSystem->MaxFilesPerProcess, S.Cap);
  }
}

TEST_P(DarwinProcess, SystemQueriesPreserveExplicitValuesWidthsAndCopyOrder) {
  auto Missing = run("system-info");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Diagnostic, "Darwin sysctl observation is not configured");
  EXPECT_TRUE(Missing->StandardOutput.empty());
  Options.DarwinSystem = darwin_test::systemOptions();
  for (auto Mode : {"system-info", "virtual-system"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("system-info")
                  ? "i"
                  : llvm::fromHex(darwin_test::SystemHex));
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
    ASSERT_GE(Result->Services.size(), 3u);
    const uint64_t Class =
        GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
    EXPECT_EQ(Result->Services[0].Number, Class + 274);
    EXPECT_EQ(Result->Services[2].Number, Class + 202);
    EXPECT_EQ(Result->Services[2].Arguments[1], 0x1234567800000002ULL);
    EXPECT_EQ(Result->Services[2].Result, 0u);
    EXPECT_EQ(Result->Services[2].Error, false);
  }
}
TEST_P(DarwinProcess, ProcessQueriesKeepIndependentSelfObservations) {
  struct Query {
    const char *Mode;
    const char *Diagnostic;
    unsigned Number;
  };
  const Query Queries[] = {
      {"process-group-query",
       "Darwin process group observation is not configured", 151},
      {"process-session-query",
       "Darwin process session observation is not configured", 310},
      {"process-taint-query",
       "Darwin process taint observation is not configured", 327}};
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x02000000 : 0;
  auto Negative = run("process-negative");
  ASSERT_TRUE(bool(Negative)) << llvm::toString(Negative.takeError());
  ASSERT_EQ(Negative->Stop, ProcessStopReason::Exited) << Negative->Diagnostic;
  EXPECT_EQ(Negative->ExitStatus, 37);
  EXPECT_EQ(Negative->StandardOutput, "E");
  unsigned NegativeResults = 0;
  for (const auto &E : Negative->Services)
    if (E.Number == Class + 151 || E.Number == Class + 310) {
      EXPECT_EQ(E.Result, 3u);
      EXPECT_EQ(E.Error, true);
      ++NegativeResults;
    }
  EXPECT_EQ(NegativeResults, 8u);
  for (const auto &Q : Queries) {
    Options.DarwinSystem.reset();
    auto Missing = run(Q.Mode);
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic, Q.Diagnostic);
    EXPECT_TRUE(Missing->StandardOutput.empty());
    ASSERT_EQ(Missing->Services.size(), 1u);
    EXPECT_EQ(Missing->Services.front().Number, Class + Q.Number);
    EXPECT_FALSE(Missing->Services.front().Result);
    Options.DarwinSystem.emplace();
    Options.DarwinSystem->Credentials = DarwinCredentials{0, 7, 9, 11, {}};
    if (Q.Number == 151)
      Options.DarwinSystem->ProcessGroupID = 7;
    if (Q.Number == 310)
      Options.DarwinSystem->SessionID = 9;
    if (Q.Number == 327)
      Options.DarwinSystem->ProcessTainted = false;
    auto R = run(Q.Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, llvm::fromHex(Q.Number == 151   ? "07000000"
                                               : Q.Number == 310 ? "09000000"
                                                                 : "00000000"));
    EXPECT_EQ(R->Services.front().Error, false);
  }
  for (bool Tainted : {false, true}) {
    Options.DarwinSystem = darwin_test::processObservationOptions();
    Options.DarwinSystem->ProcessTainted = Tainted;
    Options.DarwinFiles.emplace().DescriptorLimit = 3;
    for (const char *Mode :
         {"process-observations", "virtual-process-observations"}) {
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 37);
      EXPECT_EQ(R->StandardOutput,
                llvm::StringRef(Mode) == "process-observations" ? "P"
                : Tainted ? llvm::fromHex(darwin_test::ProcessObservationsHex)
                          : llvm::fromHex("070000000403020100000000"));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      unsigned Negative = 0, SelfGroup = 0, SelfSession = 0;
      bool HighSelf = false;
      for (const auto &E : R->Services) {
        if (E.Number == Class + 151 || E.Number == Class + 310) {
          if (uint32_t(E.Arguments[0]) & 0x80000000u) {
            EXPECT_EQ(E.Result, 3u);
            EXPECT_EQ(E.Error, true);
            ++Negative;
          } else {
            EXPECT_EQ(E.Result, E.Number == Class + 151 ? 7u : 0x01020304u);
            EXPECT_EQ(E.Error, false);
            (E.Number == Class + 151 ? SelfGroup : SelfSession)++;
            HighSelf |= E.Arguments[0] == 0xffffffff000003e8ULL;
          }
          EXPECT_EQ(E.Arguments[2], 0x1122334455667788ULL);
        }
      }
      EXPECT_EQ(Negative, 8u);
      EXPECT_EQ(SelfGroup, 6u);
      EXPECT_EQ(SelfSession, 7u);
      EXPECT_TRUE(HighSelf);
      EXPECT_EQ(Options.DarwinFiles->DescriptorLimit, 3u);
    }
  }
  Options.DarwinSystem.reset();
  auto Missing = run("process-missing-after");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->StandardOutput, "!");
  EXPECT_EQ(Missing->Diagnostic,
            "Darwin process group observation is not configured");
  for (bool Known : {false, true}) {
    if (Known)
      Options.DarwinSystem = darwin_test::processObservationOptions();
    auto Peer = run("process-peer-query");
    ASSERT_TRUE(bool(Peer)) << llvm::toString(Peer.takeError());
    EXPECT_EQ(Peer->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Peer->Diagnostic, "Darwin other-process queries are not modeled");
    EXPECT_EQ(Peer->StandardOutput, "!");
    EXPECT_EQ(Peer->Services.back().Arguments[0], 0xffffffff00001000ULL);
    EXPECT_FALSE(Peer->Services.back().Result);
  }
  Options.DarwinSystem->Credentials.emplace();
  for (const auto &[Mode, Number] :
       {std::pair{"process-setpgid", 82u}, std::pair{"process-setsid", 147u}}) {
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->StandardOutput, "!");
    EXPECT_EQ(R->Services.back().Number, Class + Number);
    EXPECT_FALSE(R->Services.back().Result);
  }
}

TEST_P(DarwinProcess, PriorityKeepsSignedSuccessArgumentOrderAndScope) {
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Config = 0; Config != 3; ++Config) {
    Options.DarwinSystem.reset();
    if (Config == 1)
      Options.DarwinSystem.emplace();
    if (Config == 2) {
      Options.DarwinSystem = darwin_test::processObservationOptions();
      Options.DarwinSystem->Credentials.emplace().GroupAccessList =
          std::vector<uint32_t>{0, 1000, 1000};
      Options.DarwinSystem->HostName = "abcd";
      Options.DarwinSystem->LoginNameBytes.emplace(255, 0);
    }
    auto Errors = run("priority-errors");
    ASSERT_TRUE(bool(Errors)) << llvm::toString(Errors.takeError());
    EXPECT_EQ(Errors->Stop, ProcessStopReason::Exited) << Errors->Diagnostic;
    EXPECT_EQ(Errors->ExitStatus, 37);
    EXPECT_EQ(Errors->StandardOutput, "E");
    auto Missing = run("priority-missing-after");
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic,
              "Darwin process nice observation is not configured");
    EXPECT_EQ(Missing->StandardOutput, "!");
    EXPECT_EQ(Missing->Services.back().Number, Class + 100);
    EXPECT_FALSE(Missing->Services.back().Result);
  }
  struct Sample {
    int32_t Nice;
    const char *Hex;
    uint64_t Raw;
  };
  const Sample Samples[] = {{-20, "ecffffffffffffff", 0xffffffffffffffecULL},
                            {-1, "ffffffffffffffff", UINT64_MAX},
                            {0, "0000000000000000", 0},
                            {20, "1400000000000000", 20}};
  for (const auto &S : Samples) {
    Options.DarwinSystem = darwin_test::priorityOptions();
    Options.DarwinSystem->ProcessNice = S.Nice;
    for (const char *Mode : {"process-priority", "virtual-process-priority"}) {
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 37);
      EXPECT_EQ(R->StandardOutput, llvm::StringRef(Mode) == "process-priority"
                                       ? "Q"
                                       : llvm::fromHex(S.Hex));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      std::vector<const ProcessServiceEvent *> Priority;
      for (const auto &E : R->Services) {
        if (E.Number != Class + 100)
          continue;
        Priority.push_back(&E);
        EXPECT_EQ(E.Arguments[2], 0x1122334455667788ULL);
        EXPECT_EQ(E.Arguments[3], UINT64_MAX);
        EXPECT_EQ(E.Arguments[4], UINT64_MAX);
        EXPECT_EQ(E.Arguments[5], UINT64_MAX);
        if (E.Error == true)
          EXPECT_EQ(E.Result, 22u);
        else {
          EXPECT_EQ(E.Error, false);
          EXPECT_EQ(E.Result, S.Raw);
        }
      }
      ASSERT_EQ(Priority.size(), 66u);
      EXPECT_EQ(Priority[0]->Result, S.Raw);
      EXPECT_EQ(Priority[0]->Error, false);
      EXPECT_EQ(Priority[1]->Result, 22u);
      EXPECT_EQ(Priority[1]->Error, true);
      EXPECT_EQ(Priority[1]->Arguments[0], 5u);
      EXPECT_EQ(Priority[2]->Result, S.Raw);
      EXPECT_EQ(Priority[2]->Error, false);
      EXPECT_EQ(Priority[2]->Arguments[0], 0xffffffff00000000ULL);
      EXPECT_EQ(Priority[2]->Arguments[1], 0xffffffff00000000ULL);
    }
  }
  for (bool Known : {false, true}) {
    Options.DarwinSystem = darwin_test::priorityOptions();
    if (!Known)
      Options.DarwinSystem->ProcessNice.reset();
    Options.DarwinSystem->Credentials.emplace().GroupAccessList =
        std::vector<uint32_t>{0, 1000, 1000};
    Options.DarwinSystem->ProcessGroupID = 1000;
    Options.DarwinSystem->SessionID = 1000;
    for (const char *Mode :
         {"priority-peer", "priority-group", "priority-user", "priority-thread",
          "priority-background", "priority-role", "priority-game",
          "priority-carplay", "priority-set"}) {
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R->StandardOutput, "!");
      EXPECT_FALSE(R->Services.back().Result);
      EXPECT_EQ(R->Services.back().Number,
                Class + (llvm::StringRef(Mode) == "priority-set" ? 96 : 100));
      EXPECT_EQ(R->Diagnostic,
                llvm::StringRef(Mode) == "priority-peer"
                    ? "Darwin other-process queries are not modeled"
                : llvm::StringRef(Mode) == "priority-set"
                    ? (Class ? "unsupported Darwin service 0x2000060"
                             : "unsupported Darwin service 0x60")
                    : "Darwin selected priority observation is not modeled");
      if (llvm::StringRef(Mode) == "priority-thread") {
        EXPECT_EQ(R->Services.back().Arguments[0], 0x1234567800000003ULL);
        EXPECT_EQ(R->Services.back().Arguments[1], 0xffffffff00000000ULL);
      }
    }
  }
}

TEST_P(DarwinProcess, LoginBufferPreservesExactBytesZeroLengthAndAuthority) {
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Config = 0; Config != 3; ++Config) {
    Options.DarwinSystem.reset();
    if (Config == 1)
      Options.DarwinSystem.emplace();
    if (Config == 2) {
      Options.DarwinSystem = darwin_test::processObservationOptions();
      Options.DarwinSystem->Credentials.emplace();
      Options.DarwinSystem->HostName = "abcd";
    }
    auto Zero = run("login-zero");
    ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
    EXPECT_EQ(Zero->Stop, ProcessStopReason::Exited) << Zero->Diagnostic;
    EXPECT_EQ(Zero->ExitStatus, 37);
    EXPECT_EQ(Zero->StandardOutput, "Z");
    auto Missing = run("login-missing-after");
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Missing->Diagnostic,
              "Darwin login buffer observation is not configured");
    EXPECT_EQ(Missing->StandardOutput, "!");
    ASSERT_FALSE(Missing->Services.empty()) << Missing->Diagnostic;
    EXPECT_EQ(Missing->Services.back().Number, Class + 49);
    EXPECT_FALSE(Missing->Services.back().Result);
  }
  for (bool Zero : {false, true}) {
    Options.DarwinSystem = darwin_test::loginBufferOptions();
    if (Zero)
      Options.DarwinSystem->LoginNameBytes->assign(255, 0);
    for (const char *Mode : {"login-buffer", "virtual-login-buffer"}) {
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 37);
      EXPECT_EQ(R->StandardOutput, llvm::StringRef(Mode) == "login-buffer" ? "L"
                                   : Zero ? std::string(255, '\0')
                                          : std::string("L\0\xff", 3) +
                                                std::string(251, '\xa5') + "~");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      unsigned ZeroReturns = 0, Errors = 0, Copies = 0;
      bool HighLength = false, HugeLength = false;
      for (const auto &E : R->Services) {
        if (E.Number != Class + 49)
          continue;
        if (!uint32_t(E.Arguments[1])) {
          EXPECT_EQ(E.Result, 0u);
          EXPECT_EQ(E.Error, false);
          ++ZeroReturns;
        } else if (E.Error == true) {
          EXPECT_EQ(E.Result, 14u);
          EXPECT_EQ(E.Arguments[2], 0x1122334455667788ULL);
          ++Errors;
        } else {
          EXPECT_EQ(E.Result, 0u);
          EXPECT_EQ(E.Error, false);
          HighLength |= E.Arguments[1] == 0xffffffff00000001ULL;
          HugeLength |= E.Arguments[1] == UINT64_MAX;
          ++Copies;
        }
      }
      EXPECT_EQ(ZeroReturns, 21u);
      EXPECT_EQ(Errors, 42u);
      EXPECT_EQ(Copies, 13u);
      EXPECT_TRUE(HighLength);
      EXPECT_TRUE(HugeLength);
    }
    Options.DarwinSystem->Credentials.emplace();
    auto Setter = run("login-set");
    ASSERT_TRUE(bool(Setter)) << llvm::toString(Setter.takeError());
    EXPECT_EQ(Setter->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Setter->StandardOutput, "!");
    EXPECT_EQ(Setter->Services.back().Number, Class + 50);
    EXPECT_FALSE(Setter->Services.back().Result);
  }
}

TEST_P(DarwinProcess, HostNameKeepsTruncationObservationAndWriteAuthority) {
  auto Missing = run("hostname-missing-after");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Diagnostic, "Darwin sysctl observation is not configured");
  EXPECT_EQ(Missing->StandardOutput, "!");
  Options.DarwinSystem = darwin_test::hostNameOptions();
  for (const char *Mode : {"hostname", "virtual-hostname", "hostname-query"}) {
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, llvm::StringRef(Mode) == "hostname" ? "n"
                                 : llvm::StringRef(Mode) == "hostname-query"
                                     ? std::string(1, '\0')
                                     : llvm::fromHex(darwin_test::HostNameHex));
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    const uint64_t Class =
        GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
    ASSERT_GE(R->Services.size(),
              llvm::StringRef(Mode) == "hostname-query" ? 2u : 3u);
    EXPECT_EQ(R->Services.front().Number, Class + 274);
    if (llvm::StringRef(Mode) != "hostname-query") {
      EXPECT_EQ(R->Services[1].Number, Class + 202);
      EXPECT_EQ(R->Services[1].Arguments[1], 0x1234567800000002ULL);
    }
    EXPECT_EQ(R->Services.front().Error, false);
  }
  Options.DarwinSystem->HostName = "";
  auto Empty = run("hostname-query");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_EQ(Empty->Stop, ProcessStopReason::Exited) << Empty->Diagnostic;
  EXPECT_EQ(Empty->StandardOutput, std::string(1, '\0'));
  for (bool Root : {false, true}) {
    for (bool Known : {false, true}) {
      Options.DarwinSystem.emplace();
      if (Known)
        Options.DarwinSystem->HostName = "abcd";
      if (Root)
        Options.DarwinSystem->Credentials.emplace();
      auto R = run("hostname-write");
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, Root ? ProcessStopReason::UnsupportedService
                              : ProcessStopReason::Exited);
      EXPECT_EQ(R->StandardOutput, Root ? "!" : "!w");
      if (Root)
        EXPECT_EQ(R->Diagnostic,
                  "Darwin privileged system write is not modeled");
      else
        EXPECT_EQ(R->ExitStatus, 37);
    }
  }
}
TEST_P(DarwinProcess, ExplicitTimeObservationsPreserveBytesErrorsAndCopyOrder) {
  auto Null = run("time-null");
  ASSERT_TRUE(bool(Null)) << llvm::toString(Null.takeError());
  EXPECT_EQ(Null->Stop, ProcessStopReason::Exited) << Null->Diagnostic;
  EXPECT_EQ(Null->ExitStatus, 37);
  auto Missing = run("time");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_TRUE(Missing->StandardOutput.empty());
  Options.DarwinTime = darwin_test::timeOptions();
  for (auto Mode : {"time", "time-values"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("time")
                  ? "t"
                  : llvm::fromHex(darwin_test::TimeHex));
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
    ASSERT_FALSE(Result->Services.empty());
    EXPECT_EQ(Result->Services.front().Number,
              (GetParam().ISA == GuestArchitecture::X64 ? 0x2000000u : 0u) +
                  116u);
    EXPECT_EQ(Result->Services.front().Result, 0u);
    EXPECT_EQ(Result->Services.front().Error, false);
  }
}
TEST_P(DarwinProcess, MachTimePreservesReturnStateAndExplicitObservations) {
  auto Missing = run("mach-time");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Diagnostic, "Darwin timebase is not configured");
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const uint64_t Timebase = X64 ? 0x01000059ULL : uint64_t(-89);
  Options.DarwinTime = darwin_test::timeOptions();
  for (auto Mode : {"mach-time", "mach-timebase-values"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("mach-time")
                  ? "h"
                  : llvm::fromHex(darwin_test::TimebaseHex));
    ASSERT_GE(Result->Services.size(), 13u);
    for (size_t I = 0; I != 12; ++I) {
      EXPECT_EQ(Result->Services[I].Result, 0u);
      EXPECT_FALSE(Result->Services[I].Error.has_value());
    }
    EXPECT_EQ(Result->Services[0].Number, Timebase);
    EXPECT_EQ(Result->Services[4].Number, uint32_t(Timebase));
    EXPECT_EQ(Result->Services[5].Number,
              0x1234567800000000ULL | uint32_t(Timebase));
    EXPECT_EQ(Result->Services.back().Error, false); // BSD write
    if (Mode == llvm::StringRef("mach-time")) {
      EXPECT_EQ(Result->Services[12].Result, 1000u);
      EXPECT_EQ(Result->Services[13].Number,
                X64 ? 0x1234567802000014ULL : 0x1234567800000014ULL);
      EXPECT_EQ(Result->Services[13].Error, false);
      EXPECT_EQ(Result->Services[14].Error, true); // BSD read(999)
      EXPECT_EQ(Result->Services[15].Error, true); // BSD write(999)
    }
  }
  auto Clocks = run("mach-clock-values");
  ASSERT_TRUE(bool(Clocks)) << llvm::toString(Clocks.takeError());
  if (X64) {
    EXPECT_EQ(Clocks->Stop, ProcessStopReason::UnsupportedService);
  } else {
    ASSERT_EQ(Clocks->Stop, ProcessStopReason::Exited) << Clocks->Diagnostic;
    EXPECT_EQ(Clocks->ExitStatus, 37);
    EXPECT_EQ(Clocks->StandardOutput, llvm::fromHex(darwin_test::MachClockHex));
    ASSERT_EQ(Clocks->Services.size(), 3u);
    EXPECT_EQ(Clocks->Services[0].Result, 0xfedcba9876543210ULL);
    EXPECT_EQ(Clocks->Services[1].Result, UINT64_MAX);
    EXPECT_FALSE(Clocks->Services[0].Error);
    EXPECT_FALSE(Clocks->Services[1].Error);
  }
  for (auto Mode : {"mach-absolute", "mach-continuous"}) {
    SCOPED_TRACE(Mode);
    Options.DarwinTime.emplace();
    for (bool Configured : {false, true}) {
      if (Configured) {
        if (Mode == llvm::StringRef("mach-absolute"))
          Options.DarwinTime->MachAbsoluteTime = 0;
        else
          Options.DarwinTime->MachContinuousTime = 0;
      }
      auto Result = run(Mode);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      if (X64 || !Configured) {
        EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService);
        EXPECT_TRUE(Result->StandardOutput.empty());
      } else {
        EXPECT_EQ(Result->Stop, ProcessStopReason::Exited)
            << Result->Diagnostic;
        EXPECT_EQ(Result->ExitStatus, 37);
        EXPECT_EQ(Result->StandardOutput, std::string(8, '\0'));
      }
    }
  }
}
TEST_P(DarwinProcess, FiniteStandardInputRetainsBinaryBytesAndSharedCursor) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->StandardInput = {0, 0xff, 'x'};
  auto Result = run("stdin");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, std::string("\0\xffx", 3));
  EXPECT_TRUE(Result->StandardError.empty());
}
TEST_P(DarwinProcess,
       Stat64ObservationsPreserveLayoutErrorsAndDescriptorState) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  auto Missing = run("file-status");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Services.size(), 1u);
  EXPECT_TRUE(Missing->StandardOutput.empty());
  Options.DarwinFiles->Metadata["/data"] = darwin_test::metadata();
  auto Result = run("file-status");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "s");
  EXPECT_TRUE(Result->StandardError.empty());
  ASSERT_FALSE(Result->Services.empty());
  EXPECT_EQ(Result->Services.front().Number,
            (GetParam().ISA == GuestArchitecture::X64 ? 0x2000000u : 0u) +
                338u);
  EXPECT_EQ(Result->Services.front().Result, 0u);
  EXPECT_EQ(Result->Services.front().Error, false);
}
TEST_P(DarwinProcess, OutputDescriptorsCanBeClosedReusedAndRedirected) {
  auto Result = run("output-descriptors");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "ok");
  EXPECT_TRUE(Result->StandardError.empty());
  Options.OutputLimit = 1;
  Result = run("output-descriptors");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::OutputLimit);
  EXPECT_EQ(Result->StandardOutput, "o");
}
TEST_P(DarwinProcess, MissingFileAndInputConfigurationStopWithoutHostAccess) {
  for (auto Mode : {"files", "stdin"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService);
    ASSERT_FALSE(Result->Services.empty());
    EXPECT_FALSE(Result->Services.back().Result);
    EXPECT_FALSE(Result->Services.back().Error);
    EXPECT_TRUE(Result->StandardOutput.empty());
  }
}
TEST_P(DarwinProcess, PartialCopyRetainsEFAULTAndSubsequentWriteRecovers) {
  auto Result = run("partial");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "pqp");
  ASSERT_EQ(Result->Services.size(), 3u);
  EXPECT_EQ(Result->Services[1].Result, 14);
  EXPECT_EQ(Result->Services[1].Error, true);
  EXPECT_EQ(Result->Services[2].Result, 1);
  EXPECT_EQ(Result->Services[2].Error, false);
}
TEST_P(DarwinProcess, OversizedWritePrecedesDescriptorPointerAndOutputChecks) {
  Options.OutputLimit = 1;
  auto Result = run("write-length");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "w");
  EXPECT_TRUE(Result->StandardError.empty());
  ASSERT_EQ(Result->Services.size(), 7u);
  for (size_t I = 0; I < 4; ++I) {
    EXPECT_EQ(Result->Services[I].Result, 22u) << I;
    EXPECT_EQ(Result->Services[I].Error, true) << I;
  }
  EXPECT_EQ(Result->Services[4].Result, 9u);
  EXPECT_EQ(Result->Services[5].Result, 14u);
  EXPECT_EQ(Result->Services[6].Result, 1u);
  EXPECT_EQ(Result->Services[6].Error, false);
}
TEST_P(DarwinProcess, AnonymousMemoryAlignmentAtomicProtectionAndReuse) {
  auto Result = run("memory");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "d");
}
TEST_P(DarwinProcess, InitialStackAndDataPartialUnmapReleasesPhysicalBudget) {
  const auto Profile =
      GetParam().OS == ProcessProfile::MacOSMachO64
          ? darwin_model::macOSProfile()
          : darwin_model::iOSProfile(GetParam().OS ==
                                     ProcessProfile::IOSSimulatorMachO64);
  auto Image = darwin_model::loadImage(Path, Profile, Options);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Options.StackSize = Image->Memory.PageSize * 4;
  Options.MemoryLimit =
      Image->Plan.MappedBytes + Options.StackSize + Image->Memory.PageSize;
  Options.OutputLimit = Image->Memory.PageSize;
  auto Result = run("release");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
}
TEST_P(DarwinProcess, FaultAndReadOnlyStoreRemainCPUFailures) {
  for (auto Mode : {"fault", "permission"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::CPUFailure)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->ExitStatus);
  }
}
TEST_P(DarwinProcess, UnknownBSDMachAndForeignTrapFailExplicitly) {
  Options.DarwinTime = darwin_test::timeOptions();
  for (auto Mode : {"unknown", "mach", "badtrap", "mach-int32-min",
                    "mach-wrong-class", "mach-foreign-class"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->ExitStatus);
  }
}
TEST_P(DarwinProcess, InstructionBudgetSurvivesQuanta) {
  Options.Limits.Instructions = 200;
  auto Result = run("loop");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::InstructionLimit)
      << Result->Diagnostic;
  EXPECT_EQ(Result->Instructions, 200u);
}
TEST_P(DarwinProcess, DeadlineTerminatesANonreturningGuestAcrossQuanta) {
  Options.Limits.Instructions = uint64_t(1) << 40;
  Options.Limits.TimeoutMicroseconds = 1000;
  auto Result = run("loop");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Timeout) << Result->Diagnostic;
  EXPECT_FALSE(Result->ExitStatus);
  EXPECT_LT(Result->Instructions, Options.Limits.Instructions);
}
TEST_P(DarwinProcess, EventAndOutputLimitsStopBeforeServiceEffects) {
  Options.Limits.Events = 1;
  auto Limited = run("normal");
  ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
  EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
  EXPECT_EQ(Limited->Events, 1u);
  EXPECT_TRUE(Limited->StandardOutput.empty());
  Options.Limits.Events = 100;
  Options.OutputLimit = 8;
  auto Output = run("normal");
  ASSERT_TRUE(bool(Output)) << llvm::toString(Output.takeError());
  EXPECT_EQ(Output->Stop, ProcessStopReason::OutputLimit) << Output->Diagnostic;
  EXPECT_TRUE(Output->StandardOutput.empty());
  Options.DarwinFiles = darwin_test::symbolicLinkOptions();
  Options.Arguments[2] = "/data";
  Options.Limits.Events = 1;
  auto Links = run("symbolic-links");
  ASSERT_TRUE(bool(Links)) << llvm::toString(Links.takeError());
  EXPECT_EQ(Links->Stop, ProcessStopReason::EventLimit) << Links->Diagnostic;
  EXPECT_EQ(Links->Events, 1u);
  EXPECT_EQ(Links->Services.size(), 1u);
  EXPECT_TRUE(Links->StandardOutput.empty());
}
TEST_P(DarwinProcess, ExplicitProfileCannotBeReplacedByHostPlatform) {
  auto Wrong = emulateProcess(Path,
                              GetParam().OS == ProcessProfile::MacOSMachO64
                                  ? ProcessProfile::IOSMachO64
                                  : ProcessProfile::MacOSMachO64,
                              Options);
  ASSERT_FALSE(bool(Wrong));
  llvm::consumeError(Wrong.takeError());
}
TEST_P(DarwinProcess, RuntimeLinksCreateOpaqueTargetsAndRetainObjects) {
  Options.DarwinFiles = darwin_test::mixedSymbolicLinkOptions();
  Options.Arguments[2] = "/work/data";
  auto Result = run("symbolic-link-creation");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "b");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {57u, 474u, 58u, 473u, 465u, 472u, 475u, 197u, 73u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Error == false;
    })) << Number;
}

TEST_P(DarwinProcess, RuntimeCreatedSymbolicLinksCanBeRemoved) {
  Options.DarwinFiles = darwin_test::mixedSymbolicLinkOptions();
  Options.Arguments[2] = "/work/data";
  auto Result = run("symbolic-link-unlink");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "U");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {10u, 57u, 474u, 472u, 473u, 475u, 197u, 73u, 13u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Error == false;
    })) << Number;
  for (auto Error : {2u, 62u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + 472 && Event.Error == true &&
             Event.Result == Error;
    })) << Error;
}

TEST_P(DarwinProcess, RuntimeCreatedSymbolicLinksCanBeRenamed) {
  Options.DarwinFiles = darwin_test::mixedSymbolicLinkOptions();
  Options.DarwinFiles->SwapRenameDirectories.insert("/work");
  Options.Arguments[2] = "/work/data";
  auto Result = run("symbolic-link-rename");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "R");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (auto Number : {57u, 474u, 128u, 465u, 488u, 473u, 472u, 197u, 73u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + Number && Event.Error == false;
    })) << Number;
  for (auto Error : {17u, 62u, 2u, 22u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + 488 && Event.Error == true &&
             Event.Result == Error;
    })) << Error;
  EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
    return Event.Number == Class + 465 && Event.Error == true &&
           Event.Result == 66;
  }));
  for (auto Flags : {2u, 18u})
    EXPECT_TRUE(llvm::any_of(Result->Services, [&](const auto &Event) {
      return Event.Number == Class + 488 && Event.Error == false &&
             Event.Result == 0 && uint32_t(Event.Arguments[4]) == Flags;
    })) << Flags;
}

TEST_P(DarwinProcess,
       ImmediatePollPreservesFiltersCopiesAndExplicitResourceKnowledge) {
  Options.InstructionQuantum = 1024;
  Options.DarwinFiles.emplace().Files["/data"] = {'a', 'b', 0, 0xff, 'e', 'f'};
  Options.DarwinSystem.emplace().ResourceLimits[8] = {10240, 10240};
  Options.Arguments[2] = "/data";
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const uint64_t Class = X64 ? 0x2000000 : 0;
  constexpr uint64_t Expected[] = {0, 0, 1, 0, 1, 1, 2, 2, 0, 2, 22, 14};
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("immediate-poll");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, "p");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 86u);
    EXPECT_EQ(R->Services[5].Number, Class | 199);
    EXPECT_EQ(R->Services[5].Result, 6u);
    EXPECT_EQ(R->Services[5].Error, false);
    EXPECT_EQ(R->Services[5].Arguments[2], 2u);
    for (unsigned API = 0; API != 2; ++API)
      for (unsigned H = 0; H != 3; ++H)
        for (unsigned I = 0; I != 12; ++I) {
          const auto &E = R->Services[7 + (API * 3 + H) * 12 + I];
          EXPECT_EQ(E.Number, High[H] | Class | (API ? 417 : 230));
          EXPECT_EQ(E.Result, Expected[I]);
          EXPECT_EQ(E.Error, I >= 10);
          EXPECT_EQ(E.Arguments[2], High[H]);
        }
    for (unsigned I = 0; I != 4; ++I) {
      const auto &E = R->Services[80 + I];
      EXPECT_EQ(E.Number, Class | (I < 2 ? 230 : 417));
      EXPECT_EQ(E.Result, 1u);
      EXPECT_EQ(E.Error, false);
    }
  }
  Options.DarwinSystem->ResourceLimits.clear();
  auto Missing = run("immediate-poll-missing");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Diagnostic,
            "Darwin poll NOFILE resource observation is not configured");
  EXPECT_EQ(Missing->StandardOutput, "!");
  ASSERT_EQ(Missing->Services.size(), 2u);
  EXPECT_EQ(Missing->Services.back().Number, Class | 230);
  EXPECT_FALSE(Missing->Services.back().Result);
  EXPECT_FALSE(Missing->Services.back().Error);
}

TEST_P(DarwinProcess,
       ImmediateSelectPreservesBitmapsCopiesAndProviderPremises) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = BatchedProcessTimeoutMicroseconds;
  Options.DarwinFiles.emplace().Files["/data"] = {'a', 'b', 0, 0xff, 'e', 'f'};
  Options.Arguments[2] = "/data";
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const uint64_t Class = X64 ? 0x2000000 : 0;
  constexpr uint64_t Expected[] = {0,  3,  6,  3,  6,  9,  9, 12,
                                   22, 14, 22, 22, 14, 14, 9};
  constexpr uint32_t Counts[] = {0, 4,          5,          31, 32, 33, 64, 65,
                                 0, UINT32_MAX, UINT32_MAX, 0,  4,  6,  6};
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    SCOPED_TRACE(Repeat);
    auto R = run("immediate-select");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
        << R->Diagnostic << " instructions=" << R->Instructions
        << " services=" << R->Services.size();
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, "s");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 115u);
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | (I ? 41 : 5));
      EXPECT_EQ(R->Services[I].Result, 3u + I);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(R->Services[4 + I].Number, Class | 92);
      EXPECT_EQ(R->Services[4 + I].Result, I == 0 ? 31u : I == 1 ? 32u : 64u);
    }
    EXPECT_EQ(R->Services[7].Number, Class | 199);
    EXPECT_EQ(R->Services[7].Result, 6u);
    EXPECT_EQ(R->Services[7].Arguments[2], 2u);
    for (unsigned API = 0; API != 2; ++API)
      for (unsigned H = 0; H != 3; ++H)
        for (unsigned I = 0; I != 15; ++I) {
          const auto &E = R->Services[9 + (API * 3 + H) * 15 + I];
          EXPECT_EQ(E.Number, High[H] | Class | (API ? 407 : 93));
          EXPECT_EQ(E.Result, Expected[I]);
          EXPECT_EQ(E.Error, I >= 8);
          EXPECT_EQ(E.Arguments[0], High[H] | Counts[I]);
          EXPECT_EQ(E.Arguments[5], UINT64_MAX);
          if (I == 9)
            EXPECT_EQ(E.Arguments[4], UINT64_MAX);
          else
            EXPECT_NE(E.Arguments[4], 0u);
        }
    EXPECT_EQ(R->Services[99].Number, Class | 199);
    EXPECT_EQ(R->Services[99].Result, 6u);
    EXPECT_EQ(R->Services[100].Number, Class | 92);
    EXPECT_EQ(R->Services[100].Result, 4u);
    for (unsigned I = 0; I != 4; ++I) {
      const auto &E = R->Services[102 + I];
      EXPECT_EQ(E.Number, Class | (I < 2 ? 93 : 407));
      EXPECT_EQ(E.Result, I % 2 ? 3u : 9u);
      EXPECT_EQ(E.Error, !(I % 2));
    }
    EXPECT_EQ(R->Services[106].Number, Class | 5);
    EXPECT_EQ(R->Services[106].Result, 3u);
    EXPECT_EQ(R->Services[107].Result, 0u);
    EXPECT_EQ(R->Services[108].Result, 6u);
  }
  for (bool Capacity : {false, true}) {
    auto R =
        run(Capacity ? "immediate-select-capacity" : "immediate-select-wait");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic,
              Capacity
                  ? "Darwin select descriptor table capacity is not modeled"
                  : "Darwin select waiting is not implemented");
    EXPECT_EQ(R->StandardOutput, "!");
    EXPECT_TRUE(R->StandardError.empty());
    ASSERT_EQ(R->Services.size(), 2u);
    const auto &E = R->Services.back();
    EXPECT_EQ(E.Number, Class | (Capacity ? 407 : 93));
    EXPECT_EQ(E.Arguments[0], Capacity ? 4u : 0u);
    EXPECT_EQ(E.Arguments[1], UINT64_MAX);
    EXPECT_FALSE(E.Result);
    EXPECT_FALSE(E.Error);
  }
}

TEST_P(DarwinProcess, OrdinaryModesKeepCurrentMetadataAuthorityAndObjectState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::modeOptions();
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  constexpr uint64_t Numbers[] = {15, 124, 467};
  constexpr uint64_t Modes[] = {0000, 0640, 0604};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-mode-mutations");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, "M");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 54u);
    constexpr uint64_t PrefixNumbers[] = {5, 41, 199, 339};
    constexpr uint64_t PrefixResults[] = {3, 4, 2, 0};
    for (unsigned I = 0; I != 4; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | PrefixNumbers[I]);
      EXPECT_EQ(R->Services[I].Result, PrefixResults[I]);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned API = 0; API != 3; ++API)
      for (unsigned H = 0; H != 3; ++H) {
        const unsigned Start = 4 + (API * 3 + H) * 4;
        const auto &E = R->Services[Start];
        EXPECT_EQ(E.Number, Class | High[H] | Numbers[API]);
        EXPECT_EQ(E.Result, 0u);
        EXPECT_EQ(E.Error, false);
        EXPECT_EQ(E.Arguments[API == 2 ? 2 : 1],
                  High[H] | 0x7fff0000 | Modes[H]);
        for (unsigned I = 1; I != 4; ++I) {
          EXPECT_EQ(R->Services[Start + I].Number,
                    Class | (I == 2 ? 338 : 339));
          EXPECT_EQ(R->Services[Start + I].Result, 0u);
          EXPECT_EQ(R->Services[Start + I].Error, false);
        }
      }
    constexpr uint64_t TailNumbers[] = {15,  124, 467, 467, 339, 467, 339,
                                        199, 6,   124, 124, 339, 6,   4};
    constexpr uint64_t TailResults[] = {14, 9, 22, 9, 0, 0, 0,
                                        2,  0, 9,  0, 0, 0, 1};
    for (unsigned I = 0; I != 14; ++I) {
      EXPECT_EQ(R->Services[40 + I].Number, Class | TailNumbers[I]);
      EXPECT_EQ(R->Services[40 + I].Result, TailResults[I]);
      EXPECT_EQ(R->Services[40 + I].Error, I < 4 || I == 9);
    }
    EXPECT_EQ(R->ReturnValue, 37u);
    EXPECT_EQ(R->Services.back().Number, Class | 4);
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").Mode, 0100600);
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
  }
  Options.DarwinFiles->ModeMutations->Objects.clear();
  auto Unknown = run("ordinary-mode-mutations");
  ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
  EXPECT_EQ(Unknown->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Unknown->Diagnostic,
            "Darwin object mode provider is not configured");
  ASSERT_EQ(Unknown->Services.size(), 5u);
  EXPECT_EQ(Unknown->Services.back().Number, Class | 15);
  EXPECT_FALSE(Unknown->Services.back().Result);
  EXPECT_FALSE(Unknown->Services.back().Error);
}

TEST_P(DarwinProcess, SymbolicOwnershipKeepsVnodeAndTargetGrantsSeparate) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinSystem.emplace().Credentials =
      darwin_test::symbolicOwnershipCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr const char *Modes[] = {
      "symbolic-ownership-mutations", "virtual-symbolic-ownership",
      "created-symbolic-ownership", "virtual-created-symbolic-ownership"};
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    SCOPED_TRACE(Modes[Kind]);
    Options.Arguments[2] = Kind >= 2 ? "/dir/new" : "/dir/link";
    Options.DarwinFiles = darwin_test::symbolicOwnershipOptions(true);
    const auto Wanted = darwin_test::symbolicOwnershipServices(Kind);
    const auto Output = darwin_test::symbolicOwnershipOutput(Kind);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Repeat);
      auto R = run(Modes[Kind]);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " pc=" << R->PC << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 58);
      EXPECT_EQ(R->ReturnValue, 58u);
      EXPECT_EQ(R->StandardOutput, Output);
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        EXPECT_EQ(R->Services[I].Number, Class | Wanted[I].Number);
        EXPECT_EQ(R->Services[I].Result, Wanted[I].Value);
        EXPECT_EQ(R->Services[I].Error, Wanted[I].Error);
        for (unsigned J = 0; J != 6; ++J)
          if (Wanted[I].Arguments[J])
            EXPECT_EQ(R->Services[I].Arguments[J], *Wanted[I].Arguments[J]);
      }
      EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").GID, 31u);
      EXPECT_EQ(Options.DarwinFiles->SymbolicLinks.at("/dir/link"),
                (std::vector<uint8_t>{'.', '.', '/', 'd', 'a', 't', 'a'}));
    }
    for (bool Late : {false, true}) {
      if (Kind >= 2 && Late)
        continue;
      Options.DarwinFiles = darwin_test::symbolicOwnershipOptions(true);
      if (Kind >= 2 || Late)
        Options.DarwinFiles->OwnershipMutations->CreatedSymbolicLinkObjects =
            false;
      else
        Options.DarwinFiles->OwnershipMutations->Objects.erase("/dir/link");
      const uint64_t FD = Late ? 8 : 3;
      unsigned Last = 0;
      while (Last != Wanted.size() &&
             !(Wanted[Last].Number == 123 && Wanted[Last].Arguments[0] == FD))
        ++Last;
      ASSERT_NE(Last, Wanted.size());
      auto R = run(Modes[Kind]);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R->Diagnostic,
                "Darwin object ownership provider is not configured");
      EXPECT_EQ(R->StandardOutput,
                Late && (Kind & 1) ? Output.substr(0, 6 * 160) : "");
      EXPECT_TRUE(R->StandardError.empty());
      ASSERT_EQ(R->Services.size(), Last + 1);
      EXPECT_EQ(R->Services.back().Number, Class | 123);
      EXPECT_EQ(R->Services.back().Arguments[0], FD);
      EXPECT_FALSE(R->Services.back().Result);
      EXPECT_FALSE(R->Services.back().Error);
    }
  }
}

TEST_P(DarwinProcess, LchownKeepsVnodeOwnershipAcrossNamespaceChanges) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinSystem.emplace().Credentials =
      darwin_test::symbolicOwnershipCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr const char *Modes[] = {"lchown-ownership-mutations",
                                   "virtual-lchown-ownership"};
  for (unsigned Kind = 0; Kind != 2; ++Kind) {
    SCOPED_TRACE(Modes[Kind]);
    Options.Arguments[2] = Kind >= 2 ? "/dir/new" : "/dir/link";
    Options.DarwinFiles = darwin_test::symbolicOwnershipOptions(true);
    const auto BaseWanted = darwin_test::symbolicOwnershipServices(Kind);
    const auto Wanted = darwin_test::symbolicOwnershipServices(Kind, true);
    const auto Output = darwin_test::symbolicOwnershipOutput(Kind);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Repeat);
      auto R = run(Modes[Kind]);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " pc=" << R->PC << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 58);
      EXPECT_EQ(R->ReturnValue, 58u);
      EXPECT_EQ(R->StandardOutput, Output);
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        EXPECT_EQ(R->Services[I].Number, Class | Wanted[I].Number);
        EXPECT_EQ(R->Services[I].Result, Wanted[I].Value);
        EXPECT_EQ(R->Services[I].Error, Wanted[I].Error);
        for (unsigned J = 0; J != 6; ++J)
          if (Wanted[I].Arguments[J])
            EXPECT_EQ(R->Services[I].Arguments[J], *Wanted[I].Arguments[J]);
      }
      EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").GID, 31u);
      EXPECT_EQ(Options.DarwinFiles->SymbolicLinks.at("/dir/link"),
                (std::vector<uint8_t>{'.', '.', '/', 'd', 'a', 't', 'a'}));
    }
    for (bool Late : {false, true}) {
      Options.DarwinFiles = darwin_test::symbolicOwnershipOptions(true);
      if (Kind >= 2 || Late)
        Options.DarwinFiles->OwnershipMutations->CreatedSymbolicLinkObjects =
            false;
      else
        Options.DarwinFiles->OwnershipMutations->Objects.erase("/dir/link");
      const uint64_t FD = Late ? 8 : 3;
      unsigned Last = 0;
      while (Last != Wanted.size() && !(BaseWanted[Last].Number == 123 &&
                                        BaseWanted[Last].Arguments[0] == FD))
        ++Last;
      ASSERT_NE(Last, Wanted.size());
      auto R = run(Modes[Kind]);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R->Diagnostic,
                "Darwin object ownership provider is not configured");
      EXPECT_EQ(R->StandardOutput,
                Late && (Kind & 1) ? Output.substr(0, 6 * 160) : "");
      EXPECT_TRUE(R->StandardError.empty());
      ASSERT_EQ(R->Services.size(), Last + 1);
      EXPECT_EQ(R->Services.back().Number, Class | 364);
      EXPECT_EQ(R->Services.back().Arguments[1], Late ? 501u : UINT32_MAX);
      EXPECT_EQ(R->Services.back().Arguments[2],
                Late ? (Kind & 1 ? 20u : 31u) : UINT32_MAX);
      EXPECT_FALSE(R->Services.back().Result);
      EXPECT_FALSE(R->Services.back().Error);
    }
  }
}

TEST_P(DarwinProcess,
       DirectoryOwnershipMutationsPreserveIndependentObjectAndNamespaceState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (const char *Mode :
       {"directory-ownership-mutations", "virtual-directory-ownership",
        "created-directory-ownership"}) {
    const bool Created = llvm::StringRef(Mode) == "created-directory-ownership";
    Options.Arguments[2] = Created ? "/child" : "/dir";
    Options.DarwinFiles = darwin_test::directoryOwnershipOptions(Created);
    const auto Wanted = darwin_test::directoryOwnershipServices(Created);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Repeat);
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " pc=" << R->PC << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 55);
      EXPECT_EQ(R->StandardOutput, "O");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        EXPECT_EQ(R->Services[I].Number, Class | Wanted[I].Number);
        EXPECT_EQ(R->Services[I].Result, Wanted[I].Value);
        EXPECT_EQ(R->Services[I].Error, Wanted[I].Error);
      }
      EXPECT_EQ(R->ReturnValue, 55u);
      EXPECT_EQ(Options.DarwinFiles->Metadata.at("/").Mode, 0040700);
      EXPECT_EQ(Options.DarwinFiles->OwnershipMutations->CreatedRegularObjects,
                false);
    }
    Options.DarwinFiles->OwnershipMutations->Objects.clear();
    Options.DarwinFiles->OwnershipMutations->CreatedDirectoryObjects = false;
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic,
              "Darwin object ownership provider is not configured");
    ASSERT_EQ(R->Services.size(), Created ? 6u : 5u);
    EXPECT_EQ(R->Services.back().Number, Class | 16);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
}

TEST_P(DarwinProcess,
       DirectoryTimestampMutationsPreserveIndependentObjectAndNamespaceState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  Options.DarwinTime.emplace().TimeOfDay = DarwinTimeOfDay{21, 22};
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (const char *Mode :
       {"directory-timestamp-mutations", "virtual-directory-timestamps",
        "created-directory-timestamps"}) {
    const bool Created =
        llvm::StringRef(Mode) == "created-directory-timestamps";
    Options.Arguments[2] = Created ? "/child" : "/dir";
    Options.DarwinFiles = darwin_test::directoryTimestampOptions(Created);
    const auto Wanted = darwin_test::directoryTimestampServices(Created);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Repeat);
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " pc=" << R->PC << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 54);
      EXPECT_EQ(R->StandardOutput, "T");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        EXPECT_EQ(R->Services[I].Number, Class | Wanted[I].Number);
        EXPECT_EQ(R->Services[I].Result, Wanted[I].Value);
        EXPECT_EQ(R->Services[I].Error, Wanted[I].Error);
      }
      EXPECT_EQ(R->ReturnValue, 54u);
      EXPECT_EQ(Options.DarwinFiles->Metadata.at("/").Mode, 0040700);
      EXPECT_EQ(Options.DarwinFiles->TimestampMutations->CreatedRegularObjects,
                false);
    }
    Options.DarwinFiles->TimestampMutations->Objects.clear();
    Options.DarwinFiles->TimestampMutations->CreatedDirectoryObjects = false;
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic,
              "Darwin object timestamp provider is not configured");
    ASSERT_EQ(R->Services.size(), Created ? 6u : 5u);
    EXPECT_EQ(R->Services.back().Number, Class | 138);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
}

TEST_P(DarwinProcess,
       DirectoryModesPreserveIndependentObjectAndNamespaceState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (const char *Mode : {"directory-mode-mutations", "virtual-directory-mode",
                           "created-directory-mode"}) {
    const bool Created = llvm::StringRef(Mode) == "created-directory-mode";
    Options.Arguments[2] = Created ? "/child" : "/dir";
    Options.DarwinFiles = darwin_test::directoryModeOptions(Created);
    const auto Wanted = darwin_test::directoryModeServices(Created);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Repeat);
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " pc=" << R->PC << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 53);
      EXPECT_EQ(R->StandardOutput, "Q");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        EXPECT_EQ(R->Services[I].Number, Class | Wanted[I].Number);
        EXPECT_EQ(R->Services[I].Result, Wanted[I].Value);
        EXPECT_EQ(R->Services[I].Error, Wanted[I].Error);
      }
      EXPECT_EQ(R->ReturnValue, 53u);
      EXPECT_EQ(Options.DarwinFiles->Metadata.at("/").Mode, 0040700);
      EXPECT_EQ(Options.DarwinFiles->ModeMutations->CreatedRegularObjects,
                false);
    }
    Options.DarwinFiles->ModeMutations->Objects.clear();
    Options.DarwinFiles->ModeMutations->CreatedDirectoryObjects = false;
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic, "Darwin object mode provider is not configured");
    ASSERT_EQ(R->Services.size(), Created ? 6u : 5u);
    EXPECT_EQ(R->Services.back().Number, Class | 15);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
}

TEST_P(DarwinProcess,
       FileSystemSnapshotsPreserveLayoutAndIndependentObjectLifetime) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  Options.DarwinTime.reset();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  const auto Common = darwin_test::fileSystemServices(),
             Virtual = darwin_test::createdFileSystemServices();
  const auto Bytes = darwin_test::createdFileSystemRecords();
  const std::string Literal(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size());
  ASSERT_EQ(Common.size(), 49u);
  ASSERT_EQ(Virtual.size(), 9u);
  for (bool Created : {false, true}) {
    const char *Mode =
        Created ? "virtual-filesystem-creation" : "filesystem-observations";
    const auto &Wanted = Created ? Virtual : Common;
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      Options.DarwinFiles = Created ? darwin_test::createdFileSystemOptions()
                                    : darwin_test::fileSystemOptions();
      Options.DarwinFiles->WritableFiles.insert("/data");
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, Created ? 51 : 50);
      EXPECT_EQ(R->StandardOutput, Created ? Literal : "W");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        const auto &E = R->Services[I];
        const auto &W = Wanted[I];
        EXPECT_EQ(E.Number, Class | W.Number);
        EXPECT_EQ(E.Result, W.Value);
        EXPECT_EQ(E.Error, W.Error);
        if (W.Object)
          EXPECT_EQ(E.Arguments[0], *W.Object);
        if (W.Output)
          EXPECT_EQ(E.Arguments[1], *W.Output);
        const auto N = W.Number & 0xffffff;
        if (N == 345 || N == 346)
          for (unsigned J = 2; J != 6; ++J)
            EXPECT_EQ(E.Arguments[J], UINT64_MAX);
      }
    }
    for (bool Missing : {false, true}) {
      Options.DarwinFiles = Created ? darwin_test::createdFileSystemOptions()
                                    : darwin_test::fileSystemOptions();
      Options.DarwinFiles->WritableFiles.insert("/data");
      if (Missing)
        Options.DarwinFiles->FileSystemObservations.reset();
      else {
        Options.DarwinFiles->FileSystemObservations->Objects.clear();
        Options.DarwinFiles->FileSystemObservations->CreatedRegularObjects =
            false;
      }
      auto R = run(Mode);
      ASSERT_TRUE(bool(R));
      EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(
          R->Diagnostic,
          Missing
              ? "Darwin filesystem observation environment is not configured"
              : "Darwin object filesystem snapshot is not configured");
      ASSERT_EQ(R->Services.size(), Created ? 2u : 8u);
      EXPECT_FALSE(R->Services.back().Result);
      EXPECT_FALSE(R->Services.back().Error);
    }
  }
}

TEST_P(DarwinProcess,
       CreatedDirectoryFileSystemsKeepActualParentAndHeldLifetime) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/child";
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  Options.DarwinTime.reset();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (bool Literal : {false, true}) {
    const char *Mode = Literal ? "virtual-created-directory-filesystem"
                               : "created-directory-filesystem";
    const auto Wanted =
        darwin_test::createdDirectoryFileSystemServices(Literal);
    ASSERT_EQ(Wanted.size(), Literal ? 35u : 33u);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      Options.DarwinFiles = darwin_test::createdDirectoryFileSystemOptions();
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 56);
      EXPECT_EQ(R->StandardOutput,
                darwin_test::createdDirectoryFileSystemOutput(Literal));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        const auto &E = R->Services[I];
        const auto &W = Wanted[I];
        EXPECT_EQ(E.Number, Class | W.Number);
        EXPECT_EQ(E.Result, W.Value);
        EXPECT_EQ(E.Error, W.Error);
        if (W.Object)
          EXPECT_EQ(E.Arguments[0], *W.Object);
        if (W.Output)
          EXPECT_EQ(E.Arguments[1], *W.Output);
        const auto N = W.Number & 0xffffff;
        if (N == 345 || N == 346)
          for (unsigned J = 2; J != 6; ++J)
            EXPECT_EQ(E.Arguments[J], UINT64_MAX);
      }
    }
  }
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    Options.DarwinFiles = darwin_test::createdDirectoryFileSystemOptions();
    auto &F = Options.DarwinFiles->FileSystemObservations;
    if (Variant == 0)
      F.reset();
    else if (Variant == 1)
      F->Objects.clear();
    else {
      F->CreatedDirectoryObjects = false;
      F->CreatedRegularObjects = Variant == 2;
    }
    auto R = run("created-directory-filesystem");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(
        R->Diagnostic,
        Variant == 0
            ? "Darwin filesystem observation environment is not configured"
            : "Darwin object filesystem snapshot is not configured");
    EXPECT_TRUE(R->StandardOutput.empty());
    ASSERT_EQ(R->Services.size(), 4u);
    EXPECT_EQ(R->Services.back().Number, Class | 346);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
}

TEST_P(DarwinProcess,
       SymbolicFileSystemsKeepOwnSnapshotAndFollowedTargetSeparate) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = BatchedProcessTimeoutMicroseconds;
  Options.Arguments[2] = "/link";
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  Options.DarwinTime.reset();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  const char *Modes[] = {"symbolic-filesystem-observations",
                         "created-symbolic-filesystem",
                         "virtual-symbolic-filesystem"};
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Modes[Kind]);
    const auto Wanted = darwin_test::symbolicFileSystemServices(Kind);
    ASSERT_EQ(Wanted.size(), Kind == 1 ? 33u : Kind == 2 ? 35u : 32u);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Repeat);
      Options.DarwinFiles =
          darwin_test::symbolicLinkFileSystemOptions(Kind == 1);
      if (Kind == 2)
        Options.DarwinFiles->FileSystemObservations->Objects["/link"].Owner =
            17;
      auto R = run(Modes[Kind]);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, 57);
      EXPECT_EQ(R->StandardOutput, darwin_test::symbolicFileSystemOutput(Kind));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        const auto &E = R->Services[I];
        const auto &W = Wanted[I];
        EXPECT_EQ(E.Number, Class | W.Number);
        EXPECT_EQ(E.Result, W.Value);
        EXPECT_EQ(E.Error, W.Error);
        if (W.Object)
          EXPECT_EQ(E.Arguments[0], *W.Object);
        if (W.Output)
          EXPECT_EQ(E.Arguments[1], *W.Output);
        const auto N = W.Number & 0xffffff;
        if (N == 345 || N == 346)
          for (unsigned J = 2; J != 6; ++J)
            EXPECT_EQ(E.Arguments[J], UINT64_MAX);
      }
    }
  }
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    Options.DarwinFiles = darwin_test::symbolicLinkFileSystemOptions(true);
    auto &F = Options.DarwinFiles->FileSystemObservations;
    if (Variant == 0)
      F.reset();
    else if (Variant == 1)
      F->Objects.clear();
    else {
      F->CreatedSymbolicLinkObjects = false;
      F->CreatedRegularObjects = F->CreatedDirectoryObjects = true;
    }
    auto R = run(Modes[1]);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(
        R->Diagnostic,
        Variant == 0
            ? "Darwin filesystem observation environment is not configured"
            : "Darwin object filesystem snapshot is not configured");
    EXPECT_TRUE(R->StandardOutput.empty());
    ASSERT_EQ(R->Services.size(), 4u);
    EXPECT_EQ(R->Services.back().Number, Class | 346);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
  }
}

TEST_P(DarwinProcess, MountEnumerationKeepsOrderedSnapshotsAndRefreshCache) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinTime.reset();
  Options.DarwinSystem.reset();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (bool Virtual : {false, true}) {
    const auto Wanted = darwin_test::mountEnumerationServices(Virtual);
    ASSERT_EQ(Wanted.size(), Virtual ? 19u : 16u);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      Options.DarwinFiles = darwin_test::mountEnumerationOptions();
      auto R = run(Virtual ? "virtual-mount-enumeration" : "mount-enumeration");
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, Virtual ? 53 : 52);
      EXPECT_EQ(R->StandardOutput,
                Virtual ? darwin_test::mountEnumerationOutput() : "M");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        const auto &E = R->Services[I];
        const auto &W = Wanted[I];
        EXPECT_EQ(E.Number, Class | W.Number);
        EXPECT_EQ(E.Result, W.Value);
        EXPECT_EQ(E.Error, W.Error);
        for (unsigned J = 0; J != 6; ++J)
          if (W.Arguments[J])
            EXPECT_EQ(E.Arguments[J], *W.Arguments[J]);
      }
    }
  }
  Options.DarwinFiles.emplace();
  auto R = run("mount-enumeration");
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R->Diagnostic,
            "Darwin mount enumeration environment is not configured");
  EXPECT_TRUE(R->StandardOutput.empty());
  ASSERT_EQ(R->Services.size(), 1u);
  EXPECT_FALSE(R->Services[0].Result);
  EXPECT_FALSE(R->Services[0].Error);
}

TEST_P(DarwinProcess, TimestampMutationsKeepExplicitClockAndObjectAuthority) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  Options.DarwinTime.emplace().TimeOfDay = DarwinTimeOfDay{0, 0};
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  const auto Common = darwin_test::timestampServices(),
             Virtual = darwin_test::createdTimestampServices();
  const auto Bytes = darwin_test::createdTimestampRecords();
  const std::string Literal(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size());
  ASSERT_EQ(Common.size(), 124u);
  ASSERT_EQ(Virtual.size(), 10u);
  for (bool Created : {false, true}) {
    const char *Mode =
        Created ? "virtual-timestamp-creation" : "ordinary-timestamp-mutations";
    const auto &Wanted = Created ? Virtual : Common;
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      Options.DarwinFiles = Created ? darwin_test::createdTimestampOptions()
                                    : darwin_test::timestampOptions();
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited)
          << R->Diagnostic << " instructions=" << R->Instructions
          << " services=" << R->Services.size();
      EXPECT_EQ(R->ExitStatus, Created ? 49 : 48);
      EXPECT_EQ(R->StandardOutput, Created ? Literal : "T");
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      ASSERT_EQ(R->Services.size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        SCOPED_TRACE(I);
        const auto &E = R->Services[I];
        const auto &W = Wanted[I];
        EXPECT_EQ(E.Number, Class | W.Number);
        EXPECT_EQ(E.Result, W.Value);
        EXPECT_EQ(E.Error, W.Error);
        if (W.Object)
          EXPECT_EQ(E.Arguments[0], *W.Object);
        if (W.Input)
          EXPECT_EQ(E.Arguments[1], *W.Input);
        const auto N = W.Number & 0xffffff;
        if (N == 138 || N == 139)
          for (unsigned J = 2; J != 6; ++J)
            EXPECT_EQ(E.Arguments[J], UINT64_MAX);
      }
    }
    for (bool Missing : {false, true}) {
      Options.DarwinFiles = Created ? darwin_test::createdTimestampOptions()
                                    : darwin_test::timestampOptions();
      if (Missing)
        Options.DarwinFiles->TimestampMutations.reset();
      else {
        Options.DarwinFiles->TimestampMutations->Objects.clear();
        Options.DarwinFiles->TimestampMutations->CreatedRegularObjects = false;
      }
      auto R = run(Mode);
      ASSERT_TRUE(bool(R));
      EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R->Diagnostic,
                Missing
                    ? "Darwin ordinary timestamp environment is not configured"
                    : "Darwin object timestamp provider is not configured");
      ASSERT_EQ(R->Services.size(), Created ? 3u : 8u);
      EXPECT_FALSE(R->Services.back().Result);
      EXPECT_FALSE(R->Services.back().Error);
    }
  }
  Options.DarwinFiles = darwin_test::createdTimestampOptions();
  Options.DarwinTime.reset();
  auto R = run("virtual-timestamp-creation");
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R->Diagnostic, "Darwin time_of_day is not configured");
  ASSERT_EQ(R->Services.size(), 5u);
  EXPECT_EQ(R->Services.back().Arguments[1], 0u);
}

TEST_P(DarwinProcess, FullAndBarrierSynchronizationKeepIndependentObjectState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  const auto Common = darwin_test::fullSynchronizationServices();
  const auto Virtual = darwin_test::createdFullSynchronizationServices();
  const auto Bytes = darwin_test::createdFullSynchronizationRecord();
  std::string Literal(reinterpret_cast<const char *>(Bytes.data()),
                      Bytes.size());
  Literal += Literal;
  auto check =
      [&](const ProcessResult &R,
          llvm::ArrayRef<darwin_test::FullSynchronizationService> Wanted,
          int Code, llvm::StringRef Output) {
        ASSERT_EQ(R.Stop, ProcessStopReason::Exited)
            << R.Diagnostic << " instructions=" << R.Instructions
            << " services=" << R.Services.size();
        EXPECT_EQ(R.ExitStatus, Code);
        EXPECT_EQ(R.ReturnValue, uint64_t(Code));
        EXPECT_EQ(R.StandardOutput, Output);
        EXPECT_TRUE(R.StandardError.empty());
        EXPECT_EQ(R.SelectedBackend, GetParam().Backend);
        ASSERT_EQ(R.Services.size(), Wanted.size());
        for (unsigned I = 0; I != Wanted.size(); ++I) {
          SCOPED_TRACE(I);
          const auto &E = R.Services[I];
          const auto &W = Wanted[I];
          EXPECT_EQ(E.Number, Class | W.Number);
          EXPECT_EQ(E.Result, W.Value);
          EXPECT_EQ(E.Error, W.Error);
          if (W.FD)
            EXPECT_EQ(E.Arguments[0], *W.FD);
          if (W.Command)
            EXPECT_EQ(E.Arguments[1], *W.Command);
          if (W.Argument)
            EXPECT_EQ(E.Arguments[2], *W.Argument);
          const bool Sync =
              (W.Number & 0xffffff) == 92 || (W.Number & 0xffffff) == 406;
          const bool Full =
              Sync && W.Command &&
              (uint32_t(*W.Command) == 51 || uint32_t(*W.Command) == 85);
          for (unsigned J = 3; J != 6; ++J)
            EXPECT_EQ(E.Arguments[J], Full ? UINT64_MAX : 0u);
        }
      };
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    Options.DarwinFiles = darwin_test::fullSynchronizationOptions();
    auto R = run("ordinary-full-barrier-synchronization");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    check(*R, Common, 46, "F");
    Options.DarwinFiles = darwin_test::createdFullSynchronizationOptions();
    R = run("virtual-full-barrier-creation");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    check(*R, Virtual, 47, Literal);
  }
  for (bool Created : {false, true})
    for (bool Full : {true, false})
      for (bool Missing : {false, true}) {
        Options.DarwinFiles =
            Created ? darwin_test::createdFullSynchronizationOptions()
                    : darwin_test::fullSynchronizationOptions();
        if (Full) {
          if (Missing)
            Options.DarwinFiles->FullSynchronization.reset();
          else {
            Options.DarwinFiles->FullSynchronization->Objects.clear();
            Options.DarwinFiles->FullSynchronization->CreatedRegularObjects =
                false;
          }
        } else {
          if (Missing)
            Options.DarwinFiles->BarrierSynchronization.reset();
          else {
            Options.DarwinFiles->BarrierSynchronization->Objects.clear();
            Options.DarwinFiles->BarrierSynchronization->CreatedRegularObjects =
                false;
          }
        }
        auto R = run(Created ? "virtual-full-barrier-creation"
                             : "ordinary-full-barrier-synchronization");
        ASSERT_TRUE(bool(R));
        EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
        EXPECT_EQ(R->Diagnostic,
                  std::string("Darwin ") +
                      (Missing
                           ? (Full ? "full synchronization environment is not "
                                     "configured"
                                   : "barrier synchronization environment is "
                                     "not configured")
                           : (Full ? "regular-object full synchronization "
                                     "provider is not configured"
                                   : "regular-object barrier synchronization "
                                     "provider is not configured")));
        const unsigned Prefix = Created ? (Full ? 2 : 3) : (Full ? 6 : 17);
        ASSERT_EQ(R->Services.size(), Prefix + 1);
        const auto &Last = R->Services.back();
        EXPECT_EQ(Last.Number, Class | (Created && !Full ? 406u : 92u));
        EXPECT_EQ(Last.Arguments[0], 3u);
        EXPECT_EQ(Last.Arguments[1], Full ? 51u : 85u);
        EXPECT_FALSE(Last.Result);
        EXPECT_FALSE(Last.Error);
      }
  Options.DarwinFiles = darwin_test::fullSynchronizationOptions();
  Options.DarwinFiles->FullSynchronization.reset();
  Options.DarwinFiles->BarrierSynchronization.reset();
  Options.DarwinFiles->Synchronization.emplace().Objects.insert("/data");
  Options.DarwinFiles->DataSynchronization.emplace().Objects.insert("/data");
  auto R = run("ordinary-full-barrier-synchronization");
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
  ASSERT_EQ(R->Services.size(), 7u);
  EXPECT_FALSE(R->Services.back().Result);
}

TEST_P(DarwinProcess,
       OrdinaryFileSynchronizationKeepsProviderAndCommittedState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::synchronizationOptions();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  constexpr uint64_t FDs[] = {3, 5, 4, 0xffffffffULL}, Values[] = {0, 0, 0, 9};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-file-synchronization");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 40);
    EXPECT_EQ(R->ReturnValue, 40u);
    EXPECT_EQ(R->StandardOutput, "S");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 68u);
    constexpr uint64_t PrefixNumbers[] = {5, 5, 41, 199, 339},
                       PrefixValues[] = {3, 4, 5, 2, 0};
    for (unsigned I = 0; I != 5; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | PrefixNumbers[I]);
      EXPECT_EQ(R->Services[I].Result, PrefixValues[I]);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned H = 0; H != 3; ++H)
      for (unsigned N = 0; N != 2; ++N) {
        const unsigned Start = 5 + H * 18 + N * 9;
        for (unsigned I = 0; I != 4; ++I) {
          const auto &E = R->Services[Start + I];
          EXPECT_EQ(E.Number, Class | High[H] | (N ? 408 : 95));
          EXPECT_EQ(E.Result, Values[I]);
          EXPECT_EQ(E.Error, I == 3);
          EXPECT_EQ(E.Arguments[0], High[H] | FDs[I]);
          for (unsigned J = 1; J != 6; ++J)
            EXPECT_EQ(E.Arguments[J], 0u);
        }
        for (unsigned I = 0; I != 3; ++I) {
          const auto &E = R->Services[Start + 4 + I];
          EXPECT_EQ(E.Number, Class | 339);
          EXPECT_EQ(E.Result, 0u);
          EXPECT_EQ(E.Error, false);
        }
        for (unsigned I = 0; I != 2; ++I) {
          const auto &E = R->Services[Start + 7 + I];
          EXPECT_EQ(E.Number, Class | 199);
          EXPECT_EQ(E.Result, I ? 0u : 2u);
          EXPECT_EQ(E.Error, false);
        }
      }
    constexpr uint64_t TailNumbers[] = {6, 95, 95, 6, 408, 408, 339, 6, 4},
                       TailValues[] = {0, 9, 0, 0, 9, 0, 0, 0, 1};
    for (unsigned I = 0; I != 9; ++I) {
      const auto &E = R->Services[59 + I];
      EXPECT_EQ(E.Number, Class | TailNumbers[I]);
      EXPECT_EQ(E.Result, TailValues[I]);
      EXPECT_EQ(E.Error, I == 1 || I == 4);
    }
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").ChangeTime.Seconds, 5);
    EXPECT_EQ(Options.DarwinFiles->Synchronization->Objects,
              (std::set<std::string>{"/data"}));
  }
  Options.DarwinFiles->Synchronization->Objects.clear();
  auto Unknown = run("ordinary-file-synchronization");
  ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
  EXPECT_EQ(Unknown->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Unknown->Diagnostic, "Darwin regular-object file integrity "
                                 "completion provider is not configured");
  ASSERT_EQ(Unknown->Services.size(), 6u);
  EXPECT_EQ(Unknown->Services.back().Number, Class | 95);
  EXPECT_FALSE(Unknown->Services.back().Result);
  EXPECT_FALSE(Unknown->Services.back().Error);
}

TEST_P(DarwinProcess,
       CreatedRegularAttributesKeepIndependentProvidersAndObjectState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinSystem.emplace().Credentials =
      darwin_test::createdAttributeCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  auto check = [&](const ProcessResult &R,
                   llvm::ArrayRef<darwin_test::CreatedAttributeService> Wanted,
                   int Status, llvm::StringRef Output) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited)
        << R.Diagnostic << " instructions=" << R.Instructions << " pc=" << R.PC
        << " services=" << R.Services.size() << " last_number="
        << (R.Services.empty() ? 0 : R.Services.back().Number);
    EXPECT_EQ(R.ExitStatus, Status);
    EXPECT_EQ(R.ReturnValue, uint64_t(Status));
    EXPECT_EQ(R.StandardOutput, Output);
    EXPECT_TRUE(R.StandardError.empty());
    EXPECT_EQ(R.SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R.Services.size(), Wanted.size());
    for (unsigned I = 0; I != Wanted.size(); ++I) {
      SCOPED_TRACE(I);
      const auto &E = R.Services[I];
      EXPECT_EQ(E.Number, Class | Wanted[I].Number);
      EXPECT_EQ(E.Result, Wanted[I].Value);
      EXPECT_EQ(E.Error, Wanted[I].Error);
      if (Wanted[I].FD)
        EXPECT_EQ(E.Arguments[0], *Wanted[I].FD);
    }
  };
  for (bool Virtual : {false, true}) {
    const char *Mode = Virtual ? "virtual-created-regular-attributes"
                               : "created-regular-attributes";
    const auto Wanted =
        Virtual ? llvm::ArrayRef(darwin_test::VirtualCreatedAttributeServices)
                : llvm::ArrayRef(darwin_test::CreatedAttributeServices);
    const auto Output =
        Virtual ? llvm::fromHex(darwin_test::VirtualCreatedAttributeStdoutHex)
                : "A";
    Options.DarwinFiles = darwin_test::createdAttributeOptions();
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Repeat);
      auto R = run(Mode);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_NO_FATAL_FAILURE(check(*R, Wanted, Virtual ? 45 : 44, Output));
      EXPECT_EQ(Options.DarwinFiles->CreationPolicy->FirstInode, 100u);
      EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
                (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
      EXPECT_EQ(Options.DarwinSystem->Credentials->GroupAccessList,
                (std::vector<uint32_t>{20, 31}));
    }
    for (bool Owner : {false, true})
      for (unsigned Variant = 0; Variant != 4; ++Variant) {
        Options.DarwinFiles =
            darwin_test::createdAttributeOptions(Owner, !Owner);
        if (Variant == 1) {
          if (Owner)
            Options.DarwinFiles->OwnershipMutations.reset();
          else
            Options.DarwinFiles->ModeMutations.reset();
        } else if (Variant == 2) {
          auto &Map = Owner ? Options.DarwinFiles->OwnershipMutations->Objects
                            : Options.DarwinFiles->ModeMutations->Objects;
          Map["/data"] =
              Owner ? DarwinFileTime{81, 82} : DarwinFileTime{71, 72};
        } else if (Variant == 3) {
          if (Owner)
            Options.DarwinFiles->OwnershipMutations.emplace();
          else
            Options.DarwinFiles->ModeMutations.emplace();
        }
        auto R = run(Mode);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
        const unsigned Count = Virtual ? (Owner ? 4 : 3) : (Owner ? 52 : 12);
        ASSERT_EQ(R->Services.size(), Count);
        for (unsigned I = 0; I + 1 < Count; ++I) {
          EXPECT_EQ(R->Services[I].Number, Class | Wanted[I].Number);
          EXPECT_EQ(R->Services[I].Result, Wanted[I].Value);
          EXPECT_EQ(R->Services[I].Error, Wanted[I].Error);
        }
        EXPECT_EQ(R->Services.back().Number,
                  Class | (Owner ? (Virtual ? 123u : 16u) : 124u));
        EXPECT_FALSE(R->Services.back().Result);
        EXPECT_FALSE(R->Services.back().Error);
        EXPECT_EQ(R->Diagnostic,
                  Owner ? (Variant == 1 ? "Darwin ordinary ownership "
                                          "environment is not configured"
                                        : "Darwin object ownership "
                                          "provider is not configured")
                        : (Variant == 1 ? "Darwin ordinary mode mutation "
                                          "environment is not configured"
                                        : "Darwin object mode provider "
                                          "is not configured"));
        EXPECT_TRUE(R->StandardOutput.empty());
        EXPECT_TRUE(R->StandardError.empty());
      }
  }
}

TEST_P(DarwinProcess,
       CreatedRegularAdvisoryLocksKeepProviderAndDescriptionStatus) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::createdAdvisoryLockOptions();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("created-regular-advisory-locks");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 43);
    EXPECT_EQ(R->ReturnValue, 43u);
    EXPECT_EQ(R->StandardOutput, "L");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(),
              std::size(darwin_test::CreatedAdvisoryServices));
    for (unsigned I = 0; I != std::size(darwin_test::CreatedAdvisoryServices);
         ++I) {
      SCOPED_TRACE(I);
      const auto &Expected = darwin_test::CreatedAdvisoryServices[I];
      const auto &E = R->Services[I];
      EXPECT_EQ(E.Number, Class | Expected.Number);
      EXPECT_EQ(E.Result, Expected.Value);
      EXPECT_EQ(E.Error, Expected.Error);
      if (Expected.FD != UINT64_MAX)
        EXPECT_EQ(E.Arguments[0], Expected.FD);
      if (Expected.How != UINT64_MAX)
        EXPECT_EQ(E.Arguments[1], Expected.How);
    }
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
    EXPECT_EQ(Options.DarwinFiles->CreationPolicy->FirstInode, 100u);
  }
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    Options.DarwinFiles = darwin_test::createdAdvisoryLockOptions(false);
    if (Variant == 1)
      Options.DarwinFiles->AdvisoryLocks.reset();
    if (Variant == 2)
      Options.DarwinFiles->AdvisoryLocks->Objects.insert("/data");
    auto R = run("created-regular-advisory-locks");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    ASSERT_EQ(R->Services.size(), 11u);
    EXPECT_EQ(R->Services.back().Number, Class | 131u);
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
    EXPECT_EQ(
        R->Diagnostic,
        Variant == 1
            ? "Darwin whole-file advisory lock environment is not configured"
            : "Darwin regular-object advisory lock provider is not configured");
  }
}

TEST_P(DarwinProcess,
       CreatedRegularSynchronizationKeepsIndependentProvidersAndState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::createdSynchronizationOptions();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("created-regular-synchronization");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 42);
    EXPECT_EQ(R->ReturnValue, 42u);
    EXPECT_EQ(R->StandardOutput, "C");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 147u);
    for (unsigned I = 0; I != 9; ++I) {
      EXPECT_EQ(R->Services[I].Number,
                Class | darwin_test::CreatedSynchronizationPrefixNumbers[I]);
      EXPECT_EQ(R->Services[I].Result,
                darwin_test::CreatedSynchronizationPrefixValues[I]);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    constexpr uint64_t Highs[] = {0, 0x1234567800000000ULL,
                                  0xffffffff00000000ULL},
                       Numbers[] = {95, 408, 187},
                       FDs[] = {3, 5, 10, 20, 4, 0xffffffffULL};
    for (unsigned H = 0; H != 3; ++H)
      for (unsigned N = 0; N != 3; ++N) {
        const auto Start = 9 + H * 36 + N * 12;
        for (unsigned I = 0; I != 6; ++I) {
          const auto &E = R->Services[Start + I];
          EXPECT_EQ(E.Number, Class | Highs[H] | Numbers[N]);
          EXPECT_EQ(E.Arguments[0], Highs[H] | FDs[I]);
          EXPECT_EQ(E.Result, I == 5 ? 9u : 0u);
          EXPECT_EQ(E.Error, I == 5);
        }
        for (unsigned I = 0; I != 6; ++I) {
          const auto &E = R->Services[Start + 6 + I];
          EXPECT_EQ(E.Number, Class | (I < 3 ? 339u : I < 5 ? 199u : 92u));
          EXPECT_EQ(E.Result, I < 3    ? 0u
                              : I == 3 ? 10u
                              : I == 4 ? 3u
                                       : 0x10002u);
          EXPECT_EQ(E.Error, false);
        }
      }
    for (unsigned I = 0; I != 30; ++I) {
      const auto &E = R->Services[117 + I];
      const auto Value = darwin_test::CreatedSynchronizationTailValues[I];
      EXPECT_EQ(E.Number,
                Class | darwin_test::CreatedSynchronizationTailNumbers[I]);
      EXPECT_EQ(E.Result, Value);
      EXPECT_EQ(E.Error, Value == 9);
    }
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
    EXPECT_EQ(Options.DarwinFiles->CreationPolicy->FirstInode, 100u);
  }
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    Options.DarwinFiles =
        darwin_test::createdSynchronizationOptions(Variant == 0, Variant == 1);
    if (Variant == 3) {
      Options.DarwinFiles->Synchronization.reset();
      Options.DarwinFiles->DataSynchronization.reset();
    }
    if (Variant == 4) {
      Options.DarwinFiles->Synchronization->Objects.insert("/data");
      Options.DarwinFiles->DataSynchronization->Objects.insert("/data");
    }
    auto R = run("created-regular-synchronization");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    ASSERT_EQ(R->Services.size(), Variant == 0 ? 34u : 10u);
    EXPECT_EQ(R->Services.back().Number, Class | (Variant == 0 ? 187u : 95u));
    EXPECT_FALSE(R->Services.back().Result);
    EXPECT_FALSE(R->Services.back().Error);
    EXPECT_EQ(R->Diagnostic,
              Variant == 0 ? "Darwin regular-object data integrity completion "
                             "provider is not configured"
              : Variant == 3
                  ? "Darwin file synchronization environment is not configured"
                  : "Darwin regular-object file integrity completion provider "
                    "is not configured");
  }
}

TEST_P(DarwinProcess,
       OrdinaryDataSynchronizationKeepsProviderAndCommittedState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::dataSynchronizationOptions();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  constexpr uint64_t FDs[] = {3, 5, 4, 0xffffffffULL}, Values[] = {0, 0, 0, 9};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-data-synchronization");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 41);
    EXPECT_EQ(R->ReturnValue, 41u);
    EXPECT_EQ(R->StandardOutput, "D");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 41u);
    constexpr uint64_t PrefixNumbers[] = {5, 5, 41, 199, 339},
                       PrefixValues[] = {3, 4, 5, 2, 0};
    for (unsigned I = 0; I != 5; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | PrefixNumbers[I]);
      EXPECT_EQ(R->Services[I].Result, PrefixValues[I]);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned H = 0; H != 3; ++H) {
      const unsigned Start = 5 + H * 9;
      for (unsigned I = 0; I != 4; ++I) {
        const auto &E = R->Services[Start + I];
        EXPECT_EQ(E.Number, Class | High[H] | 187);
        EXPECT_EQ(E.Result, Values[I]);
        EXPECT_EQ(E.Error, I == 3);
        EXPECT_EQ(E.Arguments[0], High[H] | FDs[I]);
        for (unsigned J = 1; J != 6; ++J)
          EXPECT_EQ(E.Arguments[J], 0u);
      }
      for (unsigned I = 0; I != 3; ++I) {
        const auto &E = R->Services[Start + 4 + I];
        EXPECT_EQ(E.Number, Class | 339);
        EXPECT_EQ(E.Result, 0u);
        EXPECT_EQ(E.Error, false);
      }
      for (unsigned I = 0; I != 2; ++I) {
        const auto &E = R->Services[Start + 7 + I];
        EXPECT_EQ(E.Number, Class | 199);
        EXPECT_EQ(E.Result, I ? 0u : 2u);
        EXPECT_EQ(E.Error, false);
      }
    }
    constexpr uint64_t TailNumbers[] = {6, 187, 187, 6, 187, 187, 339, 6, 4},
                       TailValues[] = {0, 9, 0, 0, 9, 0, 0, 0, 1};
    for (unsigned I = 0; I != 9; ++I) {
      const auto &E = R->Services[32 + I];
      EXPECT_EQ(E.Number, Class | TailNumbers[I]);
      EXPECT_EQ(E.Result, TailValues[I]);
      EXPECT_EQ(E.Error, I == 1 || I == 4);
    }
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").ChangeTime.Seconds, 5);
    EXPECT_EQ(Options.DarwinFiles->DataSynchronization->Objects,
              (std::set<std::string>{"/data"}));
  }
  Options.DarwinFiles->DataSynchronization->Objects.clear();
  auto Unknown = run("ordinary-data-synchronization");
  ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
  EXPECT_EQ(Unknown->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Unknown->Diagnostic, "Darwin regular-object data integrity "
                                 "completion provider is not configured");
  ASSERT_EQ(Unknown->Services.size(), 6u);
  EXPECT_EQ(Unknown->Services.back().Number, Class | 187);
  EXPECT_FALSE(Unknown->Services.back().Result);
  EXPECT_FALSE(Unknown->Services.back().Error);
}

TEST_P(DarwinProcess, OrdinaryAdvisoryLocksKeepDescriptionAndObjectLifetime) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::advisoryLockOptions();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  constexpr uint64_t FDs[] = {3, 5, 4, 5, 4, 3, 3, 4, 4, 3, 4, 4};
  constexpr uint64_t Actions[] = {2, 6, 5, 8, 5, 5, 6, 8, 6, 8, 6, 8};
  constexpr uint64_t Values[] = {0, 0, 35, 0, 0, 0, 35, 0, 35, 0, 0, 0};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-advisory-locks");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 39);
    EXPECT_EQ(R->ReturnValue, 39u);
    EXPECT_EQ(R->StandardOutput, "K");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 77u);
    constexpr uint64_t PrefixNumbers[] = {5, 5, 41, 199, 339},
                       PrefixValues[] = {3, 4, 5, 2, 0};
    for (unsigned I = 0; I != 5; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | PrefixNumbers[I]);
      EXPECT_EQ(R->Services[I].Result, PrefixValues[I]);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned H = 0; H != 3; ++H) {
      const unsigned Start = 5 + H * 17;
      for (unsigned I = 0; I != 12; ++I) {
        const auto &E = R->Services[Start + I];
        EXPECT_EQ(E.Number, Class | High[H] | 131);
        EXPECT_EQ(E.Result, Values[I]);
        EXPECT_EQ(E.Error, bool(Values[I]));
        EXPECT_EQ(E.Arguments[0], High[H] | FDs[I]);
        EXPECT_EQ(E.Arguments[1], High[H] | Actions[I]);
      }
      for (unsigned I = 0; I != 3; ++I) {
        const auto &E = R->Services[Start + 12 + I];
        EXPECT_EQ(E.Number, Class | 339);
        EXPECT_EQ(E.Result, 0u);
        EXPECT_EQ(E.Error, false);
      }
      for (unsigned I = 0; I != 2; ++I) {
        const auto &E = R->Services[Start + 15 + I];
        EXPECT_EQ(E.Number, Class | 199);
        EXPECT_EQ(E.Result, I ? 0u : 2u);
        EXPECT_EQ(E.Error, false);
      }
    }
    constexpr uint64_t FlagValues[] = {9, 9, 9, 0, 0, 0, 35, 0, 9};
    for (unsigned I = 0; I != 9; ++I) {
      const auto &E = R->Services[56 + I];
      EXPECT_EQ(E.Number, Class | 131);
      EXPECT_EQ(E.Result, FlagValues[I]);
      EXPECT_EQ(E.Error, bool(FlagValues[I]));
    }
    constexpr uint64_t TailNumbers[] = {339, 339, 131, 6,   131, 131,
                                        6,   131, 131, 339, 6,   4};
    constexpr uint64_t TailValues[] = {0, 0, 0, 0, 9, 35, 0, 0, 0, 0, 0, 1};
    for (unsigned I = 0; I != 12; ++I) {
      const auto &E = R->Services[65 + I];
      EXPECT_EQ(E.Number, Class | TailNumbers[I]);
      EXPECT_EQ(E.Result, TailValues[I]);
      EXPECT_EQ(E.Error, I == 4 || I == 5);
    }
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").ChangeTime.Seconds, 5);
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
    EXPECT_EQ(Options.DarwinFiles->AdvisoryLocks->Objects,
              (std::set<std::string>{"/data"}));
  }
  Options.DarwinFiles->AdvisoryLocks->Objects.clear();
  auto Unknown = run("ordinary-advisory-locks");
  ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
  EXPECT_EQ(Unknown->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Unknown->Diagnostic,
            "Darwin regular-object advisory lock provider is not configured");
  ASSERT_EQ(Unknown->Services.size(), 6u);
  EXPECT_EQ(Unknown->Services.back().Number, Class | 131);
  EXPECT_FALSE(Unknown->Services.back().Result);
  EXPECT_FALSE(Unknown->Services.back().Error);
}

TEST_P(DarwinProcess, OrdinaryOwnershipKeepsCurrentAuthorityAndObjectState) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.Arguments[2] = "/data";
  Options.DarwinFiles = darwin_test::ownershipOptions();
  Options.DarwinSystem.emplace().Credentials = darwin_test::modeCredentials();
  const uint64_t Class =
      GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  constexpr uint64_t Numbers[] = {16, 123, 468};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("ordinary-ownership-mutations");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 38);
    EXPECT_EQ(R->ReturnValue, 38u);
    EXPECT_EQ(R->StandardOutput, "O");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 77u);
    constexpr uint64_t PrefixNumbers[] = {25, 43, 5, 41, 199, 339};
    constexpr uint64_t PrefixResults[] = {501, 20, 3, 4, 2, 0};
    for (unsigned I = 0; I != 6; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | PrefixNumbers[I]);
      EXPECT_EQ(R->Services[I].Result, PrefixResults[I]);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned API = 0; API != 3; ++API)
      for (unsigned H = 0; H != 3; ++H) {
        const unsigned Start = 6 + (API * 3 + H) * 6;
        for (unsigned Mutation : {0u, 4u}) {
          const auto &E = R->Services[Start + Mutation];
          EXPECT_EQ(E.Number, Class | High[H] | Numbers[API]);
          EXPECT_EQ(E.Result, 0u);
          EXPECT_EQ(E.Error, false);
          EXPECT_EQ(E.Arguments[API == 2 ? 2 : 1],
                    High[H] | (Mutation == 4 || H == 1 ? UINT32_MAX : 501));
          EXPECT_EQ(E.Arguments[API == 2 ? 3 : 2],
                    High[H] | (Mutation == 4 || H == 2 ? UINT32_MAX : 20));
        }
        for (unsigned I : {1u, 2u, 3u, 5u}) {
          EXPECT_EQ(R->Services[Start + I].Number,
                    Class | (I == 2 ? 338 : 339));
          EXPECT_EQ(R->Services[Start + I].Result, 0u);
          EXPECT_EQ(R->Services[Start + I].Error, false);
        }
      }
    constexpr uint64_t TailNumbers[] = {16,  123, 468, 468, 468, 339,
                                        123, 339, 468, 339, 199, 6,
                                        123, 123, 339, 6,   4};
    constexpr uint64_t TailResults[] = {14, 9, 22, 22, 9, 0, 1, 0, 0,
                                        0,  2, 0,  9,  0, 0, 0, 1};
    for (unsigned I = 0; I != 17; ++I) {
      EXPECT_EQ(R->Services[60 + I].Number, Class | TailNumbers[I]);
      EXPECT_EQ(R->Services[60 + I].Result, TailResults[I]);
      EXPECT_EQ(R->Services[60 + I].Error, I < 5 || I == 6 || I == 12);
    }
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").UID, 501u);
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").GID, 31u);
    EXPECT_EQ(Options.DarwinFiles->Metadata.at("/data").ChangeTime.Seconds, 5);
    EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
              (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
  }
  Options.DarwinFiles->OwnershipMutations->Objects.clear();
  auto Unknown = run("ordinary-ownership-mutations");
  ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
  EXPECT_EQ(Unknown->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Unknown->Diagnostic,
            "Darwin object ownership provider is not configured");
  ASSERT_EQ(Unknown->Services.size(), 7u);
  EXPECT_EQ(Unknown->Services.back().Number, Class | 16);
  EXPECT_FALSE(Unknown->Services.back().Result);
  EXPECT_FALSE(Unknown->Services.back().Error);
}

TEST_P(DarwinProcess, ImmediatePselectPreservesTimespecMasksAndSharedBitmaps) {
  Options.InstructionQuantum = 1024;
  Options.Limits.TimeoutMicroseconds = 5000000;
  Options.DarwinFiles.emplace().Files["/data"] = {'a', 'b', 0, 0xff, 'e', 'f'};
  Options.Arguments[2] = "/data";
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const uint64_t Class = X64 ? 0x2000000 : 0;
  constexpr uint64_t Expected[] = {0,  3,  6,  3,  6,  9,  9,  12, 22, 14,
                                   22, 22, 22, 22, 22, 14, 14, 14, 9};
  constexpr uint32_t Counts[] = {
      0,          4, 5, 31, 32,         33,         64, 65, 0, UINT32_MAX,
      UINT32_MAX, 0, 0, 0,  UINT32_MAX, UINT32_MAX, 4,  6,  6};
  constexpr uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = run("immediate-pselect");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 37);
    EXPECT_EQ(R->StandardOutput, "q");
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    ASSERT_EQ(R->Services.size(), 139u);
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(R->Services[I].Number, Class | (I ? 41 : 5));
      EXPECT_EQ(R->Services[I].Result, 3u + I);
      EXPECT_EQ(R->Services[I].Error, false);
    }
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(R->Services[4 + I].Number, Class | 92);
      EXPECT_EQ(R->Services[4 + I].Result, I == 0 ? 31u : I == 1 ? 32u : 64u);
    }
    EXPECT_EQ(R->Services[7].Number, Class | 199);
    EXPECT_EQ(R->Services[7].Result, 6u);
    EXPECT_EQ(R->Services[7].Arguments[2], 2u);
    for (unsigned API = 0; API != 2; ++API)
      for (unsigned H = 0; H != 3; ++H)
        for (unsigned I = 0; I != 19; ++I) {
          const auto &E = R->Services[9 + (API * 3 + H) * 19 + I];
          EXPECT_EQ(E.Number, High[H] | Class | (API ? 395 : 394));
          EXPECT_EQ(E.Result, Expected[I]);
          EXPECT_EQ(E.Error, I >= 8);
          EXPECT_EQ(E.Arguments[0], High[H] | Counts[I]);
          EXPECT_EQ(E.Arguments[5], I == 15 ? UINT64_MAX : 0u);
          if (I == 9)
            EXPECT_EQ(E.Arguments[4], UINT64_MAX);
          else
            EXPECT_NE(E.Arguments[4], 0u);
        }
    EXPECT_EQ(R->Services[123].Number, Class | 199);
    EXPECT_EQ(R->Services[123].Result, 6u);
    EXPECT_EQ(R->Services[124].Number, Class | 92);
    EXPECT_EQ(R->Services[124].Result, 4u);
    for (unsigned I = 0; I != 4; ++I) {
      const auto &E = R->Services[126 + I];
      EXPECT_EQ(E.Number, Class | (I < 2 ? 394 : 395));
      EXPECT_EQ(E.Result, I % 2 ? 3u : 9u);
      EXPECT_EQ(E.Error, !(I % 2));
    }
    EXPECT_EQ(R->Services[130].Number, Class | 5);
    EXPECT_EQ(R->Services[130].Result, 3u);
    EXPECT_EQ(R->Services[131].Result, 0u);
    EXPECT_EQ(R->Services[132].Result, 6u);
  }
  for (const char *Mode :
       {"immediate-pselect-wait", "immediate-pselect-capacity",
        "immediate-pselect-mask"}) {
    const bool Capacity = llvm::StringRef(Mode) == "immediate-pselect-capacity";
    const bool Mask = llvm::StringRef(Mode) == "immediate-pselect-mask";
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->Diagnostic,
              Capacity
                  ? "Darwin select descriptor table capacity is not modeled"
              : Mask ? "Darwin pselect signal-mask state is not implemented"
                     : "Darwin pselect waiting is not implemented");
    EXPECT_EQ(R->StandardOutput, "!");
    EXPECT_TRUE(R->StandardError.empty());
    ASSERT_EQ(R->Services.size(), 2u);
    const auto &Event = R->Services.back();
    EXPECT_EQ(Event.Number, Class | (Capacity ? 395 : 394));
    EXPECT_EQ(Event.Arguments[0], Capacity ? 4u : 0u);
    EXPECT_EQ(Event.Arguments[1], UINT64_MAX);
    EXPECT_EQ(Event.Arguments[5] != 0, Mask);
    EXPECT_FALSE(Event.Result);
    EXPECT_FALSE(Event.Error);
  }
}

TEST_P(DarwinProcess, LibSystemImportsPreserveErrnoAndRawKernelAuthority) {
  Options.InstructionQuantum = 1024;
  Options.Arguments = {"libsystem"};
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->StandardInput = {'a', 'b', 0, 'c', 'd'};
  const char *Names[] = {"___error", "___error", "_getpid", "_getpid", "_write",
                         "_write",   "_read",    "_write",  "_read",   "_close",
                         "_close",   "_read",    "_write",  "_close"};
  const unsigned Counts[] = {0, 0, 0, 0, 3, 3, 3, 3, 3, 1, 1, 3, 3, 1};
  const uint64_t Errno =
      0x00007fffffbf0000ULL +
      (GetParam().ISA == GuestArchitecture::X64 ? 4096 : 16384);
  const uint64_t Values[] = {
      Errno,      Errno, 1000,       1000, 4,          UINT64_MAX, UINT64_MAX,
      UINT64_MAX, 3,     UINT64_MAX, 0,    UINT64_MAX, 1,          UINT64_MAX};
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    auto Linked = std::filesystem::path(Path.string() + "-libsystem-" + Kind);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto R = emulateProcess(Linked, GetParam().OS, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 58);
      EXPECT_EQ(R->ReturnValue, 58);
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      EXPECT_EQ(R->StandardOutput, std::string("A\0\xff"
                                               "B!",
                                               5));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_TRUE(R->Services.empty());
      EXPECT_EQ(R->Events, 15u);
      ASSERT_EQ(R->NativeCalls.size(), 14u);
      for (unsigned I = 0; I != 14; ++I) {
        SCOPED_TRACE(I);
        const auto &Call = R->NativeCalls[I];
        EXPECT_EQ(Call.Name, Names[I]);
        EXPECT_EQ(Call.Module, "/usr/lib/libSystem.B.dylib");
        EXPECT_EQ(Call.ArgumentCount, Counts[I]);
        EXPECT_EQ(Call.Result, Values[I]);
        EXPECT_TRUE(Call.ReturnAddress.has_value());
      }
      EXPECT_EQ(R->NativeCalls[12].Arguments[0], 0x1234567800000001ULL);
      EXPECT_EQ(R->NativeCalls[13].Arguments[0], 0x12345678ffffffffULL);
    }
    Options.Arguments = {"libsystem", "exit"};
    auto Exit = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    ASSERT_EQ(Exit->Stop, ProcessStopReason::Exited) << Exit->Diagnostic;
    EXPECT_EQ(Exit->ExitStatus, 35);
    EXPECT_EQ(Exit->StandardOutput, "X");
    EXPECT_TRUE(Exit->Services.empty());
    EXPECT_EQ(Exit->Events, 2u);
    ASSERT_EQ(Exit->NativeCalls.size(), 2u);
    EXPECT_EQ(Exit->NativeCalls[1].Name, "__exit");
    EXPECT_EQ(Exit->NativeCalls[1].Arguments[0], 0x123u);
    EXPECT_FALSE(Exit->NativeCalls[1].Result);
    Options.Arguments = {"libsystem", "noreturn"};
    auto Terminal = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Terminal)) << llvm::toString(Terminal.takeError());
    EXPECT_EQ(Terminal->Stop, ProcessStopReason::Exited)
        << Terminal->Diagnostic;
    EXPECT_EQ(Terminal->ExitStatus, 35);
    ASSERT_EQ(Terminal->NativeCalls.size(), 1u);
    EXPECT_EQ(Terminal->NativeCalls[0].Name, "__exit");
    EXPECT_FALSE(Terminal->NativeCalls[0].Result);
    EXPECT_TRUE(Terminal->StandardOutput.empty());
    Options.Arguments = {"libsystem", "frame"};
    auto Frame = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Frame)) << llvm::toString(Frame.takeError());
    EXPECT_EQ(Frame->Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_EQ(Frame->Diagnostic, "invalid Darwin libSystem return frame");
    ASSERT_EQ(Frame->NativeCalls.size(), 1u);
    EXPECT_EQ(Frame->NativeCalls[0].Name, "_getpid");
    EXPECT_FALSE(Frame->NativeCalls[0].Result);
    EXPECT_TRUE(Frame->Services.empty());
    Options.Arguments = {"libsystem", "spoof"};
    auto Spoof = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Spoof)) << llvm::toString(Spoof.takeError());
    EXPECT_EQ(Spoof->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_TRUE(Spoof->NativeCalls.empty());
    Options.Arguments = {"libsystem"};
    Options.Limits.Events = 1;
    auto Limited = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
    EXPECT_EQ(Limited->Events, 1u);
    EXPECT_TRUE(Limited->Services.empty());
    EXPECT_TRUE(Limited->StandardOutput.empty());
    ASSERT_EQ(Limited->NativeCalls.size(), 1u);
    EXPECT_EQ(Limited->NativeCalls[0].Name, "___error");
    EXPECT_EQ(Limited->NativeCalls[0].Result, Errno);
    Options.Limits.Events = process_defaults::Events;
  }
}

TEST_P(DarwinProcess, ClassicLazyImportsResolveBeforeCallsAndKeepSlotsStable) {
  Options.InstructionQuantum = 1024;
  Options.Arguments = {"lazy"};
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->StandardInput.emplace();
  const auto Linked = std::filesystem::path(Path.string() + "-libsystem-lazy");
  const char *Names[] = {"___error", "___error", "_getpid", "_getpid", "_write",
                         "_write",   "_read",    "_close",  "_read",   "_close",
                         "_getpid",  "_write",   "__exit"};
  const unsigned Counts[] = {0, 0, 0, 0, 3, 3, 3, 1, 3, 1, 0, 3, 1};
  const uint64_t Errno =
      0x00007fffffbf0000ULL +
      (GetParam().ISA == GuestArchitecture::X64 ? 4096 : 16384);
  const std::optional<uint64_t> Values[] = {
      Errno, Errno,      1000,       1000, 3, UINT64_MAX,  0,
      0,     UINT64_MAX, UINT64_MAX, 1000, 1, std::nullopt};
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    auto R = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, 83);
    EXPECT_FALSE(R->ReturnValue);
    EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    EXPECT_EQ(R->StandardOutput, std::string("A\0ZS", 4));
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_TRUE(R->Services.empty());
    EXPECT_EQ(R->Events, 13u);
    ASSERT_EQ(R->NativeCalls.size(), 13u);
    for (unsigned I = 0; I != 13; ++I) {
      const auto &Call = R->NativeCalls[I];
      EXPECT_EQ(Call.Name, Names[I]);
      EXPECT_EQ(Call.Module, "/usr/lib/libSystem.B.dylib");
      EXPECT_EQ(Call.ArgumentCount, Counts[I]);
      EXPECT_EQ(Call.Result, Values[I]);
      EXPECT_EQ(Call.ReturnAddress.has_value(), I != 12);
    }
  }
  Options.Arguments.push_back("exit");
  auto Exit = emulateProcess(Linked, GetParam().OS, Options);
  ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_EQ(Exit->Stop, ProcessStopReason::Exited) << Exit->Diagnostic;
  EXPECT_EQ(Exit->ExitStatus, 35);
  EXPECT_EQ(Exit->StandardOutput, "X");
  ASSERT_EQ(Exit->NativeCalls.size(), 2u);
  EXPECT_EQ(Exit->NativeCalls.back().Name, "__exit");
  EXPECT_EQ(Exit->NativeCalls.back().Arguments[0], 0x123u);
  EXPECT_FALSE(Exit->NativeCalls.back().Result);
  Options.Arguments.pop_back();
  Options.Limits.Events = 1;
  auto Limited = emulateProcess(Linked, GetParam().OS, Options);
  ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
  EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
  EXPECT_EQ(Limited->Events, 1u);
  EXPECT_TRUE(Limited->Services.empty());
  EXPECT_TRUE(Limited->StandardOutput.empty());
  ASSERT_EQ(Limited->NativeCalls.size(), 1u);
  EXPECT_EQ(Limited->NativeCalls.front().Name, "___error");
  EXPECT_EQ(Limited->NativeCalls.front().Result, Errno);
}

TEST_P(DarwinProcess, LibSystemFileCallsShareCursorsAndDecodeOptionalMode) {
  Options.InstructionQuantum = 1024;
  Options.Arguments = {"files", "/data", "/created", "/missing"};
  Options.DarwinFiles = darwin_test::libSystemFileOptions();
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    const auto Linked =
        std::filesystem::path(Path.string() + "-libsystem-files-" + Kind);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto R = emulateProcess(Linked, GetParam().OS, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 73);
      EXPECT_EQ(R->ReturnValue, 73u);
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      EXPECT_EQ(R->StandardOutput, std::string("F\0D", 3));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_TRUE(R->Services.empty());
      EXPECT_EQ(R->Events, 41u);
      ASSERT_EQ(R->NativeCalls.size(),
                std::size(darwin_test::LibSystemFileCalls));
      for (unsigned I = 0; I != R->NativeCalls.size(); ++I) {
        const auto &Call = R->NativeCalls[I];
        const auto &Expected = darwin_test::LibSystemFileCalls[I];
        EXPECT_EQ(Call.Name, Expected.Name);
        EXPECT_EQ(Call.ArgumentCount, Expected.Count);
        EXPECT_EQ(Call.Module, "/usr/lib/libSystem.B.dylib");
        EXPECT_TRUE(Call.ReturnAddress);
        EXPECT_TRUE(Call.Result);
      }
      EXPECT_EQ(R->NativeCalls[20].Arguments[1], 0x100000007ULL);
      EXPECT_EQ(R->NativeCalls[20].Result, 0x100000007ULL);
      EXPECT_EQ(R->NativeCalls[21].Result, 0x100000007ULL);
      EXPECT_EQ(R->NativeCalls[22].Arguments[3], 0x100000000ULL);
      EXPECT_EQ(R->NativeCalls[24].Arguments[1], UINT64_MAX);
      EXPECT_EQ(R->NativeCalls[24].Result, UINT64_MAX);
      EXPECT_EQ(R->NativeCalls[26].Arguments[3], UINT64_MAX);
      EXPECT_EQ(uint32_t(R->NativeCalls[31].Arguments[2]), 0x123401a1u);
      EXPECT_EQ(uint32_t(R->NativeCalls[38].Arguments[2]), 0641u);
      EXPECT_EQ(Options.DarwinFiles->Files.at("/data"),
                (std::vector<uint8_t>{'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9'}));
      EXPECT_EQ(Options.DarwinFiles->Files.size(), 1u);
    }
    Options.Limits.Events = 1;
    auto Limited = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
    EXPECT_EQ(Limited->Events, 1u);
    ASSERT_EQ(Limited->NativeCalls.size(), 1u);
    EXPECT_EQ(Limited->NativeCalls.front().Name, "___error");
    EXPECT_TRUE(Limited->StandardOutput.empty());
    Options.Limits.Events = process_defaults::Events;
    Options.DarwinFiles.reset();
    auto Unknown = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
    EXPECT_EQ(Unknown->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Unknown->Diagnostic, darwin_model::diagnostic::FileInputs);
    ASSERT_EQ(Unknown->NativeCalls.size(), 2u);
    EXPECT_EQ(Unknown->NativeCalls.back().Name, "_open");
    EXPECT_FALSE(Unknown->NativeCalls.back().Result);
    EXPECT_TRUE(Unknown->StandardOutput.empty());
    Options.DarwinFiles = darwin_test::libSystemFileOptions();
  }
}

TEST_P(DarwinProcess, LibSystemMemoryCallsPreserveMappingsAndCErrorOrder) {
  const auto Page = GetParam().ISA == GuestArchitecture::X64 ? 4096u : 16384u;
  Options.InstructionQuantum = 1024;
  Options.Arguments = {"memory", std::to_string(Page), "/data"};
  Options.DarwinFiles = darwin_test::libSystemMemoryOptions(Page);
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    const auto Linked =
        std::filesystem::path(Path.string() + "-libsystem-memory-" + Kind);
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto R = emulateProcess(Linked, GetParam().OS, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 79);
      EXPECT_EQ(R->ReturnValue, 79u);
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
      EXPECT_EQ(R->StandardOutput, std::string("M\0P", 3));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_TRUE(R->Services.empty());
      EXPECT_EQ(R->Events, 19u);
      ASSERT_EQ(R->NativeCalls.size(),
                std::size(darwin_test::LibSystemMemoryCalls));
      for (unsigned I = 0; I != R->NativeCalls.size(); ++I) {
        const auto &Call = R->NativeCalls[I];
        const auto &Expected = darwin_test::LibSystemMemoryCalls[I];
        EXPECT_EQ(Call.Name, Expected.Name);
        EXPECT_EQ(Call.ArgumentCount, Expected.Count);
        EXPECT_EQ(Call.Module, "/usr/lib/libSystem.B.dylib");
        EXPECT_TRUE(Call.ReturnAddress);
        EXPECT_TRUE(Call.Result);
      }
      for (unsigned I : {1u, 2u, 5u, 9u, 10u})
        EXPECT_EQ(R->NativeCalls[I].Result, UINT64_MAX);
      const auto Address = R->NativeCalls[3].Result;
      ASSERT_TRUE(Address);
      EXPECT_NE(*Address, UINT64_MAX);
      EXPECT_EQ(R->NativeCalls[4].Arguments[0], *Address);
      EXPECT_EQ(R->NativeCalls[5].Arguments[0], *Address + 1);
      EXPECT_EQ(R->NativeCalls[8].Arguments[0], *Address);
      EXPECT_EQ(uint32_t(R->NativeCalls[3].Arguments[3]), 0x1002u);
      EXPECT_EQ(R->NativeCalls[12].Arguments[5], Page);
      EXPECT_EQ(R->NativeCalls[15].Arguments[3], Page);
      EXPECT_EQ(Options.DarwinFiles->Files,
                darwin_test::libSystemMemoryOptions(Page).Files);
    }
    Options.Limits.Events = 3;
    auto Limited = emulateProcess(Linked, GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited));
    EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
    EXPECT_EQ(Limited->Events, 3u);
    ASSERT_EQ(Limited->NativeCalls.size(), 3u);
    EXPECT_TRUE(Limited->StandardOutput.empty());
    EXPECT_EQ(Limited->NativeCalls.back().Result, UINT64_MAX);
    Options.Limits.Events = process_defaults::Events;
  }
}

TEST_P(DarwinProcess, DylibConstructorsAndTLSCallbacksKeepStartupAndExitOrder) {
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    const auto Directory =
        std::filesystem::path(Path.string() + "-callbacks-" + Kind);
    for (const auto &Test : darwin_test::CallbackCases) {
      SCOPED_TRACE(Test.Mode);
      darwin_test::callbackOptions(Options, Directory, Test.Mode);
      for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
        auto R = emulateProcess(Directory / "main", GetParam().OS, Options);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
        EXPECT_EQ(R->ExitStatus, Test.Status);
        EXPECT_EQ(R->StandardOutput, Test.Output);
        EXPECT_TRUE(R->StandardError.empty());
        EXPECT_TRUE(R->Services.empty());
        EXPECT_EQ(bool(R->ReturnValue), llvm::StringRef(Test.Mode) == "return");
        unsigned Registrations = 0;
        for (const auto &Call : R->NativeCalls)
          if (Call.Name == "__tlv_atexit") {
            ++Registrations;
            EXPECT_EQ(Call.ArgumentCount, 2u);
            EXPECT_FALSE(Call.Result);
            EXPECT_TRUE(Call.ReturnAddress);
          }
        EXPECT_EQ(Registrations, llvm::StringRef(Test.Mode) == "stack-exit" ? 4u
                                 : llvm::StringRef(Test.Mode).contains("raw")
                                     ? 2u
                                     : 3u);
      }
    }
    darwin_test::callbackOptions(Options, Directory, "dtor-exit");
    auto Reentrant = emulateProcess(Directory / "main", GetParam().OS, Options);
    ASSERT_TRUE(bool(Reentrant)) << llvm::toString(Reentrant.takeError());
    EXPECT_EQ(Reentrant->Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_NE(Reentrant->Diagnostic.find("reentrant"), std::string::npos);
    EXPECT_EQ(Reentrant->StandardOutput, "LPEabFM2");
    EXPECT_FALSE(Reentrant->ExitStatus);
    EXPECT_FALSE(Reentrant->ReturnValue);
    darwin_test::callbackOptions(Options, Directory, "return");
    auto Reference = emulateProcess(Directory / "main", GetParam().OS, Options);
    ASSERT_TRUE(bool(Reference));
    // Stop inside the last TLS destructor: main's value is still private.
    Options.Limits.Events = Reference->Events - 1;
    auto Limited = emulateProcess(Directory / "main", GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
    EXPECT_FALSE(Limited->ExitStatus);
    EXPECT_FALSE(Limited->ReturnValue);
    EXPECT_EQ(Limited->StandardOutput, "LPEabFM2N1");
    Options.Limits.Events = process_defaults::Events;
    Options.Limits.Instructions = Reference->Instructions - 1;
    Limited = emulateProcess(Directory / "main", GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, ProcessStopReason::InstructionLimit);
    EXPECT_FALSE(Limited->ExitStatus);
    EXPECT_FALSE(Limited->ReturnValue);
    Options.Limits.Instructions = process_defaults::Instructions;
  }
}
TEST_P(DarwinProcess, StartupDylibsShareDataAndKeepThreadLocalInstances) {
  Options.InstructionQuantum = 1024;
  for (const char *Kind : {"classic", "chained"}) {
    const auto Directory =
        std::filesystem::path(Path.string() + "-dylib-" + Kind);
    Options.Darwin =
        DarwinProcessOptions{{{"@loader_path/libneverd-base.dylib",
                               Directory / "libneverd-base.dylib"},
                              {"@loader_path/libneverd-leaf.dylib",
                               Directory / "libneverd-leaf.dylib"},
                              {"unused.dylib", Directory / "does-not-exist"}}};
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto R = emulateProcess(Directory / "main", GetParam().OS, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 83);
      EXPECT_EQ(R->StandardOutput, std::string("D\0T", 3));
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_TRUE(R->Services.empty());
      ASSERT_FALSE(R->NativeCalls.empty());
      EXPECT_EQ(R->NativeCalls.back().Name, "_write");
      unsigned TLV = 0;
      for (const auto &Call : R->NativeCalls) {
        if (Call.Name != "__tlv_get_addr")
          continue;
        ++TLV;
        EXPECT_EQ(Call.ArgumentCount, 1u);
        EXPECT_TRUE(Call.Result);
      }
      EXPECT_GT(TLV, 10u);
    }
    Options.Limits.Events = 2;
    auto Limited = emulateProcess(Directory / "main", GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
    EXPECT_EQ(Limited->Events, 2u);
    EXPECT_TRUE(Limited->StandardOutput.empty());
    Options.Limits.Events = process_defaults::Events;
  }
}
TEST_P(DarwinProcess, StartupDylibAddendsPreserveDataAndTLSAliasing) {
  Options.InstructionQuantum = 1024;
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    const auto Directory =
        std::filesystem::path(Path.string() + "-dylib-addends-" + Kind);
    Options.Darwin =
        DarwinProcessOptions{{{"@loader_path/libneverd-addends.dylib",
                               Directory / "libneverd-addends.dylib"}}};
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      SCOPED_TRACE(Repeat);
      auto R = emulateProcess(Directory / "main", GetParam().OS, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, 78);
      EXPECT_EQ(R->ReturnValue, 78u);
      EXPECT_TRUE(R->StandardOutput.empty());
      EXPECT_TRUE(R->StandardError.empty());
      EXPECT_TRUE(R->Services.empty());
      unsigned PID = 0, TLS = 0;
      for (const auto &Call : R->NativeCalls) {
        PID += Call.Name == "_getpid";
        TLS += Call.Name == "__tlv_get_addr";
        EXPECT_TRUE(Call.Result);
      }
      EXPECT_EQ(PID, 1u);
      EXPECT_GT(TLS, 0u);
      EXPECT_EQ(R->NativeCalls.size(), PID + TLS);
      EXPECT_EQ(R->Events, R->NativeCalls.size() + 1);
      EXPECT_EQ(R->SelectedBackend, GetParam().Backend);
    }
    Options.Limits.Events = 1;
    auto Limited = emulateProcess(Directory / "main", GetParam().OS, Options);
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
    EXPECT_EQ(Limited->Events, 1u);
    EXPECT_TRUE(Limited->StandardOutput.empty());
    Options.Limits.Events = process_defaults::Events;
  }
}
INSTANTIATE_TEST_SUITE_P(Transports, DarwinProcess,
                         testing::ValuesIn(profiles()),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return std::string(P.param.Name) + "_" +
                                  executionBackendName(P.param.Backend);
                         });
} // namespace
} // namespace neverd::emulation

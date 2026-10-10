//===- DarwinProcessTests.cpp - Real macOS/iOS Mach-O workloads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinEntropyTestData.h"
#include "DarwinFileTestData.h"
#include "DarwinSystemTestData.h"
#include "DarwinTestImage.h"
#include "DarwinTimeTestData.h"
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

INSTANTIATE_TEST_SUITE_P(Transports, DarwinProcess,
                         testing::ValuesIn(profiles()),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return std::string(P.param.Name) + "_" +
                                  executionBackendName(P.param.Backend);
                         });
} // namespace
} // namespace neverd::emulation

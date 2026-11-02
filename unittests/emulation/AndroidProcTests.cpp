//===- AndroidProcTests.cpp - Live proc through Android native programs ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real guest thread, libc buffering and raw syscall paths share OS owners.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
class AndroidProc : public testing::TestWithParam<
                        std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixture compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 2;
    Options.InstructionQuantum = 101;
    Options.Limits.Instructions = 1000000;
    Options.Limits.Events = 20000;
    Options.Limits.TimeoutMicroseconds = 30000000;
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->DynamicProc = LinuxProcOptions{"fixture", 1};
    Options.LinuxFiles->Files["/fixture/value"] = {'a', 'b', 'c'};
    Options.LinuxFiles->WriteLimits["/fixture/value"] = 16384;
    Options.LinuxFiles->TrackWriteMetadata.insert("/fixture/value");
    LinuxFileMetadata M;
    M.Mode = 0100644;
    M.Size = 3;
    M.Inode = 42;
    M.LinkCount = 1;
    M.BlockSize = 4096;
    M.Blocks = 8;
    Options.LinuxFiles->Metadata["/fixture/value"] = M;
    Options.LinuxTime.emplace().Clocks[0] = {100, 7};
#endif
  }
  ProcessResult run(const char *Entry) {
    Options.Android->EntrySymbol = Entry;
    auto R = llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    return R;
  }
};
TEST_P(AndroidProc, RealThreadCreationAndExitReachDIRAndBufferedStatus) {
  auto R = run("proc_thread_lifecycle");
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Finished);
  EXPECT_TRUE(R.NativeThreads[1].Retired);
}
TEST_P(AndroidProc, RawAndBionicReadsObserveLiveMappingChanges) {
  run("proc_live_maps");
}
TEST_P(AndroidProc, NativeWritesPublishCompleteStatAndLiveFDInfo) {
  run("proc_write_stat");
}

TEST_P(AndroidProc, NegativePrctlUsesKernelIntWidthAcrossEveryCaller) {
  constexpr uint64_t Buffer = 0x20000000;
  const uint64_t Values[] = {0x80000000ULL, 0xffffffffULL,
                             0xffffffff80000000ULL, 0x1234567880000001ULL,
                             0x800000000000000fULL};
  Options.Android->EntrySymbol = "proc_prctl_option";
  Options.Android->Libraries["libprctl.so"] = {"prctl"};
  Options.Android->Memory.push_back(
      {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
  Options.Android->ReadMemory.push_back({Buffer, 4096});
  for (uint64_t Route = 0; Route != 5; ++Route) {
    for (unsigned Case = 0; Case != 5; ++Case) {
      if (Route == 4 && Case)
        continue;
      SCOPED_TRACE(Route);
      SCOPED_TRACE(Case);
      Options.Android->Arguments = {Buffer, Values[Case], Route};
      auto Result = llvm::cantFail(
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
      const bool Refused = Route == 4 || Case == 4;
      ASSERT_EQ(Result.Stop, Refused ? ProcessStopReason::UnsupportedService
                                     : ProcessStopReason::Returned)
          << Result.Diagnostic;
      std::vector<uint8_t> Expected(4096, 0xa5);
      if (Refused) {
        EXPECT_FALSE(Result.ReturnValue);
        EXPECT_NE(
            Result.Diagnostic.find(Route == 4 ? "inactive" : "prctl operation"),
            std::string::npos);
      } else {
        EXPECT_EQ(Result.ReturnValue, 0u);
        llvm::support::endian::write64le(
            Expected.data(), Route == 1 ? uint64_t(0) - 22 : UINT64_MAX);
        llvm::support::endian::write64le(Expected.data() + 8,
                                         Route == 1 ? 733 : 22);
      }
      ASSERT_EQ(Result.MemorySnapshots.size(), 1u);
      EXPECT_EQ(Result.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidProc, PrctlSharesBorrowedNamesAcrossNativeSyscallAndRawPaths) {
  Options.LinuxKernel.emplace().AnonymousVMANames =
      LinuxAnonymousVMANames::AndroidUserPointer;
  auto R = run("proc_prctl_names");
  bool Raw = false, Native = false, Syscall = false;
  for (const auto &E : R.Services)
    Raw |= E.Number == 167;
  for (const auto &E : R.NativeCalls) {
    Native |= E.Name == "prctl";
    Syscall |= E.Name == "syscall";
  }
  EXPECT_TRUE(Raw);
  EXPECT_TRUE(Native);
  EXPECT_TRUE(Syscall);
}

TEST_P(AndroidProc, PrctlNamesRetireWithGuestThreadMappings) {
  Options.LinuxKernel.emplace().AnonymousVMANames =
      LinuxAnonymousVMANames::AndroidUserPointer;
  auto R = run("proc_prctl_thread_retirement");
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Retired);
}

TEST_P(AndroidProc, PrctlUnspecifiedAndUnknownContractsRemainUnsupported) {
  for (const char *Entry : {"proc_prctl_missing", "proc_prctl_unknown"}) {
    Options.Android->EntrySymbol = Entry;
    auto R = llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(R.NativeCalls.empty());
    if (!R.NativeCalls.empty()) {
      EXPECT_EQ(R.NativeCalls.back().Name, "prctl");
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
    Options.LinuxKernel.emplace().AnonymousVMANames =
        LinuxAnonymousVMANames::AndroidUserPointer;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidProc,
    testing::Combine(testing::Values("proc-O0-none", "proc-O2-none",
                                     "proc-O0-android", "proc-O2-android",
                                     "proc-O0-relr", "proc-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation

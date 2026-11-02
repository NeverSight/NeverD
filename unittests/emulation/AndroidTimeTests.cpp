//===- AndroidTimeTests.cpp - Explicit clocks through original guest C ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidTime : public testing::TestWithParam<
                        std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(Options.Backend, GuestArchitecture::AArch64))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.InstructionQuantum = 3;
    Options.LinuxTime.emplace();
    Options.LinuxTime->Clocks = {{0, {4294967297, 987654321}},
                                 {1, {123, 456789}}};
    Options.LinuxTime->Timezone = LinuxTimezone{-60, 2};
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  ProcessResult call(uint64_t Op, uint64_t A = 0, uint64_t B = 0,
                     uint64_t RO = 0) {
    return run("time_call", {Op, A, B, Buffer + 256, RO});
  }
  void returned(const ProcessResult &R, uint64_t Value, uint32_t Errno = 73) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 256),
              Errno);
  }
  void incomplete(const ProcessResult &R, ProcessStopReason Reason) {
    EXPECT_EQ(R.Stop, Reason) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    if (!R.NativeCalls.empty() && R.Services.empty())
      EXPECT_FALSE(R.NativeCalls.back().Result);
    if (Reason == ProcessStopReason::RuntimeFailure) {
      EXPECT_TRUE(R.MemorySnapshots.empty());
      return;
    }
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 256),
              0xa5a5a5a5u);
  }
};
TEST_P(AndroidTime, RepeatedCallsShareExactFixedClocksAndRetainErrno) {
  auto R = run("time_sequence", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  const uint64_t Expected[] = {
      4294967297, 4294967297, 4294967297, 4294967297, 987654321,
      123,        456789,     4294967297, 987654,     uint64_t(0) - 60,
      2,          4294967297, 73};
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  auto Bytes = std::vector<uint8_t>(8192, 0xa5);
  for (size_t I = 0; I < std::size(Expected); ++I)
    llvm::support::endian::write64le(Bytes.data() + I * 8, Expected[I]);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
}
TEST_P(AndroidTime, TimeUsesSigned64BitsAndOnlyWritesItsUnalignedObject) {
  for (int64_t Seconds : {int64_t(0), int64_t(-1), INT64_MIN, INT64_MAX}) {
    SCOPED_TRACE(Seconds);
    Options.LinuxTime->Clocks[0].Seconds = Seconds;
    returned(call(0), static_cast<uint64_t>(Seconds));
    auto R = call(0, Buffer + 1);
    returned(R, static_cast<uint64_t>(Seconds));
    auto Expected = std::vector<uint8_t>(8192, 0xa5);
    llvm::support::endian::write64le(Expected.data() + 1,
                                     static_cast<uint64_t>(Seconds));
    llvm::support::endian::write32le(Expected.data() + 256, 73);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidTime, KnownClockIDsKeepIndependentObservationsAndLow32ABI) {
  for (int32_t ID : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11}) {
    Options.LinuxTime->Clocks[ID] = {100 + ID, 999999999 - ID};
    auto R = call(4, 0x1234567800000000ULL | ID, Buffer + 1);
    returned(R, 0);
    auto *B = R.MemorySnapshots[0].Bytes.data() + 1;
    EXPECT_EQ(llvm::support::endian::read64le(B), uint64_t(100 + ID));
    EXPECT_EQ(llvm::support::endian::read64le(B + 8), uint64_t(999999999 - ID));
  }
}
TEST_P(AndroidTime, RawMonotonicAdvancesAcrossNamedRawAndDynamicCalls) {
  Options.LinuxTime->AdvanceOnIdle = true;
  Options.LinuxTime->Clocks[4] = {4294967301, 999999996};
  Options.Android->Libraries["libclock-model.so"] = {"clock_gettime"};
  auto R = run("raw_clock_sequence", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const uint64_t Words[] = {
      4294967301, 999999996, 4294967301, 999999996, 4294967301, 999999996,
      123,        456789,    4294967303, 3,         4294967303, 3,
      4294967303, 3,         124,        456796,    73};
  auto Expected = std::vector<uint8_t>(8192, 0xa5);
  for (size_t I = 0; I < std::size(Words); ++I)
    llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  unsigned Dynamic = 0;
  for (const auto &Call : R.NativeCalls)
    if (Call.Library == "libclock-model.so" && Call.Name == "clock_gettime") {
      EXPECT_EQ(Call.Result, 0u);
      ++Dynamic;
    }
  EXPECT_EQ(Dynamic, 2u);
  ASSERT_EQ(R.Services.size(), 2u);
  for (const auto &Service : R.Services) {
    EXPECT_EQ(Service.Number, 113u);
    EXPECT_EQ(Service.Arguments[0], 0x1234567800000004ULL);
    EXPECT_EQ(Service.Result, 0u);
  }
}

TEST_P(AndroidTime, MissingRawMonotonicNeverAliasesAnAdvancingMonotonicClock) {
  Options.LinuxTime->AdvanceOnIdle = true;
  for (unsigned Op : {2, 4}) {
    auto R = call(Op, 4, Buffer + 1);
    incomplete(R, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R.Diagnostic, "Linux clock 4 has no explicit linux_time input");
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
  }
}

TEST_P(AndroidTime, MissingValuesNeverBorrowHostTimeOrDefaultToEpoch) {
  Options.LinuxTime.reset();
  for (auto Op : {0, 1, 2})
    incomplete(call(Op, Op == 1 ? Buffer : 0, Op == 2 ? Buffer : 0),
               ProcessStopReason::UnsupportedService);
  returned(call(1), 0);
  returned(call(3), 0);
  incomplete(call(1, 0, Buffer), ProcessStopReason::UnsupportedService);
  Options.LinuxTime.emplace();
  Options.LinuxTime->Timezone = LinuxTimezone{INT32_MIN, INT32_MAX};
  auto R = call(1, 0, Buffer);
  returned(R, 0);
  EXPECT_EQ(llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data()),
            0x80000000u);
  EXPECT_EQ(
      llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data() + 4),
      0x7fffffffu);
}
TEST_P(AndroidTime, InvalidClockPrecedesPointerAccessAndEncodedClocksStop) {
  Options.LinuxTime.reset();
  for (auto ID : {10, 12, INT32_MAX}) {
    returned(call(2, ID, 1), UINT64_MAX, 22);
    returned(call(4, ID, 1), uint64_t(0) - 22);
  }
  incomplete(call(2, UINT64_MAX, 1), ProcessStopReason::UnsupportedService);
  incomplete(call(4, 0xfffffffdu, 1), ProcessStopReason::UnsupportedService);
  incomplete(call(5, 0, Buffer), ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidTime, ProcessCPUClockContractsShareRawAndBionicObservations) {
  struct KernelCase {
    std::optional<AndroidGKIKernel> Kernel;
    const char *Label;
  };
  constexpr KernelCase Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  {AndroidGKIKernel::Name, Label},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
      {std::nullopt, "closed task catalogue without GKI"},
  };
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    auto &Kernel = Options.LinuxKernel.emplace();
    Kernel.GKI = K.Kernel;
    if (!K.Kernel)
      Kernel.UnavailableSyscalls.insert(LinuxUnavailableSyscall::PidFDOpen);
    Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
    Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
    Options.LinuxTime->Clocks[2] = {3, 4};
    Options.LinuxTime->Clocks[-16006] = {7, 9};
    Options.LinuxTime->Clocks[-8] = {13, 15};
    Options.LinuxTime->Clocks[-8007] = {16, 17};
    for (int32_t ID : {2, -6, -8006, -16006, -8, -8008, -7, -8007}) {
      SCOPED_TRACE(ID);
      const bool Foreign = ID == -16006;
      const bool Prof = ID == -8 || ID == -8008;
      const bool Virt = ID == -7 || ID == -8007;
      for (unsigned Op : {2, 4}) {
        // clockid_t consumes only the low 32 bits in both interfaces.
        auto R = call(Op, 0x1234567800000000ULL | uint32_t(ID), Buffer + 1);
        returned(R, 0);
        auto Expected = std::vector<uint8_t>(8192, 0xa5);
        llvm::support::endian::write64le(Expected.data() + 1, Foreign ? 7
                                                              : Prof  ? 13
                                                              : Virt  ? 16
                                                                      : 3);
        llvm::support::endian::write64le(Expected.data() + 9, Foreign ? 9
                                                              : Prof  ? 15
                                                              : Virt  ? 17
                                                                      : 4);
        llvm::support::endian::write32le(Expected.data() + 256, 73);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      }
    }
    for (int32_t ID : {-16014, -24006, -1}) {
      for (unsigned Op : {2, 4}) {
        auto R = call(Op, uint32_t(ID), 1);
        returned(R, Op == 2 ? UINT64_MAX : uint64_t(0) - 22, Op == 2 ? 22 : 73);
        auto Expected = std::vector<uint8_t>(8192, 0xa5);
        llvm::support::endian::write32le(Expected.data() + 256,
                                         Op == 2 ? 22 : 73);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      }
    }
    returned(call(2, uint32_t(-16006), 1), UINT64_MAX, 14);
    returned(call(4, uint32_t(-16006), 1), uint64_t(0) - 14);
  }
}
TEST_P(AndroidTime, ReleasedGKIProcessCPUClocksKeepUnobservedBoundaries) {
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.GKI = AndroidGKIKernel::Android17_6_18;
  Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
  for (unsigned Op : {2, 4}) {
    incomplete(call(Op, uint32_t(-16006), 1),
               ProcessStopReason::UnsupportedService);
    incomplete(call(Op, uint32_t(-29), 1),
               ProcessStopReason::UnsupportedService);
    incomplete(call(Op, uint32_t(-2), 1),
               ProcessStopReason::UnsupportedService);
  }
  Kernel.Tasks.reset();
  for (unsigned Op : {2, 4})
    incomplete(call(Op, uint32_t(-16006), 1),
               ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidTime, EmptyTaskCatalogueAdmitsSelfAndObservesForeignAbsence) {
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.Tasks.emplace();
  Options.LinuxTime->Clocks[2] = {3, 4};
  for (unsigned Op : {2, 4}) {
    for (int32_t ID : {-6, -8006}) {
      auto R = call(Op, uint32_t(ID), Buffer + 1);
      returned(R, 0);
      auto Expected = std::vector<uint8_t>(8192, 0xa5);
      llvm::support::endian::write64le(Expected.data() + 1, 3);
      llvm::support::endian::write64le(Expected.data() + 9, 4);
      llvm::support::endian::write32le(Expected.data() + 256, 73);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
    auto R = call(Op, uint32_t(-16006), 1);
    returned(R, Op == 2 ? UINT64_MAX : uint64_t(0) - 22, Op == 2 ? 22 : 73);
    auto Expected = std::vector<uint8_t>(8192, 0xa5);
    llvm::support::endian::write32le(Expected.data() + 256, Op == 2 ? 22 : 73);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    incomplete(call(Op, uint32_t(-29), 1),
               ProcessStopReason::UnsupportedService);
    incomplete(call(Op, uint32_t(-2), 1),
               ProcessStopReason::UnsupportedService);
  }
  Kernel.Tasks->emplace(2000, LinuxKernelTask{true});
  for (unsigned Op : {2, 4})
    incomplete(call(Op, uint32_t(-16006), 1),
               ProcessStopReason::UnsupportedService);
  Kernel.Tasks.reset();
  for (unsigned Op : {2, 4})
    incomplete(call(Op, uint32_t(-6), 1),
               ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidTime, UserSpaceTimeStoreAndKernelFaultHaveDistinctOutcomes) {
  for (uint64_t Address : {uint64_t(1), UINT64_MAX, Buffer + 4096}) {
    const uint64_t RO = Address == Buffer + 4096 ? Address : 0;
    incomplete(call(0, Address, 0, RO), ProcessStopReason::RuntimeFailure);
    returned(call(1, Address, 0, RO), UINT64_MAX, 14);
    returned(call(3, Address, 0, RO), uint64_t(0) - 14);
    returned(call(2, 0, Address, RO), UINT64_MAX, 14);
    returned(call(4, 0, Address, RO), uint64_t(0) - 14);
  }
}
TEST_P(AndroidTime, GettimeofdayRetainsFirstFieldBeforeSecondFieldFault) {
  for (unsigned Op : {1, 3}) {
    auto R = call(Op, Buffer + 4088, 0, Buffer + 4096);
    returned(R, Op == 1 ? UINT64_MAX : uint64_t(0) - 14, Op == 1 ? 14 : 73);
    auto Expected = std::vector<uint8_t>(8192, 0xa5);
    llvm::support::endian::write64le(Expected.data() + 4088, 4294967297);
    llvm::support::endian::write32le(Expected.data() + 256, Op == 1 ? 14 : 73);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidTime, TimezoneWritesFollowTimevalIncludingAliasesAndFaults) {
  auto R = call(1, Buffer, 1);
  returned(R, UINT64_MAX, 14);
  EXPECT_EQ(llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data()),
            4294967297u);
  EXPECT_EQ(
      llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() + 8),
      987654u);
  R = call(1, Buffer, Buffer);
  returned(R, 0);
  EXPECT_EQ(llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data()),
            uint32_t(0) - 60);
  EXPECT_EQ(
      llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data() + 4),
      2u);
  EXPECT_EQ(
      llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() + 8),
      987654u);
  Options.LinuxTime->Timezone.reset();
  R = call(1, Buffer, Buffer + 32);
  incomplete(R, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data()),
            4294967297u);
}
TEST_P(AndroidTime, MixedAccessWithinOneKernelCopyStopsBeforeUnknownEffects) {
  for (unsigned Op : {2, 4}) {
    auto R = call(Op, 0, Buffer + 4088, Buffer + 4096);
    incomplete(R, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
  }
  auto R = call(1, Buffer + 4092, 0, Buffer + 4096);
  incomplete(R, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
}
TEST_P(AndroidTime, NamedDynamicCallsKeepProviderIdentityAndLifetime) {
  Options.Android->Libraries["libclock-model.so"] = {"time", "clock_gettime",
                                                     "gettimeofday"};
  auto R = run("time_dynamic", {Buffer, 0});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  unsigned Count = 0;
  for (const auto &C : R.NativeCalls)
    if (C.Library == "libclock-model.so" &&
        (C.Name == "time" || C.Name == "clock_gettime" ||
         C.Name == "gettimeofday")) {
      EXPECT_TRUE(C.Result);
      ++Count;
    }
  EXPECT_EQ(Count, 3u);
  R = run("time_dynamic", {Buffer, 1});
  incomplete(R, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(
    OptimizationPackingAndBackend, AndroidTime,
    testing::Combine(testing::Values("time-O0-none", "time-O0-android",
                                     "time-O0-relr", "time-O2-none",
                                     "time-O2-android", "time-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF)));
} // namespace
} // namespace neverd::emulation
